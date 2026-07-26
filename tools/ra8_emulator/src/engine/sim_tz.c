/**
 * @file sim_tz.c
 * @brief TrustZone S/NS seam implementation (see sim_tz.h)
 *
 * @details
 * The NS vector-base tracking, the hand-emulated BLXNS world switch, the
 * SAU_TYPE seed + BLXNS scan installer, and the cmse_check_address_range
 * BX-LR patch -- moved verbatim out of the board_sim main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_tz.h"

#include <stdio.h>

#include "sim_console.h"
#include "sim_elf.h"
#include "sim_exc.h"

/**
 * @brief Fallback NS vector-table base for the BLXNS world switch.
 * @details ::on_blxns first reads the live VTOR_NS word (the Secure boot's
 *          ``ra8_tz_secure_boot_jump_ns`` stores the NS vector base to the
 *          0xE002ED08 alias -- plain PPB RAM here -- right before its BLXNS),
 *          so a single-image TZ app whose NS half lives at its MRAM LMA
 *          (cpu1_pingpong_ipc: 0x02080000) and a two-image app whose NS image
 *          was copied to the SRAM2 run alias both resolve without flags. This
 *          fallback covers a zero VTOR_NS: it defaults to the RAM-resident NS
 *          run alias (@ref k_ns_sram2_base) and is overridden at `--ns` load
 *          to the loaded NS image's minimum PT_LOAD p_vaddr, so an XIP NS
 *          image linked at the OSPI window (0x90000000, VMA == LMA, no copy)
 *          still transitions correctly.
 * @note Single-threaded; set once before the run loop and read in ::on_blxns.
 * @since 0.1.0
 */
static uint32_t s_ns_vector_base = (uint32_t)k_ns_sram2_base;

/** @brief SCB VTOR_NS alias address (ARMv8-M B3.2.4; Secure-state view). */
typedef enum : uint64_t {
  k_scb_vtor_ns_addr = 0xE002ED08UL, /**< VTOR_NS: NS vector-table base. */
} vtor_ns_addr_t;

/**
 * @enum blxns_op_t
 * @brief Thumb encoding of the BLXNS instruction (scanned in jump_ns).
 * @details BLXNS Rm = 0x4780 | (Rm << 3) | 0x04; masking with k_blxns_mask
 *          isolates the fixed bits (0x4784) so any Rm matches.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_blxns_mask  = 0xFF87U, /**< Mask isolating the BLXNS fixed bits. */
  k_blxns_match = 0x4784U, /**< BLXNS fixed-bit pattern (any Rm).    */
} blxns_op_t;

/**
 * @brief UC_HOOK_CODE at the Secure->NS BLXNS -- hand-emulate the world switch.
 *
 * @details Unicorn's emulated M33 is all-Secure with no IDAU, so the real BLXNS
 *          in ra8_tz_secure_boot_jump_ns cannot transition to the Non-Secure
 *          world (it stalls / wanders). This hook fires on that instruction and
 *          performs the switch by hand in board_sim's single flat domain: it
 *          reads the NS initial MSP (NS vector[0]) and the NS reset handler (NS
 *          vector[1]) from the NS run base, sets SP + PC to them (Thumb bit
 *          masked), and stops the chunk so the run loop resumes executing the NS
 *          reset handler -- ThreadX and the e-reader threads then run directly.
 *          Mirrors the existing SG-stub-by-address TrustZone workaround.
 *
 * @param[in] uc      Unicorn engine mid-chunk at the BLXNS.
 * @param[in] address The BLXNS instruction address; unused.
 * @param[in] size    Instruction size in bytes; unused.
 * @param[in] user    Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The NS vectors are live at @ref s_ns_vector_base -- either copied to the
 *      RAM run alias by the Secure boot, or XIP-resident in the OSPI window.
 * @pre The hook is registered only for the jump_ns BLXNS site (under --ns).
 * @post SP = NS MSP, PC = NS reset handler, and the chunk is stopped.
 * @post The next run-loop chunk executes the Non-Secure reset handler.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void on_blxns(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  /* Prefer the live VTOR_NS: ra8_tz_secure_boot_jump_ns stores the NS vector
   * base to the 0xE002ED08 alias (plain PPB RAM here) right before its BLXNS,
   * so this resolves the NS table wherever the app placed it (MRAM-resident
   * 0x02080000, SRAM2 run alias 0x32100000, or OSPI XIP) with no per-app
   * knowledge. Fall back to ::s_ns_vector_base when the app never wrote it. */
  uint32_t vector_base = 0U;
  (void)uc_mem_read(uc, (uint64_t)k_scb_vtor_ns_addr, &vector_base, sizeof(vector_base));
  if (vector_base == 0U) {
    vector_base = s_ns_vector_base;
  }
  uint32_t ns_msp   = 0U;
  uint32_t ns_reset = 0U;
  (void)uc_mem_read(uc, (uint64_t)vector_base, &ns_msp, sizeof(ns_msp));
  (void)uc_mem_read(uc,
                    (uint64_t)vector_base + (uint64_t)sizeof(uint32_t),
                    &ns_reset,
                    sizeof(ns_reset));
  const uint32_t ns_pc = ns_reset & ~1U; /* mask the Thumb bit for the PC write */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &ns_msp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &ns_pc);
  (void)uc_emu_stop(uc);
}

/**
 * @brief Scan a Thumb function image for its first BLXNS instruction.
 *
 * @details
 * Walks @p size bytes from @p from a halfword at a time, reading each through
 * Unicorn (the image is already loaded there by load_elf) and matching it
 * against the BLXNS encoding. Only the first match matters: the secure boot's
 * jump routine issues exactly one.
 *
 * @param[in] uc   Unicorn engine holding the loaded image.
 * @param[in] from Function entry address.
 * @param[in] size Function size in bytes, from the ELF symbol table.
 * @return Address of the first BLXNS, or 0 when the image contains none.
 * @retval 0 No BLXNS in the scanned range.
 * @pre @p uc is non-null.
 * @pre @p size is at least ::k_thumb_hw_bytes, so one halfword is readable.
 * @post Emulated memory is unchanged (reads only).
 * @post Any returned address lies within [@p from, @p from + @p size).
 * @note Not thread-safe.
 */
static uint32_t sim_tz_find_blxns(uc_engine* uc, uint32_t from, uint32_t size)
{
  for (uint32_t a = from; (a + (uint32_t)k_thumb_hw_bytes) <= (from + size);
       a += (uint32_t)k_thumb_hw_bytes) {
    uint16_t hw = 0U;
    (void)uc_mem_read(uc, (uint64_t)a, &hw, sizeof(hw));
    if (((uint32_t)hw & (uint32_t)k_blxns_mask) == (uint32_t)k_blxns_match) {
      return a;
    }
  }
  return 0U;
}

/** @brief Implementation of `sim_tz_install()` -- SAU seed + BLXNS scan/hook. */
void sim_tz_install(uc_engine* uc, const uint8_t* elf, long elf_len)
{
  /* TrustZone S->NS boot seams -- armed whenever the firmware links the secure
   * boot's ra8_tz_secure_boot_jump_ns: a two-image --ns app, OR a single-image
   * app whose NS half is embedded at its MRAM LMA (cpu1_pingpong_ipc). The
   * Secure boot bails to its fallback main() unless SAU_TYPE.SREGION >= 4/5;
   * board_sim maps the PPB as plain RAM (SAU_TYPE reads 0), so seed the M85's
   * 8-region count to let the real SAU programming + NS-image copy + BLXNS
   * run. Firmware without the symbol keeps its current (all-Secure) path. */
  uint32_t       jn_size = 0U;
  const uint32_t jump_ns = elf_sym_addr(elf, elf_len, "ra8_tz_secure_boot_jump_ns", &jn_size);
  if ((jump_ns == 0U) || (jn_size < (uint32_t)k_thumb_hw_bytes)) {
    return; /* Not a TrustZone image: keep the all-Secure path. */
  }
  const uint32_t sau_type = (uint32_t)k_sau_type_regs;
  (void)uc_mem_write(uc, (uint64_t)k_sau_type_addr, &sau_type, sizeof(sau_type));

  /* Hand-emulate the Secure->NS BLXNS in ra8_tz_secure_boot_jump_ns.
   * Unicorn's all-Secure M33 cannot really switch worlds, so resolve the
   * BLXNS site from the Secure symtab (scan the function for the BLXNS
   * opcode) and hook it to enter NS manually (see on_blxns). Without this
   * the BLXNS stalls and the NS world never runs. */
  const uint32_t blxns_at = sim_tz_find_blxns(uc, jump_ns, jn_size);
  if (blxns_at == 0U) {
    (void)fprintf(stderr, "board_sim: TZ warning: no BLXNS found in ra8_tz_secure_boot_jump_ns\n");
    return;
  }
  uc_hook h_blxns;
  (void)uc_hook_add(uc,
                    &h_blxns,
                    UC_HOOK_CODE,
                    (void*)on_blxns,
                    nullptr,
                    (uint64_t)blxns_at,
                    (uint64_t)blxns_at);
  (void)fprintf(stderr, "board_sim: TZ BLXNS seam armed @ 0x%08X\n", blxns_at);
}

/** @brief Implementation of `sim_tz_patch_cmse()` -- flat-domain range check. */
void sim_tz_patch_cmse(uc_engine* uc, const uint8_t* elf, long elf_len)
{
  /* TrustZone NSC pointer validation. The Non-Secure-Callable veneers guard
   * their pointer args with cmse_check_address_range(), which issues Armv8-M
   * `TT`/`TTA` (Test Target) instructions to read an address's security/MPU
   * attribution and then checks the Non-Secure read/write bit. Unicorn's M33
   * has no SAU/IDAU configured (board_sim maps the PPB as plain RAM, so the
   * core's internal SAU stays at its reset all-Secure state); a native TT thus
   * reports every address as Secure, the NS range-check fails, and the veneer
   * returns k_ra8_err_invalid_arg -- stalling CGC/SD bring-up. board_sim collapses
   * the Secure/Non-Secure split into one flat, fully-accessible domain, so every
   * NS pointer the veneers pass (each already null-checked before the range check)
   * is valid. Model that by patching the routine's entry to `BX LR`: r0 still
   * holds the first argument (the pointer `p`) at entry, so an immediate return
   * yields p != NULL == "address OK". This is a one-time 2-byte memory patch
   * (the function image is already copied into Unicorn memory by load_elf), not a
   * UC_HOOK_CODE -- a code hook forces Unicorn to single-step the whole run
   * (~10x slower), whereas the patch has zero steady-state cost. Absent in
   * non-TZ firmware (symbol not found -> no patch). */
  const uint32_t cmse_check_addr = elf_sym_addr(elf, elf_len, "cmse_check_address_range", nullptr);
  if (cmse_check_addr != 0U) {
    const uint16_t bx_lr = (uint16_t)k_thumb_bx_lr;
    (void)uc_mem_write(uc, (uint64_t)cmse_check_addr, &bx_lr, sizeof(bx_lr));
  }
}

/** @brief Implementation of `sim_tz_set_ns_vector_base()` -- --ns override. */
void sim_tz_set_ns_vector_base(uint32_t base)
{
  s_ns_vector_base = base;
}

/** @brief Implementation of `sim_tz_ns_vector_base()` -- plain state read. */
uint32_t sim_tz_ns_vector_base(void)
{
  return s_ns_vector_base;
}
