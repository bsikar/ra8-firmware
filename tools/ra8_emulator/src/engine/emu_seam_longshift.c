/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file emu_seam_longshift.c
 * @brief Armv8.1-M long-shift (LSLL/LSRL/ASRL) emulation seam (see emu_seams.h)
 *
 * @details
 * Same M85-vs-M33 gap as the other seams, but with a sharper failure mode: the
 * M33 does NOT trap these. Their encoding overlaps ORR.W (register) with
 * Rm == PC, so the core silently mis-executes the shift as an ORRS and yields a
 * wrong 64-bit result with no fault. GCC emits them for any 64-bit shift that
 * crosses the word boundary, so the silent miscompile corrupts 64-bit divides
 * and miniz's ZIP central-directory size math alike. A one-time image scan
 * finds every immediate long-shift site and hooks it so the correct 64-bit
 * result is applied on the host, where 64-bit arithmetic is correct.
 *
 * The seam never edits the image and never stops the engine. Both were tried
 * and both are wrong:
 *
 * - Patching each site to a decodable no-op corrupts the firmware. The scan
 *   matches on 2-byte boundaries and cannot tell a real instruction from a
 *   halfword pair inside a literal pool that happens to fit the encoding, so a
 *   false positive that is merely never executed today would become a silently
 *   rewritten constant.
 * - Writing PC from inside the UC_HOOK_CODE callback and calling uc_emu_stop()
 *   -- the contract this seam used to use -- makes the next uc_emu_start()
 *   return UC_ERR_INSN_INVALID, reported against the relaunch address rather
 *   than any real instruction. That defect killed the run on the FIRST long
 *   shift that ever executed, so in practice no long shift was ever emulated.
 *
 * Instead the shift is applied in two phases around the mis-decoding
 * instruction, using nothing but register reads and writes:
 *
 * 1. At the site, BEFORE the core executes it, the operand pair
 *    {RdaHi:RdaLo} and the current NZCV flags are captured and the correct
 *    64-bit result is computed.
 * 2. The core executes the encoding as ORRS, which clobbers exactly RdaHi and
 *    the flags, and nothing else -- RdaLo is the ORRS Rn operand, not its Rd.
 * 3. At the following instruction the captured result is written over
 *    {RdaHi:RdaLo} and NZCV is restored, because a real immediate
 *    LSLL/LSRL/ASRL does not write the flags.
 *
 * One dispatcher serves both phases, so a long shift immediately followed by
 * another long shift resolves in the correct order regardless of the order
 * Unicorn happens to invoke hooks in.
 *
 *
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "emu_elf.h"
#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_seams.h"

/* Armv8.1-M long-shift family (LSLL / LSRL / ASRL), both the immediate form
 * (`lsll r4, r5, #7`) and the register form (`lsll r0, r1, ip`).
 *
 * Encoding (T1, two halfwords, little-endian), verified by assembling every
 * member with `arm-none-eabi-as -march=armv8.1-m.main+mve` and reading back the
 * bytes -- the family is dense and three neighbouring groups alias it:
 *
 *   hw1 = 0xEA5n, where n[3:1] = RdaLo (always an EVEN register) and
 *                       n[0]   = 1 selects the SATURATING/ROUNDING subfamily
 *                                (UQSHLL / URSHRL / SRSHRL / SQSHLL /
 *                                 UQRSHLL / SQRSHRL)
 *   hw2[11:8] = RdaHi, except 0b1111 which selects the 32-bit (non-long)
 *                      UQRSHL / SQRSHR
 *   hw2[3:0]  = 0b1111 -> immediate form, imm5 = hw2[14:12]:hw2[7:6]
 *                         (imm5 == 0 encodes a shift of 32)
 *             = 0b1101 -> register form, Rm = hw2[15:12], hw2[7:6] == 0
 *   op = hw2[5:4] (00 = LSLL, 01 = LSRL, 10 = ASRL)
 *
 * The two exclusions matter and are not cosmetic. `uqshll r0, r1, #7` encodes
 * as EA51 11CF: it shares the immediate form's 0b1111 tail and its op field, so
 * a decoder that masks the low FOUR bits of hw1 as RdaLo (as this one once did)
 * accepts it as `lsll r1, r1, #7` and computes an unsaturated shift into the
 * wrong register -- silently wrong 64-bit arithmetic, which is the exact defect
 * this seam exists to prevent. Likewise `uqrshl r0, r2` (EA50 2F0D) has the
 * register form's 0b1101 tail and a clear hw1[0], and is excluded only by its
 * RdaHi == 0b1111. Both subfamilies are therefore rejected here and reported at
 * install time rather than emulated approximately. */
typedef enum : uint32_t {
  k_lsh_hw1_mask     = 0xFFF0U, /**< hw1[15:4] selects the group.                 */
  k_lsh_hw1_match    = 0xEA50U, /**< hw1[15:4] == 0xEA5 for this family.          */
  k_lsh_hw1_sat_bit  = 0x0001U, /**< hw1[0]: 1 = saturating/rounding subfamily.   */
  k_lsh_rdalo_mask   = 0x000EU, /**< RdaLo at hw1[3:1] (an even register).        */
  k_lsh_hw2_lo_mask  = 0x000FU, /**< hw2[3:0] selects immediate vs register form. */
  k_lsh_tail_imm     = 0x000FU, /**< hw2[3:0] == 0b1111 -> immediate form.        */
  k_lsh_tail_reg     = 0x000DU, /**< hw2[3:0] == 0b1101 -> register form.         */
  k_lsh_op_shift     = 4U,      /**< op field at hw2[5:4].                        */
  k_lsh_op_mask      = 0x3U,    /**< op width (value 3 is reserved -> rejected).  */
  k_lsh_op_lsll      = 0U,      /**< 00: 64-bit logical shift left.               */
  k_lsh_op_lsrl      = 1U,      /**< 01: 64-bit logical shift right.              */
  k_lsh_op_asrl      = 2U,      /**< 10: 64-bit arithmetic shift right.           */
  k_lsh_rdahi_shift  = 8U,      /**< RdaHi at hw2[11:8].                          */
  k_lsh_rdahi_wide   = 0x0FU,   /**< RdaHi == 0b1111 -> 32-bit form, rejected.    */
  k_lsh_rm_shift     = 12U,     /**< Rm at hw2[15:12] in the register form.       */
  k_lsh_reg_zero_msk = 0x00C0U, /**< hw2[7:6] must be zero in the register form.  */
  k_lsh_imm3_shift   = 12U,     /**< imm3 at hw2[14:12].                          */
  k_lsh_imm3_mask    = 0x7U,    /**< imm3 width.                                  */
  k_lsh_imm2_shift   = 6U,      /**< imm2 at hw2[7:6].                            */
  k_lsh_imm2_mask    = 0x3U,    /**< imm2 width.                                  */
  k_lsh_imm3_to_imm5 = 2U,      /**< imm5 = (imm3 << 2) | imm2.                   */
  k_lsh_word_bits    = 32U,     /**< Word width; also the imm5==0 shift amount.   */
  k_lsh_pair_bits    = 64U,     /**< Width of the {RdaHi:RdaLo} pair, bits.       */
  k_lsh_amount_mask  = 0xFFU,   /**< Register form uses Rm[7:0] as the amount.    */
  k_lsh_amount_wrap  = 0x100U,  /**< Rm[7:0] is signed: subtract to go negative.  */
  k_lsh_amount_sign  = 0x80U,   /**< Sign bit of the 8-bit shift amount.          */
  k_lsh_insn_len     = 4U,      /**< Thumb-2 instruction length, bytes.           */
} long_shift_t;

/**
 * @struct long_shift_insn_t
 * @brief One decoded immediate-or-register Armv8.1-M long shift.
 *
 * @details
 * Produced by ::long_shift_decode and consumed by ::long_shift_begin. The
 * register form carries the shift amount in a register that can only be read
 * once the core is at the instruction, so the amount is resolved separately by
 * ::long_shift_amount rather than being decoded here.
 *
 * @invariant @c rdalo is even; @c rdahi is never 0b1111.
 * @invariant Exactly one of @c by_reg / immediate use is meaningful: when
 *            @c by_reg is false @c imm holds the amount, otherwise @c rm names
 *            the register that does.
 * @see long_shift_decode
 */
typedef struct {
  uint32_t rdalo;  /**< Low destination register index (even).            */
  uint32_t rdahi;  /**< High destination register index.                  */
  uint32_t op;     /**< ::k_lsh_op_lsll / _lsrl / _asrl.                  */
  bool     by_reg; /**< true when the amount comes from @c rm.            */
  uint32_t rm;     /**< Register holding the amount (register form only). */
  uint32_t imm;    /**< Shift amount 1..32 (immediate form only).         */
} long_shift_insn_t;

/**
 * @brief Decode an Armv8.1-M long shift (LSLL/LSRL/ASRL), immediate or register.
 *
 * @details
 * Validates the two-halfword encoding (see ::long_shift_t) and, on a match,
 * fills @p out with the destination pair, the operation, and either the
 * immediate amount or the register that supplies it. Rejects the saturating /
 * rounding subfamily (hw1[0] set) and the 32-bit UQRSHL/SQRSHR forms
 * (RdaHi == 0b1111), both of which alias this encoding but do not share its
 * semantics. Shared by the image scan that installs the per-site hooks and by
 * the hook that emulates one at run time.
 *
 * @param[in]  hw1 First (low-address) instruction halfword.
 * @param[in]  hw2 Second instruction halfword.
 * @param[out] out Receives the decoded instruction on a match.
 * @return true if @p hw1/@p hw2 form a plain long shift; false otherwise.
 * @pre @p out is non-null.
 * @post On true, @p out is fully written; on false, it is untouched.
 */
static bool long_shift_decode(uint16_t hw1, uint16_t hw2, long_shift_insn_t* out)
{
  if ((hw1 & (uint16_t)k_lsh_hw1_mask) != (uint16_t)k_lsh_hw1_match) {
    return false;
  }
  if ((hw1 & (uint16_t)k_lsh_hw1_sat_bit) != 0U) {
    return false; /* UQSHLL / URSHRL / SRSHRL / SQSHLL / UQRSHLL / SQRSHRL. */
  }
  const uint32_t op = ((uint32_t)hw2 >> (uint32_t)k_lsh_op_shift) & (uint32_t)k_lsh_op_mask;
  if (op > (uint32_t)k_lsh_op_asrl) {
    return false; /* op == 0b11 is reserved -- not a long shift. */
  }
  const uint32_t rdahi = ((uint32_t)hw2 >> (uint32_t)k_lsh_rdahi_shift) & (uint32_t)k_lo4_mask;
  if (rdahi == (uint32_t)k_lsh_rdahi_wide) {
    return false; /* UQRSHL / SQRSHR -- the 32-bit, non-long forms. */
  }
  const uint32_t tail = (uint32_t)hw2 & (uint32_t)k_lsh_hw2_lo_mask;
  *out                = (long_shift_insn_t){
    .rdalo = (uint32_t)hw1 & (uint32_t)k_lsh_rdalo_mask,
    .rdahi = rdahi,
    .op    = op,
  };
  if (tail == (uint32_t)k_lsh_tail_imm) {
    const uint32_t imm3 = ((uint32_t)hw2 >> (uint32_t)k_lsh_imm3_shift) & (uint32_t)k_lsh_imm3_mask;
    const uint32_t imm2 = ((uint32_t)hw2 >> (uint32_t)k_lsh_imm2_shift) & (uint32_t)k_lsh_imm2_mask;
    const uint32_t imm5 = (imm3 << (uint32_t)k_lsh_imm3_to_imm5) | imm2;
    out->imm            = (imm5 == 0U) ? (uint32_t)k_lsh_word_bits : imm5;
    return true;
  }
  if (tail == (uint32_t)k_lsh_tail_reg) {
    if (((uint32_t)hw2 & (uint32_t)k_lsh_reg_zero_msk) != 0U) {
      return false; /* hw2[7:6] must be zero in the register form. */
    }
    if (out->op == (uint32_t)k_lsh_op_lsrl) {
      return false; /* no LSRL register form exists in the ISA. */
    }
    out->by_reg = true;
    out->rm     = ((uint32_t)hw2 >> (uint32_t)k_lsh_rm_shift) & (uint32_t)k_lo4_mask;
    return true;
  }
  return false;
}

/**
 * @brief Resolve the shift amount of a decoded long shift, in bits.
 *
 * @details
 * The immediate form carries its amount in the encoding. The register form
 * takes the low byte of Rm as a SIGNED amount, so a negative value shifts the
 * opposite way; that is the MVE definition and it is why the amount cannot be
 * resolved until the core has reached the instruction.
 *
 * @param[in,out] uc   Unicorn engine to read Rm from.
 * @param[in]     insn Decoded instruction.
 * @return Shift amount in bits; negative means shift the opposite direction.
 * @pre @p insn came from a successful ::long_shift_decode.
 * @post No engine state is modified (register read only).
 */
static int32_t long_shift_amount(uc_engine* uc, const long_shift_insn_t* insn)
{
  if (!insn->by_reg) {
    return (int32_t)insn->imm;
  }
  uint32_t rm = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[insn->rm], &rm);
  const uint32_t byte = rm & (uint32_t)k_lsh_amount_mask;
  if ((byte & (uint32_t)k_lsh_amount_sign) != 0U) {
    return (int32_t)byte - (int32_t)k_lsh_amount_wrap;
  }
  return (int32_t)byte;
}

/**
 * @brief Apply one long shift to a 64-bit value with correct host arithmetic.
 *
 * @details
 * Shifts of 64 bits or more are resolved without invoking C's undefined
 * behaviour for an over-wide shift: a logical shift yields zero and an
 * arithmetic right shift yields the sign fill. A negative amount reverses the
 * direction, per the MVE register form.
 *
 * @param[in] val   The 64-bit {RdaHi:RdaLo} operand.
 * @param[in] op    ::k_lsh_op_lsll / _lsrl / _asrl.
 * @param[in] shift Amount in bits; negative reverses the direction.
 * @return The shifted value.
 * @pre @p op is one of the three plain long-shift operations.
 * @post The result is defined for every @p shift, including >= 64.
 */
static uint64_t long_shift_apply(uint64_t val, uint32_t op, int32_t shift)
{
  uint32_t left = (op == (uint32_t)k_lsh_op_lsll) ? 1U : 0U;
  int32_t  n    = shift;
  if (n < 0) {
    n    = -n;
    left = (left != 0U) ? 0U : 1U;
  }
  const bool arith = (op == (uint32_t)k_lsh_op_asrl);
  if (n >= (int32_t)k_lsh_pair_bits) {
    if (left != 0U) {
      return 0U;
    }
    return arith ? (uint64_t)((int64_t)val >> ((int32_t)k_lsh_pair_bits - 1)) : 0U;
  }
  if (left != 0U) {
    return val << (uint32_t)n;
  }
  return arith ? (uint64_t)((int64_t)val >> (uint32_t)n) : (val >> (uint32_t)n);
}

/** @brief Max long-shift sites tracked per image (generous; real counts tiny). */
enum : uint32_t {
  k_lsh_sites_max = 4096U, /**< Lsh sites maximum.                         */
  k_lsh_hooks_max = 8192U, /**< Two hooks per site: the site and its tail. */
};

/** @brief Execution addresses of every immediate long shift found in the image. */
static uint32_t s_lsh_sites[k_lsh_sites_max];
/** @brief Number of valid entries in ::s_lsh_sites. */
static uint32_t s_lsh_site_count;
/** @brief Hook handles for the installed sites (kept alive for the whole run). */
static uc_hook s_lsh_hooks[k_lsh_hooks_max];

/**
 * @struct long_shift_pending_t
 * @brief The correct result of one long shift, awaiting write-back.
 *
 * @details
 * Filled at the long-shift site before the core mis-executes the encoding as
 * ORRS, and consumed at the following instruction once that ORRS has done its
 * damage.
 *
 * More than one can be outstanding. The two phases are only one instruction
 * apart, but ra8_emulator delivers an interrupt by ending a run chunk and
 * vectoring, and that boundary can fall inside the window -- so a handler can
 * begin (and finish) its own long shift while an outer one is still awaiting
 * write-back. A single slot would be overwritten by the inner shift and the
 * outer result silently lost, which is precisely the wrong-64-bit-arithmetic
 * failure this seam exists to prevent, so entries are held in a small table
 * keyed by write-back address (::k_lsh_pending_max deep, far beyond the
 * interrupt nesting this emulator models).
 *
 * @invariant @c active is true only between a site and its immediately
 *            following instruction.
 * @invariant At most one active entry exists per @c at address.
 * @see on_long_shift
 */
typedef struct {
  bool     active; /**< A captured result is waiting to be written back. */
  uint32_t at;     /**< Address of the instruction after the long shift. */
  uint32_t rdalo;  /**< Low destination register index.                  */
  uint32_t rdahi;  /**< High destination register index.                 */
  uint32_t lo;     /**< Correct low word of the shifted 64-bit value.    */
  uint32_t hi;     /**< Correct high word of the shifted 64-bit value.   */
  uint32_t nzcv;   /**< NZCV as it stood before the ORRS overwrote it.   */
} long_shift_pending_t;

/** @brief Depth of the outstanding-write-back table. */
enum : uint32_t {
  k_lsh_pending_max = 8U, /**< Lsh pending maximum. */
};

/** @brief Outstanding long-shift write-backs (see ::long_shift_pending_t). */
static long_shift_pending_t s_lsh_pending[k_lsh_pending_max];

/**
 * @brief Find the pending write-back slot for @p at, or a free slot.
 *
 * @details
 * Used both to look up an outstanding entry (@p want_active true) and to claim
 * a slot for a new one (@p want_active false, preferring an entry already
 * keyed to @p at so a re-executed site cannot accumulate duplicates).
 *
 * @param[in] at          Write-back address to match.
 * @param[in] want_active true to find an armed entry for @p at; false to claim.
 * @return Pointer to the matching or claimable slot, or nullptr if none.
 * @pre The table is initialised (it is zeroed at load, and per-run by install).
 * @post No entry is modified by the lookup itself.
 */
static long_shift_pending_t* long_shift_slot(uint32_t at, bool want_active)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lsh_pending_max; i++) {
    if (s_lsh_pending[i].active && (s_lsh_pending[i].at == at)) {
      return &s_lsh_pending[i];
    }
  }
  if (want_active) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_lsh_pending_max; i++) {
    if (!s_lsh_pending[i].active) {
      return &s_lsh_pending[i];
    }
  }
  return nullptr;
}

/**
 * @brief Report whether @p addr is a long-shift site found by the image scan.
 *
 * @param[in] addr Execution address to test.
 * @return true when @p addr holds an immediate long shift.
 * @pre The image scan has run (an empty table simply answers false).
 * @post No state is modified (read-only predicate).
 */
static bool long_shift_is_site(uint32_t addr)
{
  for (uint32_t i = 0U; i < s_lsh_site_count; i++) {
    if (s_lsh_sites[i] == addr) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Compute one long shift on the host and stage it for write-back.
 *
 * @details
 * Reads the instruction at @p address, decodes it, forms the 64-bit
 * {RdaHi:RdaLo} operand, applies the shift with correct 64-bit host
 * arithmetic, and records the result plus the pre-existing NZCV flags in
 * ::s_lsh_pending. Nothing is written to the core here: the mis-decoding ORRS
 * runs next and would overwrite RdaHi and the flags anyway.
 *
 * @param[in,out] uc      Unicorn engine to read the operands from.
 * @param[in]     address Address of the long-shift instruction.
 * @return Nothing.
 * @pre @p uc is a running engine positioned at @p address.
 * @post ::s_lsh_pending is armed, or left untouched on an unreadable /
 *       non-matching site (a scan false positive).
 * @note Not thread-safe.
 */
static void long_shift_begin(uc_engine* uc, uint32_t address)
{
  uint8_t code[k_lsh_insn_len] = {};
  if (uc_mem_read(uc, address, code, sizeof(code)) != UC_ERR_OK) {
    return;
  }
  const uint16_t    hw1  = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t    hw2  = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  long_shift_insn_t insn = {};
  if (!long_shift_decode(hw1, hw2, &insn)) {
    return; /* scan false-positive at a never-executed address: leave it alone. */
  }
  uint32_t lo = 0U;
  uint32_t hi = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[insn.rdalo], &lo);
  (void)uc_reg_read(uc, k_arm_reg_id[insn.rdahi], &hi);
  const uint64_t val  = ((uint64_t)hi << (uint64_t)k_lsh_word_bits) | (uint64_t)lo;
  const uint64_t res  = long_shift_apply(val, insn.op, long_shift_amount(uc, &insn));
  uint32_t       nzcv = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_APSR_NZCV, &nzcv);
  const uint32_t        at   = address + (uint32_t)k_lsh_insn_len;
  long_shift_pending_t* slot = long_shift_slot(at, false);
  if (slot == nullptr) {
    return; /* table full: leave the core's (wrong) ORRS result rather than
             * write back against the wrong instruction. Unreachable in practice
             * -- it needs k_lsh_pending_max nested interrupts inside the
             * one-instruction window. */
  }
  *slot = (long_shift_pending_t){
    .active = true,
    .at     = at,
    .rdalo  = insn.rdalo,
    .rdahi  = insn.rdahi,
    .lo     = (uint32_t)res,
    .hi     = (uint32_t)(res >> (uint64_t)k_lsh_word_bits),
    .nzcv   = nzcv,
  };
}

/**
 * @brief Write back the staged long-shift result over the ORRS's damage.
 *
 * @details
 * Restores {RdaHi:RdaLo} to the correct 64-bit result and puts NZCV back as it
 * stood before the mis-decoded ORRS, because a real immediate LSLL/LSRL/ASRL
 * leaves the flags alone. Disarms the entry.
 *
 * @param[in,out] uc   Unicorn engine to write the registers of.
 * @param[in,out] slot The armed entry for the instruction now being entered.
 * @return Nothing.
 * @pre @p slot is a non-null, armed entry.
 * @post @p slot is disarmed and the register pair holds the result.
 * @note Not thread-safe.
 */
static void long_shift_commit(uc_engine* uc, long_shift_pending_t* slot)
{
  (void)uc_reg_write(uc, k_arm_reg_id[slot->rdalo], &slot->lo);
  (void)uc_reg_write(uc, k_arm_reg_id[slot->rdahi], &slot->hi);
  (void)uc_reg_write(uc, UC_ARM_REG_APSR_NZCV, &slot->nzcv);
  slot->active = false;
}

/**
 * @brief UC_HOOK_CODE: run both phases of the long-shift seam at one address.
 *
 * @details
 * Commits any result staged by the preceding instruction first, then stages a
 * new one if this address is itself a long-shift site. Doing both in a single
 * dispatcher is what makes back-to-back long shifts correct: were the commit
 * and the capture separate hooks on the same address, their relative order
 * would be Unicorn's registration order rather than the program order.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     address Address of the instruction about to execute.
 * @param[in]     size    Reported instruction size (unused).
 * @param[in]     user    Hook user pointer (unused).
 * @return Nothing.
 * @pre @p address is a scanned site or the instruction following one.
 * @post At most one result is staged when the hook returns.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_long_shift(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  const uint32_t        pc   = (uint32_t)address;
  long_shift_pending_t* slot = long_shift_slot(pc, true);
  if (slot != nullptr) {
    long_shift_commit(uc, slot);
  }
  if (long_shift_is_site(pc)) {
    long_shift_begin(uc, pc);
  }
}

bool emulate_long_shift_reg(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  const uint16_t    hw1  = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t    hw2  = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  long_shift_insn_t insn = {};
  if (!long_shift_decode(hw1, hw2, &insn) || !insn.by_reg) {
    return false; /* not ours: the immediate form never traps, it mis-executes. */
  }
  uint32_t lo = 0U;
  uint32_t hi = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[insn.rdalo], &lo);
  (void)uc_reg_read(uc, k_arm_reg_id[insn.rdahi], &hi);
  const uint64_t val = ((uint64_t)hi << (uint64_t)k_lsh_word_bits) | (uint64_t)lo;
  const uint64_t res = long_shift_apply(val, insn.op, long_shift_amount(uc, &insn));
  lo                 = (uint32_t)res;
  hi                 = (uint32_t)(res >> (uint64_t)k_lsh_word_bits);
  (void)uc_reg_write(uc, k_arm_reg_id[insn.rdalo], &lo);
  (void)uc_reg_write(uc, k_arm_reg_id[insn.rdahi], &hi);
  uint32_t next = pc + (uint32_t)k_lsh_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/**
 * @brief Scan one executable segment for long-shift sites, arming hooks.
 *
 * @details
 * Records each site's execution address and arms a hook there and at the
 * instruction that follows it, so ::on_long_shift can run both phases.
 *
 * @param[in]     uc       Engine to arm hooks on.
 * @param[in]     elf      Base of the resident ELF image.
 * @param[in]     p_offset Segment file offset.
 * @param[in]     p_vaddr  Segment virtual base (hook VA = p_vaddr + off).
 * @param[in]     p_filesz Segment size in the file, bytes.
 * @param[in,out] n_hooks  Running installed-site count, advanced per hook.
 * @return false if the global site cap (::k_lsh_sites_max) was reached and the
 *         caller must stop scanning further segments; true otherwise.
 */
static bool install_seg_hooks(uc_engine*     uc,
                              const uint8_t* elf,
                              uint32_t       p_offset,
                              uint32_t       p_vaddr,
                              uint32_t       p_filesz,
                              uint32_t*      n_hooks)
{
  for (uint32_t off = 0U; (off + (uint32_t)k_lsh_insn_len) <= p_filesz; off += 2U) {
    const uint8_t*    p    = elf + p_offset + off;
    const uint16_t    hw1  = (uint16_t)(p[0] | ((uint16_t)p[1] << (uint16_t)k_byte_bits));
    const uint16_t    hw2  = (uint16_t)(p[2] | ((uint16_t)p[3] << (uint16_t)k_byte_bits));
    long_shift_insn_t insn = {};
    if (!long_shift_decode(hw1, hw2, &insn)) {
      continue;
    }
    if (insn.by_reg) {
      /* The register form aliases to ORRS with Rm == SP, which the core refuses
       * outright, so it arrives as a real undefined-instruction trap and is
       * serviced by ::emulate_long_shift_reg off the seam dispatch chain. Only
       * the immediate form -- which aliases to an ORRS the core happily
       * MIS-EXECUTES -- has to be intercepted here. */
      continue;
    }
    if (*n_hooks >= (uint32_t)k_lsh_sites_max) {
      (void)fprintf(stderr, "  long-shift seam: site cap %u reached\n", (unsigned)k_lsh_sites_max);
      return false;
    }
    const uint64_t va             = (uint64_t)p_vaddr + (uint64_t)off;
    const uint64_t tail           = va + (uint64_t)k_lsh_insn_len;
    s_lsh_sites[s_lsh_site_count] = (uint32_t)va;
    s_lsh_site_count++;
    (void)uc_hook_add(uc,
                      &s_lsh_hooks[(size_t)(*n_hooks) * 2U],
                      UC_HOOK_CODE,
                      (void*)on_long_shift,
                      nullptr,
                      va,
                      va);
    (void)uc_hook_add(uc,
                      &s_lsh_hooks[((size_t)(*n_hooks) * 2U) + 1U],
                      UC_HOOK_CODE,
                      (void*)on_long_shift,
                      nullptr,
                      tail,
                      tail);
    (*n_hooks)++;
  }
  return true;
}

/**
 * @brief Scan the loaded image and install a hook at every immediate long-shift.
 *
 * @details
 * Walks the ELF32 PT_LOAD program headers, and for each executable segment scans
 * its bytes on 2-byte boundaries for the long-shift encoding (::long_shift_decode).
 * A targeted UC_HOOK_CODE is installed at each site's VMA, and at the following
 * instruction, so ::on_long_shift can emulate it. Matches use the segment's VMA
 * (p_vaddr), so a `.sram_text` ramfunc region is hooked at its execution address
 * even though it is not yet copied at install time. A scan false-positive (a
 * halfword pair inside data or mid-instruction that happens to match) is
 * harmless: the core never starts execution there, so the hook never fires, and
 * the seam never rewrites the image. Zero hooks -- hence zero steady-state cost
 * -- for firmware that contains no long shifts.
 *
 * @param[in,out] uc  Unicorn engine to install the hooks on.
 * @param[in]     elf In-memory ELF image (still alive at call time).
 * @param[in]     len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @post One UC_HOOK_CODE pair per long-shift site (up to ::k_lsh_sites_max) is armed.
 * @note Not thread-safe; call once during setup before the run loop.
 * @since 0.1.0
 */
void long_shift_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  /* Bound the program-header table against the file before walking it: a
   * malformed e_phoff/e_phnum would otherwise OOB-read past `elf`. load_elf has
   * already validated the ELF/ARM magic, so this only guards a truncated image. */
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return;
  }
  s_lsh_site_count = 0U;
  (void)memset(s_lsh_pending, 0, sizeof(s_lsh_pending));
  uint32_t n_hooks = 0U;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((size_t)(uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_offset = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + (uint32_t)k_elf_ph_offset_off, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz < (uint32_t)k_lsh_insn_len) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (((uint64_t)p_offset + (uint64_t)p_filesz) > (uint64_t)len) {
      continue; /* truncated / out-of-file segment -- skip. */
    }
    if (!install_seg_hooks(uc, elf, p_offset, p_vaddr, p_filesz, &n_hooks)) {
      return;
    }
  }
  if (n_hooks > 0U) {
    (void)fprintf(stderr,
                  "  long-shift seam: emulating %u Armv8.1-M LSLL/LSRL/ASRL site(s)\n",
                  (unsigned)n_hooks);
  }
}
