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
 * @enum inner_action_t
 * @brief Loop-control verdict a run_inner boundary helper hands back.
 *
 * @details The inner exception-resolve loop either re-enters (a zero-time
 * relaunch, tail-chain or fault-synthesis) or ends (fault / BKPT / stop /
 * quiescent budget). The helpers return this so run_inner stays a thin driver.
 *
 * @invariant Exactly one value is returned per helper call.
 * @see run_inner_check_stops()  First consumer.
 * @see run_inner_take_exception()  Second consumer.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_inner_continue = 0, /**< Re-enter the inner loop (relaunch / tail-chain). */
  k_inner_break    = 1, /**< End the inner loop (fault / BKPT / quiescent).   */
} inner_action_t;

/**
 * @brief Compute one inner chunk's instruction budget for @p run_pc.
 *
 * @details Idle fast-forward: when the core is parked on a wait-for-interrupt
 * spin (ThreadX's __tx_wait_here `b .`, a `wfi` halt, or a `cpsie i` poll),
 * running a full k_run_chunk_insns just burns wall time to reach the SAME
 * SysTick already armed once per outer chunk, so the budget collapses to
 * k_idle_spin_insns -- the tick still fires once per chunk, so the ThreadX tick
 * COUNT (and every tick-measured deadline) is identical to a full idle chunk.
 * Low-power (Model A) shrinks the busy budget by k_low_power_div (the 4:1
 * M85:M33 clock ratio); idle spins still collapse regardless.
 *
 * @param[in,out] uc     Unicorn engine (read to classify the PC's opcode).
 * @param[in]     run_pc The resume PC to classify.
 * @return The instruction budget for the next uc_emu_start.
 * @retval k_idle_spin_insns The PC sits on a wait-for-interrupt spin.
 * @pre The engine is set up and @p run_pc is a valid resume PC.
 * @pre The seams / exception hooks are installed.
 * @post No engine state changes (the classification is a read-only probe).
 * @post The returned budget is one of the three modelled bounds.
 * @note Not thread-safe; part of the single-threaded run core.
 * @since 0.1.0
 */
static size_t run_inner_budget(uc_engine* uc, uint32_t run_pc)
{
  size_t busy_budget = (size_t)k_run_chunk_insns;
  if (sim_low_power()) {
    busy_budget = (size_t)k_run_chunk_insns / (size_t)k_low_power_div;
  }
  return idle_spin_at(uc, run_pc) ? (size_t)k_idle_spin_insns : busy_budget;
}

/**
 * @brief Resolve the zero-time relaunch and run-ending stop conditions.
 *
 * @details Checked right after a chunk returns, before any exception is taken:
 * a --fast-sd seam relaunch re-enters directly (no time charged); a profiler
 * STOP_PC hit, an AIRCR.SYSRESETREQ reboot request, or a firmware BKPT each
 * end the run (only the BKPT is a fault). When none apply the caller proceeds
 * to the exception boundary.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[out]    faulted Set true iff a BKPT ended the run.
 * @param[out]    act     The loop action when this helper handled the boundary.
 * @return true when @p act is authoritative, false to fall through.
 * @retval true  One of the relaunch / stop conditions applied.
 * @retval false No condition applied; take the exception boundary next.
 * @pre @p faulted and @p act are non-NULL.
 * @pre The chunk has returned and its final PC has been read back.
 * @post @p faulted is written only when true is returned via a BKPT.
 * @post On true, @p act is ::k_inner_continue or ::k_inner_break.
 * @note Not thread-safe; part of the single-threaded run core.
 * @since 0.1.0
 */
static bool run_inner_check_stops(uc_engine* uc, bool* faulted, inner_action_t* act)
{
  (void)uc;
  if (sim_seam_take_relaunch()) {
    /* A --fast-sd byte-exchange returned to its caller. This consumed no
     * modelled time, so relaunch from the returned PC without advancing the
     * SysTick or charging a chunk (the inner-loop cap still bounds the run). */
    *act = k_inner_continue;
    return true;
  }
  if (sim_prof_stop_hit()) {
    *act = k_inner_break; /* BOARD_SIM_STOP_PC reached (prof_insn_hook) -- end the run. */
    return true;
  }
  if (sim_exc_reboot_requested()) {
    *act = k_inner_break; /* AIRCR.SYSRESETREQ -- the outer wrapper performs the reboot. */
    return true;
  }
  if (sim_exc_bkpt_hit()) {
    *faulted = true; /* firmware trapped on a BKPT -- end the run, report it. */
    *act     = k_inner_break;
    return true;
  }
  return false;
}

/**
 * @brief Take the pending exception (or end) at the chunk boundary.
 *
 * @details Runs only when ::run_inner_check_stops fell through. An EXC_RETURN
 * branch is unstacked and the NVIC re-checked so a still-pending lower-priority
 * exception tail-chains (PendSV right after SysTick) as hardware would; an
 * emulation error ends the run as a fault; MPU / div-0 faults are synthesised
 * at the boundary; a PENDSVSET yield takes PendSV; otherwise the full budget
 * elapsed and the highest-priority pend (SysTick and/or PendSV) is taken. None
 * of the zero-time paths advance the SysTick; only a full budget does.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base VTOR fallback for exception vectoring.
 * @param[in,out] run_pc    Resume PC in, post-boundary PC out.
 * @param[in]     err       The chunk's final uc_emu_start status.
 * @param[out]    faulted   Set true iff @p err ended the run as a fault.
 * @return The loop action for run_inner.
 * @retval k_inner_continue A boundary was resolved; re-enter the loop.
 * @retval k_inner_break    An emulation fault or quiescence ended the run.
 * @pre @p run_pc and @p faulted are non-NULL.
 * @pre The chunk returned and ::run_inner_check_stops returned false.
 * @post @p run_pc reflects the post-boundary PC.
 * @post @p faulted is true only on a returned ::k_inner_break via @p err.
 * @note Not thread-safe; part of the single-threaded run core.
 * @since 0.1.0
 */
static inner_action_t run_inner_take_exception(uc_engine* uc,
                                               uint32_t   vtor_base,
                                               uint32_t*  run_pc,
                                               uc_err     err,
                                               bool*      faulted)
{
  uint64_t exc_ret_pc = 0U;
  if (sim_exc_take_exc_return(&exc_ret_pc)) {
    exc_return(uc, (uint32_t)exc_ret_pc);
    /* Tail-chain the next pend, but do NOT advance the SysTick: an exception
     * return consumes no modelled time, so time advances only on a full
     * instruction budget (below). This keeps a deferred tick from firing the
     * instant a PendSV context switch unstacks into the new thread. */
    (void)exc_take_pending(uc, vtor_base, false);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (err != UC_ERR_OK) {
    *faulted = true;
    return k_inner_break;
  }
  if (sim_mpu_fault_pending()) {
    /* A store hit a read-only MPU region (on_mpu_ro_write stopped us).
     * Synthesise MemManage at this boundary -- no time advances (a fault is
     * synchronous), and the stacked PC is the faulting store. */
    sim_mpu_clear_fault();
    mpu_synth_memmanage(uc, vtor_base);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (sim_div0_fault_pending()) {
    /* A UDIV/SDIV by zero with CCR.DIV_0_TRP set (emulate_div0_patched stopped
     * us). Synthesise UsageFault at this boundary -- synchronous, no time
     * advances, and the stacked PC is the trapping divide. */
    sim_div0_clear_fault();
    div0_synth_usagefault(uc, vtor_base);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (sim_exc_pendsv_stop()) {
    /* Context-switch stop: a thread wrote PENDSVSET to yield. Take PendSV but do
     * NOT advance the SysTick -- a context switch consumes no modelled time, so a
     * thread that just suspended on a tick wait keeps waiting and the scheduler
     * runs the highest-priority READY thread. This is what lets a low-priority
     * worker (e.g. the NetX echo thread) run to completion before a higher-
     * priority sleeper's tick expires. */
    (void)exc_take_pending(uc, vtor_base, false);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  /* Full instruction budget elapsed: a tick's worth of genuine execution (or an
   * idle spin) has passed, so advance time -- take the highest-priority pending
   * exception (SysTick this period, and/or PendSV) and resolve it in this same
   * chunk so a context switch does not cost a scheduling quantum. */
  if (exc_take_pending(uc, vtor_base, true)) {
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  return k_inner_break;
}

/**
 * @brief Run one outer chunk's inner exception-resolve loop.
 *
 * @details
 * Runs a chunk, then services exceptions to a steady state before the next
 * chunk via ::run_inner_check_stops (zero-time relaunches and run-ending stops)
 * and ::run_inner_take_exception (EXC_RETURN unstack, fault synthesis, tick /
 * PendSV take). Returns once the full budget elapsed with nothing pending, or
 * on a fault / BKPT / stop request.
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
    const size_t run_budget = run_inner_budget(uc, run_pc);
    err                     = uc_emu_start(uc, (uint64_t)run_pc | 1U, 0, 0, run_budget);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
    inner_action_t act = k_inner_continue;
    if (run_inner_check_stops(uc, &faulted, &act)) {
      if (act == k_inner_break) {
        break;
      }
      continue;
    }
    if (run_inner_take_exception(uc, vtor_base, &run_pc, err, &faulted) == k_inner_break) {
      break;
    }
  }
  *run_pc_io = run_pc;
  *err_out   = err;
  return faulted;
}

/**
 * @struct run_stop_t
 * @brief How the run ended: final engine status plus every stop flag.
 *
 * @details Snapshot of the run loop's stop bookkeeping, bundled so the
 * report printers receive one read-only view of the outcome instead of a
 * dozen loose parameters.
 *
 * @invariant At most one of the early-stop flags reflects the stop cause;
 *            the report prints every one that is set.
 * @see run_print_stop_summary()  First consumer.
 * @see run_print_verdict()  Second consumer.
 * @since 0.1.0
 */
typedef struct {
  uc_err   err;              /**< Final uc_emu_start status.                 */
  uint32_t run_pc;           /**< PC where the run stopped.                  */
  uint32_t chunks;           /**< Outer chunks executed.                     */
  uint32_t max_chunks;       /**< The chunk budget in force.                 */
  double   wall_s;           /**< The wall-clock bound in force (seconds).   */
  uint32_t idle_stop_chunks; /**< BOARD_SIM_IDLE_STOP window (0 = off).      */
  bool     timed_out;        /**< Wall-clock guard fired (truncated run).    */
  bool     idle_stopped;     /**< Idle steady-state stop fired.              */
  bool     usb_stopped;      /**< USB early stop (CONFIGURED / STOP_ON hit). */
  bool     stop_sym_hit;     /**< --stop-sym threshold reached.              */
  bool     prof_stopped;     /**< Profiler compute-idle stop fired.          */
} run_stop_t;

/**
 * @brief Print the attached microSD summary line of the run report.
 *
 * @details Surfaces the SD image's size / FAT format / label the same way
 * the --view sidebar does, so a headless run records the storage setup.
 * Prints nothing when no image is attached.
 *
 * @pre The SD model is in its post-run state.
 * @pre stderr is the report stream.
 * @post Zero or one summary line has been written to stderr.
 * @post The SD model is unchanged (read-only query).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_sd_summary(void)
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

/**
 * @brief Print the stop banner, telemetry counters and peripheral summaries.
 *
 * @details The first half of the run report: the "stopped" cause line, final
 * PC / BKPT note, chunk + tick + seam counters, the profiler report, the
 * exception / touch totals, the flushed console tail, the peripheral +
 * network + MMIO observability reports, the SD summary and the MMIO table.
 * Output text and order are exactly the pre-split report's.
 *
 * @param[in,out] uc The engine (read for the peripheral report).
 * @param[in]     st How the run ended.
 * @pre The run loop has ended and @p st reflects its bookkeeping.
 * @pre stderr is the report stream.
 * @post The summary section has been written to stderr.
 * @post The engine state is unchanged (read-only queries).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_stop_summary(uc_engine* uc, const run_stop_t* st)
{
  (void)fprintf(stderr,
                "\nboard_sim: stopped -- %s%s%s%s%s%s\n",
                uc_strerror(st->err),
                st->timed_out ? " (wall-clock budget reached)" : "",
                st->idle_stopped ? " (idle steady-state)" : "",
                st->usb_stopped ? " (USB enumerated)" : "",
                st->stop_sym_hit ? " (--stop-sym threshold reached)" : "",
                st->prof_stopped ? " (profile: boot complete)" : "");
  (void)fprintf(stderr, "  final PC      : 0x%08X\n", st->run_pc);
  if (sim_exc_bkpt_hit()) {
    (void)fprintf(stderr,
                  "  => firmware executed a BKPT @ 0x%08X (deliberate trap: "
                  "Default_Handler / failed assert / fault give-up)\n",
                  sim_exc_bkpt_pc());
  }
  (void)fprintf(stderr,
                "  chunks run    : %u   SysTick ticks: %u\n",
                st->chunks,
                sim_exc_systick_fires());
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
  run_print_sd_summary();
  sim_mmio_print_bgc_and_table();
}

/**
 * @brief Print the run verdict: TRUNCATED warning or EXECUTED-to-budget.
 *
 * @details Distinguishes a wall-clock truncation (host overload, #168) from
 * a genuine full-budget or early-stop completion, with the idle / USB stop
 * variants; a BKPT run prints neither (the banner already carried it).
 *
 * @param[in] st How the run ended.
 * @pre @p st is the run's final stop snapshot.
 * @pre stderr is the report stream.
 * @post Zero or one verdict paragraph has been written to stderr.
 * @post No process state changes (pure reporting).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_verdict(const run_stop_t* st)
{
  if (st->timed_out && !st->idle_stopped && !st->usb_stopped && !st->stop_sym_hit &&
      !st->prof_stopped && !sim_exc_bkpt_hit()) {
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
                  st->chunks,
                  st->max_chunks,
                  st->wall_s);
  } else if (((st->err == UC_ERR_OK) || st->idle_stopped || st->usb_stopped || st->stop_sym_hit ||
              st->prof_stopped) &&
             !sim_exc_bkpt_hit()) {
    if (st->idle_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (idle steady-state: no "
                    "observable change for %u chunks, stopped at chunk %u).\n",
                    st->idle_stop_chunks,
                    st->chunks);
    } else if (st->usb_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (USB enumerated: device "
                    "CONFIGURED, stopped at chunk %u).\n",
                    st->chunks);
    } else {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (no invalid opcode / fault).\n");
    }
  }
}

/**
 * @brief Print each --dump-sym probe's value from emulated memory.
 *
 * @details Reads every resolved 32-bit global out of Unicorn memory and
 * prints name, address and value so a test can probe firmware state after
 * the run without a debugger; unresolved or unreadable symbols say so.
 *
 * @param[in,out] uc  The engine whose memory is probed.
 * @param[in]     cfg The run configuration carrying the probe list.
 * @pre The run has ended (values are the firmware's final state).
 * @pre @p cfg->dump_sym_addrs / names hold @p cfg->dump_sym_n entries.
 * @post One line per probe has been written to stderr.
 * @post The engine memory is unchanged (read-only probes).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_dump_syms(uc_engine* uc, const sim_run_cfg_t* cfg)
{
  /* --dump-sym: read each resolved global from Unicorn memory and print its
   * 32-bit value (and the address), so a test can probe firmware state (e.g. an
   * init-step or mismatch counter) after the run without a debugger. */
  for (uint32_t d = 0U; d < cfg->dump_sym_n; d++) {
    if (cfg->dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr, "  dump-sym      : %s = <unresolved>\n", cfg->dump_sym_names[d]);
      continue;
    }
    uint32_t v = 0U;
    if (uc_mem_read(uc, (uint64_t)cfg->dump_sym_addrs[d], &v, sizeof(v)) == UC_ERR_OK) {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = %u (0x%08X)\n",
                    cfg->dump_sym_names[d],
                    cfg->dump_sym_addrs[d],
                    v,
                    v);
    } else {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = <unreadable>\n",
                    cfg->dump_sym_names[d],
                    cfg->dump_sym_addrs[d]);
    }
  }
}

/**
 * @enum loop_action_t
 * @brief Loop-control verdict a per-phase run-loop helper hands back.
 *
 * @details The outer chunk loop is a sequence of phase helpers; each returns
 * this so the driver can `continue` (a reboot / tail-chain that re-boots the
 * chunk), `break` (a stop condition or fault), or fall through to the next
 * phase without either.
 *
 * @invariant Exactly one value is returned per phase-helper call.
 * @see run_loop()  The driver that dispatches on it.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_loop_next     = 0, /**< Fall through to the next phase in the body.       */
  k_loop_continue = 1, /**< Restart the outer loop (reboot / tail-chain).     */
  k_loop_break    = 2, /**< Leave the outer loop (stop condition / fault).    */
} loop_action_t;

/**
 * @struct run_loop_t
 * @brief The whole run's mutable state, threaded through the phase helpers.
 *
 * @details One bundle of the setup products (config, guards, presentation
 * geometry + buffers, click classification) plus every counter the chunk loop
 * mutates, so the loop body and the run-end finalizer become small phase
 * helpers over @c st-> instead of one 500-line function over three dozen
 * locals. Field semantics are unchanged from the pre-split locals.
 *
 * @invariant @c cfg and @c guards are fixed once ::run_loop_setup returns; the
 *            counter fields advance monotonically across the loop.
 * @see run_loop_setup()  The initializer.
 * @see run_loop()  The consumer.
 * @since 0.1.0
 */
typedef struct {
  const sim_run_cfg_t* cfg;              /**< Setup products (engine, image, CLI knobs). */
  run_guards_t         guards;           /**< Env-tunable budgets + stop conditions.     */
  uint16_t             panel_w;          /**< Firmware render width in pixels.          */
  uint16_t             panel_h;          /**< Firmware render height in pixels.         */
  uint16_t             disp_w;           /**< Displayed width (rotation-swapped).       */
  uint16_t             disp_h;           /**< Displayed height (rotation-swapped).      */
  uint16_t             comp_w;           /**< Composite (panel + sidebar) width.        */
  uint16_t             comp_h;           /**< Composite (panel + sidebar) height.       */
  board_view_t*        view;             /**< Live window (NULL when headless).         */
  uint16_t*            panel_fb;         /**< GLCDC render scratch (or NULL).        */
  uint16_t*            rot_fb;           /**< Rotation scratch (or NULL).            */
  uint16_t*            composite;        /**< Composite frame buffer (or NULL).      */
  board_overlay_btn_t  click_btn;        /**< Classified --click target button.      */
  bool                 click_was_tab;    /**< --click landed on a console tab.    */
  int                  reboot_count;     /**< --reboot warm reboots remaining.    */
  uint32_t             run_pc;           /**< Current resume PC.                 */
  uint32_t             chunks;           /**< Outer chunks executed so far.      */
  uint32_t             last_boot_chunk;  /**< Chunk of the last (re)boot.       */
  uint32_t             settle_left;      /**< Post-click drain countdown.       */
  uint32_t             rec_frames;       /**< --record frames written.          */
  uint32_t             prof_idle_run;    /**< Consecutive profiler-idle chunks. */
  uint32_t             idle_run;         /**< Consecutive steady-idle chunks.   */
  uint32_t             usb_stop_run;     /**< USB device settle countup.        */
  uint32_t             usbh_stop_run;    /**< USB host settle countup.          */
  uint64_t             prof_idle_prev_i; /**< Prior total insns (profiler).    */
  uint64_t             idle_sig_prev;    /**< Prior idle-signature sum.        */
  uint64_t             last_present_us;  /**< wall-us of the last live present. */
  uint32_t             prof_prev_pc;     /**< PC charged by the wall profiler.  */
  double               prof_prev_t;      /**< Start time of the prior chunk.    */
  clock_t              t0;               /**< CPU-time origin for the guard.    */
  uc_err               err;              /**< Latest uc_emu_start status.       */
  bool                 prof_stopped;     /**< Profiler stop fired.              */
  bool                 idle_stopped;     /**< Idle steady-state stop fired.     */
  bool                 usb_stopped;      /**< USB / banner early stop fired.    */
  bool                 stop_sym_hit;     /**< --stop-sym threshold reached.     */
  bool                 timed_out;        /**< Wall-clock guard fired.           */
  bool                 closed;           /**< Live window was closed.           */
  bool                 button_fired;     /**< Headless click button fired once. */
  bool                 slider_grab;      /**< Battery slider grabbed by a drag. */
  board_overlay_btn_t  held_btn;         /**< SW held down (released on up).    */
} run_loop_t;

/**
 * @brief Derive the presentation geometry from the panel size and rotation.
 *
 * @details The firmware always renders panel_w x panel_h; --rotate 90/270 swap
 * width and height for display, and the sidebar widens/heightens the displayed
 * frame into the composite that both the window and --ppm show.
 *
 * @param[in]  cfg The run configuration (panel size + rotation).
 * @param[out] st  The run state whose geometry fields are filled.
 * @return void
 * @pre @p cfg and @p st are non-NULL.
 * @pre @p cfg holds a valid panel size and rotation.
 * @post @p st panel/disp/comp dimensions are set consistently.
 * @post No allocation or engine state changes.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
static void run_setup_geometry(const sim_run_cfg_t* cfg, run_loop_t* st)
{
  st->panel_w = cfg->view_w;
  st->panel_h = cfg->view_h;
  const bool rot_swap =
    (cfg->rotate_deg == (uint32_t)k_rotate_90) || (cfg->rotate_deg == (uint32_t)k_rotate_270);
  st->disp_w = rot_swap ? st->panel_h : st->panel_w;
  st->disp_h = rot_swap ? st->panel_w : st->panel_h;
  st->comp_w = board_overlay_total_width(st->disp_w);
  st->comp_h = board_overlay_total_height(st->disp_h);
}

/**
 * @brief Open the live window (if requested) and allocate the frame buffers.
 *
 * @details --view opens a window (headless fallback on failure). Any of --view
 * / --ppm / --click / --record needs the panel + composite buffers; a non-zero
 * rotation additionally needs the rotation scratch. Allocated once for the run.
 *
 * @param[in]     cfg The run configuration (output-mode flags + rotation).
 * @param[in,out] st  The run state (geometry read; view/buffers written).
 * @return void
 * @pre ::run_setup_geometry has filled @p st geometry.
 * @pre @p cfg and @p st are non-NULL.
 * @post @p st->view is a window handle or NULL (headless).
 * @post The buffers are allocated iff an output mode needs them.
 * @note Not thread-safe; performs window open + malloc during setup.
 * @since 0.1.0
 */
static void run_open_view_buffers(const sim_run_cfg_t* cfg, run_loop_t* st)
{
  if (cfg->want_view) {
    st->view = board_view_open(st->comp_w, st->comp_h, cfg->win_title);
    if (st->view == nullptr) {
      (void)fprintf(stderr, "board_sim: could not open window; continuing headless\n");
    }
  }
  if ((st->view != nullptr) || (cfg->ppm_path != nullptr) || cfg->want_click ||
      (cfg->record_dir != nullptr)) {
    st->panel_fb  = (uint16_t*)malloc((size_t)st->panel_w * (size_t)st->panel_h * sizeof(uint16_t));
    st->composite = (uint16_t*)malloc((size_t)st->comp_w * (size_t)st->comp_h * sizeof(uint16_t));
    if (cfg->rotate_deg != (uint32_t)k_rotate_0) {
      st->rot_fb = (uint16_t*)malloc((size_t)st->disp_w * (size_t)st->disp_h * sizeof(uint16_t));
    }
  }
}

/**
 * @brief Classify a headless --click into a tab / button / panel target once.
 *
 * @details A click on the console tab bar switches the active console channel
 * (a one-shot view change); an on-screen sidebar button toggles a user switch
 * (fired once); anything else is a panel touch (re-armed until drained). The
 * classification is resolved here so the loop just replays it.
 *
 * @param[in]     cfg The run configuration (--click coordinates + flag).
 * @param[in,out] st  The run state (click_was_tab / click_btn written).
 * @return void
 * @pre ::run_setup_geometry has filled @p st->disp_w.
 * @pre @p cfg and @p st are non-NULL.
 * @post @p st->click_was_tab and @p st->click_btn reflect the click target.
 * @post A tab click has already switched the active console channel.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
static void run_classify_click(const sim_run_cfg_t* cfg, run_loop_t* st)
{
  if (!cfg->want_click) {
    return;
  }
  (void)fprintf(stderr, "board_sim: --click armed at (%d,%d)\n", cfg->click_x, cfg->click_y);
  uint32_t click_tab_idx = 0U;
  if (board_overlay_hit_console_tab((uint16_t)cfg->click_x,
                                    (uint16_t)cfg->click_y,
                                    st->disp_w,
                                    (uint32_t)k_board_console_ch_count,
                                    &click_tab_idx)) {
    sim_view_select_console_tab(click_tab_idx);
    st->click_was_tab = true;
  }
  st->click_btn =
    (!st->click_was_tab)
      ? board_overlay_hit_button((uint16_t)cfg->click_x, (uint16_t)cfg->click_y, st->disp_w)
      : k_board_overlay_btn_none;
}

/**
 * @brief Populate the run state from the config before the chunk loop.
 *
 * @details Zeroes the mutable counters, derives the presentation geometry,
 * opens the window / buffers, reads the run-guard env knobs, classifies a
 * headless --click, and seeds the resume PC / reboot count / CPU-time origin.
 *
 * @param[in]  cfg The setup products (see ::sim_run_cfg_t).
 * @param[out] st  The run state to initialize.
 * @return void
 * @pre @p cfg is fully populated and @p st is non-NULL.
 * @pre The engine referenced by @p cfg is ready to run.
 * @post @p st is fully initialized for the first chunk.
 * @post The window / buffers / guards / click are all resolved.
 * @note Not thread-safe; this is the single-threaded setup entry.
 * @since 0.1.0
 */
static void run_loop_setup(const sim_run_cfg_t* cfg, run_loop_t* st)
{
  *st              = (run_loop_t){};
  st->cfg          = cfg;
  st->click_btn    = k_board_overlay_btn_none;
  st->held_btn     = k_board_overlay_btn_none;
  st->reboot_count = cfg->reboot_count;
  st->run_pc       = cfg->initial_pc;
  st->err          = UC_ERR_OK;
  run_setup_geometry(cfg, st);
  run_open_view_buffers(cfg, st);
  st->guards = run_read_guards(cfg, st->view);
  run_classify_click(cfg, st);
  st->t0 = clock();
}

/**
 * @brief Run-loop prologue: wall-profiler charge, telemetry, warm reboots.
 *
 * @details Charges the previous chunk's wall time to its start PC (BOARD_SIM_
 * PROFILE wall mode), publishes PC + chunk telemetry to the board view, then
 * services a scheduled --reboot and any watchdog-requested reset by warm-
 * rebooting the image (a failed reload ends the run).
 *
 * @param[in,out] st The run state.
 * @return The loop action for the driver.
 * @retval k_loop_continue A warm reboot happened; restart the outer loop.
 * @retval k_loop_break    A warm reboot failed to reload the image.
 * @retval k_loop_next     No reboot; proceed to the chunk.
 * @pre @p st is initialized and its engine is ready.
 * @pre @p st->run_pc holds the current resume PC.
 * @post On k_loop_continue, @p st->run_pc / last_boot_chunk are updated.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static loop_action_t run_loop_prologue(run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  /* BOARD_SIM_PROFILE: charge the wall time of the previous chunk to the
   * function its execution started in (so a cheap WFI-halt chunk and an
   * expensive compute chunk are weighted by real time, not by chunk count). */
  if (sim_prof_mode() == k_prof_wall) {
    const double now = board_now_s();
    if (st->prof_prev_t > 0.0) {
      prof_add(st->prof_prev_pc, now - st->prof_prev_t);
    }
    st->prof_prev_pc = st->run_pc;
    st->prof_prev_t  = now;
  }
  sim_view_publish(st->run_pc, st->chunks); /* PC + chunk counter for the board view. */

  /* --reboot N: after each boot's settle window force a power-on warm reboot;
   * the reset-retained models (VBATT backup) survive, proving reset-survival. */
  if ((st->reboot_count > 0) && ((st->chunks - st->last_boot_chunk) >= (uint32_t)k_reboot_settle)) {
    st->reboot_count--;
    board_periph_reset_set_cause(true, false, false, false); /* power-on reboot */
    st->run_pc = warm_reboot(cfg->uc, cfg->elf, cfg->elf_len, cfg->want_trace);
    if (st->run_pc == 0U) {
      return k_loop_break; /* reboot failed to reload the image -- end the run */
    }
    st->last_boot_chunk = st->chunks;
    return k_loop_continue;
  }
  /* A peripheral (the watchdog) may have requested a reset on its tick: latch
   * the watchdog cause and warm-reboot so the next boot reads reset_by=wdt. */
  bool wdt_rst  = false;
  bool iwdt_rst = false;
  if (board_periph_reset_take_request(&wdt_rst, &iwdt_rst)) {
    board_periph_reset_set_cause(false, false, wdt_rst, iwdt_rst);
    st->run_pc = warm_reboot(cfg->uc, cfg->elf, cfg->elf_len, cfg->want_trace);
    if (st->run_pc == 0U) {
      return k_loop_break; /* reboot failed to reload the image -- end the run */
    }
    st->last_boot_chunk = st->chunks;
    return k_loop_continue;
  }
  return k_loop_next;
}

/**
 * @brief Arm the tick, advance time/peripherals, drain input, replay the click.
 *
 * @details Arms the once-per-chunk SysTick, advances the DWT cycle counter and
 * the modelled timers / network, drains buffered keystrokes into the console
 * UART RX, and re-arms the headless --click (an on-screen button fires once; a
 * panel touch is re-injected until the firmware drains it).
 *
 * @param[in,out] st The run state.
 * @return void
 * @pre @p st is initialized and its engine is ready.
 * @pre The seams / exception hooks are installed.
 * @post The tick is armed and one tick-period of peripheral time has advanced.
 * @post A pending headless click has been (re-)applied for this chunk.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static void run_loop_tick_inputs(run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  sim_exc_arm_systick(); /* one SysTick period per outer chunk. */
  /* DWT cycle counter: keep a masked-context ra8_delay_ms (spins on CYCCNT with
   * PRIMASK set) making progress. Inert unless the firmware enabled it. */
  dwt_cyccnt_advance(cfg->uc);
  board_periph_tick(cfg->uc); /* advance timers; pend any wrap IRQ like SysTick. */
  board_net_tick();

  /* Drain buffered keystrokes into the console UART RX (the SCI channel --input
   * targets), from the live window (keyDown) and the headless --keys injector. */
  char key_byte = 0;
  while (board_input_pop_key(&key_byte)) {
    const uint8_t kb = (uint8_t)key_byte;
    board_periph_sci_feed_rx(board_periph_sci_console_channel(), &kb, 1U);
  }

  /* Headless --click: keep one contact armed in the GT911 model until the
   * firmware's real ra8_touch_read drains it; re-arming each chunk is needed
   * because ra8_touch_open clears the GT911 status byte during bring-up. */
  if (cfg->want_click && (st->click_btn != k_board_overlay_btn_none)) {
    if (!st->button_fired) {
      if ((st->click_btn == k_board_overlay_btn_battery) ||
          (st->click_btn == k_board_overlay_btn_batt_chg) ||
          (st->click_btn == k_board_overlay_btn_lowpower)) {
        apply_battery_click(st->click_btn, (uint16_t)cfg->click_x, st->disp_w); /* SOC/CHG/LP. */
      } else {
        set_switch(st->click_btn, true); /* headless --click SW1/SW2: press + hold. */
      }
      st->button_fired = true;
    }
  } else if (cfg->want_click && !st->click_was_tab && (board_periph_touch_reported() == 0U)) {
    uint16_t cnx = (uint16_t)cfg->click_x;
    uint16_t cny = (uint16_t)cfg->click_y;
    unrotate_click((uint16_t)cfg->click_x,
                   (uint16_t)cfg->click_y,
                   st->panel_w,
                   st->panel_h,
                   cfg->rotate_deg,
                   &cnx,
                   &cny);
    board_periph_touch_inject(cnx, cny);
  }
}

/**
 * @brief Run one chunk via run_inner and service its post-chunk resets.
 *
 * @details Runs the inner exception-resolve loop, then ends the run on a fault
 * or a profiler STOP_PC, or warm-reboots on an AIRCR.SYSRESETREQ (a failed
 * reload ends the run). A successful software reset restarts the outer loop.
 *
 * @param[in,out] st The run state.
 * @return The loop action for the driver.
 * @retval k_loop_break    A fault, BKPT, STOP_PC, or failed reload ended it.
 * @retval k_loop_continue A software reset warm-rebooted; restart the loop.
 * @retval k_loop_next     The chunk resolved cleanly; proceed.
 * @pre @p st is initialized and its engine is ready.
 * @pre The seams / exception hooks are installed.
 * @post @p st->run_pc / err reflect the chunk's outcome.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static loop_action_t run_loop_run_chunk(run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  if (run_inner(cfg->uc, cfg->vtor_base, &st->run_pc, &st->err)) {
    return k_loop_break;
  }
  if (sim_prof_stop_hit()) {
    st->prof_stopped = true; /* BOARD_SIM_STOP_PC reached -- end the profiled run. */
    return k_loop_break;
  }
  /* AIRCR.SYSRESETREQ: latch a software-reset cause and warm reboot the firmware
   * from its reset vector, then keep running. */
  if (sim_exc_reboot_requested()) {
    board_periph_reset_set_cause(false, true, false, false); /* software reset */
    st->run_pc = warm_reboot(cfg->uc, cfg->elf, cfg->elf_len, cfg->want_trace);
    sim_exc_clear_reboot_request();
    if (st->run_pc == 0U) {
      return k_loop_break; /* reboot failed to reload the image -- end the run */
    }
    return k_loop_continue;
  }
  return k_loop_next;
}

/**
 * @brief Step cpu1 and dump a --record frame at the recording cadence.
 *
 * @details Interleaves one cpu1 step (a real second core sharing SRAM, not a
 * model) with cpu0, then, every k_record_every chunks with --record active,
 * composites the panel + sidebar and writes the next numbered PPM.
 *
 * @param[in,out] st The run state.
 * @return void
 * @pre @p st is initialized and its engine is ready.
 * @pre With --record, @p st->composite is allocated.
 * @post cpu1 has advanced one step.
 * @post A recorded frame count reflects any PPM written this chunk.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static void run_loop_record(run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  /* Dual-core: step cpu1 interleaved with cpu0; a cpu1 fault just stops cpu1. */
  sim_cpu1_step();
  /* --record: dump the composite every k_record_every chunks as a numbered PPM. */
  if ((cfg->record_dir != nullptr) && (st->composite != nullptr) &&
      ((st->chunks % (uint32_t)k_record_every) == 0U)) {
    build_composite(cfg->uc,
                    st->panel_fb,
                    st->rot_fb,
                    st->composite,
                    st->panel_w,
                    st->panel_h,
                    st->disp_w,
                    st->disp_h,
                    cfg->rotate_deg,
                    cfg->win_title);
    char fpath[1024];
    (void)snprintf(fpath,
                   sizeof(fpath),
                   "%s/frame_%06u.ppm",
                   cfg->record_dir,
                   (unsigned)st->rec_frames);
    if (write_ppm(fpath, st->composite, st->comp_w, st->comp_h) == 0) {
      st->rec_frames++;
    }
  }
}

/**
 * @brief Composite and present the live window at the ~60 Hz present cadence.
 *
 * @details Compositing + uploading the full panel + sidebar frame is the
 * dominant host cost, so the live present is capped to ~60 Hz wall-clock; when
 * a present is skipped the host CPU is yielded so an idle app does not spin a
 * core redrawing identical frames.
 *
 * @param[in,out] st The run state.
 * @return void
 * @pre @p st->view is a live window and its buffers are allocated.
 * @pre @p st->chunks is a multiple of k_view_present_every.
 * @post Either a frame was presented or the host CPU was yielded.
 * @post @p st->last_present_us tracks the last present time.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static void run_view_maybe_present(run_loop_t* st)
{
  const sim_run_cfg_t* cfg    = st->cfg;
  struct timespec      ts_now = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts_now);
  const uint64_t now_us = ((uint64_t)ts_now.tv_sec * (uint64_t)k_us_per_s) +
                          ((uint64_t)ts_now.tv_nsec / (uint64_t)k_ns_per_us);
  if ((now_us - st->last_present_us) >= (uint64_t)k_view_frame_us) {
    build_composite(cfg->uc,
                    st->panel_fb,
                    st->rot_fb,
                    st->composite,
                    st->panel_w,
                    st->panel_h,
                    st->disp_w,
                    st->disp_h,
                    cfg->rotate_deg,
                    cfg->win_title);
    board_view_present(st->view, st->composite, st->comp_w, st->comp_h);
    st->last_present_us = now_us;
  } else {
    (void)usleep((useconds_t)k_view_yield_us);
  }
}

/**
 * @brief Handle live-window mouse input, presenting, and the close request.
 *
 * @details Mouse-down on an on-screen SW1/SW2 presses that momentary switch (a
 * panel press arms one GT911 contact); a battery-slider press grabs the slider
 * so a drag keeps setting the SOC; mouse-up releases the held switch and the
 * grab; the wheel pages the console scrollback. Presents at the cadence and
 * ends the run when the window is closed.
 *
 * @param[in,out] st The run state.
 * @return The loop action for the driver.
 * @retval k_loop_break The window was closed.
 * @retval k_loop_next  The window is still open; proceed.
 * @pre @p st->view is a live window.
 * @pre @p st is initialized and its buffers are allocated.
 * @post On k_loop_break, @p st->closed is true.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static loop_action_t run_loop_view(run_loop_t* st)
{
  uint16_t cx = 0U;
  uint16_t cy = 0U;
  if (board_view_poll_click(st->view, &cx, &cy)) {
    const board_overlay_btn_t hit =
      route_click(cx, cy, st->panel_w, st->panel_h, st->disp_w, st->cfg->rotate_deg);
    st->slider_grab = (hit == k_board_overlay_btn_battery);
    st->held_btn    = ((hit == k_board_overlay_btn_sw1) || (hit == k_board_overlay_btn_sw2))
                        ? hit
                        : k_board_overlay_btn_none;
  }
  if (board_view_poll_release(st->view)) { /* mouse-up: release a held button + grab. */
    if (st->held_btn != k_board_overlay_btn_none) {
      set_switch(st->held_btn, false);
      st->held_btn = k_board_overlay_btn_none;
    }
    st->slider_grab = false;
  }
  uint16_t dx = 0U;
  uint16_t dy = 0U;
  if (board_view_poll_drag(st->view, &dx, &dy) && st->slider_grab) {
    apply_battery_click(k_board_overlay_btn_battery, dx, st->disp_w);
  }
  sim_view_wheel(board_view_poll_scroll(st->view)); /* wheel pages the scrollback. */
  if ((st->chunks % (uint32_t)k_view_present_every) == 0U) {
    run_view_maybe_present(st);
  }
  if (board_view_pump(st->view)) {
    st->closed = true;
    return k_loop_break;
  }
  return k_loop_next;
}

/**
 * @brief Run the bounded post-click tail and the CPU-time wall guard (headless).
 *
 * @details After the headless --click input lands (a drained touch or a fired
 * on-screen button) a bounded settle tail runs so the dumped frame shows the
 * result, then stops; the CPU-time wall guard ends a run that overran.
 *
 * @param[in,out] st The run state.
 * @return The loop action for the driver.
 * @retval k_loop_break The settle tail expired or the wall guard fired.
 * @retval k_loop_next  Keep draining the tail.
 * @pre @p st->view is NULL and --click is active.
 * @pre @p st is initialized.
 * @post On a wall-guard stop, @p st->timed_out is true.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static loop_action_t run_loop_click_tail(run_loop_t* st)
{
  const bool click_acted = (st->click_btn != k_board_overlay_btn_none)
                             ? st->button_fired
                             : (board_periph_touch_reported() > 0U);
  if (click_acted) {
    st->settle_left =
      (st->settle_left == 0U) ? st->guards.click_settle_chunks : (st->settle_left - 1U);
    if (st->settle_left == 1U) {
      return k_loop_break;
    }
  }
  if (st->guards.wall_guard_on &&
      (((double)(clock() - st->t0) / (double)CLOCKS_PER_SEC) >= st->guards.wall_s)) {
    st->timed_out = true;
    return k_loop_break;
  }
  return k_loop_next;
}

/**
 * @brief Profiler compute-idle early-stop (insn mode, build-stable).
 *
 * @details A chunk that retires very few instructions is an idle frame; after
 * enough consecutive idle chunks (armed only after a warm-up) the firmware has
 * reached its steady frame loop, so the profile spans boot, not the idle tail.
 *
 * @param[in,out] st The run state.
 * @return true when the run should end here.
 * @retval true  Enough consecutive profiler-idle chunks elapsed.
 * @retval false Not in insn-profile mode, recording, or not yet idle.
 * @pre @p st is initialized (headless plain run).
 * @pre @p st->guards holds the profiler-idle thresholds.
 * @post @p st->prof_stopped is set true only when true is returned.
 * @post @p st->prof_idle_prev_i / prof_idle_run track the idle streak.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static bool run_stop_prof_idle(run_loop_t* st)
{
  if (!((sim_prof_mode() == k_prof_insn) && (st->cfg->record_dir == nullptr) &&
        (st->chunks >= st->guards.prof_idle_arm))) {
    return false;
  }
  const uint64_t d     = sim_prof_total_insns() - st->prof_idle_prev_i;
  st->prof_idle_prev_i = sim_prof_total_insns();
  if (d < (uint64_t)st->guards.prof_idle_insns) {
    st->prof_idle_run++;
    if (st->prof_idle_run >= st->guards.prof_idle_need) {
      st->prof_stopped = true;
      return true;
    }
  } else {
    st->prof_idle_run = 0U;
  }
  return false;
}

/**
 * @brief Steady-state idle early-stop (BOARD_SIM_IDLE_STOP).
 *
 * @details All tracked counters (MMIO reads/writes, PendSV/SVCall, peripheral
 * IRQs) are monotonic, so an unchanged sum for N consecutive chunks means the
 * firmware reached steady-state idle and there is nothing left to run. Not
 * evaluated while recording (which must span its full window).
 *
 * @param[in,out] st The run state.
 * @return true when the run should end here.
 * @retval true  The idle signature held for idle_stop_chunks chunks.
 * @retval false Idle-stop off, recording, or the signature changed.
 * @pre @p st is initialized (headless plain run).
 * @pre @p st->guards.idle_stop_chunks is the configured window.
 * @post @p st->idle_stopped is set true only when true is returned.
 * @post @p st->idle_sig_prev / idle_run track the idle streak.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static bool run_stop_idle(run_loop_t* st)
{
  if (!((st->guards.idle_stop_chunks > 0U) && (st->cfg->record_dir == nullptr))) {
    return false;
  }
  const uint64_t idle_sig = (uint64_t)sim_mmio_reads() + (uint64_t)sim_mmio_writes() +
                            (uint64_t)sim_exc_pendsv_takes() + (uint64_t)sim_exc_svc_takes() +
                            (uint64_t)board_periph_irq_total();
  if (idle_sig == st->idle_sig_prev) {
    st->idle_run++;
    if (st->idle_run >= st->guards.idle_stop_chunks) {
      st->idle_stopped = true;
      return true;
    }
  } else {
    st->idle_sig_prev = idle_sig;
    st->idle_run      = 0U;
  }
  return false;
}

/**
 * @brief USB device- and host-mode enumeration early-stops.
 *
 * @details USB device apps never go idle, so once the device reaches
 * CONFIGURED a short settle window runs (for the first class traffic + report),
 * then stops; the host-mode stop is kept distinct so a host app that also runs
 * a device worker is not stopped by that worker reaching CONFIGURED first.
 *
 * @param[in,out] st The run state.
 * @return true when the run should end here.
 * @retval true  A USB device or host settle window elapsed.
 * @retval false Both USB stops are off, recording, or not yet reached.
 * @pre @p st is initialized (headless plain run).
 * @pre @p st->guards holds the USB settle windows.
 * @post @p st->usb_stopped is set true only when true is returned.
 * @post @p st->usb_stop_run / usbh_stop_run track the settle windows.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static bool run_stop_usb(run_loop_t* st)
{
  const bool recording = (st->cfg->record_dir != nullptr);
  if ((st->guards.usb_stop_settle > 0U) && !recording && board_usb_configured()) {
    st->usb_stop_run++;
    if (st->usb_stop_run >= st->guards.usb_stop_settle) {
      st->usb_stopped = true;
      return true;
    }
  }
  if ((st->guards.usbh_stop_settle > 0U) && !recording && sim_usbh_done()) {
    st->usbh_stop_run++;
    if (st->usbh_stop_run >= st->guards.usbh_stop_settle) {
      st->usb_stopped = true;
      return true;
    }
  }
  return false;
}

/**
 * @brief Generic console-banner early-stop (BOARD_SIM_STOP_ON).
 *
 * @details Ends the run as soon as the stop substring appears on any of the
 * three text endpoints -- the UART last line, the ITM/SWO stimulus stream, or
 * the SEGGER RTT up-buffer -- so a ra8_log-only or RTT-only app stops the
 * instant its verdict is emitted, exactly as for UART-banner apps.
 *
 * @param[in,out] st The run state.
 * @return true when the run should end here.
 * @retval true  The stop substring appeared on a console endpoint.
 * @retval false STOP_ON is unset or the substring has not appeared.
 * @pre @p st is initialized (headless plain run).
 * @pre @p st->guards.stop_on is the substring or NULL.
 * @post @p st->usb_stopped is set true only when true is returned.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static bool run_stop_banner(run_loop_t* st)
{
  const char* const stop_on = st->guards.stop_on;
  if (stop_on == nullptr) {
    return false;
  }
  const char* const last_uart = board_periph_uart_last_line();
  const char* const last_itm  = board_console_line(k_board_console_ch_itm, 0U);
  const char* const last_rtt  = board_console_line(k_board_console_ch_rtt, 0U);
  const bool        hit = ((last_uart != nullptr) && (strstr(last_uart, stop_on) != nullptr)) ||
                          ((last_itm != nullptr) && (strstr(last_itm, stop_on) != nullptr)) ||
                          ((last_rtt != nullptr) && (strstr(last_rtt, stop_on) != nullptr));
  if (hit) {
    st->usb_stopped = true;
    return true;
  }
  return false;
}

/**
 * @brief --stop-sym watched-global early-stop.
 *
 * @details Ends the run the instant the watched 32-bit global reaches its
 * threshold (the jlink_memprobe counter floor). Checked at the chunk boundary
 * like STOP_ON, so the stop is deterministic and host-load independent.
 *
 * @param[in,out] st The run state.
 * @return true when the run should end here.
 * @retval true  The watched global reached its threshold.
 * @retval false --stop-sym is off or the global is below the threshold.
 * @pre @p st is initialized (headless plain run).
 * @pre @p st->cfg holds the resolved --stop-sym address + threshold.
 * @post @p st->stop_sym_hit is set true only when true is returned.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static bool run_stop_sym(run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  if (cfg->stop_sym_addr == 0U) {
    return false;
  }
  uint32_t sv = 0U;
  if ((uc_mem_read(cfg->uc, (uint64_t)cfg->stop_sym_addr, &sv, sizeof(sv)) == UC_ERR_OK) &&
      (sv >= cfg->stop_sym_thresh)) {
    st->stop_sym_hit = true;
    return true;
  }
  return false;
}

/**
 * @brief CPU-time wall-clock safety guard (headless plain run).
 *
 * @details Ends a run that has burned its CPU-time budget (clock(), so a
 * heavily-loaded host trips it sooner). WALL_S=0 disables the guard (#168).
 *
 * @param[in,out] st The run state.
 * @return true when the run should end here.
 * @retval true  The CPU-time budget was reached.
 * @retval false The guard is disabled or the budget is not yet spent.
 * @pre @p st is initialized and @p st->t0 is the CPU-time origin.
 * @pre @p st->guards holds the wall bound + enable.
 * @post @p st->timed_out is set true only when true is returned.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static bool run_stop_wall(run_loop_t* st)
{
  if (st->guards.wall_guard_on &&
      (((double)(clock() - st->t0) / (double)CLOCKS_PER_SEC) >= st->guards.wall_s)) {
    st->timed_out = true;
    return true;
  }
  return false;
}

/**
 * @brief Evaluate every headless plain-run early-stop in order.
 *
 * @details Runs the profiler-idle, steady-idle, USB, banner, --stop-sym and
 * wall-guard stops in the pre-split order; the first to fire ends the run. Each
 * stop keeps its per-chunk side effects (counter updates), and short-circuit
 * evaluation preserves that only the stops up to and including the firing one
 * run this chunk -- exactly as the original sequential breaks did.
 *
 * @param[in,out] st The run state.
 * @return The loop action for the driver.
 * @retval k_loop_break An early-stop fired.
 * @retval k_loop_next  No stop fired; proceed.
 * @pre @p st->view is NULL and --click is inactive (plain headless run).
 * @pre @p st is initialized.
 * @post The stop-cause flag for the firing stop is set.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static loop_action_t run_loop_headless(run_loop_t* st)
{
  if (run_stop_prof_idle(st) || run_stop_idle(st) || run_stop_usb(st) || run_stop_banner(st) ||
      run_stop_sym(st) || run_stop_wall(st)) {
    return k_loop_break;
  }
  return k_loop_next;
}

/**
 * @brief Dispatch the per-chunk output / stop phase by run mode.
 *
 * @details A live window services mouse input and presents; a headless --click
 * runs its settle tail; a plain headless run evaluates the early-stops.
 *
 * @param[in,out] st The run state.
 * @return The loop action for the driver.
 * @retval k_loop_break A window close, settle-tail end, or early-stop fired.
 * @retval k_loop_next  Proceed to the next chunk.
 * @pre @p st is initialized.
 * @pre The chunk has been run and recorded.
 * @post The relevant stop-cause / closed flag is set on k_loop_break.
 * @note Not thread-safe; part of the single-threaded run loop.
 * @since 0.1.0
 */
static loop_action_t run_loop_present_and_stops(run_loop_t* st)
{
  if (st->view != nullptr) {
    return run_loop_view(st);
  }
  if (st->cfg->want_click) {
    return run_loop_click_tail(st);
  }
  return run_loop_headless(st);
}

/**
 * @brief Drive the chunked run loop to a stop condition.
 *
 * @details One SysTick period per outer chunk: prologue (profiler charge /
 * telemetry / reboots), tick + inputs, the inner exception-resolve chunk, cpu1
 * step + record, then the mode-specific present / stop phase. Each phase hands
 * back a ::loop_action_t the driver dispatches on.
 *
 * @param[in,out] st The run state.
 * @return void
 * @pre @p st is fully initialized by ::run_loop_setup.
 * @pre The engine and seams are ready.
 * @post @p st holds the run's final PC, counters and stop-cause flags.
 * @note Not thread-safe; this IS the single-threaded run.
 * @since 0.1.0
 */
static void run_loop(run_loop_t* st)
{
  for (; st->chunks < st->guards.max_chunks; st->chunks++) {
    const loop_action_t pro = run_loop_prologue(st);
    if (pro == k_loop_break) {
      break;
    }
    if (pro == k_loop_continue) {
      continue;
    }
    run_loop_tick_inputs(st);
    const loop_action_t chunk = run_loop_run_chunk(st);
    if (chunk == k_loop_break) {
      break;
    }
    if (chunk == k_loop_continue) {
      continue;
    }
    run_loop_record(st);
    if (run_loop_present_and_stops(st) == k_loop_break) {
      break;
    }
  }
}

/**
 * @brief Print the run-end report (stop summary, verdict, --dump-sym probes).
 *
 * @details Marks the board view parked, snapshots the stop bookkeeping into a
 * ::run_stop_t and hands it to the summary + verdict printers, then prints the
 * --dump-sym probe values.
 *
 * @param[in] st The run state (final counters + flags).
 * @return void
 * @pre The run loop has ended and @p st reflects its outcome.
 * @pre stderr is the report stream.
 * @post The report section has been written to stderr.
 * @post No engine or process state changes beyond output.
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_report(const run_loop_t* st)
{
  sim_view_mark_stopped(st->run_pc); /* final / held frame shows "parked". */
  const run_stop_t stop = {
    .err              = st->err,
    .run_pc           = st->run_pc,
    .chunks           = st->chunks,
    .max_chunks       = st->guards.max_chunks,
    .wall_s           = st->guards.wall_s,
    .idle_stop_chunks = st->guards.idle_stop_chunks,
    .timed_out        = st->timed_out,
    .idle_stopped     = st->idle_stopped,
    .usb_stopped      = st->usb_stopped,
    .stop_sym_hit     = st->stop_sym_hit,
    .prof_stopped     = st->prof_stopped,
  };
  run_print_stop_summary(st->cfg->uc, &stop);
  run_print_verdict(&stop);
  run_print_dump_syms(st->cfg->uc, st->cfg);
}

/**
 * @brief Write the --ppm snapshot and the --record summary line.
 *
 * @details With --ppm, composites the final frame and writes it (reporting
 * success / failure); with --record, prints how many frames were dumped.
 *
 * @param[in] st The run state (buffers + output paths).
 * @return void
 * @pre The run loop has ended and @p st reflects its outcome.
 * @pre stderr is the report stream.
 * @post The --ppm file is written and/or the --record line printed as requested.
 * @post No engine or process state changes beyond output.
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_write_outputs(const run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  if ((cfg->ppm_path != nullptr) && (st->composite != nullptr)) {
    build_composite(cfg->uc,
                    st->panel_fb,
                    st->rot_fb,
                    st->composite,
                    st->panel_w,
                    st->panel_h,
                    st->disp_w,
                    st->disp_h,
                    cfg->rotate_deg,
                    cfg->win_title);
    if (write_ppm(cfg->ppm_path, st->composite, st->comp_w, st->comp_h) == 0) {
      (void)fprintf(stderr,
                    "  wrote %s (%ux%u)\n",
                    cfg->ppm_path,
                    (unsigned)st->comp_w,
                    (unsigned)st->comp_h);
    } else {
      (void)fprintf(stderr, "  could not write %s\n", cfg->ppm_path);
    }
  }
  if (cfg->record_dir != nullptr) {
    (void)fprintf(stderr,
                  "  recorded %u frame(s) to %s (%ux%u, ~%u fps)\n",
                  st->rec_frames,
                  cfg->record_dir,
                  (unsigned)st->comp_w,
                  (unsigned)st->comp_h,
                  (unsigned)k_record_fps);
  }
}

/**
 * @brief Hold the live window on the final frame until the user closes it.
 *
 * @details When a --view run ended on its own (not by a window close), re-
 * presents the last composite and blocks until the window is closed, then
 * closes it. Inert in headless mode.
 *
 * @param[in] st The run state (window + buffers).
 * @return void
 * @pre The run loop has ended and @p st reflects its outcome.
 * @pre @p st->view is a live window or NULL.
 * @post Any live window has been presented (if needed) and closed.
 * @post No engine or process state changes beyond the window.
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_hold_view(const run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  if (st->view == nullptr) {
    return;
  }
  if (!st->closed) { /* run ended on its own -- keep the last frame up until closed */
    build_composite(cfg->uc,
                    st->panel_fb,
                    st->rot_fb,
                    st->composite,
                    st->panel_w,
                    st->panel_h,
                    st->disp_w,
                    st->disp_h,
                    cfg->rotate_deg,
                    cfg->win_title);
    board_view_present(st->view, st->composite, st->comp_w, st->comp_h);
    (void)fprintf(stderr, "board_sim: run ended; close the window to exit\n");
    while (!board_view_pump(st->view)) {
      (void)usleep((useconds_t)k_view_idle_us);
    }
  }
  board_view_close(st->view);
}

/**
 * @brief Save the SD image, release buffers + engine, map the exit code.
 *
 * @details Dumps the (possibly firmware-modified) SD image with --save-sd,
 * frees the presentation buffers and the run-long ELF, closes the engine, and
 * maps the outcome to the #67 matrix exit code (BKPT / fault / timeout each a
 * distinct non-zero code, else success).
 *
 * @param[in] st The run state (buffers, engine, stop flags).
 * @return The process exit status.
 * @retval k_board_sim_exit_ok      Clean run-to-budget.
 * @retval k_board_sim_exit_bkpt    Firmware executed a BKPT.
 * @retval k_board_sim_exit_fault   Emulation fault ended the run.
 * @retval k_board_sim_exit_timeout Wall-clock budget reached.
 * @pre The run loop has ended and outputs have been written.
 * @pre @p st owns the buffers / engine / image being released.
 * @post The buffers, image and engine have been freed / closed.
 * @note Not thread-safe; part of the single-threaded teardown.
 * @since 0.1.0
 */
static int run_cleanup(const run_loop_t* st)
{
  const sim_run_cfg_t* cfg = st->cfg;
  if (cfg->save_sd_path != nullptr) {
    (void)board_sd_save(cfg->save_sd_path);
  }
  free(st->panel_fb);
  free(st->rot_fb);
  free(st->composite);
  free(cfg->elf); /* kept alive for the whole run so a warm reboot can re-load it */
  (void)uc_close(cfg->uc);

  board_sim_exit_t exit_code = k_board_sim_exit_ok;
  if (sim_exc_bkpt_hit()) {
    exit_code = k_board_sim_exit_bkpt;
  } else if (st->err != UC_ERR_OK) {
    exit_code = k_board_sim_exit_fault;
  } else if (st->timed_out) {
    exit_code = k_board_sim_exit_timeout;
  }
  return (int)exit_code;
}

int sim_run_and_report(const sim_run_cfg_t* cfg)
{
  run_loop_t st = {};
  run_loop_setup(cfg, &st);
  run_loop(&st);
  run_report(&st);
  run_write_outputs(&st);
  run_hold_view(&st);
  return run_cleanup(&st);
}
