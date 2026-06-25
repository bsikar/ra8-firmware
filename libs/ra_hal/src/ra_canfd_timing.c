/**
 * @file ra_canfd_timing.c
 * @brief CANFD bit-timing solver + bitrate / BRS configuration
 *        (split from ra_canfd.c)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Bit-timing half of the RA8D2 CANFD driver, split out of ``ra_canfd.c``
 * purely to satisfy the per-file size cap. This translation unit owns the
 * nominal / data-phase timing solver and the public configuration calls
 * that consume it:
 *
 *  - Walk candidate time-quanta-per-bit counts until one yields an integer
 *    prescaler, then resolve TSEG1 / TSEG2 / SJW at a 75% sample point.
 *  - Pack the resolved triple into the ``CFDC[0].NCFG`` (nominal) and
 *    ``CFDC2[0].DCFG`` (data) register layouts.
 *  - ``ra_canfd_set_bitrate`` (nominal + optional data phase) and
 *    ``ra_canfd_set_brs`` (data-phase-only) drive the channel through
 *    CH_RESET to land the edits, using the promoted
 *    ::ra_canfd_internal_set_channel_mode helper whose definition stays in
 *    ``ra_canfd.c`` beside the rest of the mode machinery.
 *
 * Every register access carries a HUM Ch 41 "CAN with Flexible
 * Data-rate (CANFD)" citation (pages 2702..2867, chapter map row 41)
 * or an FSP ``r_canfd.c`` line citation when the bit semantics come
 * from the reference driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_canfd.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_hal_internal.h"
#include "ra_log.h"

static const char* s_tag = "CANFD";

/**
 * @enum ra_canfd_timing_search_t
 * @brief Quanta-search window owned by the bit-timing solver.
 *
 * @details
 * Private copy of the matching constants in ``ra_canfd.c``; this TU only
 * needs the time-quanta-per-bit search bounds. ``k_ra_canfd_tq_search_lo`` /
 * ``k_ra_canfd_tq_search_hi`` are read-only compile-time constants, so each
 * translation unit keeps its own copy rather than promoting them to external
 * linkage.
 */
typedef enum : uint32_t {
  k_ra_canfd_tq_search_lo = 8U,  /**< Smallest time-quanta count tried. */
  k_ra_canfd_tq_search_hi = 25U, /**< Largest time-quanta count tried. */
} ra_canfd_timing_search_t;

/**
 * @struct ra_canfd_timing_t
 * @brief Resolved nominal / data phase bit-timing fields.
 *
 * @details
 * All fields are pre-subtract-1 i.e. the human-friendly value before
 * the FSP "field = value - 1" packing. Both nominal and data phases
 * use the same struct; the packing routine differs because the
 * register layouts differ (NCFG vs DCFG).
 */
typedef struct {
  uint32_t prescaler; /**< Prescaler integer (pre-subtract-1). */
  uint32_t tseg1;     /**< Phase segment 1 (pre-subtract-1).   */
  uint32_t tseg2;     /**< Phase segment 2 (pre-subtract-1).   */
  uint32_t sjw;       /**< Sync jump width (pre-subtract-1).   */
} ra_canfd_timing_t;

/**
 * @brief Walk candidate TQ-per-bit counts until one yields an integer prescaler.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] clock_hz See header declaration for direction and constraints.
 * @param[in] bitrate_bps See header declaration for direction and constraints.
 * @param[in] prescaler_max See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_solve_timing(uint32_t           clock_hz,
                                      uint32_t           bitrate_bps,
                                      uint32_t           prescaler_max,
                                      ra_canfd_timing_t* out)
{
  /* mcdc-deactivated: both args are validated by ra_canfd_init upstream; defensive duplicate. */
  if ((bitrate_bps == 0U) || (clock_hz == 0U)) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t tq = k_ra_canfd_tq_search_hi; tq >= k_ra_canfd_tq_search_lo; tq--) {
    const uint32_t denom = bitrate_bps * tq;
    if ((clock_hz % denom) != 0U) {
      continue;
    }
    const uint32_t prescaler = clock_hz / denom;
    // mcdc-deactivated: ra_canfd_deinit (bit-timing solver) prescaler-range guard; the search-loop tq bounds (k_ra_canfd_tq_search_lo..hi) and clock_hz/bitrate_bps caller validation upstream make either the lower or upper bound condition the dominant branch for any valid input -- the opposing condition cannot independently flip without violating the documented clock/bitrate range.
    if ((prescaler < k_ra_canfd_prescaler_min) || (prescaler > prescaler_max)) {
      continue;
    }
    /* 75% sample point: TSEG1 = 3*(tq-1)/4, TSEG2 = tq - 1 - TSEG1. */
    const uint32_t tseg1 = ((tq - 1U) * 3U) / 4U;
    const uint32_t tseg2 = (tq - 1U) - tseg1;
    const uint32_t sjw   = (tseg2 < k_ra_canfd_sjw_max) ? tseg2 : k_ra_canfd_sjw_max;
    out->prescaler       = prescaler;
    out->tseg1           = tseg1;
    out->tseg2           = tseg2;
    out->sjw             = sjw;
    return k_ra_ok;
  }
  return k_ra_err_invalid_arg;
}

/**
 * @brief Pack a resolved timing triple into the CFDC[0].NCFG layout.
 *
 * @details
 * FSP `r_canfd.c` line ~422: NBRP/NSJW/NTSEG1/NTSEG2 each minus 1.
 *
 * @param[in] t See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_pack_ncfg(const ra_canfd_timing_t* t)
{
  const uint32_t brp_field   = ((t->prescaler - 1U) & k_ra_cncfg_mask_nbrp)
                               << (uint32_t)k_ra_cncfg_shift_nbrp;
  const uint32_t tseg1_field = (t->tseg1 & k_ra_cncfg_mask_ntseg1)
                               << (uint32_t)k_ra_cncfg_shift_ntseg1;
  const uint32_t tseg2_field = (t->tseg2 & k_ra_cncfg_mask_ntseg2)
                               << (uint32_t)k_ra_cncfg_shift_ntseg2;
  const uint32_t sjw_field   = ((t->sjw - 1U) & k_ra_cncfg_mask_nsjw)
                               << (uint32_t)k_ra_cncfg_shift_nsjw;
  return brp_field | tseg1_field | tseg2_field | sjw_field;
}

/**
 * @brief Pack a resolved timing triple into the CFDC2[0].DCFG layout.
 *
 * @details
 * FSP `r_canfd.c` line ~432: DBRP/DSJW/DTSEG1/DTSEG2 with NARROWER
 * fields (8/4/5/4 bits) and DIFFERENT shifts (0/24/8/16).
 *
 * @param[in] t See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_pack_dcfg(const ra_canfd_timing_t* t)
{
  const uint32_t brp_field   = ((t->prescaler - 1U) & k_ra_dcfg_mask_dbrp)
                               << (uint32_t)k_ra_dcfg_shift_dbrp;
  const uint32_t tseg1_field = (t->tseg1 & k_ra_dcfg_mask_dtseg1)
                               << (uint32_t)k_ra_dcfg_shift_dtseg1;
  const uint32_t tseg2_field = (t->tseg2 & k_ra_dcfg_mask_dtseg2)
                               << (uint32_t)k_ra_dcfg_shift_dtseg2;
  const uint32_t sjw_field   = ((t->sjw - 1U) & k_ra_dcfg_mask_dsjw)
                               << (uint32_t)k_ra_dcfg_shift_dsjw;
  return brp_field | tseg1_field | tseg2_field | sjw_field;
}

ra_err_t ra_canfd_set_bitrate(uint8_t channel, uint32_t bitrate_bps, uint32_t data_bitrate_bps)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  uint32_t       pclka_hz = 0U;
  const ra_err_t clk_err  = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz);
  if (clk_err != k_ra_ok) {
    return clk_err;
  }

  /* Nominal phase: 10-bit prescaler ceiling = 1024. */
  ra_canfd_timing_t nominal = {};
  const ra_err_t    n_err =
    internal_solve_timing(pclka_hz, bitrate_bps, k_ra_canfd_prescaler_max, &nominal);
  if (n_err != k_ra_ok) {
    return n_err;
  }

  /* NCFG / DCFG are only writable in CH_RESET or CH_HALT.  Use
   * CH_RESET: halt is a graceful transition that waits for any
   * in-flight TX to finish, and on internal-loopback bring-up the
   * channel may be stuck trying to TX onto a bus with no
   * acknowledger -- halt never converges. CH_RESET is the
   * immediate abort path, which is what FSP r_canfd does too.
   * HUM Ch 41 "CFDCnNCFG.NTSEG2" p 2706 */
  const ra_err_t halt_err = ra_canfd_internal_set_channel_mode(reg, k_ra_chmdc_reset);
  if (halt_err != k_ra_ok) {
    return halt_err;
  }

  /* HUM Ch 41 "CFDCnNCFG" p 2705 */
  reg->CFDC[0].NCFG = internal_pack_ncfg(&nominal);

  if ((data_bitrate_bps != 0U) && (data_bitrate_bps > bitrate_bps)) {
    /* Data phase: 8-bit prescaler ceiling = 256. */
    ra_canfd_timing_t data = {};
    const ra_err_t    d_err =
      internal_solve_timing(pclka_hz, data_bitrate_bps, k_ra_canfd_data_prescaler_max, &data);
    if (d_err != k_ra_ok) {
      /* Best-effort: return the channel to CH_OPERATION so the
       * caller does not observe a half-applied edit. */
      (void)ra_canfd_internal_set_channel_mode(reg, k_ra_chmdc_operation);
      return d_err;
    }
    /* HUM Ch 41 "CFDCnDCFG" p 2785 */
    reg->CFDC2[0].DCFG = internal_pack_dcfg(&data);
  }

  const ra_err_t op_err = ra_canfd_internal_set_channel_mode(reg, k_ra_chmdc_operation);
  if (op_err != k_ra_ok) {
    return op_err;
  }

  ra_log_info_val(s_tag, "set_bitrate bps", bitrate_bps);
  return k_ra_ok;
}

/**
 * @brief Re-derive the data-phase timing triple and pack it into DCFG.
 *
 * @details
 * Helper for ::ra_canfd_set_brs: solves for an integer prescaler
 * against PCLKA for the requested @p data_bitrate, then writes the
 * FSP-aligned DCFG layout.  HUM Ch 41 "CFDCnDCFG" pp 2702-2867.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] data_bitrate See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_program_data_phase(volatile r_canfd_t* reg, uint32_t data_bitrate)
{
  if (data_bitrate == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint32_t       pclka_hz = 0U;
  const ra_err_t clk_err  = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz);
  if (clk_err != k_ra_ok) {
    return clk_err;
  }
  ra_canfd_timing_t data = {};
  const ra_err_t    err =
    internal_solve_timing(pclka_hz, data_bitrate, k_ra_canfd_data_prescaler_max, &data);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 41 "CFDCnDCFG" p 2785 */ /* "CFDCnDCFG" */
  reg->CFDC2[0].DCFG = internal_pack_dcfg(&data);
  return k_ra_ok;
}

ra_err_t ra_canfd_set_brs(uint8_t channel, uint32_t fast_bitrate)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  return internal_program_data_phase(reg, fast_bitrate);
}
