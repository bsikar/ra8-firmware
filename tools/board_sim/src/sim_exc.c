/**
 * @file sim_exc.c
 * @brief Hand-modelled Cortex-M exception engine (see sim_exc.h)
 *
 * @details
 * Unicorn's Cortex-M33 core carries no NVIC / exception unit, so board_sim
 * takes exceptions by hand: the active-priority stack, the basic + FP frame
 * stacking/unstacking of exception entry/return, SysTick/PendSV/SVCall and
 * peripheral-IRQ activation with hardware priority rules, the EXC_RETURN
 * recognition, the SVC/BKPT/security-instruction INTR dispatch, the ICSR
 * PendSV promptness hook, the SCB control-word watcher (SYSRESETREQ warm
 * reboot + CCR.DIV_0_TRP arming), the NVIC ISER/ICER W1S/W1C folding, the
 * idle-spin detector and the DWT cycle-counter model. Moved verbatim out of
 * the board_sim main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_exc.h"

#include <stdio.h>

#include "board_periph.h"
#include "sim_console.h"
#include "sim_engine.h"
#include "sim_memmap.h"
#include "sim_seams.h"

/** @brief Thumb decode + idle-spin scan constants for this exception engine. */
typedef enum : uint32_t {
  k_op_branch_self  = 0xE7FEU, /**< Thumb "b ." (branch-to-self idle loop).     */
  k_op_wfi          = 0xBF30U, /**< Thumb `wfi` (wait-for-interrupt).           */
  k_op_cpsie_i      = 0xB662U, /**< Thumb `cpsie i` (re-enable IRQ in a poll).  */
  k_op_bn_mask      = 0xF800U, /**< Mask selecting a Thumb T2 `b.n` opcode.     */
  k_op_bn_base      = 0xE000U, /**< Thumb T2 unconditional `b.n` base value.    */
  k_op_bn_imm       = 0x07FFU, /**< Thumb T2 `b.n` imm11 field mask.            */
  k_bn_imm_sext_shl = 21U,     /**< Shift imm11 bit10 up to bit31 (sign bit).   */
  k_bn_imm_sext_shr = 20U,     /**< Arith >> sign-extends and scales imm by 2.  */
  k_idle_scan_fwd   = 8U,      /**< Halfwords scanned ahead for a loop edge.    */
  k_idle_loop_max   = 32U,     /**< Largest idle loop (bytes) that may hold PC. */
  k_thumb_op5_shift = 11U,     /**< op5 = hw0[15:11].                           */
  k_thumb_op5_mask  = 0x1FU,   /**< 5-bit op5 field.                            */
  k_thumb32_op5_min = 0x1DU,   /**< op5 >= this -> 32-bit instruction.          */
} sim_exc_decode_t;

static uint32_t s_systick_fires;

/* Hand-modelled Cortex-M exception state. Unicorn's M33 core has no NVIC /
 * exception unit, so board_sim takes SysTick / PendSV / SVCall by hand: it
 * tracks the active-exception priority stack here (everything else -- MSP/PSP/
 * CONTROL/xPSR/PRIMASK -- is read straight from Unicorn). s_exc_stack holds the
 * priority of each handler currently active so a higher-priority exception
 * (e.g. SysTick, prio 0x40) can pre-empt a lower one (PendSV, prio 0xFF) but
 * not vice-versa, exactly as the real priority logic would nest them. */
static uint32_t s_exc_stack[k_exc_nest_max]; /**< Active-handler priorities.     */
static uint32_t s_exc_depth;                 /**< Number of active handlers.     */
static uint32_t s_pendsv_takes;              /**< PendSV exceptions taken.       */
static uint32_t s_svc_takes;                 /**< SVCall exceptions taken.       */
static uint64_t s_exc_return_pc;             /**< Pending EXC_RETURN to unstack. */
static bool     s_exc_return_hit;            /**< An EXC_RETURN branch was seen. */
static bool     s_systick_pending;           /**< SysTick exception is pended.   */
static bool     s_pendsv_stop;               /**< Chunk ended on a PENDSVSET.    */
static bool     s_bkpt_hit;                  /**< Firmware executed a BKPT.      */
static uint32_t s_bkpt_pc;                   /**< PC of the BKPT that halted.    */

/** @brief Priority value (lower = higher) of the active handler, or sentinel. */
static uint32_t exc_active_prio(void)
{
  return (s_exc_depth == 0U) ? (uint32_t)k_exc_prio_none : s_exc_stack[s_exc_depth - 1U];
}

/**
 * @brief Read a system-handler priority byte from an SHPR register.
 *
 * @details
 * Cortex-M packs four 8-bit handler priorities per SHPRn word. SVCall (#11) is
 * byte 3 of SHPR2; PendSV (#14) is byte 2 and SysTick (#15) byte 3 of SHPR3.
 * ThreadX's tx_initialize_low_level programs these (SysTick 0x40, PendSV/SVC
 * 0xFF), and the value drives whether one exception may pre-empt another. The
 * PPB is plain RAM here, so the firmware's stores are simply read back.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception/vector number (11, 14, or 15).
 * @return The 8-bit configured priority (0 = highest, 0xFF = lowest).
 * @retval 0xFF when @p exc_num is not one of the modelled system handlers.
 *
 * @pre @p uc is an initialised engine with the PPB mapped as RAM.
 * @pre SystemInit / tx_initialize_low_level have programmed SHPR2/SHPR3.
 * @post No register or memory state is modified (read-only).
 * @post The returned value is in [0, 0xFF].
 * @note Sub-priority / priority grouping is ignored -- only the raw byte is
 *       compared, which is sufficient for the SysTick > PendSV nesting ThreadX
 *       relies on.
 * @since 0.1.0
 */
static uint32_t exc_priority(uc_engine* uc, uint32_t exc_num)
{
  if (exc_num == (uint32_t)k_exc_svcall) {
    return (rd32(uc, (uint64_t)k_scb_shpr2) >> (3U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  if (exc_num == (uint32_t)k_exc_pendsv) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (2U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  if (exc_num == (uint32_t)k_exc_systick) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (3U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  return (uint32_t)k_exc_prio_max;
}

/** @brief Stack the basic {R0-R3,R12,LR,PC,xPSR} frame + optional FP frame at @p sp. */
static void exc_stack_frame(uc_engine* uc, uint32_t sp, bool fp_active, uint32_t frame_xpsr)
{
  wr32(uc, (uint64_t)sp + 0U, reg_get(uc, UC_ARM_REG_R0));
  wr32(uc, (uint64_t)sp + 4U, reg_get(uc, UC_ARM_REG_R1));
  wr32(uc, (uint64_t)sp + 8U, reg_get(uc, UC_ARM_REG_R2));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_r3, reg_get(uc, UC_ARM_REG_R3));
  wr32(uc, (uint64_t)sp + 16U, reg_get(uc, UC_ARM_REG_R12));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_lr, reg_get(uc, UC_ARM_REG_LR));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_pc, reg_get(uc, UC_ARM_REG_PC));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_xpsr, frame_xpsr);

  /* Armv8-M FP extended frame: S0-S15 + FPSCR sit directly above the 8-word
   * basic frame (ThreadX's PendSV adds S16-S31 on top of this). */
  if (fp_active) {
    for (uint32_t i = 0U; i < (uint32_t)k_fp_s_words; i++) {
      wr32(uc,
           (uint64_t)sp + (uint64_t)k_frame_off_s0 + (uint64_t)(i * (uint32_t)k_word_bytes),
           reg_get(uc, UC_ARM_REG_S0 + (int)i));
    }
    wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_fpscr, reg_get(uc, UC_ARM_REG_FPSCR));
  }
}

/** @brief Select the EXC_RETURN magic for the outgoing mode/stack/FP state. */
static uint32_t exc_return_value(bool in_thread, bool use_psp, bool fp_active)
{
  /* EXC_RETURN encodes where to unstack: Thread/PSP, Thread/MSP, or (when an
   * exception pre-empts another) Handler/MSP. */
  uint32_t exc_ret;
  if (!in_thread) {
    exc_ret = (uint32_t)k_exc_ret_handler;
  } else if (use_psp) {
    exc_ret = (uint32_t)k_exc_ret_psp;
  } else {
    exc_ret = (uint32_t)k_exc_ret_msp;
  }
  if (fp_active) {
    exc_ret &= ~(uint32_t)k_exc_ret_ftype; /* FType=0: an FP frame was stacked. */
  }
  return exc_ret;
}

/**
 * @brief Enter a Cortex-M exception: stack the basic frame and vector in.
 *
 * @details
 * Reproduces Armv7E-M / Armv8-M exception entry that Unicorn's core does not
 * model. The active stack is chosen exactly as hardware would: PSP when in
 * Thread mode with CONTROL.SPSEL set, else MSP. The 8-word basic frame
 * {R0,R1,R2,R3,R12,LR,PC,xPSR} is pushed with 8-byte alignment (the realign
 * pad is recorded in the stacked xPSR bit 9 so exit can undo it), the banked SP
 * is updated, the core is switched to Handler mode on MSP, LR is loaded with
 * the matching EXC_RETURN, IPSR is set to @p exc_num, and PC is vectored to the
 * handler fetched from the VTOR-relative table. The handler's priority is
 * pushed on the active-exception stack so nesting respects priority.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception number to take (11, 14, or 15).
 * @param[in]     handler Handler entry address (Thumb bit ignored).
 * @return Nothing.
 *
 * @pre @p uc has MSP/PSP/CONTROL/xPSR readable and the target stack mapped.
 * @pre Taking @p exc_num is permitted now (priority/PRIMASK already checked).
 * @post The core is in Handler mode (IPSR == @p exc_num) running on MSP.
 * @post LR holds a valid EXC_RETURN and the outgoing frame is on the old stack.
 * @note When CONTROL.FPCA is set, the FP extended frame (S0-S15 + FPSCR) is
 *       stacked above the basic frame and EXC_RETURN bit4 (FType) is cleared.
 * @since 0.1.0
 */
void exc_enter(uc_engine* uc, uint32_t exc_num, uint32_t handler)
{
  const uint32_t xpsr_in   = reg_get(uc, UC_ARM_REG_XPSR);
  const uint32_t control   = reg_get(uc, UC_ARM_REG_CONTROL);
  const bool     in_thread = (xpsr_in & (uint32_t)k_xpsr_ipsr_mask) == 0U;
  const bool     use_psp   = in_thread && ((control & (uint32_t)k_control_spsel) != 0U);
  const bool     fp_active = (control & (uint32_t)k_control_fpca) != 0U;

  const int sp_reg = use_psp ? UC_ARM_REG_PSP : UC_ARM_REG_MSP;
  uint32_t  sp     = reg_get(uc, sp_reg);

  /* Hardware aligns the stack pointer to 8 bytes on entry and flags the pad in
   * the stacked xPSR (bit 9) so the matching exception return can remove it. */
  uint32_t frame_xpsr = xpsr_in;
  if ((sp & 0x4U) != 0U) {
    sp -= 4U;
    frame_xpsr |= (uint32_t)k_xpsr_align9;
  } else {
    frame_xpsr &= ~(uint32_t)k_xpsr_align9;
  }
  sp -= (uint32_t)k_exc_frame_bytes;
  if (fp_active) {
    sp -= (uint32_t)k_fp_frame_extra;
  }

  exc_stack_frame(uc, sp, fp_active, frame_xpsr);

  /* Commit the new value of whichever stack the frame went onto. */
  reg_set(uc, sp_reg, sp);

  const uint32_t exc_ret = exc_return_value(in_thread, use_psp, fp_active);

  /* Handler mode always runs on MSP with CONTROL.SPSEL clear. */
  reg_set(uc, UC_ARM_REG_CONTROL, control & ~(uint32_t)k_control_spsel);
  reg_set(uc, UC_ARM_REG_SP, reg_get(uc, UC_ARM_REG_MSP));

  uint32_t handler_xpsr =
    (xpsr_in & ~(uint32_t)k_xpsr_ipsr_mask) | (exc_num & (uint32_t)k_xpsr_ipsr_mask);
  handler_xpsr |= (uint32_t)k_xpsr_t_bit; /* M-profile is always Thumb. */
  reg_set(uc, UC_ARM_REG_XPSR, handler_xpsr);
  reg_set(uc, UC_ARM_REG_LR, exc_ret);
  reg_set(uc, UC_ARM_REG_PC, handler & ~1U);

  if (s_exc_depth < (uint32_t)k_exc_nest_max) {
    s_exc_stack[s_exc_depth] = exc_priority(uc, exc_num);
    s_exc_depth++;
  }
}

/** @brief Restore the Armv8-M FP extended frame (S0-S15 + FPSCR) from @p sp. */
static void exc_restore_fp_frame(uc_engine* uc, uint32_t sp)
{
  /* Armv8-M FP extended frame (EXC_RETURN bit4 clear): S0-S15 + FPSCR sit above
   * the basic frame. Restore them so the thread's scalar FP state survives.
   * PC/xPSR keep their basic-frame offsets (accounted for by the caller). */
  for (uint32_t i = 0U; i < (uint32_t)k_fp_s_words; i++) {
    reg_set(
      uc,
      UC_ARM_REG_S0 + (int)i,
      rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_s0 + (uint64_t)(i * (uint32_t)k_word_bytes)));
  }
  reg_set(uc, UC_ARM_REG_FPSCR, rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_fpscr));
}

/** @brief Restore CONTROL.SPSEL + xPSR/IPSR for the returned-to context. */
static void exc_restore_mode(uc_engine* uc, uint32_t xpsr, bool to_thread, bool to_psp)
{
  /* Restore mode: on return to Thread, CONTROL.SPSEL follows EXC_RETURN bit2;
   * on return to a pre-empted handler the core stays on MSP. */
  uint32_t control = reg_get(uc, UC_ARM_REG_CONTROL);
  if (to_thread && to_psp) {
    control |= (uint32_t)k_control_spsel;
  } else {
    control &= ~(uint32_t)k_control_spsel;
  }
  reg_set(uc, UC_ARM_REG_CONTROL, control);

  uint32_t new_xpsr = xpsr | (uint32_t)k_xpsr_t_bit;
  if (to_thread) {
    new_xpsr &= ~(uint32_t)k_xpsr_ipsr_mask; /* Thread mode: IPSR == 0 */
  }
  reg_set(uc, UC_ARM_REG_XPSR, new_xpsr);
}

/**
 * @brief Perform a Cortex-M exception return for an observed EXC_RETURN branch.
 *
 * @details
 * The inverse of ::exc_enter. @p exc_ret (the magic value the core branched
 * to) selects the stack to unstack from (bit2: PSP vs MSP) and the mode to
 * return to (bit3: Thread vs Handler). The 8-word basic frame is popped (plus
 * the S0-S15 + FPSCR words when FType, bit4, is clear), the recorded 8-byte
 * realignment (stacked xPSR bit 9) is undone, the banked SP and CONTROL.SPSEL
 * are restored, xPSR (hence IPSR) is reloaded, the active-exception stack is
 * popped, and PC resumes the interrupted instruction stream.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_ret The EXC_RETURN value (prefix bits[31:7] set) returned to.
 * @return Nothing.
 *
 * @pre @p uc is in Handler mode with a valid basic frame on the indicated stack.
 * @pre @p exc_ret has the EXC_RETURN prefix (bits[31:7] all set).
 * @post The core has resumed the unstacked context (PC/SP/xPSR restored).
 * @post The active-exception nesting depth has decreased by one (if non-zero).
 * @note When FType (bit4) is clear, the FP extended frame (S0-S15 + FPSCR) is
 *       unstacked too, matching ::exc_enter.
 * @since 0.1.0
 */
void exc_return(uc_engine* uc, uint32_t exc_ret)
{
  const bool to_psp    = (exc_ret & (uint32_t)k_exc_ret_spsel) != 0U;
  const bool to_thread = (exc_ret & (uint32_t)k_exc_ret_mode) != 0U;
  const bool fp_frame  = (exc_ret & (uint32_t)k_exc_ret_ftype) == 0U;
  const int  sp_reg    = to_psp ? UC_ARM_REG_PSP : UC_ARM_REG_MSP;
  uint32_t   sp        = reg_get(uc, sp_reg);

  const uint32_t r0   = rd32(uc, (uint64_t)sp + 0U);
  const uint32_t r1   = rd32(uc, (uint64_t)sp + 4U);
  const uint32_t r2   = rd32(uc, (uint64_t)sp + 8U);
  const uint32_t r3   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_r3);
  const uint32_t r12  = rd32(uc, (uint64_t)sp + 16U);
  const uint32_t lr   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_lr);
  const uint32_t pc   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_pc);
  const uint32_t xpsr = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_xpsr);

  if (fp_frame) {
    exc_restore_fp_frame(uc, sp);
  }

  sp += (uint32_t)k_exc_frame_bytes;
  if (fp_frame) {
    sp += (uint32_t)k_fp_frame_extra;
  }
  if ((xpsr & (uint32_t)k_xpsr_align9) != 0U) {
    sp += 4U; /* undo the entry-time 8-byte realignment pad */
  }
  reg_set(uc, sp_reg, sp);

  reg_set(uc, UC_ARM_REG_R0, r0);
  reg_set(uc, UC_ARM_REG_R1, r1);
  reg_set(uc, UC_ARM_REG_R2, r2);
  reg_set(uc, UC_ARM_REG_R3, r3);
  reg_set(uc, UC_ARM_REG_R12, r12);
  reg_set(uc, UC_ARM_REG_LR, lr);

  exc_restore_mode(uc, xpsr, to_thread, to_psp);

  /* Active SP becomes whichever stack the returned-to context uses. */
  reg_set(uc, UC_ARM_REG_SP, reg_get(uc, to_psp && to_thread ? UC_ARM_REG_PSP : UC_ARM_REG_MSP));
  reg_set(uc, UC_ARM_REG_PC, pc & ~1U);

  if (s_exc_depth > 0U) {
    s_exc_depth--;
  }
}

/**
 * @brief True if @p pc is an EXC_RETURN magic value.
 *
 * @details Matches the Armv8-M EXC_RETURN prefix -- bits[31:7] all set
 * (0xFFFFFF80..0xFFFFFFFF). This covers both the Armv7-M values board_sim
 * itself generates (0xFFFFFFF1/F9/FD) and the Armv8-M Non-Secure thread
 * returns ThreadX uses (0xFFFFFFBC basic, 0xFFFFFFAC with an FP frame), where
 * bit6 (S) is clear. Nothing in this firmware's map executes at 0xFFFFFFxx, so
 * a fetch into that range is always an exception return, never a real branch.
 */
static bool is_exc_return(uint64_t pc)
{
  return (pc & (uint64_t)k_exc_ret_v8_mask) == (uint64_t)k_exc_ret_v8_mask;
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
    if ((hw & (uint16_t)k_op_bn_mask) != (uint16_t)k_op_bn_base) {
      continue; /* not an unconditional b.n -- still inside the loop body */
    }
    /* Unconditional b.n: target = at + 4 + sign_extend(imm11) * 2. */
    const uint32_t imm11 = (uint32_t)(hw & (uint16_t)k_op_bn_imm);
    const int32_t  off =
      (int32_t)(imm11 << (uint32_t)k_bn_imm_sext_shl) >> (int32_t)k_bn_imm_sext_shr;
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
  return false;
}

/**
 * @brief Read the handler address for an exception from the vector table.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base used when VTOR reads as 0.
 * @param[in]     exc_num   Exception/vector index to look up.
 * @return Handler entry address with the Thumb bit cleared.
 * @retval 0 when no usable handler is installed at that vector slot.
 *
 * @pre @p uc has the vector table mapped at VTOR (or @p vtor_base).
 * @pre @p exc_num is a valid vector index (< table length).
 * @post No engine state is modified (read-only).
 * @post The returned address (when non-zero) is halfword-aligned code.
 * @note VTOR lives in PPB RAM here, written by SystemInit at boot.
 * @since 0.1.0
 */
uint32_t exc_vector(uc_engine* uc, uint32_t vtor_base, uint32_t exc_num)
{
  uint32_t vtor = rd32(uc, (uint64_t)k_scb_vtor);
  if (vtor == 0U) {
    vtor = vtor_base;
  }
  const uint32_t handler = rd32(uc, (uint64_t)vtor + ((uint64_t)exc_num * 4U)) & ~1U;
  if ((handler == 0U) || (handler == (uint32_t)k_vector_erased)) {
    return 0U;
  }
  return handler;
}

/**
 * @brief Take one pending peripheral NVIC IRQ the ICU has queued, if allowed.
 *
 * @details
 * The peripheral counterpart to the SysTick / PendSV logic in
 * ::exc_take_pending. board_periph's ICU model queues an IRQ whenever a
 * peripheral event is event-linked through IELSR and its NVIC line is enabled;
 * this pops one and -- if its NVIC priority (IPR byte, top nibble used) outranks
 * the active execution priority -- vectors it in as a real Cortex-M exception
 * (vector 16 + IRQn read from VTOR), exactly the path a hardware IRQ takes. The
 * ISR therefore runs in genuine handler context and returns via the same
 * EXC_RETURN unstack as every other exception. The matching ISPR pending bit is
 * cleared on activation, as hardware does.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base when VTOR reads as 0.
 * @param[in]     active    Current active-handler priority (sentinel if none).
 * @return true if a peripheral IRQ was taken (PC now points at its ISR).
 *
 * @pre @p uc has stopped at an instruction boundary; PRIMASK already checked.
 * @post At most one IRQ is taken; its ISPR pending bit is cleared if so.
 * @note If no handler is installed at the vector, the IRQ is dropped, not spun.
 * @since 0.1.0
 */
static bool exc_take_periph_irq(uc_engine* uc, uint32_t vtor_base, uint32_t active)
{
  uint32_t irq = 0U;
  if (!board_periph_next_irq(&irq)) {
    return false;
  }
  const uint8_t  prio_byte = (uint8_t)rd32(uc, (uint64_t)k_nvic_ipr_base + irq);
  const uint32_t prio =
    (uint32_t)(prio_byte >> (uint32_t)k_nvic_prio_shift) & (uint32_t)k_lo4_mask; /* 4 MSBs used */
  if (prio >= active) {
    return false; /* an equal/higher-priority handler is active -- defer */
  }
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_irq_vec0 + irq);
  if (handler == 0U) {
    return false; /* no ISR installed: drop (default handler would just return) */
  }
  /* Clear the NVIC ISPR pending bit on activation, as hardware does. */
  const uint64_t ispr_word = (uint64_t)k_nvic_ispr_base + ((uint64_t)(irq / 32U) * 4U);
  uint32_t       ispr      = rd32(uc, ispr_word);
  ispr &= ~(1U << (irq % 32U));
  wr32(uc, ispr_word, ispr);

  exc_enter(uc, (uint32_t)k_exc_irq_vec0 + irq, handler);
  board_periph_note_irq_taken(irq);
  return true;
}

/**
 * @brief Take the highest-priority pending exception, if one may activate now.
 *
 * @details
 * The software replacement for the NVIC's "take the highest-priority pending,
 * enabled exception whose priority is greater than the current execution
 * priority" rule -- called at every instruction boundary AND immediately after
 * each exception return (so a lower-priority pend tail-chains exactly as
 * hardware would instead of returning to the interrupted code first). Two
 * sources are modelled:
 *
 *   - SysTick (#15): periodic. ::s_systick_pending is armed once per tick
 *     period by the run loop; this routine consumes it and vectors in #15 so
 *     _tx_timer_interrupt runs in real handler context (correct for ThreadX and
 *     for bare-metal SysTick handlers alike). Arming it elsewhere -- rather than
 *     re-deriving "armed" from SYST_CSR on every call -- is what lets a pending
 *     PendSV run between ticks instead of being starved by a perpetual SysTick.
 *   - PendSV (#14): level-pending via ICSR.PENDSVSET (ThreadX's context-switch
 *     request); the bit is cleared on activation, as hardware does.
 *
 * SysTick (priority 0x40) outranks PendSV (0xFF), so when both are pending
 * SysTick activates first and may even pre-empt a PendSV that is spinning in
 * its idle wait -- exactly the nesting ThreadX relies on to make a sleeping
 * thread runnable. PRIMASK and the active-priority stack are both honoured.
 *
 * @param[in,out] uc            Unicorn engine.
 * @param[in]     vtor_base     Fallback vector base if VTOR reads as 0.
 * @param[in]     allow_systick When false, the armed SysTick is left pending
 *                              (not taken) so modelled time does NOT advance.
 *                              The run loop passes false on a context-switch
 *                              stop (a PENDSVSET write consumes no time), so a
 *                              thread that just suspended on a tick-based wait
 *                              is not woken before lower-priority ready threads
 *                              run. Passes true on a full-budget boundary,
 *                              where genuine execution (or an idle spin) has
 *                              elapsed a tick's worth of time.
 * @return true if an exception was taken (PC now points at a handler).
 *
 * @pre @p uc has stopped at an instruction boundary or just returned.
 * @pre The PPB (SYST_CSR / ICSR / SHPRn / VTOR) is mapped as RAM.
 * @post At most one exception is taken per call (the highest-priority due one).
 * @post ICSR.PENDSVSET / ::s_systick_pending is cleared iff that one was taken.
 * @note SysTick is dropped (not queued) if SYST_CSR is disarmed when its period
 *       elapses, matching a masked/disabled SysTick on hardware.
 * @since 0.1.0
 */
bool exc_take_pending(uc_engine* uc, uint32_t vtor_base, bool allow_systick)
{
  const uint32_t primask = reg_get(uc, UC_ARM_REG_PRIMASK);
  if ((primask & 1U) != 0U) {
    return false; /* interrupts masked -- no exception may be taken now */
  }
  const uint32_t active = exc_active_prio();

  /* SysTick first: highest-priority of the modelled exceptions, so it can
   * pre-empt a lower-priority PendSV that is spinning for a runnable thread.
   * Skipped on a context-switch stop (allow_systick == false) so the tick does
   * not advance while ready threads still have work to run. */
  if (allow_systick && s_systick_pending) {
    const bool armed =
      (rd32(uc, (uint64_t)k_syst_csr) & (uint32_t)k_syst_csr_run) == (uint32_t)k_syst_csr_run;
    if (!armed) {
      s_systick_pending = false; /* disabled SysTick: drop the pended tick */
    } else if (exc_priority(uc, (uint32_t)k_exc_systick) < active) {
      const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_systick);
      if (handler != 0U) {
        s_systick_pending = false;
        exc_enter(uc, (uint32_t)k_exc_systick, handler);
        s_systick_fires++;
        return true;
      }
    }
  }

  /* PendSV: taken when the firmware has requested a context switch. */
  uint32_t icsr = rd32(uc, (uint64_t)k_scb_icsr);
  if ((icsr & (1U << (uint32_t)k_icsr_pendsvset)) != 0U) {
    if (exc_priority(uc, (uint32_t)k_exc_pendsv) < active) {
      const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_pendsv);
      if (handler != 0U) {
        /* Hardware clears PENDSVSET when PendSV is activated. */
        icsr &= ~(1U << (uint32_t)k_icsr_pendsvset);
        wr32(uc, (uint64_t)k_scb_icsr, icsr);
        exc_enter(uc, (uint32_t)k_exc_pendsv, handler);
        s_pendsv_takes++;
        return true;
      }
    }
  }

  /* Peripheral NVIC IRQs queued by the ICU model (timer overflow / underflow
   * routed through IELSR). Taken last among the modelled exceptions but via the
   * identical real entry/return path, with NVIC IPR priority honoured. */
  return exc_take_periph_irq(uc, vtor_base, active);
}

/** @brief Hook fired on access to unmapped memory (peripheral surface gap). */
static bool
on_unmapped(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)size;
  (void)value;
  (void)user;
  /* A FETCH into the EXC_RETURN range is not a fault -- it is the core taking
   * an exception return (the handler ran "BX lr" with an EXC_RETURN magic in
   * LR). Capture it and stop cleanly so the run loop can unstack the frame and
   * resume; the Unicorn fetch-fault error this produces is expected/handled. */
  if ((type == UC_MEM_FETCH_UNMAPPED) && is_exc_return(addr)) {
    s_exc_return_pc  = addr;
    s_exc_return_hit = true;
    (void)uc_emu_stop(uc);
    return false;
  }
  (void)fprintf(stderr,
                "  UNMAPPED %s @ 0x%08llX (extend the memory/peripheral map)\n",
                (type == UC_MEM_READ_UNMAPPED)    ? "read"
                : (type == UC_MEM_WRITE_UNMAPPED) ? "write"
                                                  : "fetch",
                (unsigned long long)addr);
  return false; /* stop emulation and report */
}

/**
 * @brief Model the Armv8-M Security-Extension opcodes Unicorn's M33 lacks.
 *
 * @details
 * board_sim is a single flat (no Secure/Non-Secure split) address space, so
 * these reduce to their plain effects here:
 *   - `SG` (secure gateway, 32-bit): a NOP -- the following B.W reaches the
 *     __acle_se_ entry directly.
 *   - `VSTR FPCXTNS,[sp,#-4]!` / `VLDR FPCXTNS,[sp],#4`: the FP context across
 *     the security boundary is meaningless with one FP bank, so model only the
 *     stack push/pop they perform (keeping SP balanced for the C frame).
 * Without this the unrecognised opcode is mis-taken as an `svc`, vectors to
 * Default_Handler's bkpt, and re-traps forever until the stack underflows (the
 * tz_nsc_cgc_usb fault). On a match PC (and SP) are advanced and emulation is
 * stopped so the run loop relaunches from the next instruction.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   PC of the trapping instruction.
 * @param[in]     insn The 32-bit opcode word read at @p pc.
 * @return true if @p insn was an SG / FPCXTNS opcode and was handled.
 */
static bool on_intr_sec_insn(uc_engine* uc, uint32_t pc, uint32_t insn)
{
  if (insn == (uint32_t)k_armv8m_sg_opcode) {
    const uint32_t next = pc + (uint32_t)k_thumb2_insn_bytes;
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    (void)uc_emu_stop(uc);
    return true;
  }
  if ((insn == (uint32_t)k_fpcxtns_push) || (insn == (uint32_t)k_fpcxtns_pop)) {
    uint32_t sp = 0U;
    (void)uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    if (insn == (uint32_t)k_fpcxtns_push) {
      sp -= (uint32_t)k_word_bytes;
      const uint32_t zero = 0U;
      (void)uc_mem_write(uc, (uint64_t)sp, &zero, sizeof(zero));
    } else {
      sp += (uint32_t)k_word_bytes;
    }
    const uint32_t next = pc + (uint32_t)k_thumb2_insn_bytes;
    (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    (void)uc_emu_stop(uc);
    return true;
  }
  return false;
}

/**
 * @brief Model a firmware `BKPT` as a halt (record the site and stop).
 *
 * @details A `BKPT` (0xBExx) is a deliberate firmware trap -- Default_Handler's
 * `bkpt #0`, a failed assert, or a fault give-up. It is NOT an `svc`: taking
 * SVCall here would stack a frame, vector to the (often Default_Handler) SVC
 * slot, return to the same bkpt, and re-trap forever -- the stack grows until
 * it underflows (the historical tz_nsc_cgc_usb storm). Model it as a halt:
 * record the site and stop so the run loop ends and the report shows where the
 * firmware trapped.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   PC of the trapping instruction.
 * @param[in]     insn The 32-bit opcode word read at @p pc.
 * @return true if @p insn was a BKPT and the core was halted.
 */
static bool on_intr_bkpt(uc_engine* uc, uint32_t pc, uint32_t insn)
{
  if (((uint16_t)(insn & (uint32_t)k_lo16_mask) & (uint16_t)k_bkpt_hw_mask) ==
      (uint16_t)k_bkpt_hw_base) {
    s_bkpt_hit = true;
    s_bkpt_pc  = pc;
    (void)uc_emu_stop(uc);
    return true;
  }
  return false;
}

/**
 * @brief UC_HOOK_INTR handler: take the SVCall exception on an `svc` opcode.
 *
 * @details
 * Unicorn raises UC_HOOK_INTR when the firmware executes the Thumb `svc`
 * instruction but, lacking an exception unit, does not vector it. This models
 * SVCall (#11): the basic frame is stacked and the core vectors to SVC_Handler
 * via ::exc_enter, then emulation is stopped so the chunked run loop relaunches
 * cleanly from the handler entry (editing PC mid-block and continuing corrupts
 * Unicorn's block/Thumb state -- the same stop-then-relaunch contract the touch
 * and conditional-select stubs use). ThreadX in single-mode never issues an
 * SVC, but bare-metal / future RTOS paths that start the first thread via `svc`
 * are handled correctly here. PRIMASK does not mask SVCall (it is synchronous),
 * matching hardware.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     int_no    Interrupt/exception number reported by Unicorn.
 * @param[in]     user_data Hook user pointer (unused; signature fixed by Unicorn).
 * @return Nothing.
 *
 * @pre @p uc has just executed an `svc` instruction or branched to EXC_RETURN.
 * @pre The vector table (at VTOR or the MRAM fallback) holds SVC_Handler.
 * @post Either an exception was taken/returned (PC updated) or, on a missing
 *       SVC handler, the core is left untouched.
 * @post Emulation is stopped so the run loop resumes from the new PC.
 * @note Only the SVC interrupt class is acted on; other int_no values are
 *       ignored so unrelated traps fall through.
 * @since 0.1.0
 */
static void on_intr(uc_engine* uc, uint32_t int_no, void* user_data)
{
  (void)int_no;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);

  /* Cortex-M exception RETURN. Unicorn's M-profile core does not pop the
   * exception frame itself, but it DOES trap a branch to an EXC_RETURN magic
   * (0xFFFFFFFx) by raising an interrupt with the magic left in PC (Thumb bit
   * masked off, but the stack/mode selector bits 2/3 intact). Unstack the basic
   * frame and resume the interrupted context. This is how every handler that
   * board_sim vectors in (SysTick / PendSV / SVCall) returns. */
  if (is_exc_return((uint64_t)pc)) {
    s_exc_return_pc  = (uint64_t)pc;
    s_exc_return_hit = true;
    (void)uc_emu_stop(uc);
    return;
  }

  /* Armv8-M secure gateway: every Non-Secure-Callable veneer starts with `SG`
   * (0xE97FE97F) then `B.W __acle_se_<fn>`. Unicorn's M33 has no Security
   * Extension, so it raises INTR on the unrecognised SG instead of switching to
   * Secure state. board_sim has a single flat address space (no S/NS split), so
   * the faithful model is to treat SG as a NOP and let the following branch reach
   * the secure entry directly. Without this the SG is mis-taken as an `svc` and
   * re-taken forever (the firmware has no SVC), looping until the stack
   * underflows -- the tz_nsc_cgc_usb fault. The matching `BXNS`/`BLXNS` returns
   * are handled below. */
  uint32_t insn = 0U;
  (void)uc_mem_read(uc, (uint64_t)pc, &insn, sizeof(insn));

  /* Armv8-M Security Extension instructions (SG / FPCXTNS) reduce to their plain
   * effects in this single-domain model; a BKPT is a deliberate firmware halt. */
  if (on_intr_sec_insn(uc, pc, insn)) {
    return;
  }
  if (on_intr_bkpt(uc, pc, insn)) {
    return;
  }

  /* Otherwise it is a synchronous `svc` -- take SVCall (#11). ThreadX in single
   * mode never issues one, but bare-metal / future RTOS first-thread-start
   * paths do. The VTOR fallback is the MRAM vector-table base; the live VTOR
   * (set by SystemInit) is read inside exc_vector. */
  (void)user_data;
  const uint32_t vtor_base = (uint32_t)sim_memmap_mram_base();
  const uint32_t handler   = exc_vector(uc, vtor_base, (uint32_t)k_exc_svcall);
  if (handler != 0U) {
    exc_enter(uc, (uint32_t)k_exc_svcall, handler);
    s_svc_takes++;
  }
  (void)uc_emu_stop(uc);
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for SCB ICSR -- take PendSV promptly.
 *
 * @details
 * On hardware, writing ICSR.PENDSVSET pends PendSV, and -- because ThreadX
 * follows the store with DSB+ISB and PendSV is enabled at a priority above
 * thread level with interrupts unmasked -- the exception activates at the very
 * next instruction. board_sim runs the CPU in long chunks, so without help it
 * would only notice PENDSVSET at the end of a 500k-instruction chunk; by then
 * the requesting thread (e.g. a thread suspending inside tx_queue_receive) has
 * run far past the point where it expected to be switched out and observes
 * inconsistent scheduler state. Stopping the chunk the instant PENDSVSET is
 * written hands control straight back to the run loop, which takes PendSV at
 * that boundary -- restoring next-instruction activation semantics. The store
 * itself has already landed in PPB RAM, so exc_take_pending sees the bit set.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Memory access type (write); unused.
 * @param[in]     addr  Faulting/observed address (the ICSR word).
 * @param[in]     size  Access width in bytes; unused.
 * @param[in]     value The value being written to ICSR.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre @p uc is mid-chunk executing the store to ICSR.
 * @pre The hook is registered for the 4-byte ICSR word only.
 * @post Emulation is stopped iff the write sets PENDSVSET.
 * @post The PENDSVSET bit is left in PPB RAM for exc_take_pending to read.
 * @note PENDSVCLR / status-only writes do not stop the chunk.
 * @since 0.1.0
 */
static void
on_icsr_write(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  if (((uint32_t)value & (1U << (uint32_t)k_icsr_pendsvset)) == 0U) {
    return; /* not a PendSV request (e.g. PENDSVCLR / status write) */
  }
  /* Only end the chunk when PendSV could actually activate now: interrupts
   * unmasked and no equal/higher-priority handler already running. If masked
   * (the store sits inside a ThreadX critical section), do NOT stop -- the run
   * loop would otherwise relaunch from this same store and re-pend forever,
   * since uc_emu_stop here leaves PC on the store. With those guards, the run
   * loop's exc_take_pending takes PendSV and moves PC to the handler, so the
   * store is never re-executed. The masked case is picked up at the next
   * boundary once TX_RESTORE re-enables interrupts. */
  uint32_t primask = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PRIMASK, &primask);
  if ((primask & 1U) != 0U) {
    return;
  }
  if (exc_priority(uc, (uint32_t)k_exc_pendsv) >= exc_active_prio()) {
    return; /* a higher/equal-priority handler is active -- defer */
  }
  /* Advance PC past the storing instruction before stopping so the PendSV we
   * are about to take stacks the return address of the NEXT instruction -- as
   * real hardware does (PendSV activates after the store retires), not the store
   * itself. Re-stacking the store would re-pend PendSV on every return and spin.
   * Thumb length: a halfword whose top 5 bits are 0b111xx with xx != 00 starts a
   * 32-bit instruction; otherwise it is 16-bit. */
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint16_t hw0 = 0U;
  (void)uc_mem_read(uc, pc, &hw0, sizeof(hw0));
  const uint32_t op5  = (uint32_t)(hw0 >> (uint32_t)k_thumb_op5_shift) & (uint32_t)k_thumb_op5_mask;
  const uint32_t step = (op5 >= (uint32_t)k_thumb32_op5_min) ? 4U : 2U;
  uint32_t       next = pc + step;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  /* Mark this as a context-switch stop so the run loop takes PendSV WITHOUT
   * advancing the SysTick: a context switch consumes no modelled time, so a
   * thread that suspends (e.g. tx_thread_sleep) must not have its tick-based
   * wait expired the instant it yields -- that would starve lower-priority
   * ready threads. Time (SysTick) advances only on a full-budget run (genuine
   * execution / idle spin). See the run loop's inner dispatch. */
  s_pendsv_stop = true;
  (void)uc_emu_stop(uc);
}

/** @brief Implementation of `sim_exc_install_core()` -- unmapped/INTR/ICSR hooks. */
void sim_exc_install_core(uc_engine* uc)
{
  static uc_hook s_h_unmapped;
  static uc_hook s_h_intr;
  static uc_hook s_h_icsr;
  (void)uc_hook_add(uc, &s_h_unmapped, UC_HOOK_MEM_UNMAPPED, (void*)on_unmapped, nullptr, 1, 0);
  /* SVCall / exception-return: Unicorn raises UC_HOOK_INTR on a Thumb `svc` and
   * on a branch to an EXC_RETURN magic; on_intr vectors / unstacks accordingly. */
  (void)uc_hook_add(uc, &s_h_intr, UC_HOOK_INTR, (void*)on_intr, nullptr, 1, 0);
  /* Watch the ICSR word so a PENDSVSET store ends the chunk at once, giving
   * PendSV next-instruction activation (see on_icsr_write). ICSR lives in PPB
   * RAM, so this memory-write hook is the only way to observe the request. */
  (void)uc_hook_add(uc,
                    &s_h_icsr,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_icsr_write,
                    nullptr,
                    (uint64_t)k_scb_icsr,
                    (uint64_t)k_scb_icsr + 3U);
}

/** @brief Implementation of `sim_exc_arm_systick()` -- pend the periodic tick. */
void sim_exc_arm_systick(void)
{
  s_systick_pending = true;
}

/** @brief Implementation of `sim_exc_take_exc_return()` -- read + clear latch. */
bool sim_exc_take_exc_return(uint64_t* out_pc)
{
  if (!s_exc_return_hit) {
    return false;
  }
  s_exc_return_hit = false;
  *out_pc          = s_exc_return_pc;
  return true;
}

/** @brief Implementation of `sim_exc_clear_pendsv_stop()` -- per-launch clear. */
void sim_exc_clear_pendsv_stop(void)
{
  s_pendsv_stop = false;
}

/** @brief Implementation of `sim_exc_pendsv_stop()` -- plain flag read. */
bool sim_exc_pendsv_stop(void)
{
  return s_pendsv_stop;
}

/** @brief Implementation of `sim_exc_bkpt_hit()` -- plain flag read. */
bool sim_exc_bkpt_hit(void)
{
  return s_bkpt_hit;
}

/** @brief Implementation of `sim_exc_bkpt_pc()` -- plain state read. */
uint32_t sim_exc_bkpt_pc(void)
{
  return s_bkpt_pc;
}

/** @brief Implementation of `sim_exc_systick_fires()` -- plain counter read. */
uint32_t sim_exc_systick_fires(void)
{
  return s_systick_fires;
}

/** @brief Implementation of `sim_exc_pendsv_takes()` -- plain counter read. */
uint32_t sim_exc_pendsv_takes(void)
{
  return s_pendsv_takes;
}

/** @brief Implementation of `sim_exc_svc_takes()` -- plain counter read. */
uint32_t sim_exc_svc_takes(void)
{
  return s_svc_takes;
}

/** @brief Implementation of `sim_exc_reset()` -- warm-reboot exception state. */
void sim_exc_reset(void)
{
  s_exc_depth       = 0U;
  s_systick_pending = true;
  s_bkpt_hit        = false;
  s_exc_return_hit  = false;
  s_pendsv_stop     = false;
  s_systick_fires   = 0U;
  s_pendsv_takes    = 0U;
  s_svc_takes       = 0U;
}
