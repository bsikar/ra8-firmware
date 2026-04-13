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
#include "ra_mstp.h"
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

/**
 * @enum ra_spi_channel_count_t
 * @brief Number of SPI channels addressed by this driver.
 */
typedef enum : uint8_t {
  k_ra_spi_channel_count = 2U,
} ra_spi_channel_count_t;

/**
 * @var s_spi_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 */
static const ra_mstp_t s_spi_mstp_table[k_ra_spi_channel_count] = {
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  k_ra_mstp_spi0,
  k_ra_mstp_spi1,
};

ra_err_t ra_spi_master_init(uint8_t channel)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t mst_err = ra_mstp_enable(s_spi_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

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

/* =============================================================================
 * Wave 3.3: full-featured API
 * =============================================================================
 */

/**
 * @struct ra_spi_state_t
 * @brief Per-channel dispatch state.
 */
typedef struct {
  ra_spi_complete_fn_t cb;          /**< Transfer-complete callback. */
  void*                ctx;         /**< Callback context.           */
  bool                 initialised; /**< Tracked by ra_spi_init.     */
} ra_spi_state_t;

/**
 * @var s_spi_state
 * @brief Per-channel state table indexed by channel.
 */
static ra_spi_state_t s_spi_state[k_ra_spi_channel_count];

/**
 * @enum ra_spi_brdv_t
 * @brief SPBR base-clock divider bits used by the SPBR calculation.
 */
typedef enum : uint8_t {
  k_ra_spi_spbr_max = 0xFFU, /**< SPBR is 8-bit unsigned. */
} ra_spi_brdv_t;

/**
 * @enum ra_spi_spcmd_bit_t
 * @brief Bit positions in SPCMDn.
 */
typedef enum : uint16_t {
  k_ra_spi_spcmd_bit_cpha = 0U,  /**< CPHA: phase selection. */
  k_ra_spi_spcmd_bit_cpol = 1U,  /**< CPOL: polarity.         */
  k_ra_spi_spcmd_bit_lsbf = 12U, /**< LSB-first.              */
} ra_spi_spcmd_bit_t;

/**
 * @brief Compute SPBR for a requested bit-rate.
 */
static uint8_t internal_spbr(uint32_t baud_hz, uint32_t pclka_hz)
{
  if ((baud_hz == 0U) || (pclka_hz == 0U)) {
    return 0U;
  }
  const uint32_t divisor = 2U * baud_hz;
  const uint32_t n       = pclka_hz / divisor;
  if (n == 0U) {
    return 0U;
  }
  const uint32_t result = n - 1U;
  if (result > (uint32_t)k_ra_spi_spbr_max) {
    return (uint8_t)k_ra_spi_spbr_max;
  }
  return (uint8_t)result;
}

/**
 * @brief Build an SPCMD[0] value from a ``ra_spi_cfg_t``.
 */
static uint16_t internal_spcmd(const ra_spi_cfg_t* cfg)
{
  uint16_t v = (uint16_t)k_ra_spi_spcmd_default;
  if ((cfg->mode == k_ra_spi_mode_1) || (cfg->mode == k_ra_spi_mode_3)) {
    v |= (uint16_t)(1U << (uint8_t)k_ra_spi_spcmd_bit_cpha);
  }
  if ((cfg->mode == k_ra_spi_mode_2) || (cfg->mode == k_ra_spi_mode_3)) {
    v |= (uint16_t)(1U << (uint8_t)k_ra_spi_spcmd_bit_cpol);
  }
  if (cfg->lsb_first) {
    v |= (uint16_t)(1U << (uint8_t)k_ra_spi_spcmd_bit_lsbf);
  }
  return v;
}

ra_err_t ra_spi_init(uint8_t channel, const ra_spi_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "spi_init: cfg");
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_spi_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "spi_init: mstp"); /* GCOVR_EXCL_BR_LINE */

  /* Disable SPE before reprogramming.
   * HUM Ch 43.2 "SPCR : SPI Control Register", p 2877 */
  reg->SPCR     = 0U;
  reg->SPPCR    = 0U;
  reg->SPBR     = internal_spbr(cfg->baud_hz, cfg->pclka_hz);
  reg->SPDCR    = 0U;
  reg->SPCKD    = 0U;
  reg->SSLND    = 0U;
  reg->SPND     = 0U;
  reg->SPCR2    = 0U;
  reg->SPCMD[0] = internal_spcmd(cfg);
  /* Enable SPE + MSTR. */
  reg->SPCR = (uint8_t)k_ra_spi_spcr_enable;

  s_spi_state[channel].cb          = nullptr;
  s_spi_state[channel].ctx         = nullptr;
  s_spi_state[channel].initialised = true;
  ra_log_info_val(s_tag, "spi_init channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_spi_deinit(uint8_t channel)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* Clear SPE.
   * HUM Ch 43.2 "SPCR : SPI Control Register", p 2877 */
  reg->SPCR                        = 0U;
  s_spi_state[channel].cb          = nullptr;
  s_spi_state[channel].ctx         = nullptr;
  s_spi_state[channel].initialised = false;
  return ra_mstp_disable(s_spi_mstp_table[channel]);
}

ra_err_t ra_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclka_hz)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (baud_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 43.2 "SPBR : SPI Bit Rate Register", p 2877 */
  reg->SPBR = internal_spbr(baud_hz, pclka_hz);
  return k_ra_ok;
}

ra_err_t ra_spi_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "spi get_errors");
  volatile const r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t ss = reg->SPSR;
  uint8_t       m  = (uint8_t)k_ra_spi_err_none;
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_spsr_bit_ovrf)) != 0U) {
    m |= (uint8_t)k_ra_spi_err_overrun;
  }
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_spsr_bit_moderf)) != 0U) {
    m |= (uint8_t)k_ra_spi_err_mode;
  }
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_spsr_bit_perf)) != 0U) {
    m |= (uint8_t)k_ra_spi_err_parity;
  }
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_spsr_bit_udrf)) != 0U) {
    m |= (uint8_t)k_ra_spi_err_underrun;
  }
  *out_mask = m;
  return k_ra_ok;
}

ra_err_t ra_spi_clear_errors(uint8_t channel)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* Clear SPSR error flags via write-zero.
   * HUM Ch 43.2 "SPSR : SPI Status Register", p 2877 */
  const uint8_t clr_mask =
    (uint8_t)~((1U << (uint8_t)k_ra_spsr_bit_ovrf) | (1U << (uint8_t)k_ra_spsr_bit_moderf) |
               (1U << (uint8_t)k_ra_spsr_bit_perf) | (1U << (uint8_t)k_ra_spsr_bit_udrf));
  reg->SPSR = (uint8_t)(reg->SPSR & clr_mask);
  return k_ra_ok;
}

ra_err_t ra_spi_attach_transfer_handler(uint8_t channel, ra_spi_complete_fn_t fn, void* ctx)
{
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_spi_state[channel].cb  = fn;
  s_spi_state[channel].ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_spi_enter_stop(uint8_t channel)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* Clear SPE.
   * HUM Ch 43.2 "SPCR : SPI Control Register", p 2877 */
  reg->SPCR = 0U;
  return ra_mstp_disable(s_spi_mstp_table[channel]);
}

ra_err_t ra_spi_exit_stop(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_spi_mstp_table[channel]);
}

void ra_spi_dispatch_spti(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return;
  }
  (void)s_spi_state[channel].cb;
}

void ra_spi_dispatch_spri(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return;
  }
  (void)s_spi_state[channel].cb;
}

void ra_spi_dispatch_spei(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_spi_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra_spi_get_errors(channel, &mask);
  (void)ra_spi_clear_errors(channel);
  const ra_spi_complete_fn_t cb = s_spi_state[channel].cb;
  if ((mask != 0U) && (cb != nullptr)) {
    cb(s_spi_state[channel].ctx, mask);
  }
}
