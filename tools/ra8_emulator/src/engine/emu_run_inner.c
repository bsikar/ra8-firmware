/**
 * @file emu_run_inner.c
 * @brief Inner per-chunk exception-resolve loop (see emu_run_internal.h)
 *
 * @details
 * The inner exception-resolve loop that services one outer chunk: the idle /
 * low-power budget, the zero-time relaunch + run-ending stop checks, and the
 * exception boundary (EXC_RETURN unstack + tail-chain, MPU / div-0 fault
 * synthesis, tick / PendSV take). Split out of emu_run.c so each TU stays under
 * the file-size bar. The contract for run_inner() lives on its declaration in
 * emu_run_internal.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_mpu.h"
#include "emu_prof.h"
#include "emu_run_internal.h"
#include "emu_seams.h"
#include "emu_view.h"

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
  if (emu_low_power()) {
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
 * @pre @p faulted and @p act are non-nullptr.
 * @pre The chunk has returned and its final PC has been read back.
 * @post @p faulted is written only when true is returned via a BKPT.
 * @post On true, @p act is ::k_inner_continue or ::k_inner_break.
 * @note Not thread-safe; part of the single-threaded run core.
 * @since 0.1.0
 */
static bool run_inner_check_stops(uc_engine* uc, bool* faulted, inner_action_t* act)
{
  (void)uc;
  if (emu_seam_take_relaunch()) {
    /* A --fast-sd byte-exchange returned to its caller. This consumed no
     * modelled time, so relaunch from the returned PC without advancing the
     * SysTick or charging a chunk (the inner-loop cap still bounds the run). */
    *act = k_inner_continue;
    return true;
  }
  if (emu_prof_stop_hit()) {
    *act = k_inner_break; /* RA8_EMU_STOP_PC reached (prof_insn_hook) -- end the run. */
    return true;
  }
  if (emu_exc_reboot_requested()) {
    *act = k_inner_break; /* AIRCR.SYSRESETREQ -- the outer wrapper performs the reboot. */
    return true;
  }
  if (emu_exc_bkpt_hit()) {
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
 * @pre @p run_pc and @p faulted are non-nullptr.
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
  if (emu_exc_take_exc_return(&exc_ret_pc)) {
    exc_return(uc, (uint32_t)exc_ret_pc);
    /* Tail-chain the next pend, but do NOT advance the SysTick: an exception
     * return consumes no modelled time, so time advances only on a full
     * instruction budget (below). This keeps a deferred tick from firing the
     * instant a PendSV context switch unstacks into the new thread. */
    (void)exc_take_pending(uc, vtor_base, false);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (emu_mve_nocp_take()) {
    /* MVE NoCP fault: the seam already did the access + advanced PC; resume, synchronous. */
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  /* UC_ERR_EXCEPTION from a PENDSVSET store is not a real fault: Unicorn's
   * internal ARM exception engine sees the pend bit in ICSR (PPB RAM) and
   * tries to take PendSV, which it cannot model for Cortex-M -- returning
   * UC_ERR_EXCEPTION.  Our ICSR write hook already set s_pendsv_stop, so
   * treat this as a normal context-switch boundary, not a fatal error. */
  if (err == UC_ERR_EXCEPTION && emu_exc_pendsv_stop()) {
    (void)exc_take_pending(uc, vtor_base, false);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (err != UC_ERR_OK) {
    *faulted = true;
    return k_inner_break;
  }
  if (emu_mpu_fault_pending()) {
    /* A store hit a read-only MPU region (on_mpu_ro_write stopped us).
     * Synthesise MemManage at this boundary -- no time advances (a fault is
     * synchronous), and the stacked PC is the faulting store. */
    emu_mpu_clear_fault();
    mpu_synth_memmanage(uc, vtor_base);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (emu_div0_fault_pending()) {
    /* A UDIV/SDIV by zero with CCR.DIV_0_TRP set (emulate_div0_patched stopped
     * us). Synthesise UsageFault at this boundary -- synchronous, no time
     * advances, and the stacked PC is the trapping divide. */
    emu_div0_clear_fault();
    div0_synth_usagefault(uc, vtor_base);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, run_pc);
    return k_inner_continue;
  }
  if (emu_exc_pendsv_stop()) {
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

bool run_inner(uc_engine* uc, uint32_t vtor_base, uint32_t* run_pc_io, uc_err* err_out)
{
  uint32_t run_pc  = *run_pc_io;
  uc_err   err     = UC_ERR_OK;
  bool     faulted = false;
  for (uint32_t inner = 0U; inner < (uint32_t)k_run_inner_max; inner++) {
    emu_exc_clear_pendsv_stop(); /* set by on_icsr_write iff this run ends on PENDSVSET */
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
