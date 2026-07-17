/**
 * @file sim_run_guards.c
 * @brief Run-guard environment knob reader (see sim_run.h)
 *
 * @details
 * The BOARD_SIM_* budget / stop-condition environment parsing that precedes
 * the chunked run loop -- moved verbatim out of the run TU so each of the
 * two stays under the file-size bar. The contract lives on the declaration
 * of run_read_guards() in sim_run.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "sim_prof.h"
#include "sim_run.h"
#include "sim_view.h"

run_guards_t run_read_guards(const sim_run_cfg_t* cfg, const board_view_t* view)
{
  const char* const record_dir  = cfg->record_dir;
  const uint32_t    record_secs = cfg->record_secs;
  enum : uint32_t {
    /* ra8_delay_ms(16) in the render loop spins on SysTick, which advances one
     * tick per chunk, so ~16 chunks == one loop iteration. Give the injected
     * tap many iterations to flow DOWN -> UP -> CLICKED -> tab switch -> repaint. */
    k_click_settle_chunks = 512U, /**< Extra chunks after the click lands. */
  };
  /* BOARD_SIM_CLICK_SETTLE=N: widen the post-click drain for a tap that kicks off
   * a long operation (e.g. opening a big book from SD: read + inflate + decode +
   * render can need far more than the default window). Mirrors BOARD_SIM_MAX_CHUNKS;
   * unset keeps the default so ordinary tap captures stay snappy. */
  uint32_t click_settle_chunks = (uint32_t)k_click_settle_chunks;
  {
    const char* e_settle = getenv("BOARD_SIM_CLICK_SETTLE");
    if (e_settle != nullptr) {
      const long v = strtol(e_settle, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        click_settle_chunks = (uint32_t)v;
      }
    }
  }

  /* Chunked run: emulate a block, take a SysTick (and any pending PendSV),
   * repeat. Within a chunk, exception returns (a handler's "BX lr" into an
   * EXC_RETURN magic) and SVCalls are resolved inline -- the inner loop unstacks
   * / vectors and relaunches from the new PC without consuming a chunk -- so a
   * context switch does not cost a whole scheduling quantum and the once-per-
   * chunk SysTick cadence (one ThreadX tick) is preserved. Headless runs stop on
   * a chunk budget + wall-clock guard; in --view the loop runs until the window
   * is closed, presenting the live GLCDC output every k_view_present_every. */
  uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
  /* Headless runs of heavy apps (e.g. sd_font_render: a 400 KB SD font read
   * plus stb_truetype rasterisation) can need more than the default budget.
   * BOARD_SIM_WALL_S / BOARD_SIM_MAX_CHUNKS override the guards without a
   * recompile; they have no effect in --view (window-driven) mode.
   *
   * The wall-clock guard is CPU-time (clock(), see the run loop), so a
   * heavily-loaded host burns it faster than wall time -- which made it
   * truncate the otherwise instruction-deterministic chunk budget under CI load
   * and silently drop a late-printing banner (#168). A run that must stay
   * deterministic regardless of host load sets BOARD_SIM_WALL_S=0 to DISABLE the
   * guard entirely and rely on the deterministic BOARD_SIM_MAX_CHUNKS bound (and
   * BOARD_SIM_STOP_ON) instead. (Previously WALL_S=0 silently fell back to the
   * default -- the footgun that hid the #168 root cause.) */
  double wall_s        = (double)k_run_wall_s;
  bool   wall_guard_on = true;
  {
    const char* e_wall = getenv("BOARD_SIM_WALL_S");
    if (e_wall != nullptr) {
      const long v = strtol(e_wall, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        wall_s = (double)v;
      } else if (v == 0L) {
        wall_guard_on = false; /* explicit opt-out: bound the run by chunks only */
      }
    }
    const char* e_chunks = getenv("BOARD_SIM_MAX_CHUNKS");
    if ((e_chunks != nullptr) && (view == nullptr)) {
      const long v = strtol(e_chunks, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        max_chunks = (uint32_t)v;
      }
    }
  }
  /* Headless --record-secs bounds the run to exactly the recording window so the
   * dumped frame sequence spans the requested emulated duration. */
  if ((record_dir != nullptr) && (record_secs > 0U) && (view == nullptr)) {
    max_chunks = record_secs * (uint32_t)k_record_ms_per_sec;
  }
  if (record_dir != nullptr) {
    (void)mkdir(record_dir, (mode_t)k_record_dir_mode);
    (void)fprintf(stderr,
                  "board_sim: recording to %s/frame_NNNNNN.ppm (every %u chunks, ~%u fps)\n",
                  record_dir,
                  (unsigned)k_record_every,
                  (unsigned)k_record_fps);
  }
  /* BOARD_SIM_IDLE_STOP=N: in a plain headless run, stop early once observable
   * state has not changed for N consecutive chunks -- the firmware has reached
   * steady-state idle (an RTOS idle spin, a `while(1) wfi`), so there is nothing
   * left to run. Off (0) by default, so normal runs are unaffected; opt in for
   * RTOS/idle apps that would otherwise burn the whole wall-clock budget. The
   * tracked counters (peripheral MMIO reads/writes, PendSV/SVCall, peripheral
   * IRQs) are all monotonic, so an unchanged sum means none of them advanced. */
  uint32_t idle_stop_chunks = 0U;
  {
    const char* e_idle = getenv("BOARD_SIM_IDLE_STOP");
    if ((e_idle != nullptr) && (view == nullptr)) {
      const long v = strtol(e_idle, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        idle_stop_chunks = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_USB_STOP=N: stop a plain headless run N chunks after the virtual USB
   * host reports the device CONFIGURED. The USB device apps never go idle -- HID
   * jiggles its boot mouse forever, MSC keeps answering host polls -- so the
   * idle-stop above never fires for them. This makes the smoke gate fast and
   * deterministic: it runs exactly long enough to confirm enumeration + the first
   * class traffic, then stops, instead of burning the whole chunk/wall budget. */
  uint32_t usb_stop_settle = 0U;
  {
    const char* e_usb = getenv("BOARD_SIM_USB_STOP");
    if ((e_usb != nullptr) && (view == nullptr)) {
      const long v = strtol(e_usb, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        usb_stop_settle = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_USBH_STOP=N: as above but for a USB HOST-mode app -- stop N chunks
   * after the virtual host-mode keyboard has streamed its reports. Kept distinct
   * from USB_STOP so a host app that also runs a device worker is not stopped by
   * that worker reaching CONFIGURED before the host side completes. */
  uint32_t usbh_stop_settle = 0U;
  {
    const char* e_usbh = getenv("BOARD_SIM_USBH_STOP");
    if ((e_usbh != nullptr) && (view == nullptr)) {
      const long v = strtol(e_usbh, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        usbh_stop_settle = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_STOP_ON="<substr>": stop the headless run as soon as the console
   * UART's last line contains <substr> -- a generic "stop on a banner" guard for
   * apps that loop forever after a success line (e.g. usb_host_file_ops retries
   * its ladder every 5 s). Empty / unset disables it. */
  const char* stop_on = getenv("BOARD_SIM_STOP_ON");
  if ((stop_on != nullptr) && ((stop_on[0] == '\0') || (view != nullptr))) {
    stop_on = nullptr;
  }
  /* BOARD_SIM_STOP_PC=0x...: end the run the first time PC reaches this address
   * (effective in profile insn mode, via prof_insn_hook). Lets the profiler cover
   * exactly the boot path -- the idle main-loop PC the firmware parks at -- when
   * the build-stable compute-idle auto-stop below is not specific enough. */
  {
    const char* e_spc = getenv("BOARD_SIM_STOP_PC");
    if (e_spc != nullptr) {
      const unsigned long v = strtoul(e_spc, nullptr, (int)k_env_strtol_base);
      sim_prof_set_stop_pc((uint32_t)(v & ~1UL)); /* clear the Thumb bit if supplied. */
    }
  }
  /* BOARD_SIM_PROFILE compute-idle stop (insn mode, build-stable): once boot has
   * fallen into the steady frame loop the firmware retires almost no instructions
   * per chunk (each frame polls a little, then WFI-halts), whereas boot chunks are
   * compute-heavy. Stop after k_prof_idle_need consecutive chunks that each retire
   * fewer than k_prof_idle_insns instructions, so the profile spans boot + the
   * first rendered frame and not the idle tail. Armed only after k_prof_idle_arm
   * chunks so an early cheap chunk cannot trip it. */
  enum : uint32_t {
    k_prof_idle_insns = 4000U, /**< Per-chunk insns below which a chunk is idle. */
    k_prof_idle_need  = 600U,  /**< Consecutive idle chunks that end the run.    */
    k_prof_idle_arm   = 16U,   /**< Chunks to run before the stop is armed.      */
  };
  /* All three are overridable so the boot window can be tuned per app (a long
   * boot-time settle delay is a run of cheap chunks that must not be mistaken
   * for the steady idle loop). NEED defaults high enough to clear the settle
   * delays in these apps; raise it (or set BOARD_SIM_STOP_PC) for a longer boot. */
  uint32_t prof_idle_insns = (uint32_t)k_prof_idle_insns;
  uint32_t prof_idle_need  = (uint32_t)k_prof_idle_need;
  uint32_t prof_idle_arm   = (uint32_t)k_prof_idle_arm;
  {
    const char* e_pi = getenv("BOARD_SIM_PROFILE_IDLE_INSNS");
    const char* e_pn = getenv("BOARD_SIM_PROFILE_IDLE_NEED");
    const char* e_pa = getenv("BOARD_SIM_PROFILE_IDLE_ARM");
    if (e_pi != nullptr) {
      prof_idle_insns = (uint32_t)strtoul(e_pi, nullptr, (int)k_strtol_base10);
    }
    if (e_pn != nullptr) {
      prof_idle_need = (uint32_t)strtoul(e_pn, nullptr, (int)k_strtol_base10);
    }
    if (e_pa != nullptr) {
      prof_idle_arm = (uint32_t)strtoul(e_pa, nullptr, (int)k_strtol_base10);
    }
  }
  return (run_guards_t){
    .click_settle_chunks = click_settle_chunks,
    .max_chunks          = max_chunks,
    .wall_s              = wall_s,
    .wall_guard_on       = wall_guard_on,
    .idle_stop_chunks    = idle_stop_chunks,
    .usb_stop_settle     = usb_stop_settle,
    .usbh_stop_settle    = usbh_stop_settle,
    .stop_on             = stop_on,
    .prof_idle_insns     = prof_idle_insns,
    .prof_idle_need      = prof_idle_need,
    .prof_idle_arm       = prof_idle_arm,
  };
}
