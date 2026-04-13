/**
 * @file ra_sdramc.c
 * @brief SDRAM controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Wave 5 driver for the RA8D2 SDRAM controller block (lives
 * under the Buses chapter, HUM Ch 15). Programmes the timing,
 * mode, refresh, and address decode registers for the on-board
 * 64 MiB SDRAM at 0x68000000. Exposes init/deinit, runtime
 * refresh-interval change, status read, and power transition.
 * Every register access carries a HUM Ch 15 citation.
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

  /* HUM Ch 15.10 "SDCCR : SDC Control Register" p 712 */
  reg->SDCCR = (uint16_t)k_ra_sdccr_default;
  /* HUM Ch 15.10 "SDCMOD : SDC Mode Register" p 713 */
  reg->SDCMOD = 0U;
  /* HUM Ch 15.10 "SDAMOD : SDC Address Mode Register" p 713 */
  reg->SDAMOD = 0U;
  /* HUM Ch 15.10 "SDTR : SDC Timing Register" p 714 */
  reg->SDTR = (uint32_t)k_ra_sdtr_default;
  /* HUM Ch 15.10 "SDRFCR : SDC Refresh Cycle Register" p 715 */
  reg->SDRFCR = (uint16_t)k_ra_sdrfcr_default;
  /* HUM Ch 15.10 "SDRFEN : SDC Refresh Enable Register" p 716 */
  reg->SDRFEN = 1U;
  /* HUM Ch 15.10 "SDICR : SDC Init Sequence Control Register" p 716 */
  reg->SDICR = 1U;

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
  /* HUM Ch 15.10 "SDRFEN : SDC Refresh Enable Register" p 716 */
  reg->SDRFEN = 0U;
  /* HUM Ch 15.10 "SDICR : SDC Init Sequence Control Register" p 716 */
  reg->SDICR = 0U;
  /* HUM Ch 15.10 "SDCCR : SDC Control Register" p 712 */
  reg->SDCCR = 0U;
  return k_ra_ok;
}

ra_err_t ra_sdramc_set_refresh_interval(uint16_t sdrfcr)
{
  /* HUM Ch 15.10 "SDRFCR : SDC Refresh Cycle Register" p 715 */
  ra_sdramc()->SDRFCR = sdrfcr;
  return k_ra_ok;
}

ra_err_t ra_sdramc_get_status(uint8_t* out_enabled)
{
  RA_CHECK_NULL_PTR(out_enabled, s_tag, "out_enabled must not be nullptr");
  /* HUM Ch 15.10 "SDRFEN : SDC Refresh Enable Register" p 716 */
  *out_enabled = ra_sdramc()->SDRFEN;
  return k_ra_ok;
}

ra_err_t ra_sdramc_enter_stop(void)
{
  /* HUM Ch 15.10 "SDRFEN : SDC Refresh Enable Register" p 716 */
  ra_sdramc()->SDRFEN = 0U;
  return k_ra_ok;
}

ra_err_t ra_sdramc_exit_stop(void)
{
  /* HUM Ch 15.10 "SDRFEN : SDC Refresh Enable Register" p 716 */
  ra_sdramc()->SDRFEN = 1U;
  return k_ra_ok;
}
