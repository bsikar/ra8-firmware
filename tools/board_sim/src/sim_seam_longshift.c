/**
 * @file sim_seam_longshift.c
 * @brief Armv8.1-M long-shift (LSLL/LSRL/ASRL) emulation seam (see sim_seams.h)
 *
 * @details
 * Same M85-vs-M33 gap as the other seams, but with a sharper failure mode:
 * the M33 does NOT trap these -- their encoding overlaps ORR.W (register),
 * so the core silently mis-executes the shift and yields a wrong 64-bit
 * result with no fault. A one-time image scan finds every immediate
 * long-shift site and hooks it so the shift is applied on the host (where
 * 64-bit arithmetic is correct) before the core reaches the bad encoding.
 * Moved verbatim out of the board_sim main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "sim_elf.h"
#include "sim_engine.h"
#include "sim_exc.h"
#include "sim_seams.h"

/* Armv8.1-M long-shift family (LSLL/LSRL/ASRL, immediate). Same M85-vs-M33 gap
 * as the conditional-selects above, but with a sharper failure mode: the M33
 * does NOT trap these. Their encoding overlaps ORR.W (register), so the core
 * silently mis-executes the shift and yields a wrong 64-bit result with no
 * fault -- there is no invalid-instruction hook to catch them. GCC emits them
 * for any 64-bit shift that crosses the word boundary (e.g. miniz's ZIP
 * central-directory size math: `(u64)entries * 46` then `>> 32`), so the silent
 * miscompile corrupts the parse. They are instead found by a one-time image scan
 * and emulated at each site (see long_shift_seam_install / on_long_shift).
 * Encoding (T1, two halfwords, little-endian): hw1 = 0xEA5n (n = RdaLo);
 * hw2[3:0] = 0b1111, op = bits[5:4] (0=LSLL, 1=LSRL, 2=ASRL), RdaHi = bits[11:8],
 * imm5 = bits[14:12]:bits[7:6] (0 encodes a shift of 32). Verified against the
 * GNU assembler for armv8.1-m.main. */
typedef enum : uint32_t {
  k_lsh_hw1_mask     = 0xFFF0U, /**< hw1[15:4] selects the group; RdaLo in [3:0]. */
  k_lsh_hw1_match    = 0xEA50U, /**< hw1[15:4] == 0xEA5 for this family.          */
  k_lsh_hw2_lo_mask  = 0x000FU, /**< hw2[3:0] fixed pattern for the imm form.     */
  k_lsh_hw2_lo_match = 0x000FU, /**< hw2[3:0] == 0b1111.                          */
  k_lsh_op_shift     = 4U,      /**< op field at hw2[5:4].                        */
  k_lsh_op_mask      = 0x3U,    /**< op width (value 3 is reserved -> rejected).  */
  k_lsh_op_lsll      = 0U,      /**< 00: 64-bit logical shift left.               */
  k_lsh_op_lsrl      = 1U,      /**< 01: 64-bit logical shift right.              */
  k_lsh_op_asrl      = 2U,      /**< 10: 64-bit arithmetic shift right.           */
  k_lsh_rdahi_shift  = 8U,      /**< RdaHi at hw2[11:8].                          */
  k_lsh_imm3_shift   = 12U,     /**< imm3 at hw2[14:12].                          */
  k_lsh_imm3_mask    = 0x7U,    /**< imm3 width.                                  */
  k_lsh_imm2_shift   = 6U,      /**< imm2 at hw2[7:6].                            */
  k_lsh_imm2_mask    = 0x3U,    /**< imm2 width.                                  */
  k_lsh_imm3_to_imm5 = 2U,      /**< imm5 = (imm3 << 2) | imm2.                   */
  k_lsh_word_bits    = 32U,     /**< Word width; also the imm5==0 shift amount.   */
  k_lsh_insn_len     = 4U,      /**< Thumb-2 instruction length, bytes.           */
} long_shift_t;

/**
 * @brief Decode an Armv8.1-M immediate long-shift (LSLL/LSRL/ASRL) if present.
 *
 * @details
 * Validates the two-halfword encoding (see ::long_shift_t) and, on a match,
 * extracts the destination register pair, the operation, and the shift amount
 * (imm5, where 0 encodes 32). Shared by the image scan that installs the
 * per-site hooks and by the hook that emulates one at run time.
 *
 * @param[in]  hw1   First (low-address) instruction halfword.
 * @param[in]  hw2   Second instruction halfword.
 * @param[out] rdalo Receives the low destination register index (0..12).
 * @param[out] rdahi Receives the high destination register index (0..12).
 * @param[out] op    Receives the op (::k_lsh_op_lsll / _lsrl / _asrl).
 * @param[out] shift Receives the shift amount in bits (1..32).
 * @return true if @p hw1/@p hw2 form an immediate long shift; false otherwise.
 * @pre All out-params are non-null.
 * @post On true, every out-param is written; on false, none are.
 */
static bool long_shift_decode(uint16_t  hw1,
                              uint16_t  hw2,
                              uint32_t* rdalo,
                              uint32_t* rdahi,
                              uint32_t* op,
                              uint32_t* shift)
{
  if (((hw1 & (uint16_t)k_lsh_hw1_mask) != (uint16_t)k_lsh_hw1_match) ||
      ((hw2 & (uint16_t)k_lsh_hw2_lo_mask) != (uint16_t)k_lsh_hw2_lo_match)) {
    return false;
  }
  const uint32_t o = ((uint32_t)hw2 >> (uint32_t)k_lsh_op_shift) & (uint32_t)k_lsh_op_mask;
  if (o > (uint32_t)k_lsh_op_asrl) {
    return false; /* op == 0b11 is reserved -- not a long shift. */
  }
  const uint32_t imm3 = ((uint32_t)hw2 >> (uint32_t)k_lsh_imm3_shift) & (uint32_t)k_lsh_imm3_mask;
  const uint32_t imm2 = ((uint32_t)hw2 >> (uint32_t)k_lsh_imm2_shift) & (uint32_t)k_lsh_imm2_mask;
  const uint32_t imm5 = (imm3 << (uint32_t)k_lsh_imm3_to_imm5) | imm2;
  *rdalo              = (uint32_t)hw1 & (uint32_t)k_lo4_mask;
  *rdahi              = ((uint32_t)hw2 >> (uint32_t)k_lsh_rdahi_shift) & (uint32_t)k_lo4_mask;
  *op                 = o;
  *shift              = (imm5 == 0U) ? (uint32_t)k_lsh_word_bits : imm5;
  return true;
}

/**
 * @brief UC_HOOK_CODE: emulate one immediate long-shift, then relaunch past it.
 *
 * @details
 * Reads the 4 instruction bytes at @p address, decodes the LSLL/LSRL/ASRL (see
 * ::long_shift_decode), forms the 64-bit {RdaHi:RdaLo} operand, applies the
 * shift on the host -- where 64-bit arithmetic is correct -- writes the result
 * back to the register pair, advances PC past the 4-byte instruction, and stops
 * the engine so the chunked run loop resumes at the new PC. This is the same
 * stop-and-relaunch contract the eth/usb seams use; editing PC and continuing
 * in-place corrupts Unicorn's block/Thumb state. The mis-decoding instruction is
 * therefore never executed by the core.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     address Address of the long-shift instruction.
 * @param[in]     size    Reported instruction size (unused).
 * @param[in]     user    Hook user pointer (unused).
 * @return Nothing.
 * @pre @p address holds an immediate long shift (the scan installed this hook).
 * @post PC has advanced one 32-bit instruction and the engine is stopped.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_long_shift(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  uint8_t code[4] = {};
  if (uc_mem_read(uc, address, code, sizeof(code)) != UC_ERR_OK) {
    return;
  }
  const uint16_t hw1   = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2   = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  uint32_t       rdalo = 0U;
  uint32_t       rdahi = 0U;
  uint32_t       op    = 0U;
  uint32_t       shift = 0U;
  if (!long_shift_decode(hw1, hw2, &rdalo, &rdahi, &op, &shift)) {
    return; /* scan false-positive at a never-executed address: leave it alone. */
  }
  uint32_t lo = 0U;
  uint32_t hi = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[rdalo], &lo);
  (void)uc_reg_read(uc, k_arm_reg_id[rdahi], &hi);
  const uint64_t val = ((uint64_t)hi << (uint64_t)k_lsh_word_bits) | (uint64_t)lo;
  uint64_t       res = 0U;
  switch (op) {
    case (uint32_t)k_lsh_op_lsll:
      res = val << shift;
      break;
    case (uint32_t)k_lsh_op_asrl:
      res = (uint64_t)((int64_t)val >> shift);
      break;
    case (uint32_t)k_lsh_op_lsrl:
    default:
      res = val >> shift;
      break;
  }
  lo = (uint32_t)res;
  hi = (uint32_t)(res >> (uint64_t)k_lsh_word_bits);
  (void)uc_reg_write(uc, k_arm_reg_id[rdalo], &lo);
  (void)uc_reg_write(uc, k_arm_reg_id[rdahi], &hi);
  uint32_t next = (uint32_t)address + (uint32_t)k_lsh_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  (void)uc_emu_stop(uc); /* relaunch past the long shift (chunk contract). */
}

/** @brief Max long-shift sites hooked per image (generous; real counts tiny). */
enum : uint32_t {
  k_lsh_sites_max = 4096U, /**< Lsh sites maximum. */
};
/** @brief Hook handles for the installed long-shift sites (kept for the run). */
static uc_hook s_lsh_hooks[k_lsh_sites_max];

/**
 * @brief Scan one executable segment for long-shift sites, arming a hook each.
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
    const uint8_t* p     = elf + p_offset + off;
    const uint16_t hw1   = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    const uint16_t hw2   = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
    uint32_t       rdalo = 0U;
    uint32_t       rdahi = 0U;
    uint32_t       op    = 0U;
    uint32_t       shift = 0U;
    if (!long_shift_decode(hw1, hw2, &rdalo, &rdahi, &op, &shift)) {
      continue;
    }
    if (*n_hooks >= (uint32_t)k_lsh_sites_max) {
      (void)fprintf(stderr, "  long-shift seam: site cap %u reached\n", (unsigned)k_lsh_sites_max);
      return false;
    }
    const uint64_t va = (uint64_t)p_vaddr + (uint64_t)off;
    (void)
      uc_hook_add(uc, &s_lsh_hooks[*n_hooks], UC_HOOK_CODE, (void*)on_long_shift, nullptr, va, va);
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
 * A targeted UC_HOOK_CODE is installed at each site's VMA so ::on_long_shift can
 * emulate it. Matches use the segment's VMA (p_vaddr), so a `.sram_text` ramfunc
 * region is hooked at its execution address even though it is not yet copied at
 * install time. A scan false-positive (a halfword pair inside data or mid-
 * instruction that happens to match) is harmless: the core never starts execution
 * there, so the hook never fires. Zero hooks -- hence zero steady-state cost --
 * for firmware that contains no long shifts.
 *
 * @param[in,out] uc  Unicorn engine to install the hooks on.
 * @param[in]     elf In-memory ELF image (still alive at call time).
 * @param[in]     len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @post One UC_HOOK_CODE per long-shift site (up to ::k_lsh_sites_max) is armed.
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
