/**
 * @file ra_qspi.c
 * @brief Legacy QSPI driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling driver mirroring FSP ``r_qspi.c`` minus the dedicated
 * erase/write/auto-calibrate paths. Flow:
 *
 *  - ``ra_qspi_open``: programme SFMSMD (mode/CPOL/CPHA), SFMSKC
 *    (clock divisor), SFMSPC (single/dual/quad protocol),
 *    SFMSDC (dummy cycle count) and SFMSIC (XIP read instruction).
 *  - ``ra_qspi_send``: poll ``SFMSST.TXFULL == 0`` then write each
 *    byte into ``SFMCOM``.
 *  - ``ra_qspi_receive``: copy bytes out of the memory-mapped XIP
 *    window starting at ``offset``.
 *  - ``ra_qspi_xip_enter`` / ``_exit``: write ``SFMCMD`` and snapshot
 *    ``SFMCST`` for the status helper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_qspi.h"

#include <string.h>

#include "ra8d2_qspi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "QSPI";

/**
 * @enum ra_qspi_internal_t
 * @brief Internal tunables.
 */
typedef enum : uint32_t {
  k_ra_qspi_spin_budget   = 50000U, /**< Bounded poll iterations.    */
  k_ra_qspi_max_clock_div = 255U,   /**< 8-bit divisor.              */
} ra_qspi_internal_t;

/**
 * @enum ra_qspi_sfmsmd_bits_t
 * @brief SFMSMD field positions used by the driver.
 */
typedef enum : uint32_t {
  k_ra_qspi_sfmsmd_cpol_pos = 3U,
  k_ra_qspi_sfmsmd_cpha_pos = 4U,
} ra_qspi_sfmsmd_bits_t;

/**
 * @brief Bounded wait until SFMSST.BUSY clears.
 */
static ra_err_t internal_wait_idle(volatile r_qspi_t* reg)
{
  for (uint32_t i = 0U; i < k_ra_qspi_spin_budget; ++i) {
    if ((reg->SFMSST & k_ra_qspi_sfmsst_busy) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Bounded wait until SFMSST.TXFULL clears.
 */
static ra_err_t internal_wait_tx(volatile r_qspi_t* reg)
{
  for (uint32_t i = 0U; i < k_ra_qspi_spin_budget; ++i) {
    if ((reg->SFMSST & k_ra_qspi_sfmsst_txfull) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_qspi_open(uint8_t channel, const ra_qspi_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "ra_qspi_open: cfg null");
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    ra_log_error(s_tag, "ra_qspi_open: channel out of range");
    return k_ra_err_invalid_arg;
  }
  if ((cfg->clock_div == 0U) || (cfg->clock_div > k_ra_qspi_max_clock_div)) {
    ra_log_error(s_tag, "ra_qspi_open: bad clock_div");
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->protocol > k_ra_qspi_protocol_qpi) {
    return k_ra_err_invalid_arg;
  }

  reg->SFMSMD = ((uint32_t)cfg->cpol << k_ra_qspi_sfmsmd_cpol_pos) |
                ((uint32_t)cfg->cpha << k_ra_qspi_sfmsmd_cpha_pos);
  reg->SFMSKC = cfg->clock_div;
  reg->SFMSDC = (uint32_t)cfg->dummy_cycles;
  reg->SFMSPC = (uint32_t)cfg->protocol;
  reg->SFMSIC = (uint32_t)cfg->xip_read_op;
  reg->SFMSSC = 0U;
  ra_log_info_val(s_tag, "ra_qspi_open ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_qspi_close(uint8_t channel)
{
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  reg->SFMCMD = k_ra_qspi_sfmcmd_xip_exit;
  reg->SFMSMD = 0U;
  return k_ra_ok;
}

ra_err_t ra_qspi_send(uint8_t channel, const uint8_t* data, uint32_t len)
{
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (data == nullptr)) {
    ra_log_error(s_tag, "ra_qspi_send: data null");
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    RA_RETURN_ON_ERROR(internal_wait_tx(reg), s_tag, "ra_qspi_send: TXFULL wait");
    reg->SFMCOM = (uint32_t)data[i];
  }
  return internal_wait_idle(reg);
}

ra_err_t ra_qspi_receive(uint8_t channel, uint32_t offset, uint8_t* buf, uint32_t len)
{
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (buf == nullptr)) {
    ra_log_error(s_tag, "ra_qspi_receive: buf null");
    return k_ra_err_null_ptr;
  }
  if ((offset >= k_ra_qspi_xip_size) || (k_ra_qspi_xip_size - offset < len)) {
    ra_log_error(s_tag, "ra_qspi_receive: range out of XIP window");
    return k_ra_err_invalid_arg;
  }
  /* Touch the channel registers so cppcheck sees the parameter consumed. */
  (void)reg->SFMSST;
  volatile uint8_t* xip = ra_qspi_xip();
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = xip[offset + i];
  }
  return k_ra_ok;
}

ra_err_t ra_qspi_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "ra_qspi_status: out_mask null");
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t sfmsst = reg->SFMSST;
  const uint32_t sfmcst = reg->SFMCST;
  uint8_t        mask   = 0U;
  if ((sfmsst & k_ra_qspi_sfmsst_busy) != 0U) {
    mask |= k_ra_qspi_status_busy;
  }
  if ((sfmsst & k_ra_qspi_sfmsst_txfull) != 0U) {
    mask |= k_ra_qspi_status_tx_full;
  }
  if ((sfmsst & k_ra_qspi_sfmsst_rxrdy) != 0U) {
    mask |= k_ra_qspi_status_rx_rdy;
  }
  if (sfmcst != 0U) {
    mask |= k_ra_qspi_status_xip;
  }
  *out_mask = mask;
  return k_ra_ok;
}

ra_err_t ra_qspi_xip_enter(uint8_t channel)
{
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  reg->SFMCMD = k_ra_qspi_sfmcmd_xip_enter;
  reg->SFMCST = 1U;
  return k_ra_ok;
}

ra_err_t ra_qspi_xip_exit(uint8_t channel)
{
  volatile r_qspi_t* reg = ra_qspi_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  reg->SFMCMD = k_ra_qspi_sfmcmd_xip_exit;
  reg->SFMCST = 0U;
  return k_ra_ok;
}
