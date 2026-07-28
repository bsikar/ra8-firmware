/**
 * @file emu_idle.c
 * @brief Wait-for-interrupt idle-spin detector (see emu_exc.h)
 *
 * @details
 * Decides whether the guest PC is parked in a wait-for-interrupt spin so the
 * run loop can cap the idle chunk's instruction budget instead of burning a
 * full chunk on a loop that cannot make progress until an interrupt arrives.
 * Pure Thumb instruction decoding over a read-only view of guest memory: it
 * recognises the one-instruction forms (`b .`, `wfi`) directly and otherwise
 * walks forward to a backward `b.n` that brackets the PC, requiring a wait
 * signature inside the bracketed body so a tight busy loop is not mistaken
 * for an idle one.
 *
 * Split out of emu_exc.c: the detector shares none of the exception engine's
 * state, and both translation units are clearer for the separation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "emu_exc.h"
#include "emu_run.h"

/**
 * @brief Does the halfword at @p at close a tight idle loop around @p aligned?
 *
 * @details
 * Decodes an unconditional Thumb `b.n` and requires three things of it: the
 * branch goes backwards, its target brackets @p aligned within
 * @ref k_idle_loop_max bytes, and the bracketed body contains a wait signature
 * (`cpsie i` or `wfi`). A tight backward loop with no wait is a busy loop, not
 * an idle one, and must not be reported as idle.
 *
 * @param[in]  uc      Engine to read instruction memory from.
 * @param[in]  at      Address of the candidate branch halfword.
 * @param[in]  hw      The halfword already read at @p at.
 * @param[in]  aligned The PC under test, halfword-aligned.
 * @param[out] done    Set true when the answer is final either way.
 *
 * @return True when @p at closes an idle loop around @p aligned.
 *
 * @pre @p uc and @p done are non-NULL.
 * @pre @p hw is the halfword actually stored at @p at.
 * @post `*done` is true whenever the caller must stop scanning.
 * @post No judgement is returned unless the branch is unconditional `b.n`.
 *
 * @note Not thread-safe; the emulator is single-threaded host-side.
 */
static bool idle_back_edge(uc_engine* uc, uint32_t at, uint16_t hw, uint32_t aligned, bool* done)
{
  *done = false;
  if ((hw & (uint16_t)k_op_bn_mask) != (uint16_t)k_op_bn_base) {
    return false; /* not an unconditional b.n -- still inside the loop body */
  }
  *done = true;
  /* Unconditional b.n: target = at + 4 + sign_extend(imm11) * 2. */
  const uint32_t imm11 = (uint32_t)(hw & (uint16_t)k_op_bn_imm);
  const int32_t off = (int32_t)(imm11 << (uint32_t)k_bn_imm_sext_shl) >> (int32_t)k_bn_imm_sext_shr;
  if (off >= 0) {
    return false; /* forward branch -- not a spin-in-place back-edge */
  }
  const uint32_t target = at + (uint32_t)k_thumb2_insn_bytes + (uint32_t)off;
  if ((target > aligned) || ((aligned - target) > (uint32_t)k_idle_loop_max)) {
    return false; /* back-edge does not bracket pc in a tight loop */
  }
  /* pc is enclosed by [target, at]: require a wait (cpsie/wfi) in the body. */
  for (uint32_t k = target; k <= at; k += (uint32_t)k_thumb_hw_bytes) {
    uint16_t bhw = 0U;
    if (uc_mem_read(uc, (uint64_t)k, &bhw, sizeof(bhw)) != UC_ERR_OK) {
      return false;
    }
    if ((bhw == (uint16_t)k_op_cpsie_i) || (bhw == (uint16_t)k_op_wfi)) {
      return true;
    }
  }
  return false; /* tight loop but no wait signature -- a busy loop, not idle */
}

/**
 * @brief True if @p pc sits in a wait-for-interrupt spin (the core is idle).
 *
 * @details Reports whether the core at @p pc is parked in a loop that can only
 * make progress once an interrupt arrives -- genuine idle, where the next thing
 * that can happen is the periodic SysTick. Two cases are recognised:
 *   1. The instruction AT @p pc is itself a halt: `b .` (0xE7FE, branch-to-self)
 *      or `wfi` (0xBF30).
 *   2. @p pc is ENCLOSED by a wait-for-interrupt poll loop: scanning forward a
 *      few halfwords finds an unconditional backward `b.n` (the loop back-edge)
 *      whose target is at or before @p pc (so the loop wraps around @p pc), and
 *      the loop body holds a `wfi` or a `cpsie i` -- the "re-enable interrupts
 *      and poll" idiom ThreadX's __tx_ts_wait uses (cpsid/ldr/str/cbnz/cpsie/
 *      b .-N, spinning on execute_ptr until a tick makes a thread runnable).
 *
 * The enclosing-loop test is deliberately tight: it requires the back-edge to
 * bracket @p pc, so STRAIGHT-LINE code is never matched even when it sits in
 * memory next to an idle loop (an ISR returns via `bx lr`, not a backward
 * branch over itself -- matching a nearby opcode would wrongly truncate it).
 * A compute/busy loop is also excluded: it exits on a conditional branch and
 * never re-enables interrupts mid-loop, so it carries no wfi/cpsie wait. The
 * run loop uses this to cap the idle chunk's budget to ::k_idle_spin_insns
 * instead of spinning a full ::k_run_chunk_insns to reach the same already-armed
 * tick. Tick COUNT is unchanged; only idle wall-time is skipped.
 *
 * @param[in,out] uc Unicorn engine (instructions are read from its memory).
 * @param[in]     pc Program counter to inspect (Thumb bit ignored).
 * @return true if @p pc is on, or enclosed by, a wait-for-interrupt idle loop.
 *
 * @pre @p uc has the code region containing @p pc mapped.
 * @pre @p pc is halfword-aligned once the Thumb bit is cleared.
 * @post @p uc is unchanged (a read-only probe).
 * @note Detection only; advancing time stays the run loop's job, so the tick
 *       count -- and every tick-based sleep/heartbeat deadline -- is preserved.
 * @since 0.1.0
 */
bool idle_spin_at(uc_engine* uc, uint32_t pc)
{
  const uint32_t aligned = pc & ~1U;
  uint16_t       hw0     = 0U;
  if (uc_mem_read(uc, (uint64_t)aligned, &hw0, sizeof(hw0)) != UC_ERR_OK) {
    return false;
  }
  /* Case 1: the core is literally on a halt instruction. */
  if ((hw0 == (uint16_t)k_op_branch_self) || (hw0 == (uint16_t)k_op_wfi)) {
    return true;
  }

  /* Case 2: scan forward for the loop's unconditional backward back-edge. */
  for (uint32_t j = 0U; j < (uint32_t)k_idle_scan_fwd; j++) {
    const uint32_t at = aligned + (j * (uint32_t)k_thumb_hw_bytes);
    uint16_t       hw = 0U;
    if (uc_mem_read(uc, (uint64_t)at, &hw, sizeof(hw)) != UC_ERR_OK) {
      return false;
    }
    bool       done    = false;
    const bool verdict = idle_back_edge(uc, at, hw, aligned, &done);
    if (done) {
      return verdict;
    }
  }
  return false;
}
