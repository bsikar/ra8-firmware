/**
 * @file ra8d2_pfs_regs.h
 * @brief Pin Function Select (PFS) register layout for the Renesas RA8D2
 *
 * @details
 * Every GPIO pin on an RA8D2 has a 32-bit `PmnPFS` register inside the
 * PFS block at `0x40400800`. The register controls direction, output
 * level, pull-up, open-drain, drive strength, IRQ/analog enable and
 * (most importantly) which peripheral function is routed to the pin.
 *
 * Pins are addressed by a flat `(port * 16) + pin` index; each entry
 * is 4 bytes. The whole PFS array covers 15 ports x 16 pins x 4 bytes
 * = 960 bytes.
 *
 * ## Register field layout (PmnPFS, 32-bit)
 *
 * | Bits    | Name  | RW | Purpose                                  |
 * |--------:|-------|----|------------------------------------------|
 * | 0       | PODR  | RW | Port output data                          |
 * | 1       | PIDR  | R  | Port input data                           |
 * | 2       | PDR   | RW | Port direction (0 = in, 1 = out)          |
 * | 4       | PCR   | RW | Pull-up control                           |
 * | 6       | NCODR | RW | N-channel open-drain                      |
 * | 11:10   | DSCR  | RW | Drive strength                            |
 * | 13:12   | EOFR  | RW | Event on falling / rising trigger control |
 * | 14      | ISEL  | RW | IRQ input enable                          |
 * | 15      | ASEL  | RW | Analog input enable                       |
 * | 16      | PMR   | RW | Peripheral mode (0 = GPIO, 1 = peripheral) |
 * | 28:24   | PSEL  | RW | Peripheral function select (0..31)        |
 *
 * ## PWPR unlock sequence (MANDATORY)
 *
 * `PmnPFS` writes are gated by the `PWPR` (Write Protect Register).
 * The sequence is:
 *
 * @code{.c}
 *   ra_pfs_pwpr_unlock();                        // clears B0WI, sets PFSWE
 *   ra_pfs_pmn(port, pin)->PmnPFS = new_value;   // programme pin
 *   ra_pfs_pwpr_lock();                          // clears PFSWE
 * @endcode
 *
 * Skipping the unlock makes the write silently go nowhere.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_bit_constants.h"
#include "ra_port_constants.h"

/* =============================================================================
 * Base addresses + layout
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_pfs_base_addr   = 0x40400800UL, /**< PFS array base.                      */
  k_ra_pmisc_base_addr = 0x40400D00UL, /**< PMISC: contains PWPR and PWPRS.    */
} ra_pfs_addr_t;

typedef enum : uint16_t {
  k_ra_pmisc_off_pfenet = 0x000U, /**< PFENET: Ethernet Mode Select.          */
  k_ra_pmisc_off_pwpr   = 0x00CU, /**< PWPR: PFS Write Protect (HUM 20.2.5). */
  k_ra_pmisc_off_pwprs  = 0x014U, /**< PWPRS: Secure PFS Write Protect.       */
  k_ra_pmisc_off_pmsar  = 0x030U, /**< PMSAR[0..14]: Port Security Attribution. */
} ra_pmisc_off_t;

/* =============================================================================
 * Packed PFS entry -- one per pin
 * =============================================================================
 *
 * Each pin has a 32-bit register. We expose it as a volatile uint32_t*
 * rather than a bitfield union because bitfield writes are
 * implementation-defined for write-protected registers: GCC may emit
 * a read-modify-write that hits the protected read path. A plain
 * 32-bit write is guaranteed to do one store.
 */

/**
 * @enum ra_pfs_bit_t
 * @brief Bit positions inside the 32-bit `PmnPFS` register.
 */
typedef enum : uint8_t {
  k_ra_pfs_bit_podr  = 0U,  /**< Output data.                 */
  k_ra_pfs_bit_pidr  = 1U,  /**< Input data (read-only).      */
  k_ra_pfs_bit_pdr   = 2U,  /**< Direction (0=in, 1=out).     */
  k_ra_pfs_bit_pcr   = 4U,  /**< Pull-up enable.              */
  k_ra_pfs_bit_ncodr = 6U,  /**< N-channel open-drain enable. */
  k_ra_pfs_bit_dscr0 = 10U, /**< Drive strength (low bit).    */
  k_ra_pfs_bit_dscr1 = 11U, /**< Drive strength (high bit).   */
  k_ra_pfs_bit_eofr0 = 12U, /**< Event on rising/falling lo.  */
  k_ra_pfs_bit_eofr1 = 13U, /**< Event on rising/falling hi.  */
  k_ra_pfs_bit_isel  = 14U, /**< IRQ input enable.            */
  k_ra_pfs_bit_asel  = 15U, /**< Analog input enable.         */
  k_ra_pfs_bit_pmr   = 16U, /**< 0=GPIO, 1=peripheral.        */
  k_ra_pfs_bit_psel0 = 24U, /**< Peripheral select bit 0.     */
} ra_pfs_bit_t;

/**
 * @enum ra_pfs_mask_t
 * @brief Full-width masks for PmnPFS fields.
 */
typedef enum : uint32_t {
  k_ra_pfs_mask_podr  = 0x00000001UL,
  k_ra_pfs_mask_pdr   = 0x00000004UL,
  k_ra_pfs_mask_pcr   = 0x00000010UL,
  k_ra_pfs_mask_ncodr = 0x00000040UL,
  k_ra_pfs_mask_dscr  = 0x00000C00UL, /**< bits 11..10. */
  k_ra_pfs_mask_eofr  = 0x00003000UL, /**< bits 13..12. */
  k_ra_pfs_mask_isel  = 0x00004000UL,
  k_ra_pfs_mask_asel  = 0x00008000UL,
  k_ra_pfs_mask_pmr   = 0x00010000UL,
  k_ra_pfs_mask_psel  = 0x1F000000UL, /**< bits 28..24 (5-bit function select). */
} ra_pfs_mask_t;

/**
 * @brief Get pointer to the `PmnPFS` register for a single pin.
 *
 * @param[in] port Port index (0..`k_ra_port_max`).
 * @param[in] pin  Pin index within the port (0..`k_ra_pin_max`).
 * @return Volatile pointer to the 32-bit `PmnPFS` register, or
 *         `nullptr` if either index is out of range.
 */
static inline volatile uint32_t* ra_pfs_pmn(ra_port_t port, ra_pin_t pin)
{
  if ((uint8_t)port > (uint8_t)k_ra_port_max) {
    return nullptr;
  }
  if ((uint8_t)pin > (uint8_t)k_ra_pin_max) {
    return nullptr;
  }

  /* Each pin is 4 bytes; each port is (k_ra_pin_count * 4) = 64 bytes apart. */
  const uintptr_t flat_idx = ((uintptr_t)port * (uintptr_t)k_ra_pin_count) + (uintptr_t)pin;
  return (volatile uint32_t*)(k_ra_pfs_base_addr + (flat_idx * sizeof(uint32_t)));
}

/* =============================================================================
 * PWPR unlock / lock
 * =============================================================================
 */

/**
 * @enum ra_pwpr_bit_t
 * @brief Bit positions inside the 8-bit PWPR register.
 */
typedef enum : uint8_t {
  k_ra_pwpr_bit_pfswe = 6U, /**< 1 = PFS writes allowed, 0 = PFS writes blocked. */
  k_ra_pwpr_bit_b0wi  = 7U, /**< 1 = PFSWE write blocked, 0 = PFSWE writable.    */
} ra_pwpr_bit_t;

/** @brief Get pointer to the PWPR register. */
static inline volatile uint8_t* ra_pfs_pwpr(void)
{
  return (volatile uint8_t*)(k_ra_pmisc_base_addr + k_ra_pmisc_off_pwpr);
}

/**
 * @brief Unlock PFS writes.
 *
 * @details
 * Sequence from HUM section 20.x:
 *  1. Clear `B0WI` (bit 7) -- allows PFSWE to be written.
 *  2. Set `PFSWE` (bit 6) -- allows subsequent PmnPFS writes.
 *
 * Must be called *once* before a burst of PmnPFS writes, then
 * balanced with `ra_pfs_pwpr_lock()`.
 */
static inline void ra_pfs_pwpr_unlock(void)
{
  volatile uint8_t* pwpr = ra_pfs_pwpr();
  *pwpr                  = 0U; /* B0WI=0, PFSWE=0 */
  *pwpr                  = (uint8_t)(1U << k_ra_pwpr_bit_pfswe);
}

/**
 * @brief Re-lock PFS writes.
 *
 * @details
 * Clears `PFSWE`, then sets `B0WI` so subsequent accidental writes to
 * `PFSWE` are silently dropped.
 */
static inline void ra_pfs_pwpr_lock(void)
{
  volatile uint8_t* pwpr = ra_pfs_pwpr();
  *pwpr                  = 0U;
  *pwpr                  = (uint8_t)(1U << k_ra_pwpr_bit_b0wi);
}

#ifdef __cplusplus
}
#endif
