/**
 * @file ra8d2_ptp_regs.h
 * @brief IEEE 1588 Precision Time Protocol (PTP) register layout for the RA8D2
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * RA8D2 implements PTP via the GPTP hardware timestamp counter (HUM
 * Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925) augmented with a
 * SYNFP / STCA control window inside the same peripheral aperture.
 * The full FSP r_ptp surface lives at a separate ETHERC_EPTPC base on
 * older RA parts; on RA8D2 the equivalent registers are folded into
 * the GPTP window. To keep the existing ``r_gptp_regs_t`` scaffold in
 * ``ra8d2_ether_regs.h`` unchanged (a sibling Wave 11 driver consumes
 * it), this header defines a *peer* SYNFP/STCA window at a distinct
 * 0x100 offset above the GPTP base. A real PTP stack walking the FSP
 * register definitions can later collapse the two windows.
 *
 * Registers modeled here (HUM Ch 35 + IEEE 1588-2019 sec 7.5):
 *
 * | Offset | Name        | Width | Purpose                              |
 * |-------:|-------------|------:|--------------------------------------|
 * |  0x00  | PTP_CTRL    |   32  | Master enable + role select          |
 * |  0x04  | PTP_DOMAIN  |    8  | clockDomain (IEEE 1588 sec 7.1)      |
 * |  0x08  | PTP_SYNINT  |    8  | log2 sync interval                   |
 * |  0x0C  | PTP_ANNINT  |    8  | log2 announce interval               |
 * |  0x10  | PTP_TIME_SECH | 16  | seconds upper 16                     |
 * |  0x14  | PTP_TIME_SECL | 32  | seconds lower 32                     |
 * |  0x18  | PTP_TIME_NS   | 32  | nanoseconds                          |
 * |  0x1C  | PTP_OFFS_NS   | 32  | last computed offset from master ns  |
 * |  0x20  | PTP_RATE_PPB  | 32  | frequency adjustment (signed ppb)    |
 * |  0x24  | PTP_TX_TRIG   | 32  | transmit-trigger (Sync / Announce)   |
 * |  0x28  | PTP_RX_LATCH  | 32  | last RX message-type latch           |
 * |  0x2C  | PTP_MAC0      | 32  | MAC address bytes 0..3               |
 * |  0x30  | PTP_MAC1      | 32  | MAC address bytes 4..5 (low half)    |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_ptp_addr_t
 * @brief PTP control-window base address.
 *
 * @details
 * Sits at GPTP_BASE + 0x100 (HUM Ch 35 "Ethernet Generic PTP Timer
 * (GPTP)" p 1925 -- documented reserved region above the
 * GPTP_CTRL/GPTP_STS/GPTP_IE/GPTP_ICLR window).
 */
typedef enum : uintptr_t {
  k_ra_ptp_base_addr = 0x403E0100UL, /**< PTP SYNFP/STCA window. */
} ra_ptp_addr_t;

/**
 * @enum ra_ptp_ctrl_bits_t
 * @brief PTP_CTRL field bit positions.
 *
 * @details
 * Mirrors the FSP r_ptp portStateSet packing (IEEE 1588-2019 sec
 * 9.2 "PTP state machines"). PORT_EN gates all message generation
 * and reception; ROLE[1:0] selects the IEEE 1588 port role.
 */
typedef enum : uint8_t {
  k_ra_ptp_bit_port_en = 0U, /**< Master enable.            */
  k_ra_ptp_bit_role0   = 1U, /**< Role[0] (LSB).            */
  k_ra_ptp_bit_role1   = 2U, /**< Role[1] (MSB).            */
  k_ra_ptp_bit_tx_sync = 8U, /**< TX trigger: Sync.         */
  k_ra_ptp_bit_tx_annc = 9U, /**< TX trigger: Announce.     */
} ra_ptp_ctrl_bits_t;

/**
 * @enum ra_ptp_ctrl_mask_t
 * @brief PTP_CTRL bit-field masks.
 */
typedef enum : uint32_t {
  k_ra_ptp_mask_port_en = 0x00000001UL, /**< Master enable bit.   */
  k_ra_ptp_mask_role    = 0x00000006UL, /**< ROLE[1:0] @ [2:1].   */
  k_ra_ptp_mask_tx_sync = 0x00000100UL, /**< TX Sync trigger.     */
  k_ra_ptp_mask_tx_annc = 0x00000200UL, /**< TX Announce trigger. */
} ra_ptp_ctrl_mask_t;

/**
 * @enum ra_ptp_role_shift_t
 * @brief Shift count for ROLE[1:0] inside PTP_CTRL.
 */
typedef enum : uint8_t {
  k_ra_ptp_role_shift = 1U, /**< ROLE field starts at bit 1. */
} ra_ptp_role_shift_t;

/**
 * @struct r_ptp_regs_t
 * @brief PTP SYNFP/STCA control window.
 *
 * @details
 * Layout matches the table in this file's @details comment; one
 * register per offset, no padding required (every entry is naturally
 * aligned to 32 bits or smaller).
 *
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read or written by the accessors in
 * ``libs/ra_hal/src/ra_ptp.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t PTP_CTRL;      /**< +0x00 Control: PORT_EN + ROLE + TX trig. */
  volatile uint32_t PTP_DOMAIN;    /**< +0x04 clockDomain (IEEE 1588 sec 7.1).   */
  volatile uint32_t PTP_SYNINT;    /**< +0x08 log2 sync interval.                */
  volatile uint32_t PTP_ANNINT;    /**< +0x0C log2 announce interval.            */
  volatile uint32_t PTP_TIME_SECH; /**< +0x10 seconds upper 16 bits.             */
  volatile uint32_t PTP_TIME_SECL; /**< +0x14 seconds lower 32 bits.             */
  volatile uint32_t PTP_TIME_NS;   /**< +0x18 nanoseconds.                       */
  volatile int32_t  PTP_OFFS_NS;   /**< +0x1C signed offsetFromMaster (ns).      */
  volatile int32_t  PTP_RATE_PPB;  /**< +0x20 frequency correction (signed ppb). */
  volatile uint32_t PTP_TX_TRIG;   /**< +0x24 raw transmit trigger ack.          */
  volatile uint32_t PTP_RX_LATCH;  /**< +0x28 last RX message-type latch.        */
  volatile uint32_t PTP_MAC0;      /**< +0x2C MAC address bytes 0..3.            */
  volatile uint32_t PTP_MAC1;      /**< +0x30 MAC address bytes 4..5 (low half). */
} r_ptp_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Get pointer to the PTP register block.
 *
 * @return Volatile pointer to the PTP SYNFP/STCA window.
 */
static inline volatile r_ptp_regs_t* ra_ptp_regs(void)
{
  return (volatile r_ptp_regs_t*)k_ra_ptp_base_addr;
}

#ifdef __cplusplus
}
#endif
