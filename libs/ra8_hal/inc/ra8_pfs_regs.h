/**
 * @file ra8_pfs_regs.h
 * @brief Pin Function Select (PFS) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
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
 *   ra8_pfs_pwpr_unlock();                        // clears B0WI, sets PFSWE
 *   ra8_pfs_pmn(port, pin)->PmnPFS = new_value;   // programme pin
 *   ra8_pfs_pwpr_lock();                          // clears PFSWE
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

#include "ra8_attributes.h"
#include "ra8_bit_constants.h"
#include "ra8_port_constants.h"

/* =============================================================================
 * Base addresses + layout
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_pfs_base_addr   = 0x40400800UL, /**< PFS array base.                 */
  k_ra8_pmisc_base_addr = 0x40400D00UL, /**< PMISC: contains PWPR and PWPRS. */
} ra8_pfs_addr_t;

typedef enum : uint16_t {
  k_ra8_pmisc_off_pfenet = 0x000U, /**< PFENET: Ethernet Mode Select.            */
  k_ra8_pmisc_off_pwpr   = 0x00CU, /**< PWPR: PFS Write Protect (HUM 20.2.5).    */
  k_ra8_pmisc_off_pwprs  = 0x014U, /**< PWPRS: Secure PFS Write Protect.         */
  k_ra8_pmisc_off_pmsar  = 0x030U, /**< PMSAR[0..14]: Port Security Attribution. */
} ra8_pmisc_off_t;

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
 * @enum ra8_pfs_bit_t
 * @brief Bit positions inside the 32-bit `PmnPFS` register.
 */
typedef enum : uint8_t {
  k_ra8_pfs_bit_podr  = 0U,  /**< Output data.                 */
  k_ra8_pfs_bit_pidr  = 1U,  /**< Input data (read-only).      */
  k_ra8_pfs_bit_pdr   = 2U,  /**< Direction (0=in, 1=out).     */
  k_ra8_pfs_bit_pcr   = 4U,  /**< Pull-up enable.              */
  k_ra8_pfs_bit_ncodr = 6U,  /**< N-channel open-drain enable. */
  k_ra8_pfs_bit_dscr0 = 10U, /**< Drive strength (low bit).    */
  k_ra8_pfs_bit_dscr1 = 11U, /**< Drive strength (high bit).   */
  k_ra8_pfs_bit_eofr0 = 12U, /**< Event on rising/falling lo.  */
  k_ra8_pfs_bit_eofr1 = 13U, /**< Event on rising/falling hi.  */
  k_ra8_pfs_bit_isel  = 14U, /**< IRQ input enable.            */
  k_ra8_pfs_bit_asel  = 15U, /**< Analog input enable.         */
  k_ra8_pfs_bit_pmr   = 16U, /**< 0=GPIO, 1=peripheral.        */
  k_ra8_pfs_bit_psel0 = 24U, /**< Peripheral select bit 0.     */
} ra8_pfs_bit_t;

/**
 * @enum ra8_pfs_mask_t
 * @brief Full-width masks for PmnPFS fields.
 */
typedef enum : uint32_t {
  k_ra8_pfs_mask_podr  = 0x00000001UL, /**< RA8 PFS mask podr.                   */
  k_ra8_pfs_mask_pdr   = 0x00000004UL, /**< RA8 PFS mask pdr.                    */
  k_ra8_pfs_mask_pcr   = 0x00000010UL, /**< RA8 PFS mask pcr.                    */
  k_ra8_pfs_mask_ncodr = 0x00000040UL, /**< RA8 PFS mask ncodr.                  */
  k_ra8_pfs_mask_dscr  = 0x00000C00UL, /**< bits 11..10.                         */
  k_ra8_pfs_mask_eofr  = 0x00003000UL, /**< bits 13..12.                         */
  k_ra8_pfs_mask_isel  = 0x00004000UL, /**< RA8 PFS mask isel.                   */
  k_ra8_pfs_mask_asel  = 0x00008000UL, /**< RA8 PFS mask asel.                   */
  k_ra8_pfs_mask_pmr   = 0x00010000UL, /**< RA8 PFS mask pmr.                    */
  k_ra8_pfs_mask_psel  = 0x1F000000UL, /**< bits 28..24 (5-bit function select). */
} ra8_pfs_mask_t;

/**
 * @enum ra8_pfs_dscr_t
 * @brief PmnPFS.DSCR[1:0] port drive-capability encodings.
 *
 * @details HUM Ch 20.2.6 "PmnPFS" p 845: the DSCR field selects the
 * output pad drive strength. The default after reset is
 * ::k_ra8_pfs_dscr_low. High-speed peripheral outputs (RGMII transmit
 * data/clock/control, parallel display) need a stronger setting --
 * HUM Ch 20.2.6 explicitly lists DSCR = 01b (middle) for RGMII/3.3 V
 * and 11b (high) for RGMII/2.5 V; any other value for an RGMII pin
 * leaves the electrical characteristics unguaranteed.
 *
 * @code
 * ra8_pfs_set_drive_strength(k_eth_txd0_pin, k_ra8_pfs_dscr_middle);
 * @endcode
 *
 * @see ra8_pfs_set_drive_strength()
 */
typedef enum : uint8_t {
  k_ra8_pfs_dscr_low             = 0U, /**< 00b: Low drive (reset default). */
  k_ra8_pfs_dscr_middle          = 1U, /**< 01b: Middle drive.              */
  k_ra8_pfs_dscr_high_speed_high = 2U, /**< 10b: High-speed high drive.     */
  k_ra8_pfs_dscr_high            = 3U, /**< 11b: High drive.                */
} ra8_pfs_dscr_t;

/**
 * @brief Get pointer to the `PmnPFS` register for a single pin.
 *
 * @param[in] port Port index (0..`k_ra8_port_max`).
 * @param[in] pin  Pin index within the port (0..`k_ra8_pin_max`).
 * @return Volatile pointer to the 32-bit `PmnPFS` register, or
 *         `nullptr` if either index is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_pfs_pmn(ra8_port_t port, ra8_pin_t pin)
{
  if ((uint8_t)port > k_ra8_port_max) {
    return nullptr;
  }
  if ((uint8_t)pin > k_ra8_pin_max) {
    return nullptr;
  }

  /* Each pin is 4 bytes; each port is (k_ra8_pin_count * 4) = 64 bytes apart. */
  const uintptr_t flat_idx = ((uintptr_t)port * (uintptr_t)k_ra8_pin_count) + (uintptr_t)pin;
  return (volatile uint32_t*)(k_ra8_pfs_base_addr + (flat_idx * sizeof(uint32_t)));
}

/* =============================================================================
 * PWPR unlock / lock
 * =============================================================================
 */

/**
 * @enum ra8_pwpr_bit_t
 * @brief Bit positions inside the 8-bit PWPR register.
 */
typedef enum : uint8_t {
  k_ra8_pwpr_bit_pfswe = 6U, /**< 1 = PFS writes allowed, 0 = PFS writes blocked. */
  k_ra8_pwpr_bit_b0wi  = 7U, /**< 1 = PFSWE write blocked, 0 = PFSWE writable.    */
} ra8_pwpr_bit_t;

/** @brief Get pointer to the PWPR register (non-secure write-protect). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint8_t* ra8_pfs_pwpr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_pmisc_base_addr + (uintptr_t)k_ra8_pmisc_off_pwpr);
}

/**
 * @brief Get pointer to the PWPRS register (Secure write-protect).
 *
 * @details
 * On RA8D2 the Cortex-M85 boots in the Secure world. Until the SAU
 * partition is programmed, PFS writes gated only by PWPR are silently
 * dropped because PWPR controls the **non-secure** path. Secure code
 * must unlock PWPRS as well; both registers share the same bit layout
 * (B0WI / PFSWE) per HUM Ch 20.2.6 / 20.2.7.
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint8_t* ra8_pfs_pwprs(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_pmisc_base_addr + (uintptr_t)k_ra8_pmisc_off_pwprs);
}

/**
 * @brief Unlock PFS writes.
 *
 * @details
 * Sequence from HUM section 20.2.6 (PWPR) and 20.2.7 (PWPRS):
 *  1. Write 0          -- clears `B0WI` (bit 7), allowing PFSWE to be written.
 *  2. Write `1<<PFSWE` -- sets `PFSWE` (bit 6), allowing PmnPFS writes.
 *
 * Must be called *once* before a burst of PmnPFS writes, then
 * balanced with `ra8_pfs_pwpr_lock()`.
 *
 * @note We touch both PWPR (NS) and PWPRS (S) unconditionally, even
 *       though FSP only writes the one matching the current TrustZone
 *       build mode. Per HUM Ch 20.2.6 / 20.2.7 the chip silently
 *       ignores writes to whichever side does not own the port (the
 *       `R_PMISC->PMSAR[]` bit decides), so writing both is harmless.
 *       PMSAR resets to "all ports owned by Secure" on RA8D2, which
 *       matches what we currently want; we do not programme PMSAR.
 *       If a future bring-up needs NS ports it must (a) write the
 *       relevant PMSAR bit Secure-side and (b) the matching PFSWE
 *       write will then take effect through the NS path.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static inline void ra8_pfs_pwpr_unlock(void)
{
  volatile uint8_t* pwpr  = ra8_pfs_pwpr();
  volatile uint8_t* pwprs = ra8_pfs_pwprs();
  /* HUM Ch 20.2.6 "PWPR" / 20.2.7 "PWPRS" -- B0WI must be cleared
   * before PFSWE can be set. Unlock both NS and Secure paths so PFS
   * writes land regardless of which world the CPU is in. */
  *pwpr  = 0U;
  *pwpr  = (uint8_t)(1U << k_ra8_pwpr_bit_pfswe);
  *pwprs = 0U;
  *pwprs = (uint8_t)(1U << k_ra8_pwpr_bit_pfswe);
}

/**
 * @brief Re-lock PFS writes.
 *
 * @details
 * Clears `PFSWE`, then sets `B0WI` so subsequent accidental writes to
 * `PFSWE` are silently dropped.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void ra8_pfs_pwpr_lock(void)
{
  volatile uint8_t* pwpr  = ra8_pfs_pwpr();
  volatile uint8_t* pwprs = ra8_pfs_pwprs();
  *pwpr                   = 0U;
  *pwpr                   = (uint8_t)(1U << k_ra8_pwpr_bit_b0wi);
  *pwprs                  = 0U;
  *pwprs                  = (uint8_t)(1U << k_ra8_pwpr_bit_b0wi);
}

#ifdef __cplusplus
}
#endif
