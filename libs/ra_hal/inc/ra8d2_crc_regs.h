/**
 * @file ra8d2_crc_regs.h
 * @brief CRC calculator register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 CRC unit lives at `0x40310000` and supports five
 * polynomials (8-bit CCITT, 8-bit, 16-bit CCITT, 16-bit, 32-bit)
 * selectable via `CRCCR0.GPS`. Input data is fed via `CRCDIR` and the
 * running result is read from `CRCDOR`. Both are accessible as byte
 * (`_BY`) or word variants.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_crc_base_addr = 0x40310000UL,
} ra_crc_addr_t;

/**
 * @enum ra_crc_poly_t
 * @brief Polynomial selection values written to CRCCR0.GPS[2:0].
 */
typedef enum : uint8_t {
  k_ra_crc_poly_none         = 0U, /**< No calculation.             */
  k_ra_crc_poly_8_ccitt      = 1U, /**< x^8 + x^2 + x + 1.          */
  k_ra_crc_poly_8            = 2U, /**< x^8 + x^5 + x^4 + 1.        */
  k_ra_crc_poly_16_ccitt     = 3U, /**< x^16 + x^12 + x^5 + 1.      */
  k_ra_crc_poly_16           = 4U, /**< x^16 + x^15 + x^2 + 1.      */
  k_ra_crc_poly_32_ieee802_3 = 6U, /**< IEEE-802.3 CRC-32.          */
  k_ra_crc_poly_32c_rev      = 7U, /**< CRC-32C (Castagnoli), rev.  */
} ra_crc_poly_t;

/* Register layout verified against FSP R_CRC_Type in
 * R7KA8D2KF_core0.h lines 5285-5386. */
typedef struct {
  volatile uint8_t  CRCCR0; /**< +0x00 Control Register 0 (GPS/LMS/DORCLR). */
  volatile uint8_t  CRCCR1; /**< +0x01 Control Register 1 (CRCSWR/CRCSEN). */
  volatile uint16_t _r0;
  volatile uint32_t CRCDIR; /**< +0x04 Input Data (word).                   */
  volatile uint32_t CRCDOR; /**< +0x08 Output Data (word).                  */
  volatile uint16_t CRCSAR; /**< +0x0C Snoop Address Register (14-bit).     */
  volatile uint16_t _r1;
} r_crc_regs_t;

/** @brief Get pointer to the CRC block. */
static inline volatile r_crc_regs_t* ra_crc(void)
{
  return (volatile r_crc_regs_t*)k_ra_crc_base_addr;
}

#ifdef __cplusplus
}
#endif
