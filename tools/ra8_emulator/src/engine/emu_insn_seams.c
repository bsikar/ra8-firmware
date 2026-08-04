/**
 * @file emu_insn_seams.c
 * @brief Invalid-instruction dispatcher + trap-path seams (see emu_seams.h)
 *
 * @details
 * The UC_HOOK_INSN_INVALID dispatcher and the seams that ride it: the armed
 * div-0 UDF service, the Armv8-M security register scrubs (CLRM / VSCCLRM),
 * the Armv8.1-M conditional-select family (CSEL/CSINC/CSINV/CSNEG), the
 * DSB/DMB/ISB barriers old Unicorn builds trap, the MVE run consumer, and
 * the low-overhead-branch (DLS/LE) hardware loops. Every handled seam edits
 * PC and stops the engine so the chunked run loop relaunches on valid code
 * (editing PC and continuing in-place corrupts Unicorn's block/Thumb state).
 * Moved verbatim out of the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <capstone/capstone.h>
#include <stdio.h>

#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_seams.h"

/** @brief Thumb halfword-two field decode for the conditional-select family. */
typedef enum : uint32_t {
  k_cs_op_shift = 12U,  /**< CSEL-family op = hw2[13:12]. */
  k_cs_op_mask  = 0x3U, /**< 2-bit op field.              */
} cs_op_field_t;

/* Armv8.1-M conditional-select family (CSEL/CSINC/CSINV/CSNEG). The RA8D2 is
 * Cortex-M85 (Armv8.1-M) but Unicorn's nearest core is M33 (Armv8-M), which
 * lacks these. GCC emits them for branchless index/modulo math -- notably in
 * the firmware's event_post on the touch path -- so the invalid-instruction
 * hook decodes and executes them and lets the real firmware path continue.
 * Encoding (T1, two halfwords, little-endian): hw1 = 0xEA5n (n = Rn);
 * hw2 bit15=1, bit14=0, op=bits[13:12], Rd=bits[11:8], cond=bits[7:4],
 * Rm=bits[3:0]. Verified against the GNU assembler for armv8.1-m.main. */
typedef enum : uint32_t {
  k_cs_hw1_mask  = 0xFFF0U, /**< hw1 high 12 bits identify the group. */
  k_cs_hw1_match = 0xEA50U, /**< hw1[15:4] == 0xEA5 for this family.  */
  k_cs_hw2_b15   = 0x8000U, /**< hw2 bit15 must be 1.                 */
  k_cs_hw2_b14   = 0x4000U, /**< hw2 bit14 must be 0.                 */
  k_cs_op_csel   = 0U,      /**< op == 00: Rd = c ? Rn : Rm.          */
  k_cs_op_csinc  = 1U,      /**< op == 01: Rd = c ? Rn : Rm + 1.      */
  k_cs_op_csinv  = 2U,      /**< op == 10: Rd = c ? Rn : ~Rm.         */
  k_cs_op_csneg  = 3U,      /**< op == 11: Rd = c ? Rn : -Rm.         */
  k_cs_insn_len  = 4U,      /**< Both halfwords: 4 bytes.             */
  k_cs_reg_sp    = 13U,     /**< SP: reserved as a CSEL operand.      */
  k_cs_reg_pc    = 15U,     /**< PC: reserved as a CSEL operand.      */
} cond_select_t;

/* APSR condition-flag bit positions inside xPSR. */
typedef enum : uint32_t {
  k_apsr_n = 31U, /**< Negative. */
  k_apsr_z = 30U, /**< Zero.     */
  k_apsr_c = 29U, /**< Carry.    */
  k_apsr_v = 28U, /**< Overflow. */
} apsr_bit_t;

/**
 * @enum arm_cond_t
 * @brief ARM/Thumb 4-bit condition-code field encodings (cond[3:0]).
 */
typedef enum : uint32_t {
  k_cond_eq = 0x0U, /**< Equal (Z==1).             */
  k_cond_ne = 0x1U, /**< Not equal (Z==0).         */
  k_cond_cs = 0x2U, /**< Carry set / unsigned >=.  */
  k_cond_cc = 0x3U, /**< Carry clear / unsigned <. */
  k_cond_mi = 0x4U, /**< Negative.                 */
  k_cond_pl = 0x5U, /**< Positive or zero.         */
  k_cond_vs = 0x6U, /**< Overflow set.             */
  k_cond_vc = 0x7U, /**< Overflow clear.           */
  k_cond_hi = 0x8U, /**< Unsigned higher.          */
  k_cond_ls = 0x9U, /**< Unsigned lower or same.   */
  k_cond_ge = 0xAU, /**< Signed >=.                */
  k_cond_lt = 0xBU, /**< Signed <.                 */
  k_cond_gt = 0xCU, /**< Signed >.                 */
  k_cond_le = 0xDU, /**< Signed <=.                */
  k_cond_al = 0xEU, /**< Always.                   */
} arm_cond_t;

/**
 * @brief Evaluate an ARM condition code against the APSR flags.
 *
 * @param[in] cond 4-bit ARM condition code (0..15).
 * @param[in] xpsr Current xPSR (APSR flags live in the top nibble).
 * @return true if the condition holds.
 */
static bool cond_holds(uint32_t cond, uint32_t xpsr)
{
  const bool n = ((xpsr >> (uint32_t)k_apsr_n) & 1U) != 0U;
  const bool z = ((xpsr >> (uint32_t)k_apsr_z) & 1U) != 0U;
  const bool c = ((xpsr >> (uint32_t)k_apsr_c) & 1U) != 0U;
  const bool v = ((xpsr >> (uint32_t)k_apsr_v) & 1U) != 0U;
  switch (cond & (uint32_t)k_lo4_mask) {
    case k_cond_eq:
      return z;
    case k_cond_ne:
      return !z;
    case k_cond_cs:
      return c;
    case k_cond_cc:
      return !c;
    case k_cond_mi:
      return n;
    case k_cond_pl:
      return !n;
    case k_cond_vs:
      return v;
    case k_cond_vc:
      return !v;
    case k_cond_hi:
      return c && !z;
    case k_cond_ls:
      return !c || z;
    case k_cond_ge:
      return n == v;
    case k_cond_lt:
      return n != v;
    case k_cond_gt:
      return !z && (n == v);
    case k_cond_le:
      return z || (n != v);
    default:
      return true; /* AL (k_cond_al) / 0xF */
  }
}

/**
 * @brief Report whether @p reg is reserved as a conditional-select operand.
 *
 * @details SP and PC make CSEL/CSINC/CSINV/CSNEG UNPREDICTABLE, so no compiler
 * emits them and their presence means the decode window landed on a different
 * instruction entirely.
 *
 * @param[in] reg Register index [0, 15].
 * @return true when @p reg is SP or PC.
 * @retval true  @p reg is SP (13) or PC (15).
 * @retval false @p reg is a general-purpose operand register.
 * @pre @p reg was masked to four bits by the caller.
 * @post No state is modified (pure predicate).
 * @note Not thread-safe by inheritance only; the predicate itself is pure.
 * @since 0.1.0
 */
static bool cs_reserved_reg(uint32_t reg)
{
  return (reg == (uint32_t)k_cs_reg_sp) || (reg == (uint32_t)k_cs_reg_pc);
}

/**
 * @brief Emulate one Armv8.1-M conditional-select instruction if present at PC.
 *
 * @details
 * Decodes the CSEL/CSINC/CSINV/CSNEG encoding (see ::cond_select_t), evaluates
 * the condition against the APSR, computes Rd, writes it back, and advances PC
 * past the 4-byte instruction. This lets Unicorn's M33 core execute the M85
 * firmware's branchless index math instead of trapping on an opcode it does not
 * implement. Anything that is not this family is left untouched.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapped instruction.
 * @param[in]     code The 4 instruction bytes already read at @p pc.
 * @return true if a conditional-select was recognised, executed, and PC advanced.
 */
static bool emulate_cond_select(uc_engine* uc, uint32_t pc, const uint8_t* code)
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << 8));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << 8));
  if (((hw1 & (uint16_t)k_cs_hw1_mask) != (uint16_t)k_cs_hw1_match) ||
      ((hw2 & (uint16_t)k_cs_hw2_b15) == 0U) || ((hw2 & (uint16_t)k_cs_hw2_b14) != 0U)) {
    return false;
  }
  const uint32_t rn   = (uint32_t)(hw1 & (uint32_t)k_lo4_mask);
  const uint32_t op   = (uint32_t)((hw2 >> (uint32_t)k_cs_op_shift) & (uint32_t)k_cs_op_mask);
  const uint32_t rd   = (uint32_t)((hw2 >> 8) & (uint32_t)k_lo4_mask);
  const uint32_t cond = (uint32_t)((hw2 >> 4) & (uint32_t)k_lo4_mask);
  const uint32_t rm   = (uint32_t)(hw2 & (uint32_t)k_lo4_mask);

  /* The Armv8.1-M long shifts share this family's 0xEA5n prefix and put 0b1101
   * (SP) or 0b1111 (PC) in what reads here as Rm, so a register-form long shift
   * such as LSLL r2, r3, r8 (EA52 830D) satisfies every condition above -- it
   * would execute here as CSEL r3, r2, sp, EQ and silently corrupt r3. Reserved
   * operands mean this is not a conditional select; fall through and let the
   * long-shift seam claim it. */
  if (cs_reserved_reg(rn) || cs_reserved_reg(rd) || cs_reserved_reg(rm)) {
    return false;
  }

  uint32_t xpsr = 0U;
  uint32_t vn   = 0U;
  uint32_t vm   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)uc_reg_read(uc, k_arm_reg_id[rn], &vn);
  (void)uc_reg_read(uc, k_arm_reg_id[rm], &vm);

  uint32_t result;
  if (cond_holds(cond, xpsr)) {
    result = vn;
  } else {
    switch (op) {
      case (uint32_t)k_cs_op_csinc:
        result = vm + 1U;
        break;
      case (uint32_t)k_cs_op_csinv:
        result = ~vm;
        break;
      case (uint32_t)k_cs_op_csneg:
        result = (uint32_t)(-(int32_t)vm);
        break;
      case (uint32_t)k_cs_op_csel:
      default:
        result = vm;
        break;
    }
  }
  (void)uc_reg_write(uc, k_arm_reg_id[rd], &result);
  uint32_t next = pc + (uint32_t)k_cs_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/**
 * @brief Emulate an Armv8-M memory barrier (DSB/DMB/ISB) as a NOP if present.
 *
 * @details
 * Some Unicorn builds -- e.g. 2.0.1, as packaged on the Linux CI runner -- do
 * not decode the self-synchronising barrier instructions DSB / DMB / ISB and
 * trap them as invalid, where a newer build executes them. They have no
 * architectural effect in this single-threaded, in-order emulator (there is no
 * real memory ordering or pipeline to enforce), so recognising the encoding and
 * advancing PC past the 4-byte instruction is a faithful NOP. This keeps the
 * firmware's boot-path barriers (e.g. after a clock / SDRAM register write) from
 * faulting regardless of the host Unicorn version. Anything else is left
 * untouched.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapped instruction.
 * @param[in]     code The 4 instruction bytes already read at @p pc.
 * @return true if a DSB/DMB/ISB barrier was recognised and PC advanced past it.
 */
static bool emulate_barrier(uc_engine* uc, uint32_t pc, const uint8_t* code)
{
  enum : uint16_t {
    k_barrier_hw1       = 0xF3BFU, /**< First half-word of DSB/DMB/ISB.       */
    k_barrier_hw2_mask  = 0xFF00U, /**< Fixed high byte of the second h-word. */
    k_barrier_hw2_match = 0x8F00U, /**< 0x8F: the barrier group.              */
    k_barrier_op_mask   = 0x00F0U, /**< Barrier subtype field, bits [7:4].    */
    k_barrier_op_dsb    = 0x0040U, /**< DSB.                                  */
    k_barrier_op_dmb    = 0x0050U, /**< DMB.                                  */
    k_barrier_op_isb    = 0x0060U, /**< ISB.                                  */
    k_barrier_len       = 0x0004U, /**< Thumb-2 barrier instruction length.   */
  };
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << 8));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << 8));
  if ((hw1 != (uint16_t)k_barrier_hw1) ||
      ((hw2 & (uint16_t)k_barrier_hw2_mask) != (uint16_t)k_barrier_hw2_match)) {
    return false;
  }
  const uint16_t op = (uint16_t)(hw2 & (uint16_t)k_barrier_op_mask);
  if ((op != (uint16_t)k_barrier_op_dsb) && (op != (uint16_t)k_barrier_op_dmb) &&
      (op != (uint16_t)k_barrier_op_isb)) {
    return false;
  }
  const uint32_t next = pc + (uint32_t)k_barrier_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/**
 * @brief Emulate the Armv8-M security register-scrub ops as NOPs.
 *
 * @details `CLRM {regs}` and `VSCCLRM {s..,VPR}` zero caller-saved core / FP
 * registers on a Non-Secure-Callable return so Secure data cannot leak to the
 * Non-Secure caller. ra8_emulator is a single flat domain with one register/FP
 * bank, so the scrub has no observable effect on a correct caller (the cleared
 * registers are already caller-saved/clobbered) -- model them as NOPs so the
 * cmse veneer epilogue runs. Unicorn's M33 rejects both as invalid.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the instruction.
 * @param[in]     code Four instruction bytes at @p pc.
 * @return true if @p code was a scrub op (PC advanced past it); false otherwise.
 * @pre @p code holds the 4 bytes at @p pc.
 * @pre @p uc is running.
 * @post On true, PC is advanced one 32-bit instruction.
 * @post On false, no state changes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool emulate_sec_scrub(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  const uint16_t hw0 = (uint16_t)(((uint16_t)code[1] << (uint16_t)k_byte_bits) | (uint16_t)code[0]);
  if ((hw0 != (uint16_t)k_clrm_hw0) && (hw0 != (uint16_t)k_vscclrm_hw0_s) &&
      (hw0 != (uint16_t)k_vscclrm_hw0_d)) {
    return false;
  }
  const uint32_t next = pc + (uint32_t)k_thumb2_insn_bytes;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/* ---------------------------------------------------------------------------
 * Low-Overhead-Branch (LOB) emulation. The M85 has the hardware-loop extension
 * (DLS/WLS/LE) that GCC's -Og uses for counted loops; the M33 does not, so they
 * trap as invalid. Handle the two forms the toolchain emits:
 *   DLS lr, Rn        -- start a loop: LR = Rn (the iteration count)
 *   LE  lr, <label>   -- loop end: LR -= 1; branch back to <label> while LR != 0
 * (the toolchain's <label> offset is (hw2 & 0x7FF) 2-byte units backward from
 * PC+4, verified against its disassembly). Both sit amid valid code, so the same
 * stop-then-relaunch contract as the cond-select seam applies. WLS/LETP/DLSTP are
 * not emitted here and fall through to the normal invalid-instruction report.
 * ===========================================================================
 */
enum : uint32_t {
  k_lob_dls_hw1  = 0xF040U, /**< DLS lr,Rn first half-word (Rn in [3:0]). */
  k_lob_dls_h1m  = 0xFFF0U, /**< Mask isolating the fixed DLS hw1 bits.   */
  k_lob_dls_hw2  = 0xE001U, /**< DLS second half-word (fully fixed).      */
  k_lob_le_hw1   = 0xF00FU, /**< LE lr,label first half-word.             */
  k_lob_le_hw2   = 0xC000U, /**< LE second half-word fixed bits.          */
  k_lob_le_h2m   = 0xF000U, /**< Mask isolating the fixed LE hw2 bits.    */
  k_lob_le_imm10 = 0x03FFU, /**< LE offset high bits hw2[10:1] (#233).    */
  k_lob_le_lsb   = 11U,     /**< LE offset LSB scattered to hw2[11].      */
  k_lob_rn_mask  = 0xFU,    /**< Rn field (4 bits) in hw1[3:0].           */
  k_lob_insn_len = 4U,      /**< LOB instructions are 32-bit Thumb-2.     */
};
/** @brief Count of LOB instructions emulated this run (run-end telemetry). */
static uint64_t s_lob_emulated = 0U;

/** @brief Emulate a DLS/LE Low-Overhead-Branch instruction; true iff handled. */
static bool emulate_lob(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  /* DLS lr, Rn -> LR = Rn (start the counted loop). */
  if (((hw1 & (uint16_t)k_lob_dls_h1m) == (uint16_t)k_lob_dls_hw1) &&
      (hw2 == (uint16_t)k_lob_dls_hw2)) {
    /* Map Rn via k_arm_reg_id[] -- Unicorn's UC_ARM_REG_* enum is NOT contiguous
     * (UC_ARM_REG_R0 + n != UC_ARM_REG_Rn), so the prior arithmetic read the
     * wrong register for the loop count -> bogus iteration count -> over-run and
     * a wild branch (#233). This is the same table the CSEL/long-shift seams use. */
    const uint32_t rn = (uint32_t)hw1 & (uint32_t)k_lob_rn_mask;
    uint32_t       v  = 0U;
    (void)uc_reg_read(uc, k_arm_reg_id[rn], &v);
    (void)uc_reg_write(uc, UC_ARM_REG_LR, &v);
    const uint32_t next = pc + (uint32_t)k_lob_insn_len;
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    s_lob_emulated++;
    return true;
  }
  /* LE lr, label -> LR -= 1; branch back while LR != 0, else fall through. */
  if ((hw1 == (uint16_t)k_lob_le_hw1) &&
      ((hw2 & (uint16_t)k_lob_le_h2m) == (uint16_t)k_lob_le_hw2)) {
    uint32_t lr = 0U;
    (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    lr -= 1U;
    (void)uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uint32_t next = pc + (uint32_t)k_lob_insn_len;
    if (lr != 0U) {
      /* Backward branch to the loop top: the 11-bit immediate is scattered --
       * hw2[10:1] are the high bits and hw2[11] is the offset LSB (2-byte
       * granularity) -- so offset = (imm10 << 2) | (lsb << 1), taken from PC+4.
       * Verified against objdump for all 95 LE sites in the -O1 reader image
       * (#233). The previous (hw2 & 0x7FF) << 1 decode ignored the scatter and
       * mis-branched at 55 of them, running loops the wrong number of times
       * (memory corruption + a wild branch to zeros at -O1). */
      const uint32_t imm10 = ((uint32_t)hw2 >> 1U) & (uint32_t)k_lob_le_imm10;
      const uint32_t lsb   = ((uint32_t)hw2 >> (uint32_t)k_lob_le_lsb) & 1U;
      next -= ((imm10 << 2U) | (lsb << 1U)); /* backward to loop top. */
    }
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    s_lob_emulated++;
    return true;
  }
  return false;
}

/**
 * @brief Emulate one Armv8.1-M instruction Unicorn's M33 core does not provide.
 *
 * @details
 * The RA8D2 firmware is built for Cortex-M85 (Armv8.1-M) but the nearest core
 * Unicorn offers is M33 (Armv8-M), so the conditional selects, the barriers,
 * Helium, the register-form long shifts and the hardware loops all arrive as
 * undefined instructions. Each handler writes its result and advances PC; the
 * caller stops the engine so the chunked run loop relaunches from the new PC.
 * Split out of ::dispatch_insn_seam, which keeps the two seams that are about
 * ra8_emulator's own machinery (a patched divide, a security scrub) rather than a
 * missing instruction.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapping instruction.
 * @param[in]     code The 4 instruction bytes at @p pc.
 * @return true when a handler claimed and emulated the instruction.
 * @retval true  PC has advanced; the caller must stop and relaunch.
 * @retval false No handler recognised @p code.
 * @pre @p code holds the bytes the core failed to decode at @p pc.
 * @pre @p uc is stopped inside the invalid-instruction hook.
 * @post On true, PC points past the emulated instruction.
 * @note Not thread-safe; called from the single-threaded run loop.
 * @since 0.1.0
 */
static bool dispatch_armv81_seam(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  /* The RA8D2 firmware is built for Cortex-M85 (Armv8.1-M); the nearest core
   * Unicorn offers is M33 (Armv8-M), which lacks the conditional-select family.
   * GCC emits those for branchless index math on the touch path, so
   * execute them here. emulate_cond_select writes Rd and advances PC past the
   * 4-byte instruction; then uc_emu_stop so the chunked run loop relaunches from
   * the new PC -- editing PC and continuing in-place corrupts Unicorn's block /
   * Thumb state (it then misdecodes the next valid instruction), so the
   * stop-then-relaunch contract the SysTick / touch stubs use is required here. */
  if (emulate_cond_select(uc, pc, code)) {
    return true; /* handled -- run loop resumes at the advanced PC */
  }

  /* Older Unicorn builds (the runner's 2.0.1) trap DSB/DMB/ISB as invalid; a
   * barrier is a NOP in this emulator, so advance past it and relaunch. */
  if (emulate_barrier(uc, pc, code)) {
    return true; /* handled -- run loop resumes past the barrier */
  }

  /* MVE (Helium): the M85 has it, Unicorn's M33 does not, so the auto-vectoriser's
   * Helium ops trap here. Emulate the handled subset and relaunch past it. */
  if (emulate_mve(uc, pc, code)) {
    return true; /* handled -- run loop resumes past the MVE instruction */
  }

  /* Armv8.1-M long shift, REGISTER form (LSLL/ASRL by Rm). Its encoding aliases
   * to an ORR.W with Rm == SP, which the core refuses rather than
   * mis-executing, so it lands here as a real trap. (The immediate form aliases
   * to an ORRS the core happily executes with a wrong result, so that one is
   * repaired by the code-hook seam in emu_seam_longshift.c instead.) */
  if (emulate_long_shift_reg(uc, pc, code)) {
    return true; /* handled -- run loop resumes past the long shift */
  }

  /* Low-Overhead-Branch (DLS/LE): the M85's hardware-loop ops, absent on the M33.
   * Emulate the loop counter / branch and relaunch. */
  if (emulate_lob(uc, pc, code)) {
    return true; /* handled -- run loop resumes at the loop top or past the loop */
  }
  return false;
}

/** @brief Try each Armv8.1-M / security seam in turn; true (and stop) if handled. */
static bool dispatch_insn_seam(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  /* Divide-by-zero trap: a UDIV/SDIV overwritten with UDF once the firmware set
   * CCR.DIV_0_TRP. emulate_div0_patched either latches a UsageFault (zero divisor)
   * or emulates the divide and advances PC; either way stop + relaunch. Checked
   * first: it matches only the exact patched addresses, never a real UDF. */
  if (emulate_div0_patched(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true;
  }

  /* Armv8-M Security Extension register scrub (CLRM / VSCCLRM) on a cmse
   * Non-Secure-Callable return: a NOP in ra8_emulator's single-domain model. */
  if (emulate_sec_scrub(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true;
  }

  if (dispatch_armv81_seam(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes past the emulated instruction */
  }
  return false;
}

/** @brief Report + capstone-disassemble an instruction no seam could decode. */
static void report_unhandled_insn(uint32_t pc, const uint8_t code[4])
{
  (void)fprintf(stderr,
                "  INVALID INSN @ 0x%08X: bytes %02X %02X %02X %02X\n",
                pc,
                code[0],
                code[1],
                code[2],
                code[3]);

  csh cs;
  if (cs_open(CS_ARCH_ARM, (cs_mode)(CS_MODE_THUMB | CS_MODE_MCLASS), &cs) == CS_ERR_OK) {
    cs_insn* insn = nullptr;
    /* `code` decays to a pointer here, so sizeof(code) would hand capstone the
     * pointer width instead of the four instruction bytes that are valid. */
    const size_t n = cs_disasm(cs, code, (size_t)k_cs_insn_len, pc, 1, &insn);
    if (n > 0U) {
      (void)fprintf(stderr, "  disasm: %s %s\n", insn[0].mnemonic, insn[0].op_str);
      cs_free(insn, n);
    } else {
      (void)fprintf(stderr, "  disasm: capstone could not decode it either\n");
    }
    cs_close(&cs);
  }
}

/** @brief Disassemble + report an instruction the core could not decode. */
bool on_invalid_insn(uc_engine* uc, void* user)
{
  (void)user;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint8_t code[4] = {};
  (void)uc_mem_read(uc, pc, code, sizeof(code));

  if (dispatch_insn_seam(uc, pc, code)) {
    return true; /* handled -- run loop resumes at the advanced PC */
  }

  /* Unicorn re-reports the instruction FOLLOWING a serviced NoCP fault as
   * invalid even when it is perfectly valid, because the seam wrote PC and
   * stopped the engine from inside the interrupt hook. Absorb exactly that one
   * report, at exactly that one address, and relaunch. */
  if (emu_mve_nocp_spurious(pc)) {
    (void)uc_emu_stop(uc);
    return true;
  }

  report_unhandled_insn(pc, code);
  return false; /* not handled -> stop emulation with UC_ERR_INSN_INVALID */
}

/** @brief Implementation of `emu_insn_seams_install()` -- arm the dispatcher. */
void emu_insn_seams_install(uc_engine* uc)
{
  static uc_hook s_h_invalid;
  (void)uc_hook_add(uc, &s_h_invalid, UC_HOOK_INSN_INVALID, (void*)on_invalid_insn, nullptr, 1, 0);
}

/** @brief Implementation of `emu_lob_emulated_count()` -- plain counter read. */
uint64_t emu_lob_emulated_count(void)
{
  return s_lob_emulated;
}
