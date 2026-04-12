/**
 * @file spi.c
 * @brief Polling SPI master driver
 *
 * @details
 * Minimal full-duplex SPI master driver used for bring-up of SPI
 * sensors before a DMA / IRQ driver replaces it. Only targets SPI0
 * and SPI1 on the RA8D2; SPI2 (the low-speed instance at
 * `0x40072200`) is not addressed yet.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_spi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_spi.h"

static const char* s_tag = "SPI";

/**
 * @enum ra_spsr_bit_t
 * @brief SPSR status bit positions used by the polling loop.
 */
typedef enum : uint8_t {
  k_ra_spsr_bit_ovrf   = 0U, /**< Overrun.              */
  k_ra_spsr_bit_idlnf  = 1U, /**< Idle flag.            */
  k_ra_spsr_bit_moderf = 2U, /**< Mode fault.           */
  k_ra_spsr_bit_perf   = 3U, /**< Parity error.         */
  k_ra_spsr_bit_udrf   = 4U, /**< Underrun.             */
  k_ra_spsr_bit_sptef  = 5U, /**< TX buffer empty.      */
  k_ra_spsr_bit_tend   = 6U, /**< Transmit end.         */
  k_ra_spsr_bit_sprf   = 7U, /**< RX buffer full.       */
} ra_spsr_bit_t;

/**
 * @enum ra_spi_init_val_t
 * @brief Magic register values used during SPI init.
 *
 * @details Values from the RA8D2 HUM section "Serial Peripheral
 * Interface (SPI)". The bit-rate divider is a placeholder; a future
 * driver will derive it from PCLKA and a requested baud rate.
 */
typedef enum : uint16_t {
  k_ra_spi_spbr_default  = 0x000FU, /**< ~1.9 MHz @ PCLKA = 125 MHz. */
  k_ra_spi_spcmd_default = 0x0700U, /**< 8-bit, CPOL=0, CPHA=0, BRDV=0. */
  k_ra_spi_spcr_enable   = 0x0048U, /**< SPE (bit 6) + MSTR (bit 3). */
} ra_spi_init_val_t;

ra_err_t ra_spi_master_init(uint8_t channel)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->SPCR     = 0U;
  reg->SPPCR    = 0U;
  reg->SPBR     = (uint8_t)k_ra_spi_spbr_default;
  reg->SPDCR    = 0U;
  reg->SPCKD    = 0U;
  reg->SSLND    = 0U;
  reg->SPND     = 0U;
  reg->SPCR2    = 0U;
  reg->SPCMD[0] = (uint16_t)k_ra_spi_spcmd_default;
  reg->SPCR     = (uint8_t)k_ra_spi_spcr_enable;

  ra_log_info_val(s_tag, "init channel", (uint32_t)channel);
  return k_ra_ok;
}

/**
 * @brief Wait for an SPSR bit.
 */
static ra_err_t internal_wait_spsr(volatile r_spi_regs_t* reg, uint8_t bit)
{
  enum : uint32_t { k_ra_spi_poll_limit = 200000U };
  for (uint32_t i = 0U; i < k_ra_spi_poll_limit; i++) {
    if ((reg->SPSR & (uint8_t)(1U << bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  ra_err_t err = internal_wait_spsr(reg, (uint8_t)k_ra_spsr_bit_sptef);
  if (err != k_ra_ok) {
    return err;
  }
  reg->SPDR = (uint32_t)tx;

  err = internal_wait_spsr(reg, (uint8_t)k_ra_spsr_bit_sprf);
  if (err != k_ra_ok) {
    return err;
  }
  const uint8_t received = (uint8_t)reg->SPDR;
  if (rx != nullptr) {
    *rx = received;
  }
  return k_ra_ok;
}
