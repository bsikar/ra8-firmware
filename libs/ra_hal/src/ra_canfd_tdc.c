/**
 * @file ra_canfd_tdc.c
 * @brief CANFD Transmitter Delay Compensation (TDC) configuration
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Single public entry-point: ``ra_canfd_set_tdc`` -- enables, configures,
 * or disables the Transmitter Delay Compensation block in CFDCnFDCFG for
 * one CANFD channel. The write is bracketed by CH_RESET / CH_OPERATION
 * mode transitions using the shared helper
 * ``ra_canfd_internal_set_channel_mode`` (promoted to TU-external linkage
 * in ``ra_hal_internal.h``) -- the same strategy used by
 * ``ra_canfd_set_bitrate`` in ``ra_canfd_timing.c``.
 *
 * Every register access carries a HUM Ch 41 "CAN with Flexible
 * Data-rate (CANFD)" citation (pages 2702..2867, chapter map row 41).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_canfd.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_hal_internal.h"
#include "ra_log.h"

static const char* s_tag = "CANFD";

/** @brief Implementation of `ra_canfd_set_tdc()` -- RMW CFDCnFDCFG in CH_RESET. */
ra_err_t ra_canfd_set_tdc(uint8_t channel, const ra_canfd_tdc_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (cfg->offset > (uint8_t)k_ra_canfd_tdc_offset_max) {
    return k_ra_err_invalid_arg;
  }

  /* FDCFG is only writable in CH_RESET or CH_HALT; use CH_RESET as
   * the immediate abort path (mirrors ra_canfd_set_bitrate). */
  /* HUM Ch 41 "CFDCnFDCFG" p 2788 */
  const ra_err_t rst_err = ra_canfd_internal_set_channel_mode(reg, k_ra_chmdc_reset);
  if (rst_err != k_ra_ok) {
    return rst_err;
  }

  /* Read-modify-write FDCFG: preserve FDOE, REFE, CLOE, ESIC, EOCCFG.
   * Clear TDE, TDCOC, and the TDCO slot before re-stamping. */
  /* HUM Ch 41 "CFDCnFDCFG" p 2788 */
  uint32_t       fdcfg     = reg->CFDC2[0].FDCFG;
  const uint32_t tdco_slot = (uint32_t)k_ra_fdcfg_mask_tdco << (uint32_t)k_ra_fdcfg_shift_tdco;
  fdcfg &= ~(k_ra_fdcfg_mask_tde | k_ra_fdcfg_mask_tdcoc | tdco_slot);
  if (cfg->enable) {
    fdcfg |= k_ra_fdcfg_mask_tde;
    fdcfg |= ((uint32_t)cfg->offset & (uint32_t)k_ra_fdcfg_mask_tdco)
             << (uint32_t)k_ra_fdcfg_shift_tdco;
    if (cfg->manual) {
      fdcfg |= k_ra_fdcfg_mask_tdcoc;
    }
  }
  /* HUM Ch 41 "CFDCnFDCFG" p 2788 */
  reg->CFDC2[0].FDCFG = fdcfg;

  const ra_err_t op_err = ra_canfd_internal_set_channel_mode(reg, k_ra_chmdc_operation);
  if (op_err != k_ra_ok) {
    return op_err;
  }
  ra_log_info(s_tag, "tdc configured");
  return k_ra_ok;
}
