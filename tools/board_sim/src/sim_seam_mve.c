/**
 * @file sim_seam_mve.c
 * @brief Minimal MVE (Helium) emulation seam (see sim_seams.h)
 *
 * @details
 * The RA8D2 is Cortex-M85 (Armv8.1-M, has MVE) but the closest core Unicorn
 * offers is M33 (Armv8-M, NO MVE), so the Helium instructions GCC's
 * auto-vectoriser emits either trap as invalid (VMOV.I32) or silently decode
 * as legacy coprocessor stores (VSTRW.32). This seam emulates the handled
 * subset -- the invalid-instruction path consumes runs of trapped MVE ops,
 * and a one-time image scan hooks every VSTRW.32 site so the store happens
 * before the core reaches the mis-decoding instruction. Moved verbatim out
 * of the board_sim main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <capstone/capstone.h>
#include <stdio.h>
#include <string.h>

#include "sim_elf.h"
#include "sim_exc.h"
#include "sim_seams.h"

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
 * reports as invalid, so nothing is silently mis-executed. capstone (already
 * linked, and already decoding these for the error path) provides the operands;
 * the engine handle is opened once and reused. On real silicon all of this just
 * runs natively on Helium -- this only makes the M33-based sim faithful to it.
 * ==========================================================================*/
enum : uint32_t {
  k_mve_insn_len   = 4U,    /**< MVE instructions are 32-bit Thumb-2.          */
  k_mve_q_bytes    = 16U,   /**< Bytes in a Q (128-bit) register.              */
  k_mve_vstrw_pfx  = 5U,    /**< strncmp length for the "vstrw" prefix.        */
  k_mve_lane_shift = 32U,   /**< 32-bit lane width (two lanes per D register). */
  k_mve_max_run    = 4096U, /**< Loop bound: max consecutive MVE ops per trap. */
};
/** @brief Count of MVE instructions emulated this run (run-end telemetry). */
static uint64_t s_mve_emulated = 0U;

/** @brief Map capstone Q-reg @p qreg to its Unicorn D-register half (low/high). */
static int mve_q_d(unsigned int qreg, bool high)
{
  const unsigned int idx = qreg - (unsigned int)ARM_REG_Q0; /* Q index 0..7. */
  return (int)UC_ARM_REG_D0 + (int)(2U * idx) + (high ? 1 : 0);
}

/** @brief Execute one decoded MVE instruction (no PC change); true iff handled. */
static bool mve_exec_one(uc_engine* uc, const cs_insn* insn)
{
  const cs_arm* d     = &insn->detail->arm;
  const bool    op0_q = (d->op_count == 2) && (d->operands[0].type == ARM_OP_REG) &&
                        (d->operands[0].reg >= ARM_REG_Q0) && (d->operands[0].reg <= ARM_REG_Q7);
  if (!op0_q) {
    return false;
  }
  /* VMOV.I32 Qd, #imm -> replicate the 32-bit immediate into all four lanes. */
  if ((insn->id == ARM_INS_VMOV) && (d->operands[1].type == ARM_OP_IMM) &&
      (strstr(insn->mnemonic, ".i32") != nullptr)) {
    const uint32_t imm  = (uint32_t)d->operands[1].imm;
    const uint64_t pair = ((uint64_t)imm << (uint64_t)k_mve_lane_shift) | (uint64_t)imm;
    (void)uc_reg_write(uc, mve_q_d(d->operands[0].reg, false), &pair);
    (void)uc_reg_write(uc, mve_q_d(d->operands[0].reg, true), &pair);
    return true;
  }
  /* VSTRW.32 Qd, [Rn, #off] (no write-back) -> store the 16-byte vector. */
  if ((strncmp(insn->mnemonic, "vstrw", (size_t)k_mve_vstrw_pfx) == 0) && !d->writeback &&
      (d->operands[1].type == ARM_OP_MEM) && (d->operands[1].mem.base >= ARM_REG_R0) &&
      (d->operands[1].mem.base <= ARM_REG_R12)) {
    uint64_t lo = 0U;
    uint64_t hi = 0U;
    (void)uc_reg_read(uc, mve_q_d(d->operands[0].reg, false), &lo);
    (void)uc_reg_read(uc, mve_q_d(d->operands[0].reg, true), &hi);
    uint32_t  base   = 0U;
    const int base_r = (int)UC_ARM_REG_R0 + ((int)d->operands[1].mem.base - (int)ARM_REG_R0);
    (void)uc_reg_read(uc, base_r, &base);
    const uint32_t addr = base + (uint32_t)d->operands[1].mem.disp;
    uint8_t        buf[k_mve_q_bytes];
    (void)memcpy(buf, &lo, sizeof(lo));
    (void)memcpy(buf + sizeof(lo), &hi, sizeof(hi));
    (void)uc_mem_write(uc, (uint64_t)addr, buf, (size_t)k_mve_q_bytes);
    return true;
  }
  return false;
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
  static csh  s_cs;
  static bool s_cs_ok = false;
  if (!s_cs_ok) {
    if (cs_open(CS_ARCH_ARM, (cs_mode)(CS_MODE_THUMB | CS_MODE_MCLASS), &s_cs) != CS_ERR_OK) {
      return false;
    }
    (void)cs_option(s_cs, CS_OPT_DETAIL, CS_OPT_ON);
    s_cs_ok = true;
  }
  uint32_t pc = pc0;
  uint8_t  code[4];
  (void)memcpy(code, code0, sizeof(code));
  uint32_t handled = 0U;
  while (handled < (uint32_t)k_mve_max_run) {
    cs_insn*     insn = nullptr;
    const size_t n    = cs_disasm(s_cs, code, (size_t)k_mve_insn_len, pc, 1, &insn);
    if (n != 1U) {
      break;
    }
    const bool ok = mve_exec_one(uc, &insn[0]);
    cs_free(insn, n);
    if (!ok) {
      break; /* first non-MVE (valid) instruction -- relaunch resumes here. */
    }
    handled++;
    pc += (uint32_t)k_mve_insn_len;
    if (uc_mem_read(uc, (uint64_t)pc, code, sizeof(code)) != UC_ERR_OK) {
      break;
    }
  }
  if (handled > 0U) {
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    s_mve_emulated += handled;
  }
  return handled > 0U;
}

/* ---------------------------------------------------------------------------
 * VSTRW.32 seam. Unlike VMOV.I32 (which traps as invalid above), the MVE vector
 * store decodes as a *valid* legacy coprocessor store (`stc p15, ...`) on the
 * M33 core, so Unicorn would EXECUTE it -- faulting on the absent coprocessor --
 * rather than trap it. capstone mis-reads it the same way, so it cannot be
 * recognised in the invalid-instruction path. Instead, like the long-shift seam,
 * scan the loaded image for VSTRW.32 encodings and hook each site so the store
 * happens before Unicorn reaches the bad instruction.
 *
 * Encoding (Armv8.1-M, U=add): hw1 = 0xED8<Rn>, hw2 = 0bQd[15:13] 1111 0 imm7,
 * i.e. (hw1 & 0xFFF0)==0xED80 and (hw2 & 0x1F80)==0x1F00; the store offset is
 * imm7 * 4 (word-scaled). Verified against the toolchain's own disassembly.
 * ===========================================================================
 */
enum : uint32_t {
  k_mve_sites_max = 8192U,   /**< Cap on hooked VSTRW.32 sites per image.  */
  k_mve_vstrw_hw1 = 0xED80U, /**< VSTRW.32 hw1 fixed bits (Rn in [3:0]).   */
  k_mve_vstrw_h1m = 0xFFF0U, /**< Mask isolating the fixed hw1 bits.       */
  k_mve_vstrw_hw2 = 0x1F00U, /**< VSTRW.32 hw2 fixed bits (Qd in [15:13]). */
  k_mve_vstrw_h2m = 0x1F80U, /**< Mask isolating the fixed hw2 bits.       */
  k_mve_qd_shift  = 13U,     /**< Qd field position in hw2.                */
  k_mve_qd_mask   = 0x7U,    /**< Qd field width (3 bits) after shift.     */
  k_mve_rn_mask   = 0xFU,    /**< Rn field (4 bits) in hw1[3:0].           */
  k_mve_imm7_mask = 0x7FU,   /**< imm7 field (word offset) in hw2.         */
  k_mve_off_scale = 4U,      /**< VSTRW.32 immediate counts 4-byte words.  */
};
/** @brief One UC_HOOK_CODE per VSTRW.32 site (installed by mve_seam_install). */
static uc_hook s_mve_hooks[k_mve_sites_max];

/** @brief Decode VSTRW.32 Qd,[Rn,#imm7*4] (U=add); true iff hw1/hw2 match it. */
static bool mve_vstrw_decode(uint16_t hw1, uint16_t hw2, uint32_t* qd, uint32_t* rn, uint32_t* off)
{
  if (((hw1 & (uint16_t)k_mve_vstrw_h1m) != (uint16_t)k_mve_vstrw_hw1) ||
      ((hw2 & (uint16_t)k_mve_vstrw_h2m) != (uint16_t)k_mve_vstrw_hw2)) {
    return false;
  }
  *qd  = ((uint32_t)hw2 >> (uint32_t)k_mve_qd_shift) & (uint32_t)k_mve_qd_mask;
  *rn  = (uint32_t)hw1 & (uint32_t)k_mve_rn_mask;
  *off = ((uint32_t)hw2 & (uint32_t)k_mve_imm7_mask) * (uint32_t)k_mve_off_scale;
  return true;
}

/** @brief UC_HOOK_CODE at a VSTRW.32 site: store the 16-byte Qd, then relaunch. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_mve_vstrw(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  uint8_t code[4] = {};
  if (uc_mem_read(uc, address, code, sizeof(code)) != UC_ERR_OK) {
    return;
  }
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  uint32_t       qd  = 0U;
  uint32_t       rn  = 0U;
  uint32_t       off = 0U;
  if (!mve_vstrw_decode(hw1, hw2, &qd, &rn, &off)) {
    return; /* scan false-positive at a never-executed address -- leave it. */
  }
  uint64_t lo = 0U;
  uint64_t hi = 0U;
  (void)uc_reg_read(uc, (int)UC_ARM_REG_D0 + (int)(2U * qd), &lo);
  (void)uc_reg_read(uc, (int)UC_ARM_REG_D0 + (int)(2U * qd) + 1, &hi);
  uint32_t base = 0U;
  (void)uc_reg_read(uc, (int)UC_ARM_REG_R0 + (int)rn, &base);
  uint8_t buf[k_mve_q_bytes];
  (void)memcpy(buf, &lo, sizeof(lo));
  (void)memcpy(buf + sizeof(lo), &hi, sizeof(hi));
  (void)uc_mem_write(uc, (uint64_t)base + (uint64_t)off, buf, (size_t)k_mve_q_bytes);
  const uint32_t next = (uint32_t)address + (uint32_t)k_mve_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  s_mve_emulated++;
  (void)uc_emu_stop(uc);
}

/** @brief Scan the loaded image for VSTRW.32 sites and hook each one. */
void mve_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
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
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz < (uint32_t)k_mve_insn_len) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (((uint64_t)p_offset + (uint64_t)p_filesz) > (uint64_t)len) {
      continue;
    }
    for (uint32_t off = 0U; (off + (uint32_t)k_mve_insn_len) <= p_filesz; off += 2U) {
      const uint8_t* p   = elf + p_offset + off;
      const uint16_t hw1 = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
      const uint16_t hw2 = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
      uint32_t       qd  = 0U;
      uint32_t       rn  = 0U;
      uint32_t       o   = 0U;
      if (!mve_vstrw_decode(hw1, hw2, &qd, &rn, &o)) {
        continue;
      }
      if (n_hooks >= (uint32_t)k_mve_sites_max) {
        (void)fprintf(stderr, "  MVE seam: site cap %u reached\n", (unsigned)k_mve_sites_max);
        return;
      }
      const uint64_t va = (uint64_t)p_vaddr + (uint64_t)off;
      (void)
        uc_hook_add(uc, &s_mve_hooks[n_hooks], UC_HOOK_CODE, (void*)on_mve_vstrw, nullptr, va, va);
      n_hooks++;
    }
  }
  if (n_hooks > 0U) {
    (void)fprintf(stderr,
                  "  MVE seam      : hooking %u VSTRW.32 site(s) (M85 Helium the M33 lacks)\n",
                  (unsigned)n_hooks);
  }
}

/** @brief Implementation of `sim_mve_emulated_count()` -- plain counter read. */
uint64_t sim_mve_emulated_count(void)
{
  return s_mve_emulated;
}
