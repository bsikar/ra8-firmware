/**
 * @file sim_run.c
 * @brief Chunked run loop + report implementation (see sim_run.h)
 *
 * @details
 * The presentation-buffer setup, run-guard environment knobs, the chunked
 * run loop with its inner exception-resolve loop, the run-end report and the
 * exit-code mapping -- moved verbatim out of the board_sim main translation
 * unit (the loop body is unchanged; the former main() locals it consumed now
 * arrive through ::sim_run_cfg_t).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board_console.h"
#include "board_input.h"
#include "board_net.h"
#include "board_overlay.h"
#include "board_periph.h"
#include "board_periph_sd.h"
#include "board_usb.h"
#include "board_view.h"
#include "sim_console.h"
#include "sim_cpu1.h"
#include "sim_elf.h"
#include "sim_engine.h"
#include "sim_exc.h"
#include "sim_memmap.h"
#include "sim_mmio.h"
#include "sim_mpu.h"
#include "sim_prof.h"
#include "sim_seams.h"
#include "sim_usbh_seam.h"
#include "sim_view.h"

/**
 * @brief Run one outer chunk's inner exception-resolve loop.
 *
 * @details
 * Runs a chunk, then services exceptions to a steady state before the next
 * chunk: zero-time seam relaunches re-enter directly; an EXC_RETURN branch is
 * unstacked and the NVIC re-checked so a still-pending lower-priority
 * exception tail-chains (e.g. PendSV right after SysTick) exactly as hardware
 * would, instead of briefly resuming the interrupted code; MPU / div-0 faults
 * are synthesised at the boundary; SVCall (taken in on_intr) just leaves PC
 * at the handler, which the next relaunch runs. Returns once the full budget
 * elapsed with nothing pending, or on a fault / BKPT / stop request.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base VTOR fallback for exception vectoring.
 * @param[in,out] run_pc_io Resume PC in, final PC out.
 * @param[out]    err_out   Final uc_emu_start status for the report.
 * @return true when the run must end (fault or BKPT), false to continue.
 * @retval true  The outer loop breaks and reports the stop cause.
 * @retval false The chunk resolved cleanly; the outer loop continues.
 * @pre The engine is set up and @p run_pc_io holds a valid resume PC.
 * @pre The seams / exception hooks are installed.
 * @post @p run_pc_io and @p err_out reflect the loop's final state.
 * @note Not thread-safe; this is the single-threaded run core.
 * @since 0.1.0
 */
static bool run_inner(uc_engine* uc, uint32_t vtor_base, uint32_t* run_pc_io, uc_err* err_out)
{
  uint32_t run_pc  = *run_pc_io;
  uc_err   err     = UC_ERR_OK;
  bool     faulted = false;
  for (uint32_t inner = 0U; inner < (uint32_t)k_run_inner_max; inner++) {
    sim_exc_clear_pendsv_stop(); /* set by on_icsr_write iff this run ends on PENDSVSET */
    /* Idle fast-forward: when the core is parked on a wait-for-interrupt spin
     * (ThreadX's __tx_wait_here `b .`, or a `wfi` halt), running a full
     * k_run_chunk_insns just burns wall time to reach the SAME SysTick that is
     * already armed once per outer chunk. Cap the budget to k_idle_spin_insns
     * so the spin returns at once and the boundary below takes the tick now.
     * This skips only idle wall-time: the tick stays armed once per outer
     * chunk and fired once, so ThreadX's tick COUNT -- and every sleep/heartbeat
     * deadline measured in ticks -- is identical to a full idle chunk. Busy
     * firmware never parks on these opcodes, so it runs the full budget. */
    /* Low-power (Model A): the M33 runs ~4x slower than the M85, so a busy
     * chunk advances 1/4 as many instructions per modelled tick. Idle spins
     * still collapse to k_idle_spin_insns regardless. */
    size_t busy_budget = (size_t)k_run_chunk_insns;
    if (sim_low_power()) {
      busy_budget = (size_t)k_run_chunk_insns / (size_t)k_low_power_div;
    }
    const size_t run_budget = idle_spin_at(uc, run_pc) ? (size_t)k_idle_spin_insns : busy_budget;
    err                     = uc_emu_start(uc, (uint64_t)run_pc | 1U, 0, 0, run_budget);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
    if (sim_seam_take_relaunch()) {
      /* A --fast-sd byte-exchange returned to its caller. This consumed no
       * modelled time, so relaunch from the returned PC without advancing the
       * SysTick or charging a chunk (the inner-loop cap still bounds the run). */
      continue;
    }
    if (sim_prof_stop_hit()) {
      break; /* BOARD_SIM_STOP_PC reached (set in prof_insn_hook) -- end the run. */
    }
    if (sim_exc_reboot_requested()) {
      break; /* AIRCR.SYSRESETREQ -- the outer wrapper performs the reboot. */
    }
    if (sim_exc_bkpt_hit()) {
      faulted = true; /* firmware trapped on a BKPT -- end the run, report it. */
      break;
    }
    uint64_t exc_ret_pc = 0U;
    if (sim_exc_take_exc_return(&exc_ret_pc)) {
      exc_return(uc, (uint32_t)exc_ret_pc);
      /* Tail-chain the next pend, but do NOT advance the SysTick: an exception
       * return consumes no modelled time, so time advances only on a full
       * instruction budget (below). This keeps a deferred tick from firing the
       * instant a PendSV context switch unstacks into the new thread. */
      (void)exc_take_pending(uc, vtor_base, false);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      continue;
    }
    if (err != UC_ERR_OK) {
      faulted = true;
      break;
    }
    if (sim_mpu_fault_pending()) {
      /* A store hit a read-only MPU region (on_mpu_ro_write stopped us).
       * Synthesise MemManage at this boundary -- no time advances (a fault is
       * synchronous), and the stacked PC is the faulting store. */
      sim_mpu_clear_fault();
      mpu_synth_memmanage(uc, vtor_base);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      continue;
    }
    if (sim_div0_fault_pending()) {
      /* A UDIV/SDIV by zero with CCR.DIV_0_TRP set (emulate_div0_patched stopped
       * us). Synthesise UsageFault at this boundary -- synchronous, no time
       * advances, and the stacked PC is the trapping divide. */
      sim_div0_clear_fault();
      div0_synth_usagefault(uc, vtor_base);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      continue;
    }
    if (sim_exc_pendsv_stop()) {
      /* Context-switch stop: a thread wrote PENDSVSET to yield. Take PendSV
       * but do NOT advance the SysTick -- a context switch consumes no
       * modelled time, so a thread that just suspended on a tick wait keeps
       * waiting and the scheduler runs the highest-priority READY thread. This
       * is what lets a low-priority worker (e.g. the NetX echo thread) run to
       * completion before a higher-priority sleeper's tick expires. */
      (void)exc_take_pending(uc, vtor_base, false);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      continue;
    }
    /* Full instruction budget elapsed: a tick's worth of genuine execution (or
     * an idle spin) has passed, so advance time -- take the highest-priority
     * pending exception (SysTick this period, and/or PendSV) and resolve it in
     * this same chunk so a context switch does not cost a scheduling quantum. */
    if (exc_take_pending(uc, vtor_base, true)) {
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      continue;
    }
    break;
  }
  *run_pc_io = run_pc;
  *err_out   = err;
  return faulted;
}

/** @brief Implementation of `sim_run_and_report()` -- the whole run phase. */
int sim_run_and_report(const sim_run_cfg_t* cfg)
{
  uc_engine*         uc              = cfg->uc;
  uint8_t* const     elf             = cfg->elf;
  const long         elf_len         = cfg->elf_len;
  const uint32_t     pc              = cfg->initial_pc;
  const uint32_t     vtor_base       = cfg->vtor_base;
  const bool         want_trace      = cfg->want_trace;
  const bool         want_view       = cfg->want_view;
  const bool         want_click      = cfg->want_click;
  const int          click_x         = cfg->click_x;
  const int          click_y         = cfg->click_y;
  const char* const  ppm_path        = cfg->ppm_path;
  const char* const  record_dir      = cfg->record_dir;
  const uint32_t     rotate_deg      = cfg->rotate_deg;
  int                reboot_count    = cfg->reboot_count;
  const char* const  save_sd_path    = cfg->save_sd_path;
  const uint32_t     stop_sym_addr   = cfg->stop_sym_addr;
  const uint32_t     stop_sym_thresh = cfg->stop_sym_thresh;
  const char* const* dump_sym_names  = cfg->dump_sym_names;
  const uint32_t*    dump_sym_addrs  = cfg->dump_sym_addrs;
  const uint32_t     dump_sym_n      = cfg->dump_sym_n;
  const uint16_t     view_w          = cfg->view_w;
  const uint16_t     view_h          = cfg->view_h;
  const char* const  win_title       = cfg->win_title;

  /* The board view is the panel framebuffer (panel_w x panel_h, left) plus a
   * status sidebar (LEDs / USB / UART / IRQ / touch); the composite buffer is
   * what both the live window and the --ppm snapshot show. panel_fb holds the
   * GLCDC render before it is composited into the sidebar-widened composite. */
  const uint16_t panel_w = view_w;
  const uint16_t panel_h = view_h;
  /* --rotate turns the displayed panel for viewing (90/270 swap W and H, so a
   * landscape app can be shown portrait); the firmware still renders panel_w x
   * panel_h, which build_composite rotates into rot_fb. */
  const bool rot_swap =
    (rotate_deg == (uint32_t)k_rotate_90) || (rotate_deg == (uint32_t)k_rotate_270);
  const uint16_t disp_w    = rot_swap ? panel_h : panel_w;
  const uint16_t disp_h    = rot_swap ? panel_w : panel_h;
  const uint16_t comp_w    = board_overlay_total_width(disp_w);
  const uint16_t comp_h    = board_overlay_total_height(disp_h);
  board_view_t*  view      = nullptr;
  uint16_t*      panel_fb  = nullptr;
  uint16_t*      rot_fb    = nullptr;
  uint16_t*      composite = nullptr;
  if (want_view) {
    view = board_view_open(comp_w, comp_h, win_title);
    if (view == nullptr) {
      (void)fprintf(stderr, "board_sim: could not open window; continuing headless\n");
    }
  }
  if ((view != nullptr) || (ppm_path != nullptr) || want_click || (record_dir != nullptr)) {
    panel_fb  = (uint16_t*)malloc((size_t)panel_w * (size_t)panel_h * sizeof(uint16_t));
    composite = (uint16_t*)malloc((size_t)comp_w * (size_t)comp_h * sizeof(uint16_t));
    if (rotate_deg != (uint32_t)k_rotate_0) {
      rot_fb = (uint16_t*)malloc((size_t)disp_w * (size_t)disp_h * sizeof(uint16_t));
    }
  }

  if (want_click) {
    (void)fprintf(stderr, "board_sim: --click armed at (%d,%d)\n", click_x, click_y);
  }
  const run_guards_t  guards              = run_read_guards(cfg, view);
  const uint32_t      click_settle_chunks = guards.click_settle_chunks;
  uint32_t            max_chunks          = guards.max_chunks;
  const double        wall_s              = guards.wall_s;
  const bool          wall_guard_on       = guards.wall_guard_on;
  const uint32_t      idle_stop_chunks    = guards.idle_stop_chunks;
  const uint32_t      usb_stop_settle     = guards.usb_stop_settle;
  const uint32_t      usbh_stop_settle    = guards.usbh_stop_settle;
  const char* const   stop_on             = guards.stop_on;
  const uint32_t      prof_idle_insns     = guards.prof_idle_insns;
  const uint32_t      prof_idle_need      = guards.prof_idle_need;
  const uint32_t      prof_idle_arm       = guards.prof_idle_arm;
  uint64_t            prof_idle_prev_i    = 0U;
  uint32_t            prof_idle_run       = 0U;
  bool                prof_stopped        = false;
  uint64_t            idle_sig_prev       = 0U;
  uint32_t            idle_run            = 0U;
  bool                idle_stopped        = false;
  uint32_t            usb_stop_run        = 0U;
  uint32_t            usbh_stop_run       = 0U;
  bool                usb_stopped         = false;
  bool                stop_sym_hit        = false; /* --stop-sym threshold reached.           */
  uint32_t            rec_frames          = 0U;    /* frames written when --record is active. */
  const clock_t       t0                  = clock();
  uc_err              err                 = UC_ERR_OK;
  uint32_t            run_pc              = pc;
  uint32_t            chunks              = 0U;
  bool                timed_out           = false;
  bool                closed              = false;
  uint32_t            settle_left         = 0U; /* >0 once the click landed: chunks to drain. */
  uint32_t            last_boot_chunk     = 0U; /* chunk of the last (re)boot for --reboot.   */
  bool                slider_grab = false;      /* true while a press grabbed the battery slider. */
  board_overlay_btn_t held_btn    = k_board_overlay_btn_none; /* SW held down (released on up). */
  uint64_t            last_present_us = 0U; /* wall-us of the last live --view present. */
  /* Classify a headless --click once. A click on the console tab bar switches the
   * active console channel (a one-shot view change, same as the window path); an
   * on-screen sidebar button toggles a user switch (fired once); anything else is
   * a panel touch (re-armed until drained). */
  bool     click_was_tab = false;
  uint32_t click_tab_idx = 0U;
  if (want_click) {
    if (board_overlay_hit_console_tab((uint16_t)click_x,
                                      (uint16_t)click_y,
                                      disp_w,
                                      (uint32_t)k_board_console_ch_count,
                                      &click_tab_idx)) {
      sim_view_select_console_tab(click_tab_idx);
      click_was_tab = true;
    }
  }
  const board_overlay_btn_t click_btn =
    (want_click && !click_was_tab)
      ? board_overlay_hit_button((uint16_t)click_x, (uint16_t)click_y, disp_w)
      : k_board_overlay_btn_none;
  bool     button_fired = false;
  uint32_t prof_prev_pc = 0U;
  double   prof_prev_t  = 0.0;
  for (; chunks < max_chunks; chunks++) {
    /* BOARD_SIM_PROFILE: charge the wall time of the previous chunk to the
     * function its execution started in (so a cheap WFI-halt chunk and an
     * expensive compute chunk are weighted by real time, not by chunk count). */
    if (sim_prof_mode() == k_prof_wall) {
      const double now = board_now_s();
      if (prof_prev_t > 0.0) {
        prof_add(prof_prev_pc, now - prof_prev_t);
      }
      prof_prev_pc = run_pc;
      prof_prev_t  = now;
    }
    /* Publish run telemetry for the board view (PC + chunk counter). */
    sim_view_publish(run_pc, chunks);

    /* --reboot N: after each boot runs a settle window (enough to exercise its
     * first-boot path -- e.g. plant a backup-RAM sentinel), force a power-on warm
     * reboot. The reset-retained models (VBATT backup) survive, so an app proves
     * reset-survival across the reboot. This is a clean power-on (PORF stays). */
    if ((reboot_count > 0) && ((chunks - last_boot_chunk) >= (uint32_t)k_reboot_settle)) {
      reboot_count--;
      board_periph_reset_set_cause(true, false, false, false); /* power-on reboot */
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      last_boot_chunk = chunks;
      continue;
    }
    /* A peripheral (the watchdog) may have requested a reset on its tick: latch
     * the watchdog cause and warm-reboot so the next boot reads reset_by=wdt. */
    bool wdt_rst  = false;
    bool iwdt_rst = false;
    if (board_periph_reset_take_request(&wdt_rst, &iwdt_rst)) {
      board_periph_reset_set_cause(false, false, wdt_rst, iwdt_rst);
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      last_boot_chunk = chunks;
      continue;
    }

    /* Each outer chunk is one SysTick period: arm the periodic tick so the
     * boundary (or a tail-chain) takes it once interrupts permit. */
    sim_exc_arm_systick();

    /* Advance the free-running DWT cycle counter one chunk's worth of cycles so
     * a masked-context ra8_delay_ms (which spins on CYCCNT while PRIMASK is set)
     * makes progress instead of stalling the whole run. Inert unless the
     * firmware enabled the counter, so this cannot change any other app. */
    dwt_cyccnt_advance(uc);

    /* Advance the modelled timers one tick-period and let the ICU pend any IRQ
     * a counter wrap raised; the exception layer below takes it like SysTick. */
    board_periph_tick(uc);
    board_net_tick();

    /* Drain any buffered keystrokes into the console UART RX (the same SCI
     * channel --input targets). Sources are the live --view window (keyDown)
     * and the headless --keys injector, both via board_input -- so the keyboard
     * path is identical whether typed in the window or scripted on the CLI. */
    char key_byte = 0;
    while (board_input_pop_key(&key_byte)) {
      const uint8_t kb = (uint8_t)key_byte;
      board_periph_sci_feed_rx(board_periph_sci_console_channel(), &kb, 1U);
    }

    /* Headless --click: keep one contact armed in the GT911 model until the
     * firmware's real ra8_touch_read drains it (board_periph_touch_reported
     * increments). Re-arming each chunk is needed because ra8_touch_open clears
     * the GT911 status byte during bring-up; once the render loop reads the
     * point it is consumed once and never re-armed. */
    if (want_click && (click_btn != k_board_overlay_btn_none)) {
      if (!button_fired) {
        if ((click_btn == k_board_overlay_btn_battery) ||
            (click_btn == k_board_overlay_btn_batt_chg) ||
            (click_btn == k_board_overlay_btn_lowpower)) {
          apply_battery_click(click_btn, (uint16_t)click_x, disp_w); /* SOC / CHG / low-power. */
        } else {
          set_switch(click_btn, true); /* headless --click SW1/SW2: press + hold. */
        }
        button_fired = true;
      }
    } else if (want_click && !click_was_tab && (board_periph_touch_reported() == 0U)) {
      uint16_t cnx = (uint16_t)click_x;
      uint16_t cny = (uint16_t)click_y;
      unrotate_click((uint16_t)click_x,
                     (uint16_t)click_y,
                     panel_w,
                     panel_h,
                     rotate_deg,
                     &cnx,
                     &cny);
      board_periph_touch_inject(cnx, cny);
    }

    /* Inner loop (run_inner): run a chunk, then service exceptions to a steady
     * state before the next chunk. */
    const bool faulted = run_inner(uc, vtor_base, &run_pc, &err);
    if (faulted) {
      break;
    }
    if (sim_prof_stop_hit()) {
      prof_stopped = true; /* BOARD_SIM_STOP_PC reached -- end the profiled run. */
      break;
    }
    /* AIRCR.SYSRESETREQ requested a reset: latch a software-reset cause and warm
     * reboot the firmware from its reset vector, then keep running. */
    if (sim_exc_reboot_requested()) {
      board_periph_reset_set_cause(false, true, false, false); /* software reset */
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      /* Cleared after the reboot (the engine is idle during warm_reboot, so no
       * AIRCR write can be lost here, but clearing last keeps it robust if
       * warm_reboot ever runs code). */
      sim_exc_clear_reboot_request();
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      continue;
    }

    /* Dual-core: boot cpu1 on the CPU1ACTCSR release (SP/PC from its vector
     * table at CPU1INITVTOR), then step it interleaved with cpu0. cpu1 shares
     * the on-chip SRAM, so its poll of the ping struct sees cpu0's writes and
     * its pong reply is visible back to cpu0 -- a real second core, not a model.
     * cpu1 is a tight poll loop (no interrupts), so a plain stepped run suffices;
     * a cpu1 fault just stops cpu1 (cpu0 keeps running). */
    sim_cpu1_step();
    /* --record: dump the composite (panel + status) every k_record_every chunks
     * as a numbered PPM, so the run becomes a frame sequence (assemble to a video
     * with ffmpeg, e.g. `ffmpeg -framerate 20 -i frame_%06d.ppm out.mp4`). */
    if ((record_dir != nullptr) && (composite != nullptr) &&
        ((chunks % (uint32_t)k_record_every) == 0U)) {
      build_composite(uc,
                      panel_fb,
                      rot_fb,
                      composite,
                      panel_w,
                      panel_h,
                      disp_w,
                      disp_h,
                      rotate_deg,
                      win_title);
      char fpath[1024];
      (void)snprintf(fpath, sizeof(fpath), "%s/frame_%06u.ppm", record_dir, (unsigned)rec_frames);
      if (write_ppm(fpath, composite, comp_w, comp_h) == 0) {
        rec_frames++;
      }
    }
    if (view != nullptr) {
      /* Live window: mouse-down on an on-screen SW1/SW2 PRESSES that user switch
       * (route_click drives it active-low); the matching mouse-up releases it, so
       * the on-screen buttons act as momentary push-buttons, not latching switches.
       * A press anywhere on the panel arms one GT911 contact instead. */
      uint16_t cx = 0U;
      uint16_t cy = 0U;
      if (board_view_poll_click(view, &cx, &cy)) {
        /* A press on the battery slider "grabs" it, so a subsequent drag keeps
         * setting the SOC from the cursor column even if the mouse leaves the
         * track row -- standard slider grab semantics. */
        const board_overlay_btn_t hit = route_click(cx, cy, panel_w, panel_h, disp_w, rotate_deg);
        slider_grab                   = (hit == k_board_overlay_btn_battery);
        held_btn = ((hit == k_board_overlay_btn_sw1) || (hit == k_board_overlay_btn_sw2))
                     ? hit
                     : k_board_overlay_btn_none;
      }
      /* Mouse-up: release a held push-button and drop any slider grab. */
      if (board_view_poll_release(view)) {
        if (held_btn != k_board_overlay_btn_none) {
          set_switch(held_btn, false);
          held_btn = k_board_overlay_btn_none;
        }
        slider_grab = false;
      }
      uint16_t dx = 0U;
      uint16_t dy = 0U;
      if (board_view_poll_drag(view, &dx, &dy) && slider_grab) {
        apply_battery_click(k_board_overlay_btn_battery, dx, disp_w);
      }
      /* Mouse-wheel pages the console scrollback, Arduino-Serial-Monitor style:
       * scrolling up reveals older lines AND pauses autoscroll (so new output no
       * longer yanks the view to the bottom); scrolling back down to the tail
       * re-enables autoscroll. fill_status clamps the offset + holds the absolute
       * position while paused. */
      sim_view_wheel(board_view_poll_scroll(view));
      if ((chunks % (uint32_t)k_view_present_every) == 0U) {
        /* Compositing + uploading the full panel+sidebar frame is the dominant
         * host cost. The firmware advances far faster than a display needs, so
         * cap the live present to ~60 Hz wall-clock and yield the CPU when a
         * present is skipped -- otherwise an idle app (cheap chunks) would spin a
         * host core redrawing identical frames thousands of times a second, which
         * reads as window lag for "nothing happening". */
        struct timespec ts_now = {};
        (void)clock_gettime(CLOCK_MONOTONIC, &ts_now);
        const uint64_t now_us = ((uint64_t)ts_now.tv_sec * (uint64_t)k_us_per_s) +
                                ((uint64_t)ts_now.tv_nsec / (uint64_t)k_ns_per_us);
        if ((now_us - last_present_us) >= (uint64_t)k_view_frame_us) {
          build_composite(uc,
                          panel_fb,
                          rot_fb,
                          composite,
                          panel_w,
                          panel_h,
                          disp_w,
                          disp_h,
                          rotate_deg,
                          win_title);
          board_view_present(view, composite, comp_w, comp_h);
          last_present_us = now_us;
        } else {
          (void)usleep((useconds_t)k_view_yield_us);
        }
      }
      if (board_view_pump(view)) {
        closed = true;
        break;
      }
    } else if (want_click) {
      /* Headless --click: run a bounded tail after the input lands (a drained
       * touch, or a fired on-screen button), then stop deterministically so the
       * dumped frame shows the switched tab / lit LED. */
      const bool click_acted = (click_btn != k_board_overlay_btn_none)
                                 ? button_fired
                                 : (board_periph_touch_reported() > 0U);
      if (click_acted) {
        settle_left = (settle_left == 0U) ? click_settle_chunks : (settle_left - 1U);
        if (settle_left == 1U) {
          break;
        }
      }
      if (wall_guard_on && (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= wall_s)) {
        timed_out = true;
        break;
      }
    } else { /* plain headless run (no window, no scripted click) */
      /* Profiler compute-idle early-stop (insn mode): a chunk that retires very
       * few instructions is an idle frame (poll + WFI-halt), whereas boot chunks
       * are compute-heavy. After enough consecutive idle chunks the firmware has
       * reached its steady frame loop -- end the run so the profile is boot, not
       * the idle tail. Build-stable (no per-app address), unlike STOP_PC. */
      if ((sim_prof_mode() == k_prof_insn) && (record_dir == nullptr) &&
          (chunks >= prof_idle_arm)) {
        const uint64_t d = sim_prof_total_insns() - prof_idle_prev_i;
        prof_idle_prev_i = sim_prof_total_insns();
        if (d < (uint64_t)prof_idle_insns) {
          prof_idle_run++;
          if (prof_idle_run >= prof_idle_need) {
            prof_stopped = true;
            break;
          }
        } else {
          prof_idle_run = 0U;
        }
      }
      /* Steady-state idle early-stop (opt-in; not while recording, which must
       * span its full window). All tracked counters are monotonic, so an
       * unchanged sum means no MMIO / IRQ / context switch happened this chunk. */
      if ((idle_stop_chunks > 0U) && (record_dir == nullptr)) {
        const uint64_t idle_sig = (uint64_t)sim_mmio_reads() + (uint64_t)sim_mmio_writes() +
                                  (uint64_t)sim_exc_pendsv_takes() + (uint64_t)sim_exc_svc_takes() +
                                  (uint64_t)board_periph_irq_total();
        if (idle_sig == idle_sig_prev) {
          idle_run++;
          if (idle_run >= idle_stop_chunks) {
            idle_stopped = true;
            break;
          }
        } else {
          idle_sig_prev = idle_sig;
          idle_run      = 0U;
        }
      }
      /* USB device-mode early-stop: once the device reaches CONFIGURED, run a
       * short settle window (for the first class traffic + report line), stop. */
      if ((usb_stop_settle > 0U) && (record_dir == nullptr) && board_usb_configured()) {
        usb_stop_run++;
        if (usb_stop_run >= usb_stop_settle) {
          usb_stopped = true;
          break;
        }
      }
      /* USB host-mode early-stop: once the virtual device has served the host
       * its last request -- the keyboard streamed its reports, or the MSC host
       * reached its read-only WRITE(10) test (the step right before PASS) --
       * settle (for the PASS banner) and stop. Separate from USB_STOP because a
       * host app may ALSO run a device worker whose CONFIGURED would otherwise
       * stop the run before the host side finishes. */
      const bool usbh_done = sim_usbh_done();
      if ((usbh_stop_settle > 0U) && (record_dir == nullptr) && usbh_done) {
        usbh_stop_run++;
        if (usbh_stop_run >= usbh_stop_settle) {
          usb_stopped = true;
          break;
        }
      }
      /* Generic banner stop: the firmware printed its success line to the
       * console. Check ALL THREE text endpoints -- the UART (the SCI-TX `[uart]`
       * banners), the ITM/SWO stimulus stream (the `[itm]` ra8_log lines
       * surfaced by on_itm_stim_write), and the SEGGER RTT up-buffer drain (the
       * `[rtt]` lines board_periph_rtt.c pulls out of the in-RAM control
       * block). ra8_log-only apps -- e.g. the dual-core demos, whose PASS
       * verdict is an `ra8_log_info` line that never touches the UART -- and
       * RTT-only apps (rtt_log_demo) would otherwise run to the full chunk
       * budget; matching the ITM / RTT lines lets BOARD_SIM_STOP_ON end the run
       * the instant the verdict is emitted, exactly as it already does for the
       * UART-banner apps. */
      if (stop_on != nullptr) {
        const char* last_uart = board_periph_uart_last_line();
        if ((last_uart != nullptr) && (strstr(last_uart, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
        const char* last_itm = board_console_line(k_board_console_ch_itm, 0U);
        if ((last_itm != nullptr) && (strstr(last_itm, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
        const char* last_rtt = board_console_line(k_board_console_ch_rtt, 0U);
        if ((last_rtt != nullptr) && (strstr(last_rtt, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
      }
      /* --stop-sym: end the run the instant the watched 32-bit global reaches its
       * threshold (the jlink_memprobe counter floor). Checked at the chunk
       * boundary like STOP_ON, so the stop is deterministic and host-load
       * independent -- a passing probe stops in a handful of chunks. */
      if (stop_sym_addr != 0U) {
        uint32_t sv = 0U;
        if ((uc_mem_read(uc, (uint64_t)stop_sym_addr, &sv, sizeof(sv)) == UC_ERR_OK) &&
            (sv >= stop_sym_thresh)) {
          stop_sym_hit = true;
          break;
        }
      }
      if (wall_guard_on && (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= wall_s)) {
        timed_out = true;
        break;
      }
    }
  }

  /* The run has ended: the board view's final / held frame shows "parked". */
  sim_view_mark_stopped(run_pc);

  (void)fprintf(stderr,
                "\nboard_sim: stopped -- %s%s%s%s%s%s\n",
                uc_strerror(err),
                timed_out ? " (wall-clock budget reached)" : "",
                idle_stopped ? " (idle steady-state)" : "",
                usb_stopped ? " (USB enumerated)" : "",
                stop_sym_hit ? " (--stop-sym threshold reached)" : "",
                prof_stopped ? " (profile: boot complete)" : "");
  (void)fprintf(stderr, "  final PC      : 0x%08X\n", run_pc);
  if (sim_exc_bkpt_hit()) {
    (void)fprintf(stderr,
                  "  => firmware executed a BKPT @ 0x%08X (deliberate trap: "
                  "Default_Handler / failed assert / fault give-up)\n",
                  sim_exc_bkpt_pc());
  }
  (void)
    fprintf(stderr, "  chunks run    : %u   SysTick ticks: %u\n", chunks, sim_exc_systick_fires());
  if (sim_mve_emulated_count() > 0U) {
    (void)fprintf(stderr,
                  "  MVE (Helium)  : %llu instruction(s) emulated (M85 vector ops the M33 "
                  "core lacks)\n",
                  (unsigned long long)sim_mve_emulated_count());
  }
  if (sim_lob_emulated_count() > 0U) {
    (void)fprintf(stderr,
                  "  LOB (loop)    : %llu DLS/LE instruction(s) emulated (M85 hardware-loop "
                  "ops)\n",
                  (unsigned long long)sim_lob_emulated_count());
  }
  prof_report();
  (void)fprintf(stderr,
                "  exceptions    : %u PendSV  %u SVCall (real Cortex-M entry/return)\n",
                sim_exc_pendsv_takes(),
                sim_exc_svc_takes());
  (void)fprintf(stderr,
                "  touch clicks  : %u drained via ra8_touch -> I3C -> GT911\n",
                board_periph_touch_reported());
  /* Emit any console bytes still buffered without a trailing newline. */
  console_flush_line(board_periph_sci_console_channel());
  /* Peripheral-model observability: LED transitions, timer totals, IRQ counts,
   * SCI byte totals. */
  board_periph_report(uc);
  board_net_report();
  sim_mmio_print_counts();
  /* Device summary: surface the attached microSD card so a headless run shows
   * its size / format the same way the --view sidebar does. */
  {
    bool        sd_att = false;
    uint64_t    sd_b   = 0U;
    uint8_t     sd_f   = 0U;
    const char* sd_l   = nullptr;
    board_sd_info(&sd_att, &sd_b, &sd_f, &sd_l);
    const uint64_t gib_bytes = (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
    const uint64_t mib_bytes = (uint64_t)k_size_kib * (uint64_t)k_size_kib;
    const bool     sd_gb     = (sd_b >= gib_bytes);
    const unsigned long sd_sz =
      sd_gb ? (unsigned long)(sd_b / gib_bytes) : (unsigned long)(sd_b / mib_bytes);
    const char* sd_u = sd_gb ? "GB" : "MB";
    if (sd_att && (sd_f != 0U)) {
      (void)fprintf(stderr,
                    "  SD card       : %lu %s FAT%u '%s' (created by --sd-new)\n",
                    sd_sz,
                    sd_u,
                    (unsigned)sd_f,
                    (sd_l != nullptr) ? sd_l : "");
    } else if (sd_att) {
      (void)fprintf(stderr, "  SD card       : %lu %s image attached\n", sd_sz, sd_u);
    }
  }
  sim_mmio_print_bgc_and_table();
  if (timed_out && !idle_stopped && !usb_stopped && !stop_sym_hit && !prof_stopped &&
      !sim_exc_bkpt_hit()) {
    /* The wall-clock guard fired (clock() is CPU-time, so a heavily-loaded host
     * burns the budget faster). This is a TRUNCATED run, NOT a completed one --
     * say so plainly, and do NOT print the "EXECUTED to the run budget" line a
     * full-budget run prints. Conflating the two let a load-correlated truncation
     * masquerade as success, dropping a deterministic banner without failing the
     * gate (#168). A caller that wants the run bounded by a deterministic event
     * (not by CPU-time) should pass BOARD_SIM_STOP_ON / BOARD_SIM_MAX_CHUNKS. */
    (void)fprintf(stderr,
                  "  => board_sim TRUNCATED by the wall-clock guard at chunk %u of %u "
                  "(%.0fs CPU-time elapsed); this is NOT a full-budget run (host "
                  "overloaded -- see #168).\n",
                  chunks,
                  max_chunks,
                  wall_s);
  } else if (((err == UC_ERR_OK) || idle_stopped || usb_stopped || stop_sym_hit || prof_stopped) &&
             !sim_exc_bkpt_hit()) {
    if (idle_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (idle steady-state: no "
                    "observable change for %u chunks, stopped at chunk %u).\n",
                    idle_stop_chunks,
                    chunks);
    } else if (usb_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (USB enumerated: device "
                    "CONFIGURED, stopped at chunk %u).\n",
                    chunks);
    } else {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (no invalid opcode / fault).\n");
    }
  }

  /* --dump-sym: read each resolved global from Unicorn memory and print its
   * 32-bit value (and the address), so a test can probe firmware state (e.g. an
   * init-step or mismatch counter) after the run without a debugger. */
  for (uint32_t d = 0U; d < dump_sym_n; d++) {
    if (dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr, "  dump-sym      : %s = <unresolved>\n", dump_sym_names[d]);
      continue;
    }
    uint32_t v = 0U;
    if (uc_mem_read(uc, (uint64_t)dump_sym_addrs[d], &v, sizeof(v)) == UC_ERR_OK) {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = %u (0x%08X)\n",
                    dump_sym_names[d],
                    dump_sym_addrs[d],
                    v,
                    v);
    } else {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = <unreadable>\n",
                    dump_sym_names[d],
                    dump_sym_addrs[d]);
    }
  }

  if ((ppm_path != nullptr) && (composite != nullptr)) {
    build_composite(uc,
                    panel_fb,
                    rot_fb,
                    composite,
                    panel_w,
                    panel_h,
                    disp_w,
                    disp_h,
                    rotate_deg,
                    win_title);
    if (write_ppm(ppm_path, composite, comp_w, comp_h) == 0) {
      (void)fprintf(stderr, "  wrote %s (%ux%u)\n", ppm_path, (unsigned)comp_w, (unsigned)comp_h);
    } else {
      (void)fprintf(stderr, "  could not write %s\n", ppm_path);
    }
  }
  if (record_dir != nullptr) {
    (void)fprintf(stderr,
                  "  recorded %u frame(s) to %s (%ux%u, ~%u fps)\n",
                  rec_frames,
                  record_dir,
                  (unsigned)comp_w,
                  (unsigned)comp_h,
                  (unsigned)k_record_fps);
  }
  if (view != nullptr) {
    if (!closed) { /* run ended on its own -- keep the last frame up until closed */
      build_composite(uc,
                      panel_fb,
                      rot_fb,
                      composite,
                      panel_w,
                      panel_h,
                      disp_w,
                      disp_h,
                      rotate_deg,
                      win_title);
      board_view_present(view, composite, comp_w, comp_h);
      (void)fprintf(stderr, "board_sim: run ended; close the window to exit\n");
      while (!board_view_pump(view)) {
        (void)usleep((useconds_t)k_view_idle_us);
      }
    }
    board_view_close(view);
  }
  /* --save-sd: dump the (possibly firmware-modified) SD card image for reuse. */
  if (save_sd_path != nullptr) {
    (void)board_sd_save(save_sd_path);
  }
  free(panel_fb);
  free(rot_fb);
  free(composite);
  free(elf); /* kept alive for the whole run so a warm reboot can re-load it */
  (void)uc_close(uc);

  /* Exit status for the #67 run-every-example matrix: a clean run-to-budget is
   * 0; a firmware BKPT, an emulation fault, or the wall-clock timeout each map
   * to a distinct non-zero code so the matrix can flag a wedged or trapped run
   * by exit code alone. */
  board_sim_exit_t exit_code = k_board_sim_exit_ok;
  if (sim_exc_bkpt_hit()) {
    exit_code = k_board_sim_exit_bkpt;
  } else if (err != UC_ERR_OK) {
    exit_code = k_board_sim_exit_fault;
  } else if (timed_out) {
    exit_code = k_board_sim_exit_timeout;
  }
  return (int)exit_code;
}
