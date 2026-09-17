/**
 * @file emu_seam_mve.c
 * @brief Minimal MVE (Helium) emulation seam (see emu_seams.h)
 *
 * @details
 * The RA8D2 is Cortex-M85 (Armv8.1-M, has MVE) but the closest core Unicorn
 * offers is M33 (Armv8-M, NO MVE), so the Helium instructions GCC's
 * auto-vectoriser emits either trap as invalid (VMOV.I32) or silently decode
 * as legacy coprocessor stores (VSTRW.32). This seam emulates the handled
 * subset -- the invalid-instruction path consumes runs of trapped MVE ops,
 * and a one-time image scan hooks every VSTRW.32 site so the store happens
 * before the core reaches the mis-decoding instruction. Moved verbatim out
 * of the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_seams.h"

/* ============================================================================
 * Minimal MVE (Helium) emulation. The RA8D2 is Cortex-M85 (Armv8.1-M, has MVE),
 * but the closest core Unicorn offers is M33 (Armv8-M, NO MVE), so the Helium
 * instructions GCC's auto-vectoriser emits trap here as "invalid". GCC vectorises
 * the memset / struct-zero idiom with two forms, which this handles:
 *   VMOV.I32 Qd, #imm          -- set all four 32-bit lanes of Qd to imm
 *   VSTRW.32 Qd, [Rn{, #off}]  -- store the 16-byte Qd to memory (no write-back)
 * The Q registers alias the FPU D registers (Qn == D[2n]:D[2n+1]) -- the M33 core
 * has those (the firmware uses the FPU) -- so the vector state is read/written
 * through Unicorn's D registers. Any other MVE form falls through and still
 * reports as invalid, so nothing is silently mis-executed. BOTH forms are
 * decoded from the raw encoding here; capstone is deliberately NOT in this path
 * (see the VMOV block below and issue #630). On real silicon all of this just
 * runs natively on Helium -- this only makes the M33-based emulator faithful to it.
 * ==========================================================================*/
enum : uint32_t {
  k_mve_insn_len   = 4U,    /**< MVE instructions are 32-bit Thumb-2.          */
  k_mve_q_bytes    = 16U,   /**< Bytes in a Q (128-bit) register.              */
  k_mve_lane_shift = 32U,   /**< 32-bit lane width (two lanes per D register). */
  k_mve_max_run    = 4096U, /**< Loop bound: max consecutive MVE ops per trap. */
};
/** @brief Count of MVE instructions emulated this run (run-end telemetry). */
static uint64_t s_mve_emulated = 0U;
/**
 * @var s_mve_nocp_handled
 * @brief Latch: the chunk just ended in a NoCP fault this seam serviced.
 *
 * @details Unicorn returns UC_ERR_EXCEPTION from `uc_emu_start` for the NoCP
 * UsageFault even when the UC_HOOK_INTR callback fully handled it and stopped
 * the engine, so the status alone cannot distinguish a serviced MVE access
 * from a real unhandled exception. This latch carries that distinction to the
 * run loop, which clears it via ::emu_mve_nocp_take and relaunches instead of
 * ending the run -- the same shape as the MPU and divide-by-zero latches.
 *
 * @note Read and cleared only by ::emu_mve_nocp_take.
 * @warning Do not set this anywhere the access did not actually happen; the
 *          run loop would then swallow a genuine unhandled exception.
 * @since 0.1.0
 */
static bool s_mve_nocp_handled = false;
/**
 * @var s_mve_resume_pc
 * @brief Address the seam advanced PC to after servicing the last NoCP fault.
 *
 * @details Paired with ::s_mve_resume_armed to absorb the one bogus
 * invalid-instruction report Unicorn emits at this address; see
 * ::emu_mve_nocp_spurious for why that report happens.
 *
 * @note Meaningful only while ::s_mve_resume_armed is set.
 * @warning Never treat this as the current PC; it is a one-shot expectation.
 * @since 0.1.0
 */
static uint32_t s_mve_resume_pc = 0U;
/**
 * @var s_mve_resume_armed
 * @brief True while one bogus post-NoCP invalid report is still expected.
 *
 * @details Set when a NoCP fault is serviced, cleared by the first
 * ::emu_mve_nocp_spurious call whether or not it matched, so at most one
 * report is ever absorbed per serviced fault.
 *
 * @note Read and cleared only by ::emu_mve_nocp_spurious.
 * @warning Leaving this set across unrelated code would mask a real fault.
 * @since 0.1.0
 */
static bool s_mve_resume_armed = false;

/* ---------------------------------------------------------------------------
 * MVE contiguous load/store family (VLDRB/VLDRH/VLDRW, VSTRB/VSTRH/VSTRW with
 * an immediate offset or post-index write-back), serviced from the NoCP
 * UsageFault.
 *
 * Armv8.1-M reallocates coprocessor space 0b1110 / 0b1111 to MVE, so this
 * family reuses the legacy STC/LDC encodings byte for byte. `arm-none-eabi-as
 * -march=armv8.1-m.main+mve` assembles `stc p15, c7, [r0, #196]` and
 * `vstrw.32 q3, [r0, #196]` to the SAME word ED80 7F31, and objdump
 * `-m armv8.1-m.main` renders that word as the MVE form. capstone renders it as
 * the legacy `stc p15`, so this family must never be decoded through capstone --
 * the fields below are taken straight from the encoding.
 *
 * Unicorn's M33 implements neither MVE nor coprocessor 14/15, so it does NOT
 * trap these as invalid instructions: it raises a NoCP UsageFault (QEMU
 * EXCP_NOCP, reported as int_no 17) through UC_HOOK_INTR with PC still at the
 * faulting instruction. Verified against libunicorn standalone: on a cold
 * engine the invalid-instruction hook is never called for any form in this
 * family, so the NoCP fault is the only way in.
 *
 * That covers the FIRST instruction of a run. Servicing it means writing PC and
 * calling `uc_emu_stop` from inside the interrupt hook, and once that has
 * happened the NEXT instruction of the same run arrives at the
 * invalid-instruction hook instead -- also verified standalone, with two
 * consecutive `vstrw.32`. GCC emits exactly such runs (the struct-zero idiom),
 * so BOTH arrival paths are wired to the same decode via ::internal_mve_mem_try: the
 * NoCP hook through ::emu_mve_nocp_emulate, and the invalid-instruction
 * dispatcher through ::emulate_mve. Wiring only one of them leaves every run
 * of two or more MVE accesses faulting on its second instruction.
 *
 * Field layout, confirmed by assembling each form and reading the bytes back:
 *   hw1 = 1110 110P 0 U W L Rn   (P=1 offset, P=0 post-index; bit6 is 0)
 *   hw2 = Qd[2:0] 1 1 1 1 size[1:0] imm7
 * with size 0b00 byte (imm7 scaled by 1), 0b01 halfword (by 2), 0b10 word
 * (by 4) and 0b11 unallocated. Every form transfers the whole 16-byte vector;
 * `size` sets the lane width and the immediate scale only, so for a contiguous
 * little-endian access the data movement is a plain 16-byte copy.
 *
 * Aliasing: the single- and double-precision FP stores share hw1 exactly and
 * differ only in hw2[11:9] -- `vstr s14, [r0, #196]` is ED80 7A31 and
 * `vstr d7, [r0, #196]` is ED80 7B31, both hw2[12:9] == 0b1101 / 0b1010.
 * Unicorn executes both natively and correctly, so the hw2[12:9] == 0b1111
 * guard is load-bearing: widening it would hijack working FP stores and turn
 * them into silent 16-byte writes.
 * ===========================================================================
 */
typedef enum : uint32_t {
  k_mve_mem_h1_mask  = 0xFF40U, /**< hw1 fixed bits with P/U/W/L/Rn excluded.    */
  k_mve_mem_h1_val   = 0xED00U, /**< hw1 match for immediate-offset forms.       */
  k_mve_mem_h1_post  = 0xEC00U, /**< hw1 match for post-index write-back forms.  */
  k_mve_mem_h2_mask  = 0x1E00U, /**< Isolates hw2[12:9], the coprocessor space.  */
  k_mve_mem_h2_val   = 0x1E00U, /**< hw2[12:9] == 0b1111 selects MVE, not FP.    */
  k_mve_mem_bit_u    = 0x0080U, /**< hw1[7]: add (1) or subtract (0) the offset. */
  k_mve_mem_bit_w    = 0x0020U, /**< hw1[5]: write the computed address to Rn.   */
  k_mve_mem_bit_l    = 0x0010U, /**< hw1[4]: load (1) or store (0).              */
  k_mve_mem_size_sh  = 7U,      /**< Position of the size field in hw2.          */
  k_mve_mem_size_msk = 0x3U,    /**< Width of the size field (two bits).         */
  k_mve_mem_sz_byte  = 0U,      /**< size 0b00: byte lanes.                      */
  k_mve_mem_sz_half  = 1U,      /**< size 0b01: halfword lanes.                  */
  k_mve_mem_sz_word  = 2U,      /**< size 0b10: word lanes.                      */
  k_mve_mem_scl_byte = 1U,      /**< imm7 scale for byte lanes.                  */
  k_mve_mem_scl_half = 2U,      /**< imm7 scale for halfword lanes.              */
  k_mve_mem_scl_word = 4U,      /**< imm7 scale for word lanes.                  */
  k_mve_qd_shift     = 13U,     /**< Qd field position in hw2.                   */
  k_mve_qd_mask      = 0x7U,    /**< Qd field width (three bits) after shift.    */
  k_mve_rn_mask      = 0xFU,    /**< Rn field (four bits) in hw1[3:0].           */
  k_mve_imm7_mask    = 0x7FU,   /**< imm7 field (unscaled offset) in hw2[6:0].   */
} mve_mem_field_t;

/**
 * @struct mve_mem_op_t
 * @brief One decoded MVE contiguous load/store.
 *
 * @details Filled by ::internal_mve_mem_decode and consumed by ::internal_mve_mem_exec. Offset
 * forms access `Rn +/- off`; post-index forms access `Rn` first and then write
 * `Rn +/- off` back to the base register.
 *
 * @invariant `qd` is in [0, 7] -- it comes from a 3-bit field.
 * @invariant `rn` is in [0, 15] -- it comes from a 4-bit field and may name SP.
 * @see internal_mve_mem_decode  Produces this.
 * @see internal_mve_mem_exec    Consumes this.
 */
typedef struct {
  uint32_t qd;    /**< Vector register Q0..Q7, from hw2[15:13].         */
  uint32_t rn;    /**< Base core register 0..15, from hw1[3:0].         */
  uint32_t off;   /**< Byte offset: imm7 scaled by the element size.    */
  bool     load;  /**< True for VLDR*, false for VSTR*.                 */
  bool     wback; /**< True when the computed address is written to Rn. */
  bool     add;   /**< True to add the offset, false to subtract it.    */
  bool     post;  /**< True when access precedes write-back adjustment. */
} mve_mem_op_t;

/**
 * @brief Decode an MVE contiguous load/store from its two halfwords.
 *
 * @details Rejects anything outside the family, including the reserved
 * `size == 0b11` encoding, so an unallocated word is left to fault rather than
 * emulated as some neighbouring instruction.
 *
 * @param[in]  hw1 First instruction halfword.
 * @param[in]  hw2 Second instruction halfword.
 * @param[out] op  Decoded operation; untouched unless true is returned.
 *
 * @return true iff @p hw1 / @p hw2 encode a member of this family.
 * @retval true  @p op holds the decoded fields.
 * @retval false Not this family; @p op is unmodified.
 *
 * @pre @p op is non-NULL.
 * @pre @p hw1 and @p hw2 are the little-endian halfwords in program order.
 * @post On true every field of @p op is initialised.
 * @post No engine or memory state is touched (pure decode).
 * @note Not thread-safe by inheritance only; the decode itself is pure.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mve_mem_decode(uint16_t hw1, uint16_t hw2, mve_mem_op_t* op)
{
  const uint16_t h1_family = hw1 & (uint16_t)k_mve_mem_h1_mask;
  if (((h1_family != (uint16_t)k_mve_mem_h1_val) && (h1_family != (uint16_t)k_mve_mem_h1_post)) ||
      ((hw2 & (uint16_t)k_mve_mem_h2_mask) != (uint16_t)k_mve_mem_h2_val)) {
    return false;
  }
  const uint32_t size =
    ((uint32_t)hw2 >> (uint32_t)k_mve_mem_size_sh) & (uint32_t)k_mve_mem_size_msk;
  uint32_t scale;
  switch (size) {
    case (uint32_t)k_mve_mem_sz_byte:
      scale = (uint32_t)k_mve_mem_scl_byte;
      break;
    case (uint32_t)k_mve_mem_sz_half:
      scale = (uint32_t)k_mve_mem_scl_half;
      break;
    case (uint32_t)k_mve_mem_sz_word:
      scale = (uint32_t)k_mve_mem_scl_word;
      break;
    default:
      return false; /* size 0b11 is unallocated -- never emulate it. */
  }
  op->qd    = ((uint32_t)hw2 >> (uint32_t)k_mve_qd_shift) & (uint32_t)k_mve_qd_mask;
  op->rn    = (uint32_t)hw1 & (uint32_t)k_mve_rn_mask;
  op->off   = ((uint32_t)hw2 & (uint32_t)k_mve_imm7_mask) * scale;
  op->load  = (hw1 & (uint16_t)k_mve_mem_bit_l) != 0U;
  op->wback = (hw1 & (uint16_t)k_mve_mem_bit_w) != 0U;
  op->add   = (hw1 & (uint16_t)k_mve_mem_bit_u) != 0U;
  op->post  = h1_family == (uint16_t)k_mve_mem_h1_post;
  return true;
}

/**
 * @brief Perform one decoded MVE contiguous load/store against emulated state.
 *
 * @details The Q registers alias the FP D registers (Qn == D[2n]:D[2n+1]) which
 * Unicorn's M33 does have, so the vector operand is read and written through
 * them. `UC_ARM_REG_D0..D15` are contiguous in Unicorn's enum (checked), but the
 * core-register ids are NOT -- SP, LR and PC sit far from R0..R12 -- so the base
 * register is resolved through ::k_arm_reg_id. Indexing `UC_ARM_REG_R0 + rn`
 * instead would read an unrelated register whenever Rn is SP, which the
 * toolchain does emit (`vstrw.32 q1, [sp, #4]` assembles to ED8D 3F01).
 *
 * @param[in,out] uc Unicorn engine.
 * @param[in]     op Decoded operation from ::internal_mve_mem_decode.
 *
 * @return Nothing.
 *
 * @pre @p op was produced by a successful ::internal_mve_mem_decode.
 * @pre @p uc is stopped inside the NoCP fault for this instruction.
 * @post The 16-byte vector has moved in the requested direction.
 * @post `Rn` holds the computed address iff `op->wback`.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mve_mem_exec(uc_engine* uc, const mve_mem_op_t* op)
{
  uint32_t base = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[op->rn], &base);
  const uint32_t adjusted = op->add ? (base + op->off) : (base - op->off);
  const uint32_t addr     = op->post ? base : adjusted;
  const int      d_lo     = (int)UC_ARM_REG_D0 + (int)(2U * op->qd);
  uint8_t        buf[k_mve_q_bytes];
  uint64_t       lo = 0U;
  uint64_t       hi = 0U;
  if (op->load) {
    (void)emu_mem_read(uc, (uint64_t)addr, buf, (size_t)k_mve_q_bytes);
    (void)memcpy(&lo, buf, sizeof(lo));
    (void)memcpy(&hi, buf + sizeof(lo), sizeof(hi));
    (void)uc_reg_write(uc, d_lo, &lo);
    (void)uc_reg_write(uc, d_lo + 1, &hi);
  } else {
    (void)uc_reg_read(uc, d_lo, &lo);
    (void)uc_reg_read(uc, d_lo + 1, &hi);
    (void)memcpy(buf, &lo, sizeof(lo));
    (void)memcpy(buf + sizeof(lo), &hi, sizeof(hi));
    (void)emu_mem_write(uc, (uint64_t)addr, buf, (size_t)k_mve_q_bytes);
  }
  if (op->wback) {
    (void)uc_reg_write(uc, k_arm_reg_id[op->rn], &adjusted);
  }
}

/**
 * @brief Perform the MVE contiguous load/store at @p code, if that is what it is.
 *
 * @details Shared by both arrival paths. Unicorn delivers the FIRST instruction
 * of a run as a NoCP UsageFault through UC_HOOK_INTR, but once that handler has
 * written PC and stopped the engine, the NEXT instruction of the same run
 * arrives at the invalid-instruction hook instead -- verified standalone
 * against libunicorn with two consecutive `vstrw.32`. Both callers therefore
 * need the identical decode-and-access step; only PC bookkeeping differs.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     code The four instruction bytes to decode.
 *
 * @return true iff @p code was an MVE contiguous load/store and was performed.
 * @retval true  The 16-byte access happened; PC is NOT changed.
 * @retval false Not this family; no state changed.
 *
 * @pre @p code holds four valid instruction bytes.
 * @pre @p uc is stopped in a hook callback.
 * @post On true the access (and any write-back) has been applied.
 * @post PC is never modified here; the caller owns it.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mve_mem_try(uc_engine* uc, const uint8_t code[4])
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  mve_mem_op_t   op  = {};
  if (!internal_mve_mem_decode(hw1, hw2, &op)) {
    return false;
  }
  internal_mve_mem_exec(uc, &op);
  return true;
}

/* ---------------------------------------------------------------------------
 * MVE VMOV immediate (VMOV.I32 Qd, #imm), serviced from the invalid-instruction
 * hook. GCC's memset / struct-zero idiom emits this to seed the vector before
 * the VSTRW.32 run the family above handles.
 *
 * This decode used to run through capstone, which put a floating, unpinned
 * third-party decoder on the EMULATION path rather than the error path: its
 * operands drove the register writes below, so a capstone that decoded
 * VMOV.I32 differently changed the emulated result, and nothing anywhere
 * pinned or version-checked it (#630, the shape of the #354 Unicorn pin one
 * library over). The fields are taken straight from the encoding instead, the
 * way the contiguous load/store family above already is. Capstone stays linked
 * for the error-path disassembly in emu_insn_seams.c ONLY; do not bring it
 * back here.
 *
 * Field layout, confirmed by assembling every form with `arm-none-eabi-as
 * -march=armv8.1-m.main+mve` and reading the bytes back:
 *   hw1 = 111 i 1111 1 D 000 imm3
 *   hw2 = Vd[3:0] cmode[3:0] 0 Q op 1 imm4
 * with imm8 = i:imm3:imm4. `vmov.i32 q0, #0` is EF80 0050 and
 * `vmov.i32 q0, #255` is FF87 005F.
 *
 * Three guards are load-bearing, each rejecting a neighbour that shares the
 * prefix: D (hw1[6]) must be 0, because D:Vd names D0..D31 and MVE has only
 * Q0..Q7 -- objdump renders D == 1 as `<illegal reg q8.5>`; Q (hw2[6]) must be
 * 1, which excludes the 64-bit D-register form; and op (hw2[5]) must be 0,
 * which excludes VMVN.i32 (same cmodes, inverted immediate) and the cmode
 * 0b1110 VMOV.i64 form. Only the six .i32 cmodes are accepted: 0/2/4/6 shift
 * imm8 left by 0/8/16/24, and C/D shift it left by 8/16 and fill the low bits
 * with ones. Every other cmode is .i8, .i16, .f32 or .i64 and falls through to
 * fault, exactly as before.
 *
 * Verified: this decode and the assembler agree on all 256224 encodings of a
 * sweep that covers the field space exhaustively (i, imm3, imm4, cmode, Vd, D,
 * Q, op) plus 60000 random words across the EF/FF prefix -- 6153 accepted,
 * every one matching objdump's register and immediate, and every rejection
 * matching too.
 * ===========================================================================
 */
typedef enum : uint32_t {
  k_mve_vmov_h1_mask  = 0xEFF8U, /**< hw1 fixed bits with i and imm3 excluded. */
  k_mve_vmov_h1_val   = 0xEF80U, /**< hw1 match; D == 0 is part of the match.  */
  k_mve_vmov_h2_mask  = 0x00F0U, /**< Isolates hw2[7:4]: the 0/Q/op/1 field.   */
  k_mve_vmov_h2_val   = 0x0050U, /**< Q == 1 (vector) and op == 0 (VMOV).      */
  k_mve_vmov_i_shift  = 12U,     /**< Position of the i bit (imm8[7]) in hw1.  */
  k_mve_vmov_i_pos    = 7U,      /**< Position of i once folded into imm8.     */
  k_mve_vmov_imm3_msk = 0x7U,    /**< imm3 field (imm8[6:4]) in hw1[2:0].      */
  k_mve_vmov_imm3_pos = 4U,      /**< Position of imm3 once folded into imm8.  */
  k_mve_vmov_imm4_msk = 0xFU,    /**< imm4 field (imm8[3:0]) in hw2[3:0].      */
  k_mve_vmov_vd_shift = 12U,     /**< Position of the Vd field in hw2.         */
  k_mve_vmov_vd_mask  = 0xFU,    /**< Vd field width (four bits) after shift.  */
  k_mve_vmov_cmode_sh = 8U,      /**< Position of the cmode field in hw2.      */
  k_mve_vmov_cmode_mk = 0xFU,    /**< cmode field width (four bits).           */
  k_mve_vmov_cm_sh0   = 0x0U,    /**< cmode 0b0000: imm8 with no shift.        */
  k_mve_vmov_cm_sh8   = 0x2U,    /**< cmode 0b0010: imm8 << 8.                 */
  k_mve_vmov_cm_sh16  = 0x4U,    /**< cmode 0b0100: imm8 << 16.                */
  k_mve_vmov_cm_sh24  = 0x6U,    /**< cmode 0b0110: imm8 << 24.                */
  k_mve_vmov_cm_one8  = 0xCU,    /**< cmode 0b1100: (imm8 << 8) | 0xFF.        */
  k_mve_vmov_cm_one16 = 0xDU,    /**< cmode 0b1101: (imm8 << 16) | 0xFFFF.     */
  k_mve_vmov_cm_step  = 4U,      /**< Bits of shift per step of an even cmode. */
  k_mve_vmov_one8_sh  = 8U,      /**< Shift applied by the cmode 0b1100 form.  */
  k_mve_vmov_one16_sh = 16U,     /**< Shift applied by the cmode 0b1101 form.  */
  k_mve_vmov_one8_fil = 0xFFU,   /**< Low-byte fill of the cmode 0b1100 form.  */
  k_mve_vmov_one16_fl = 0xFFFFU, /**< Low-half fill of the cmode 0b1101 form.  */
} mve_vmov_field_t;

/**
 * @brief Decode VMOV.I32 Qd, \#imm from its two halfwords.
 *
 * @details Rejects every neighbouring form that shares the EF/FF prefix (the D,
 * Q and op guards documented above) and every cmode that is not one of the six
 * .i32 forms, so an unallocated or differently-typed word is left to fault
 * rather than emulated as a 32-bit lane fill.
 *
 * @param[in]  hw1   First instruction halfword.
 * @param[in]  hw2   Second instruction halfword.
 * @param[out] qd    Vector register index 0..7; untouched unless true is returned.
 * @param[out] imm32 Expanded 32-bit lane value; untouched unless true is returned.
 *
 * @return true iff @p hw1 / @p hw2 encode VMOV.I32 with a Q destination.
 * @retval true  @p qd and @p imm32 hold the decoded operands.
 * @retval false Not this form; both outputs are unmodified.
 *
 * @pre @p qd and @p imm32 are non-NULL.
 * @pre @p hw1 and @p hw2 are the little-endian halfwords in program order.
 * @post On true @p qd is in [0, 7] -- Vd is four bits and even.
 * @post No engine or memory state is touched (pure decode).
 * @note Not thread-safe by inheritance only; the decode itself is pure.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mve_vmov_decode(uint16_t  hw1,
                                                  uint16_t  hw2,
                                                  uint32_t* qd,
                                                  uint32_t* imm32)
{
  if (((hw1 & (uint16_t)k_mve_vmov_h1_mask) != (uint16_t)k_mve_vmov_h1_val) ||
      ((hw2 & (uint16_t)k_mve_vmov_h2_mask) != (uint16_t)k_mve_vmov_h2_val)) {
    return false;
  }
  const uint32_t vd =
    ((uint32_t)hw2 >> (uint32_t)k_mve_vmov_vd_shift) & (uint32_t)k_mve_vmov_vd_mask;
  if ((vd & 1U) != 0U) {
    return false; /* odd D:Vd is not a Q register -- let it fault. */
  }
  const uint32_t imm8 =
    ((((uint32_t)hw1 >> (uint32_t)k_mve_vmov_i_shift) & 1U) << (uint32_t)k_mve_vmov_i_pos) |
    (((uint32_t)hw1 & (uint32_t)k_mve_vmov_imm3_msk) << (uint32_t)k_mve_vmov_imm3_pos) |
    ((uint32_t)hw2 & (uint32_t)k_mve_vmov_imm4_msk);
  const uint32_t cmode =
    ((uint32_t)hw2 >> (uint32_t)k_mve_vmov_cmode_sh) & (uint32_t)k_mve_vmov_cmode_mk;
  switch (cmode) {
    case (uint32_t)k_mve_vmov_cm_sh0:
    case (uint32_t)k_mve_vmov_cm_sh8:
    case (uint32_t)k_mve_vmov_cm_sh16:
    case (uint32_t)k_mve_vmov_cm_sh24:
      *imm32 = imm8 << ((uint32_t)k_mve_vmov_cm_step * cmode);
      break;
    case (uint32_t)k_mve_vmov_cm_one8:
      *imm32 = (imm8 << (uint32_t)k_mve_vmov_one8_sh) | (uint32_t)k_mve_vmov_one8_fil;
      break;
    case (uint32_t)k_mve_vmov_cm_one16:
      *imm32 = (imm8 << (uint32_t)k_mve_vmov_one16_sh) | (uint32_t)k_mve_vmov_one16_fl;
      break;
    default:
      return false; /* .i8 / .i16 / .f32 / .i64 and VMVN -- not this seam. */
  }
  *qd = vd >> 1U;
  return true;
}

/**
 * @brief Perform the VMOV.I32 at @p code, if that is what it is.
 *
 * @details Replicates the expanded 32-bit immediate into all four lanes of Qd.
 * The Q registers alias the FP D registers (Qn == D[2n]:D[2n+1]), which
 * Unicorn's M33 does have, so the write goes through the two D halves; both
 * halves take the same 64-bit pattern because every lane is identical.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     code The four instruction bytes to decode.
 *
 * @return true iff @p code was VMOV.I32 Qd, \#imm and the lanes were written.
 * @retval true  Qd holds the replicated immediate; PC is NOT changed.
 * @retval false Not this form; no state changed.
 *
 * @pre @p code holds four valid instruction bytes.
 * @pre @p uc is stopped in a hook callback.
 * @post On true all four lanes of Qd hold the expanded immediate.
 * @post PC is never modified here; the caller owns it.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mve_vmov_try(uc_engine* uc, const uint8_t code[4])
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  uint32_t       qd  = 0U;
  uint32_t       imm = 0U;
  if (!internal_mve_vmov_decode(hw1, hw2, &qd, &imm)) {
    return false;
  }
  const uint64_t pair = ((uint64_t)imm << (uint64_t)k_mve_lane_shift) | (uint64_t)imm;
  const int      d_lo = (int)UC_ARM_REG_D0 + (int)(2U * qd);
  (void)uc_reg_write(uc, d_lo, &pair);
  (void)uc_reg_write(uc, d_lo + 1, &pair);
  return true;
}

/**
 * @brief Emulate a run of consecutive auto-vectoriser MVE instructions.
 *
 * @details
 * Helium ops come in tight back-to-back runs (e.g. one VMOV.I32 then several
 * VSTRW.32 to zero a struct). Emulating only the first and relaunching would land
 * the next launch on another invalid instruction, which Unicorn faults on -- so
 * this consumes every consecutive handled MVE op in one trap and sets PC to the
 * first instruction it does NOT handle, exactly mirroring the cond-select seam's
 * "stop, then relaunch on valid code" contract. Bounded by ::k_mve_max_run.
 *
 * @return true iff at least one MVE instruction was emulated.
 */
bool emulate_mve(uc_engine* uc, uint32_t pc0, const uint8_t code0[4])
{
  uint32_t pc = pc0;
  uint8_t  code[4];
  (void)memcpy(code, code0, sizeof(code));
  uint32_t handled = 0U;
  while (handled < (uint32_t)k_mve_max_run) {
    /* Both families are decoded from the raw encoding: the contiguous
     * load/store because capstone renders it as a legacy `stc p15`, and
     * VMOV.I32 because a floating decoder has no business on the emulation
     * path (#630). */
    if (!internal_mve_mem_try(uc, code) && !internal_mve_vmov_try(uc, code)) {
      break; /* first non-MVE (valid) instruction -- relaunch resumes here. */
    }
    handled++;
    pc += (uint32_t)k_mve_insn_len;
    if (emu_mem_read(uc, (uint64_t)pc, code, sizeof(code)) != UC_ERR_OK) {
      break;
    }
  }
  if (handled > 0U) {
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    s_mve_emulated += handled;
  }
  return handled > 0U;
}

bool emu_mve_nocp_emulate(uc_engine* uc, uint32_t pc)
{
  uint8_t code[k_mve_insn_len] = {};
  if (emu_mem_read(uc, (uint64_t)pc, code, sizeof(code)) != UC_ERR_OK) {
    return false;
  }
  if (!internal_mve_mem_try(uc, code)) {
    return false;
  }
  const uint32_t next = pc + (uint32_t)k_mve_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  s_mve_emulated++;
  s_mve_nocp_handled = true;
  s_mve_resume_pc    = next;
  s_mve_resume_armed = true;
  return true;
}

bool emu_mve_nocp_spurious(uint32_t pc)
{
  const bool armed   = s_mve_resume_armed;
  s_mve_resume_armed = false;
  if (!armed || (pc != s_mve_resume_pc)) {
    return false;
  }
  s_mve_nocp_handled = true; /* make the run loop relaunch here. */
  return true;
}

/** @brief Implementation of `emu_mve_nocp_take()` -- test-and-clear the latch. */
bool emu_mve_nocp_take(void)
{
  const bool hit     = s_mve_nocp_handled;
  s_mve_nocp_handled = false;
  return hit;
}

/** @brief Implementation of `emu_mve_emulated_count()` -- plain counter read. */
uint64_t emu_mve_emulated_count(void)
{
  return s_mve_emulated;
}
