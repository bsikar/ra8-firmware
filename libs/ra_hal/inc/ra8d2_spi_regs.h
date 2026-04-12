/**
 * @file ra8d2_spi_regs.h
 * @brief SPI register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 has two standard SPI channels (SPI0, SPI1) at
 * `0x4035C000` with a `0x100` stride, plus a low-speed SPI2 at
 * `0x40072200`. This header models the standard block.
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
  k_ra_spi0_base_addr = 0x4035C000UL,
  k_ra_spi2_base_addr = 0x40072200UL,
} ra_spi_addr_t;

typedef enum : uint16_t {
  k_ra_spi_primary_count  = 2U,
  k_ra_spi_channel_stride = 0x100U,
} ra_spi_limits_t;

typedef struct {
  volatile uint8_t  SPCR;      /**< +0x00 Control.              */
  volatile uint8_t  SSLP;      /**< +0x01 Slave Select Polarity. */
  volatile uint8_t  SPPCR;     /**< +0x02 Pin Control.          */
  volatile uint8_t  SPSR;      /**< +0x03 Status.               */
  volatile uint32_t SPDR;      /**< +0x04 Data.                 */
  volatile uint8_t  SPSCR;     /**< +0x08 Sequence Control.     */
  volatile uint8_t  SPSSR;     /**< +0x09 Sequence Status.      */
  volatile uint8_t  SPBR;      /**< +0x0A Bit Rate.             */
  volatile uint8_t  _r0;
  volatile uint8_t  SPDCR;     /**< +0x0C Data Control.         */
  volatile uint8_t  SPCKD;     /**< +0x0D Clock Delay.          */
  volatile uint8_t  SSLND;     /**< +0x0E SSL Negation Delay.   */
  volatile uint8_t  SPND;      /**< +0x0F Next-Access Delay.    */
  volatile uint8_t  SPCR2;     /**< +0x10 Control 2.            */
  volatile uint8_t  _r1[3];
  volatile uint16_t SPCMD[8];  /**< +0x14..+0x22 Command 0..7.  */
} r_spi_regs_t;

/** @brief Get pointer to standard SPI channel N (0..1). */
static inline volatile r_spi_regs_t* ra_spi(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_spi_primary_count) {
    return nullptr;
  }
  return (volatile r_spi_regs_t*)(k_ra_spi0_base_addr +
    ((uintptr_t)channel * (uintptr_t)k_ra_spi_channel_stride));
}

#ifdef __cplusplus
}
#endif
