/**
 * @file ra8_crc_regs.h
 * @brief CRC calculator register layout for the Renesas RA8D2
 * @ingroup grp_hal_analog
 *
 * @details
 * The RA8D2 CRC unit lives at `0x40310000` (HUM Ch 48 p 3180) and is a
 * single 16-byte register block. The unit supports five hard-wired
 * polynomials selected via `CRCCR0.GPS[2:0]`:
 *   - GPS=1 : CRC-8     (X^8 + X^2 + X + 1)
 *   - GPS=2 : CRC-16    (X^16 + X^15 + X^2 + 1)
 *   - GPS=3 : CRC-CCITT (X^16 + X^12 + X^5 + 1)
 *   - GPS=4 : CRC-32    (IEEE 802.3)
 *   - GPS=5 : CRC-32C   (Castagnoli)
 *
 * Input data is fed via `CRCDIR` (32-bit access for CRC-32/32C) or
 * `CRCDIR_BY` (8-bit access alias for CRC-8/16/CCITT). The running
 * result is read from `CRCDOR` (32-bit), `CRCDOR_HA` (16-bit half-word
 * alias for CRC-16/CCITT), or `CRCDOR_BY` (8-bit alias for CRC-8).
 * `CRCDIR` and `CRCDIR_BY` share the same physical address +0x04;
 * `CRCDOR`, `CRCDOR_HA` and `CRCDOR_BY` share the same physical
 * address +0x08. They are union aliases in the FSP CMSIS header.
 *
 * Layout cross-verified against FSP `R_CRC_Type` in
 * `R7KA8D2KF_core0.h` (lines 5289-5386, total size 16 bytes).
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
 * @enum ra8_crc_addr_t
 * @brief CRC peripheral base address.
 */
typedef enum : uintptr_t {
  k_ra8_crc_base_addr = 0x40310000UL, /**< HUM Ch 48 p 3180 "CRC base". */
} ra8_crc_addr_t;

/**
 * @enum ra8_crc_poly_t
 * @brief Polynomial selection values written to CRCCR0.GPS[2:0].
 *
 * Values mirror FSP `crc_polynomial_t` and HUM Ch 48.2.1
 * "CRCCR0 : CRC Control Register 0" (p 3181) GPS field encoding.
 */
typedef enum : uint8_t {
  k_ra8_crc_poly_none         = 0U, /**< GPS=000 No calculation.                  */
  k_ra8_crc_poly_8            = 1U, /**< GPS=001 CRC-8 (X^8+X^2+X+1).             */
  k_ra8_crc_poly_16           = 2U, /**< GPS=010 CRC-16 (X^16+X^15+X^2+1).        */
  k_ra8_crc_poly_16_ccitt     = 3U, /**< GPS=011 CRC-CCITT (X^16+X^12+X^5+1).     */
  k_ra8_crc_poly_32_ieee802_3 = 4U, /**< GPS=100 CRC-32 (IEEE 802.3).             */
  k_ra8_crc_poly_32c_rev      = 5U, /**< GPS=101 CRC-32C (Castagnoli, reflected). */
} ra8_crc_poly_t;

/**
 * @enum ra8_crc_bit_order_t
 * @brief Bit-order for CRC calculation, written to CRCCR0.LMS (bit 6).
 *
 * Mirrors FSP `crc_bit_order_t`. HUM Ch 48.2.1 p 3181.
 */
typedef enum : uint8_t {
  k_ra8_crc_lsb_first = 0U, /**< LMS=0 LSB-first communication. */
  k_ra8_crc_msb_first = 1U, /**< LMS=1 MSB-first communication. */
} ra8_crc_bit_order_t;

/**
 * @struct r_crc_regs_t
 * @brief CRC peripheral register block.
 *
 * Re-derived against FSP `R_CRC_Type` in `R7KA8D2KF_core0.h`. Total
 * size is exactly 16 bytes. CRCDIR/CRCDIR_BY and CRCDOR/CRCDOR_HA/
 * CRCDOR_BY share their physical address; the BY/HA alias members
 * are placed in unions so byte/half-word/word writes hit the same
 * register and the hardware uses CRCCR0.GPS to interpret width.
 */
typedef struct {
  volatile uint8_t  CRCCR0; /**< +0x00 Control Register 0 (GPS/LMS/DORCLR). */
  volatile uint8_t  CRCCR1; /**< +0x01 Control Register 1 (CRCSWR/CRCSEN).  */
  volatile uint16_t _r0;    /**< +0x02 Reserved.                            */
  union {
    volatile uint32_t CRCDIR;    /**< +0x04 Input Data (32-bit, CRC-32/32C). */
    volatile uint8_t  CRCDIR_BY; /**< +0x04 Input Data (8-bit alias).        */
  }; /**< (anon) register. */
  union {
    volatile uint32_t CRCDOR;    /**< +0x08 Output Data (32-bit, CRC-32/32C). */
    volatile uint16_t CRCDOR_HA; /**< +0x08 Output Data (16-bit alias).       */
    volatile uint8_t  CRCDOR_BY; /**< +0x08 Output Data (8-bit alias).        */
  }; /**< (anon) register. */
  volatile uint16_t CRCSAR; /**< +0x0C Snoop Address Register (14-bit). */
  volatile uint16_t _r1;    /**< +0x0E Reserved.                        */
} r_crc_regs_t;

/**
 * @brief Get pointer to the CRC peripheral register block.
 * @return Volatile pointer to the CRC registers at `k_ra8_crc_base_addr`.
 */
static inline volatile r_crc_regs_t* ra8_crc(void)
{
  return (volatile r_crc_regs_t*)k_ra8_crc_base_addr;
}

#ifdef __cplusplus
}
#endif
