/**
 * @file examples/ra_bootloader/main.c
 * @brief Minimal A/B bank-switch bootloader stub for RA8D2 OTA
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * The RA8D2 has 1 MiB of MRAM organised as two 512 KiB banks for
 * safe A/B firmware updates. This bootloader runs first out of
 * reset, reads a single bank-config word from a fixed MRAM
 * location, picks the active bank base address (``0x02000000`` for
 * bank A, ``0x02080000`` for bank B), validates the bank's vector
 * table, sets MSP from ``vector_table[0]`` of that bank, then
 * branches to ``vector_table[1]`` (the bank's ``Reset_Handler``).
 *
 * @par Bank-config word
 * The bank-config word lives at ``k_ra_boot_bankcfg_addr``. Its low
 * byte selects the active bank:
 * - ``0x00`` / ``0xFF`` (erased) -> bank A (default).
 * - ``0xA5`` -> bank B.
 *
 * Any other byte value falls back to bank A. A real OTA stack would
 * additionally check a CRC and a roll-back counter, but for this
 * stub the single byte is enough to demonstrate the dispatch.
 *
 * @par Hand-off sequence
 * The hand-off mirrors what the Cortex-M85 boot ROM does at reset:
 *   1. Disable IRQs (``cpsid i``).
 *   2. Read ``app_msp = *(uint32_t*)bank_base``.
 *   3. Read ``app_reset = *(uint32_t*)(bank_base + 4)``.
 *   4. Re-point ``VTOR`` at ``bank_base``.
 *   5. ``msr msp, app_msp``.
 *   6. ``bx app_reset``.
 *
 * @par Architectural ring
 * See ``docs/RING_AND_WORLD.md`` for what
 * ``[Ring 1 / Boot] {World: S}`` means -- short version: this is
 * the first code that runs out of reset and stays in the secure
 * world the entire time.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-01
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/* =============================================================================
 * Bank layout
 * =============================================================================
 */

/**
 * @enum ra_boot_bank_addr_t
 * @brief Fixed MRAM addresses for the bootloader / A/B layout.
 *
 * @details
 * The RA8D2 1 MiB MRAM is split into two 512 KiB banks. The
 * bootloader image sits in the first 16 KiB of bank A; the rest of
 * bank A and the whole of bank B hold the two application slots.
 * The single bank-config byte at ``k_ra_boot_bankcfg_addr`` selects
 * which slot the bootloader hands control to.
 */
typedef enum : uintptr_t {
  k_ra_boot_bank_a_base    = 0x02000000UL, /**< Bank A vector table base. */
  k_ra_boot_bank_b_base    = 0x02080000UL, /**< Bank B vector table base. */
  k_ra_boot_bankcfg_addr   = 0x02003F00UL, /**< Bank-config word (within
                                                 boot region, before bank-A
                                                 app vectors at 0x02004000). */
  k_ra_boot_scb_vtor_addr  = 0xE000ED08UL, /**< SCB.VTOR -- vector offset.   */
  k_ra_boot_mram_origin    = 0x02000000UL, /**< Start of MRAM.               */
  k_ra_boot_mram_end       = 0x02100000UL, /**< End of 1 MiB MRAM (exclusive)*/
  k_ra_boot_sram_origin    = 0x22000000UL, /**< Start of ECC SRAM.           */
  k_ra_boot_sram_end       = 0x22200000UL, /**< End of 2 MiB SRAM (exclusive)*/
} ra_boot_bank_addr_t;

/**
 * @enum ra_boot_bankcfg_t
 * @brief Magic values for the bank-config selector byte.
 */
typedef enum : uint8_t {
  k_ra_boot_select_bank_a = 0x00U, /**< Default / erased -> bank A.         */
  k_ra_boot_select_bank_b = 0xA5U, /**< Magic value -> bank B.              */
  k_ra_boot_select_erased = 0xFFU, /**< Erased MRAM -> bank A.              */
} ra_boot_bankcfg_t;

/* =============================================================================
 * Tiny register accessors (no HAL dependency, NASA Rule 8 compliant)
 * =============================================================================
 */

static inline uint32_t internal_read32(uintptr_t addr)
{
  return *(volatile const uint32_t*)addr;
}

static inline uint8_t internal_read8(uintptr_t addr)
{
  return *(volatile const uint8_t*)addr;
}

static inline void internal_write32(uintptr_t addr, uint32_t value)
{
  *(volatile uint32_t*)addr = value;
}

/* =============================================================================
 * Bank selection + validation
 * =============================================================================
 */

/**
 * @brief Read the bank-config byte and pick the active bank base.
 *
 * @details
 * Reads the low byte of the bank-config word and returns the matching
 * bank base address. Anything other than ``k_ra_boot_select_bank_b``
 * falls back to bank A (the default-deny choice -- the previously
 * known-good slot).
 *
 * @return Base address of the bank to boot.
 * @retval k_ra_boot_bank_a_base Default / erased / unknown selector.
 * @retval k_ra_boot_bank_b_base Selector byte == 0xA5.
 *
 * @pre Bootloader has not yet enabled the D-cache (so the read is
 *      coherent with whatever the OTA writer flashed).
 * @post Returned address is one of the two known bank bases.
 *
 * @note Not thread-safe; runs once at boot.
 */
static uintptr_t internal_pick_active_bank(void)
{
  const uint8_t selector = internal_read8(k_ra_boot_bankcfg_addr);
  if (selector == (uint8_t)k_ra_boot_select_bank_b) {
    return (uintptr_t)k_ra_boot_bank_b_base;
  }
  return (uintptr_t)k_ra_boot_bank_a_base;
}

/**
 * @brief Sanity-check the candidate vector table before jumping.
 *
 * @details
 * Rejects banks whose initial MSP does not point into SRAM, or whose
 * Reset_Handler does not point into MRAM. This is the bare-minimum
 * sanity guard that prevents a fully-erased bank (vector words ==
 * 0xFFFFFFFF) from being entered.
 *
 * @param[in] bank_base Candidate bank vector-table base address.
 *
 * @return 1 if the candidate looks bootable, 0 otherwise.
 * @retval 1 MSP is in SRAM and Reset_Handler is in MRAM.
 * @retval 0 Erased bank or out-of-range vector words.
 *
 * @pre bank_base equals one of the two documented bank addresses.
 * @post Returns 0 for an all-ones erased bank.
 *
 * @note Not thread-safe; runs once at boot.
 */
static uint32_t internal_bank_is_valid(uintptr_t bank_base)
{
  const uint32_t app_msp   = internal_read32(bank_base + 0U);
  const uint32_t app_reset = internal_read32(bank_base + 4U);

  if (app_msp < (uint32_t)k_ra_boot_sram_origin) {
    return 0U;
  }
  if (app_msp > (uint32_t)k_ra_boot_sram_end) {
    return 0U;
  }
  if (app_reset < (uint32_t)k_ra_boot_mram_origin) {
    return 0U;
  }
  if (app_reset >= (uint32_t)k_ra_boot_mram_end) {
    return 0U;
  }
  return 1U;
}

/* =============================================================================
 * Hand-off: jump to the application bank
 * =============================================================================
 */

/**
 * @brief Re-point VTOR, set MSP, branch to the application Reset_Handler.
 *
 * @details
 * Performs the standard ARMv8-M boot hand-off. Marked ``noreturn``
 * because control never comes back -- the application's
 * ``Reset_Handler`` will eventually call its own ``main()``.
 *
 * @param[in] bank_base Base address of the active bank's vector table.
 *
 * @pre IRQs are masked.
 * @pre bank_base passed ``internal_bank_is_valid``.
 * @post Control transfers to the application; this function does not
 *       return.
 *
 * @note Not thread-safe; runs once at boot.
 * @warning Must be the very last call -- nothing after the
 *          ``bx`` instruction executes.
 */
__attribute__((noreturn))
static void internal_jump_to_bank(uintptr_t bank_base)
{
  const uint32_t app_msp   = internal_read32(bank_base + 0U);
  const uint32_t app_reset = internal_read32(bank_base + 4U);

  /* Re-point VTOR so the application sees its own vector table. */
  internal_write32((uintptr_t)k_ra_boot_scb_vtor_addr, (uint32_t)bank_base);

#ifndef RA_SIMULATOR_MODE
  __asm__ volatile("dsb 0xF\n"
                   "isb 0xF\n"
                   "msr msp, %0\n"
                   "bx  %1\n"
                   :
                   : "r"(app_msp), "r"(app_reset)
                   : "memory");
  __builtin_unreachable();
#else
  /* Host build never gets here; keep the symbols referenced. */
  (void)app_msp;
  (void)app_reset;
  while (1) {
    /* spin */
  }
#endif
}

/* =============================================================================
 * Entry
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Bootloader entry point.
 *
 * @details
 * Picks the active bank, validates it, falls back to the other bank
 * if the primary is unbootable, and jumps. If neither bank passes
 * the validation check the bootloader spins -- a real product
 * would jump to a recovery / DFU path instead.
 *
 * @return Never returns on the embedded target.
 * @retval 0 Only on the host syntax-check build.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post Either control transfers to an application bank, or the
 *       bootloader spins forever.
 *
 * @note Not thread-safe; runs once at boot.
 */
int32_t main(void)
{
#ifndef RA_SIMULATOR_MODE
  __asm__ volatile("cpsid i" ::: "memory");
#endif

  uintptr_t primary  = internal_pick_active_bank();
  uintptr_t fallback = (primary == (uintptr_t)k_ra_boot_bank_a_base)
                         ? (uintptr_t)k_ra_boot_bank_b_base
                         : (uintptr_t)k_ra_boot_bank_a_base;

  if (internal_bank_is_valid(primary) != 0U) {
    internal_jump_to_bank(primary);
  }
  if (internal_bank_is_valid(fallback) != 0U) {
    internal_jump_to_bank(fallback);
  }

  /* Both banks unbootable -- spin so a debugger can attach. */
  while (1) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("wfi");
#endif
  }

  return 0;
}
#pragma GCC diagnostic pop
