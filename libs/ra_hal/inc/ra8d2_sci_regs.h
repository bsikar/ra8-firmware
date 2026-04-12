/**
 * @file ra8d2_sci_regs.h
 * @brief SCI (Serial Communication Interface) register layout for the RA8D2
 *
 * @details
 * RA8D2 has 10 SCI channels (SCI0..SCI9) at `0x40358000` with stride
 * `0x100` per channel. Each SCI supports four operating modes (async
 * UART, clock-synchronous master/slave, smart card, simple I2C/SPI).
 * This header models the subset of registers used for 8-N-1 UART
 * bring-up; additional fields will be added as drivers need them.
 *
 * ## Register map (per channel, partial)
 *
 * | Offset | Name  | Width | Purpose                                 |
 * |-------:|-------|------:|-----------------------------------------|
 * | 0x00   | SMR   | 8     | Serial Mode Register                    |
 * | 0x01   | BRR   | 8     | Bit Rate Register                       |
 * | 0x02   | SCR   | 8     | Serial Control Register                 |
 * | 0x03   | TDR   | 8     | Transmit Data Register                  |
 * | 0x04   | SSR   | 8     | Serial Status Register                  |
 * | 0x05   | RDR   | 8     | Receive Data Register                   |
 * | 0x06   | SCMR  | 8     | Smart Card Mode Register                |
 * | 0x07   | SEMR  | 8     | Serial Extended Mode Register           |
 * | 0x08   | SNFR  | 8     | Noise Filter Setting Register           |
 * | 0x0C   | SPMR  | 8     | SPI Mode Register                       |
 *
 * @note The RA8 Hardware User's Manual models each SCI variant with
 *       its own memory layout. We use the "SCI" variant (not the newer
 *       "SCI_B"). The register set is a strict subset that covers
 *       UART, synchronous SPI, and basic I2C modes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Base addresses + layout
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_sci0_base_addr = 0x40358000UL,
  k_ra_sci1_base_addr = 0x40358100UL,
  k_ra_sci2_base_addr = 0x40358200UL,
  k_ra_sci3_base_addr = 0x40358300UL,
  k_ra_sci4_base_addr = 0x40358400UL,
  k_ra_sci5_base_addr = 0x40358500UL,
  k_ra_sci6_base_addr = 0x40358600UL,
  k_ra_sci7_base_addr = 0x40358700UL,
  k_ra_sci8_base_addr = 0x40358800UL,
  k_ra_sci9_base_addr = 0x40358900UL,
} ra_sci_base_addr_t;

typedef enum : uint16_t {
  k_ra_sci_channel_count          = 10U,    /**< SCI0..SCI9.                      */
  k_ra_sci_channel_max            = 9U,     /**< Highest valid channel index.     */
  k_ra_sci_channel_stride         = 0x100U, /**< Bytes between SCI channels.    */
  k_ra_sci_reserved_padding_bytes = 3U,     /**< Bytes of reserved padding
                                             after SEMR+SNFR block.      */
} ra_sci_limits_t;

/* =============================================================================
 * Register layout (per channel)
 * =============================================================================
 */

/**
 * @struct r_sci_regs_t
 * @brief Per-channel SCI register window (UART subset).
 *
 * @details
 * Only the first 16 bytes of the 256-byte channel window are modelled;
 * the remaining registers (SCI-specific extensions, FIFO modes)
 * are added as drivers need them.
 */
typedef struct {
  volatile uint8_t SMR;  /**< +0x00 Serial Mode.       */
  volatile uint8_t BRR;  /**< +0x01 Bit Rate.          */
  volatile uint8_t SCR;  /**< +0x02 Serial Control.    */
  volatile uint8_t TDR;  /**< +0x03 Transmit Data.     */
  volatile uint8_t SSR;  /**< +0x04 Serial Status.     */
  volatile uint8_t RDR;  /**< +0x05 Receive Data.      */
  volatile uint8_t SCMR; /**< +0x06 Smart Card Mode.   */
  volatile uint8_t SEMR; /**< +0x07 Ext Mode.          */
  volatile uint8_t SNFR; /**< +0x08 Noise Filter.      */
  volatile uint8_t _r0[k_ra_sci_reserved_padding_bytes];
  volatile uint8_t SPMR; /**< +0x0C SPI Mode.          */
} r_sci_regs_t;

/* =============================================================================
 * Accessors
 * =============================================================================
 */

/** @brief Get pointer to SCI channel N (0..9). */
static inline volatile r_sci_regs_t* ra_sci(uint8_t channel)
{
  if (channel > k_ra_sci_channel_max) {
    return nullptr;
  }
  return (volatile r_sci_regs_t*)(k_ra_sci0_base_addr +
                                  ((uintptr_t)channel * (uintptr_t)k_ra_sci_channel_stride));
}

/* =============================================================================
 * Bit fields
 * =============================================================================
 */

/**
 * @enum ra_scr_bit_t
 * @brief SCR (Serial Control) bit positions.
 */
typedef enum : uint8_t {
  k_ra_scr_bit_cke0 = 0U, /**< Clock enable 0.     */
  k_ra_scr_bit_cke1 = 1U, /**< Clock enable 1.     */
  k_ra_scr_bit_teie = 2U, /**< TX end IRQ enable.  */
  k_ra_scr_bit_mpie = 3U, /**< Multi-proc IRQ.     */
  k_ra_scr_bit_re   = 4U, /**< Receiver enable.    */
  k_ra_scr_bit_te   = 5U, /**< Transmitter enable. */
  k_ra_scr_bit_rie  = 6U, /**< RX IRQ enable.      */
  k_ra_scr_bit_tie  = 7U, /**< TX IRQ enable.      */
} ra_scr_bit_t;

/**
 * @enum ra_ssr_bit_t
 * @brief SSR (Serial Status) bit positions.
 */
typedef enum : uint8_t {
  k_ra_ssr_bit_mpbt  = 0U, /**< Multi-proc bit (TX). */
  k_ra_ssr_bit_mpb   = 1U, /**< Multi-proc bit (RX). */
  k_ra_ssr_bit_teend = 2U, /**< Transmit end.        */
  k_ra_ssr_bit_per   = 3U, /**< Parity error.        */
  k_ra_ssr_bit_fer   = 4U, /**< Framing error.       */
  k_ra_ssr_bit_orer  = 5U, /**< Overrun error.       */
  k_ra_ssr_bit_rdrf  = 6U, /**< RX data ready.       */
  k_ra_ssr_bit_tdre  = 7U, /**< TX data empty.       */
} ra_ssr_bit_t;

#ifdef __cplusplus
}
#endif
