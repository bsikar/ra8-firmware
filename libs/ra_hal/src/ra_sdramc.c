/**
 * @file ra_sdramc.c
 * @brief SDRAM controller driver framework
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sdramc.h"

#include <stdint.h>

#include "ra8d2_sdramc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "SDRAM";

typedef enum : uint16_t {
  k_ra_sdccr_default  = 0x0011U, /**< 16-bit data bus, CAS=2 -- HUM default. */
  k_ra_sdrfcr_default = 0x002BU, /**< Refresh interval for 64 MiB part.      */
} ra_sdramc_defaults_t;

typedef enum : uint32_t {
  k_ra_sdtr_default = 0x00010222UL, /**< Conservative timing defaults. */
} ra_sdramc_timing_t;

ra_err_t ra_sdramc_init(void)
{
  volatile r_sdramc_regs_t* reg = ra_sdramc();

  reg->SDCCR  = (uint16_t)k_ra_sdccr_default;
  reg->SDCMOD = 0U;
  reg->SDAMOD = 0U;
  reg->SDTR   = (uint32_t)k_ra_sdtr_default;
  reg->SDRFCR = (uint16_t)k_ra_sdrfcr_default;
  reg->SDRFEN = 1U;
  reg->SDICR  = 1U;

  ra_log_info(s_tag, "sdramc_init (64 MiB @ 0x68000000)");
  return k_ra_ok;
}

/* =============================================================================
 * Wave 5.2 -- lifecycle + power
 * =============================================================================
 */

ra_err_t ra_sdramc_deinit(void)
{
  volatile r_sdramc_regs_t* reg = ra_sdramc();
  reg->SDRFEN                   = 0U;
  reg->SDICR                    = 0U;
  reg->SDCCR                    = 0U;
  return k_ra_ok;
}

ra_err_t ra_sdramc_set_refresh_interval(uint16_t sdrfcr)
{
  ra_sdramc()->SDRFCR = sdrfcr;
  return k_ra_ok;
}

ra_err_t ra_sdramc_get_status(uint8_t* out_enabled)
{
  RA_CHECK_NULL_PTR(out_enabled, s_tag, "out_enabled must not be nullptr");
  *out_enabled = ra_sdramc()->SDRFEN;
  return k_ra_ok;
}

ra_err_t ra_sdramc_enter_stop(void)
{
  ra_sdramc()->SDRFEN = 0U;
  return k_ra_ok;
}

ra_err_t ra_sdramc_exit_stop(void)
{
  ra_sdramc()->SDRFEN = 1U;
  return k_ra_ok;
}
