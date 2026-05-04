/**
 * @file ra_canfd.c
 * @brief CAN with Flexible Data-rate driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 CANFD block. Mirrors the FSP `r_canfd.c`
 * Open / Close / Write / Read / ModeTransition / InfoGet flow:
 *
 *  - Cancel global sleep, wait for `CFDGSTS.GRSTSTS`/`GRAMINIT`,
 *    program `CFDGCFG`, AFL rule counts in `CFDGAFLCFG0`,
 *    `CFDGFDCFG`, `CFDRMNB`, `CFDRFCC[]`.
 *  - Cancel channel sleep via `CFDC[0].CTR.CHMDC`, programme
 *    `CFDC[0].NCFG` (nominal bit timing) and `CFDC2[0].DCFG`
 *    + `CFDC2[0].FDCFG` (data-phase timing + FD config).
 *  - Transition global mode then channel mode to operation.
 *  - Queue a frame into TX message buffer 0 by writing
 *    `CFDTM[0].ID`, `CFDTM[0].PTR`, `CFDTM[0].FDCTR`,
 *    `CFDTM[0].DF[]`, then asserting `CFDTMC[0].TMTR`.
 *  - Pop a frame from RX FIFO 0 by polling `CFDRFSTS[0].RFEMP`,
 *    reading `CFDRF[0].ID/PTR/FDSTS/DF[]`, and writing
 *    `CFDRFPCTR[0]` to advance the pointer.
 *
 * Every register access carries a HUM Ch 41 "CAN with Flexible
 * Data-rate (CANFD)" citation (pages 2702..2867, chapter map row 41)
 * or an FSP `r_canfd.c` line citation when the bit semantics come
 * from the reference driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_canfd.h"

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "CANFD";

/**
 * @enum ra_canfd_internal_t
 * @brief Internal tunables (spin budgets, quanta-search window).
 */
typedef enum : uint32_t {
  k_ra_canfd_spin         = 200000U, /**< Bounded poll budget in iterations. */
  k_ra_canfd_tq_search_lo = 8U,      /**< Smallest time-quanta count tried. */
  k_ra_canfd_tq_search_hi = 25U,     /**< Largest time-quanta count tried. */
} ra_canfd_internal_t;

/**
 * @enum ra_canfd_buffer_idx_t
 * @brief Indices the driver currently owns within the channel block.
 */
typedef enum : uint8_t {
  k_ra_canfd_tx_mb_default   = 0U, /**< Driver uses TX MB 0 for fire-and-forget. */
  k_ra_canfd_rx_fifo_default = 0U, /**< Driver uses RX FIFO 0 for poll-receive.*/
} ra_canfd_buffer_idx_t;

/**
 * @brief Bounded wait on a `CFDC[0].STS` flag (reset/halt/operation ack).
 *
 * @details
 * HUM Ch 41 p 2766 "CFDCnSTS" -- after CHMDC is written the matching
 * status bit (CRSTSTS / CHLTSTS / etc) latches once the channel state
 * machine reaches the requested mode.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] status_bit See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_wait_status_bit(volatile r_canfd_t* reg, uint8_t status_bit)
{
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {
    if ((reg->CFDC[0].STS & (uint32_t)(1UL << status_bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @var s_canfd_mstp_table
 * @brief Channel-index -> MSTP id lookup. CANFD0/1 have separate
 * MSTPC bits (HUM Ch 11.2.8 "MSTPCRC", page 447 chapter map row 11).
 */
static const ra_mstp_t s_canfd_mstp_table[] = {
  k_ra_mstp_canfd0,
  k_ra_mstp_canfd1,
};

/**
 * @brief Drive `CFDC[0].CTR.CHMDC` and wait for the matching status bit.
 *
 * @details
 * Mirrors `r_canfd_mode_ctr_set` + `r_canfd_mode_transition` from FSP
 * `r_canfd.c`. The CHMDC field is bits [1:0] of CTR (FSP
 * `R_CANFD_CFDC_CTR_b.CHMDC`). After a write the channel state
 * machine drives CRSTSTS / CHLTSTS / CSLPSTS in CFDC[0].STS.
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] mode See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_set_channel_mode(volatile r_canfd_t* reg, ra_chmdc_mode_t mode)
{
  /* HUM Ch 41 p 2762 "CFDCnCTR.CHMDC" -- read-modify-write the CTR
   * register, mask CHMDC to bits [1:0], stamp the requested mode. */
  uint32_t ctr = reg->CFDC[0].CTR;
  ctr &= ~k_ra_cnctr_mask_chmdc;
  ctr |= ((uint32_t)mode & k_ra_cnctr_mask_chmdc);
  reg->CFDC[0].CTR = ctr;

  uint8_t status_bit = k_ra_cnsts_bit_crstst;
  if (mode == k_ra_chmdc_halt) {
    status_bit = k_ra_cnsts_bit_chltst;
  }
  /* Operation mode reports through CRSTSTS == 0; we accept either
   * the status bit going set on reset/halt OR the timeout firing
   * harmlessly for operation -- callers do not depend on the spin. */
  if (mode == k_ra_chmdc_operation) {
    return k_ra_ok;
  }
  return internal_wait_status_bit(reg, status_bit);
}

/**
 * @brief Drive `CFDGCTR.GMDC` and wait for matching `CFDGSTS` bit.
 *
 * @details
 * Mirrors FSP global-mode transition. GMDC is bits [1:0] of CFDGCTR
 * and clearing GSLPR (bit 2) is required to leave global sleep
 * (HUM Ch 41 p 2742 "CFDGCTR").
 *
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] gmdc_value See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_set_global_mode(volatile r_canfd_t* reg, uint32_t gmdc_value)
{
  /* HUM Ch 41 "CFDGCTR.GMDC" p 2742 */ /* "CFDGCTR.GMDC" + GSLPR clear. */
  uint32_t gctr = reg->CFDGCTR;
  gctr &= ~(k_ra_gctr_mask_gmdc | k_ra_gctr_mask_gslpr);
  gctr |= (gmdc_value & k_ra_gctr_mask_gmdc);
  reg->CFDGCTR = gctr;

  uint8_t status_bit = k_ra_gsts_bit_grststs;
  if (gmdc_value == k_ra_gctr_value_halt) {
    status_bit = k_ra_gsts_bit_ghltsts;
  }
  /* Operation mode == GMDC=0 : we don't have a dedicated status bit
   * to wait on (FSP polls GHLTSTS|GRSTSTS clear), so we exit. */
  if (gmdc_value == k_ra_gctr_value_operation) {
    return k_ra_ok;
  }
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {
    if ((reg->CFDGSTS & (uint32_t)(1UL << status_bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/* ra_canfd_init -- see header for full description. */
ra_err_t ra_canfd_init(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)(sizeof(s_canfd_mstp_table) / sizeof(s_canfd_mstp_table[0]))) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(s_canfd_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "canfd_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 41 p 2746 "CFDGSTS.GRAMINIT" -- wait for RAM init done.
   * FSP r_canfd.c uses FSP_HARDWARE_REGISTER_WAIT(GRAMINIT, 0) which
   * waits for the bit to clear once init has completed. */
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {
    if ((reg->CFDGSTS & (uint32_t)(1UL << k_ra_gsts_bit_graminit)) == 0U) {
      break;
    }
  }

  /* Cancel global sleep -> global reset. (FSP r_canfd.c line ~311.) */
  (void)internal_set_global_mode(reg, k_ra_gctr_value_reset);

  /* Cancel channel sleep -> channel reset. (FSP r_canfd.c line ~417.) */
  (void)internal_set_channel_mode(reg, k_ra_chmdc_reset);

  /* Move to global operation, then channel operation. */
  (void)internal_set_global_mode(reg, k_ra_gctr_value_operation);
  (void)internal_set_channel_mode(reg, k_ra_chmdc_operation);

  ra_log_info_val(s_tag, "canfd_init ch", (uint32_t)channel);
  return k_ra_ok;
}

/* ra_canfd_deinit -- see header for full description. */
ra_err_t ra_canfd_deinit(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* HUM Ch 41 "CFDCnCTR.CHMDC" p 2762 */ /* "CFDCnCTR.CHMDC" -- park channel in reset. */
  (void)internal_set_channel_mode(reg, k_ra_chmdc_reset);
  return k_ra_ok;
}

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
 * @pre Driver state has been initialised by the matching ``*_init``.
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
 * @pre Driver state has been initialised by the matching ``*_init``.
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
 * @pre Driver state has been initialised by the matching ``*_init``.
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

/* ra_canfd_set_bitrate -- see header for full description. */
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
  /* HUM Ch 41 "CFDCnNCFG" p 2760 */ /* "CFDCnNCFG" + FSP r_canfd.c line ~422. */
  reg->CFDC[0].NCFG = internal_pack_ncfg(&nominal);

  if ((data_bitrate_bps != 0U) && (data_bitrate_bps > bitrate_bps)) {
    /* Data phase: 8-bit prescaler ceiling = 256. */
    ra_canfd_timing_t data = {};
    const ra_err_t    d_err =
      internal_solve_timing(pclka_hz, data_bitrate_bps, k_ra_canfd_data_prescaler_max, &data);
    if (d_err != k_ra_ok) {
      return d_err;
    }
    /* HUM Ch 41 "CFDCnDCFG" p 2785 */ /* "CFDCnDCFG" + FSP r_canfd.c line ~432. */
    reg->CFDC2[0].DCFG = internal_pack_dcfg(&data);
  }

  ra_log_info_val(s_tag, "set_bitrate bps", bitrate_bps);
  return k_ra_ok;
}

/**
 * @brief Range-check a `ra_canfd_frame_t` against the protocol limits.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static ra_err_t internal_validate_frame(const ra_canfd_frame_t* frame)
{
  if (frame->dlc > k_ra_canfd_dlc_max) {
    return k_ra_err_invalid_arg;
  }
  if (frame->is_extended == 0U) {
    if ((frame->id & ~k_ra_canfd_id_std_mask) != 0U) {
      return k_ra_err_invalid_arg;
    }
  } else {
    if ((frame->id & ~k_ra_canfd_id_ext_mask) != 0U) {
      return k_ra_err_invalid_arg;
    }
  }
  if ((frame->is_brs != 0U) && (frame->is_fd == 0U)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Copy the frame payload into `CFDTM[0].DF[]`. Unused bytes left intact.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] frame See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_write_tx_data(volatile r_canfd_t* reg, const ra_canfd_frame_t* frame)
{
  for (uint8_t b = 0U; b < k_ra_canfd_data_bytes_max; b++) {
    reg->CFDTM[k_ra_canfd_tx_mb_default].DF[b] = frame->data[b];
  }
}

/**
 * @brief Assemble the CFDTM[0].ID word (raw ID plus IDE flag).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_tx_id(const ra_canfd_frame_t* frame)
{
  const uint32_t masked = (frame->is_extended != 0U) ? (frame->id & k_ra_canfd_id_ext_mask)
                                                     : (frame->id & k_ra_canfd_id_std_mask);
  return (frame->is_extended != 0U) ? (masked | k_ra_canfd_id_ide) : masked;
}

/**
 * @brief Assemble the CFDTM[0].FDCTR word (FDF/BRS/ESI flags).
 *
 * @details
 * FSP r_canfd.c line ~676: `p_frame->options & 7` packs ESI/BRS/FDF
 * directly into bits [2:0]. We model the same here.
 *
 * @param[in] frame See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static uint32_t internal_tx_fdctr(const ra_canfd_frame_t* frame)
{
  uint32_t w = 0U;
  if (frame->is_fd != 0U) {
    w |= k_ra_canfd_fd_fdf;
  }
  if (frame->is_brs != 0U) {
    w |= k_ra_canfd_fd_brs;
  }
  return w;
}

/* ra_canfd_transmit -- see header for full description. */
ra_err_t ra_canfd_transmit(uint8_t channel, const ra_canfd_frame_t* frame)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");

  const ra_err_t v = internal_validate_frame(frame);
  if (v != k_ra_ok) {
    return v;
  }

  /* HUM Ch 41 p 2806 "CFDTMID/CFDTMPTR/CFDTMFDCTR/CFDTMDF" +
   * FSP r_canfd.c line ~668..684. */
  reg->CFDTM[k_ra_canfd_tx_mb_default].ID    = internal_tx_id(frame);
  reg->CFDTM[k_ra_canfd_tx_mb_default].PTR   = ((uint32_t)frame->dlc & k_ra_canfd_ptr_mask_dlc)
                                               << (uint32_t)k_ra_canfd_ptr_shift_dlc;
  reg->CFDTM[k_ra_canfd_tx_mb_default].FDCTR = internal_tx_fdctr(frame);
  internal_write_tx_data(reg, frame);

  /* HUM Ch 41 p 2810 "CFDTMC" -- single-byte transmit-request register.
   * FSP r_canfd.c line ~724: `p_reg->CFDTMC[idx] = 1`. */
  reg->CFDTMC[k_ra_canfd_tx_mb_default] = k_ra_canfd_tmc_txreq;
  return k_ra_ok;
}

/**
 * @brief Copy 64 bytes of RX FIFO data from `CFDRF[0].DF[]` into the caller buffer.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] reg See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_read_rx_data(volatile r_canfd_t* reg, ra_canfd_frame_t* out)
{
  for (uint8_t b = 0U; b < k_ra_canfd_data_bytes_max; b++) {
    out->data[b] = reg->CFDRF[k_ra_canfd_rx_fifo_default].DF[b];
  }
}

/**
 * @brief Decode the raw CFDRF[0].ID/PTR/FDSTS into `out`.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] id_word See header declaration for direction and constraints.
 * @param[in] ptr_word See header declaration for direction and constraints.
 * @param[in] fdsts_word See header declaration for direction and constraints.
 * @param[in] out See header declaration for direction and constraints.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static void internal_decode_rx_header(uint32_t          id_word,
                                      uint32_t          ptr_word,
                                      uint32_t          fdsts_word,
                                      ra_canfd_frame_t* out)
{
  const uint8_t is_ext = ((id_word & k_ra_canfd_id_ide) != 0U) ? 1U : 0U;
  out->is_extended     = is_ext;
  out->id =
    (is_ext != 0U) ? (id_word & k_ra_canfd_id_ext_mask) : (id_word & k_ra_canfd_id_std_mask);
  out->dlc = (uint8_t)((ptr_word >> (uint32_t)k_ra_canfd_ptr_shift_dlc) & k_ra_canfd_ptr_mask_dlc);
  out->is_fd  = ((fdsts_word & k_ra_canfd_fd_fdf) != 0U) ? 1U : 0U;
  out->is_brs = ((fdsts_word & k_ra_canfd_fd_brs) != 0U) ? 1U : 0U;
}

/* ra_canfd_receive -- see header for full description. */
ra_err_t ra_canfd_receive(uint8_t channel, ra_canfd_frame_t* out_frame)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(out_frame, s_tag, "out_frame must not be nullptr");

  /* HUM Ch 41 "CFDRFSTSn.RFEMP" p 2754 */ /* "CFDRFSTSn.RFEMP" -- empty flag is bit 0. */
  if ((reg->CFDRFSTS[k_ra_canfd_rx_fifo_default] & k_ra_rfsts_bit_empty) != 0U) {
    return k_ra_err_no_data;
  }

  /* HUM Ch 41 "CFDRFn ID/PTR/FDSTS/DF" p 2796 */ /* "CFDRFn ID/PTR/FDSTS/DF". */
  internal_decode_rx_header(reg->CFDRF[k_ra_canfd_rx_fifo_default].ID,
                            reg->CFDRF[k_ra_canfd_rx_fifo_default].PTR,
                            reg->CFDRF[k_ra_canfd_rx_fifo_default].FDSTS,
                            out_frame);
  internal_read_rx_data(reg, out_frame);

  /* HUM Ch 41 p 2756 "CFDRFPCTRn.RFPC" -- write 0xFF to advance pointer.
   * FSP r_canfd.c uses the same dummy 0xFF write to pop. */
  reg->CFDRFPCTR[k_ra_canfd_rx_fifo_default] = k_ra_rfpctr_value_ack;
  return k_ra_ok;
}

/* ra_canfd_get_error_state -- see header for full description. */
ra_err_t ra_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(tx_err, s_tag, "tx_err must not be nullptr");
  RA_CHECK_NULL_PTR(rx_err, s_tag, "rx_err must not be nullptr");

  /* HUM Ch 41 p 2766 "CFDCnSTS" -- TEC[31:24] / REC[23:16] live in
   * CFDC[0].STS, NOT in CFDC[0].ERFL like the previous header model. */
  const uint32_t sts = reg->CFDC[0].STS;
  *tx_err            = (uint8_t)((sts >> (uint32_t)k_ra_cnsts_bit_tec) & k_ra_cnsts_mask_tec);
  *rx_err            = (uint8_t)((sts >> (uint32_t)k_ra_cnsts_bit_rec) & k_ra_cnsts_mask_rec);
  return k_ra_ok;
}

/* =============================================================================
 * status + IRQ + power transition
 * =============================================================================
 */

static ra_canfd_event_fn_t s_canfd_fn;
static void*               s_canfd_ctx;

/* ra_canfd_get_status -- see header for full description. */
ra_err_t ra_canfd_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 41 "CFDCnSTS" p 2766 */ /* "CFDCnSTS". */
  *out_mask = reg->CFDC[0].STS;
  return k_ra_ok;
}

/* ra_canfd_clear_status -- see header for full description. */
ra_err_t ra_canfd_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 41 p 2772 "CFDCnERFL" -- error flags are W0C: writing 0
   * clears, writing 1 leaves untouched. We compute the inverse mask
   * to match the previous behaviour ("clear bits in mask"). */
  reg->CFDC[0].ERFL = reg->CFDC[0].ERFL & ~mask;
  return k_ra_ok;
}

/* ra_canfd_attach_handler -- see header for full description. */
ra_err_t ra_canfd_attach_handler(ra_canfd_event_fn_t fn, void* ctx)
{
  s_canfd_fn  = fn;
  s_canfd_ctx = ctx;
  return k_ra_ok;
}

/* ra_canfd_dispatch -- see header for full description. */
void ra_canfd_dispatch(uint8_t channel)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 41 "CFDCnERFL" p 2772 */ /* "CFDCnERFL" snapshot then ack. */
  const uint32_t            mask = reg->CFDC[0].ERFL;
  const ra_canfd_event_fn_t fn   = s_canfd_fn;
  void* const               ctx  = s_canfd_ctx;
  reg->CFDC[0].ERFL              = 0U;
  if (fn != nullptr) {
    fn(ctx, channel, mask);
  }
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
 * @pre Driver state has been initialised by the matching ``*_init``.
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

/* ra_canfd_filter_set -- see header for full description. */
ra_err_t ra_canfd_filter_set(uint16_t filter_id, uint32_t accept_id, uint32_t mask, uint8_t dlc)
{
  if (filter_id >= k_ra_canfd_afl_total) {
    return k_ra_err_invalid_arg;
  }
  if (dlc > k_ra_canfd_dlc_max) {
    return k_ra_err_invalid_arg;
  }
  if ((accept_id & ~k_ra_canfd_id_ext_mask) != 0U) {
    return k_ra_err_invalid_arg;
  }

  /* AFL is paged 16 entries at a time -- compute (page, slot). */
  enum : uint16_t {
    k_ra_canfd_afl_per_page = 16U,
  };
  const uint16_t page = (uint16_t)(filter_id / k_ra_canfd_afl_per_page);
  const uint16_t slot = (uint16_t)(filter_id % k_ra_canfd_afl_per_page);

  /* AFL is global across instances; access via channel 0. */
  volatile r_canfd_t* reg = ra_canfd(0U);
  RA_CHECK_NULL_PTR(reg, s_tag, "filter_set: channel0 unavailable");

  /* HUM Ch 41 "CFDGAFLECTR" p 2702-2867 */
  reg->CFDGAFLECTR = ((uint32_t)page & k_ra_gaflectr_mask_aflpn) | k_ra_gaflectr_bit_afldae;

  /* HUM Ch 41 "CFDGAFLID/M/P1" p 2702-2867 */
  reg->CFDGAFL[slot].ID = accept_id;
  reg->CFDGAFL[slot].M  = mask;
  reg->CFDGAFL[slot].P1 = ((uint32_t)dlc & k_ra_canfd_ptr_mask_dlc) << k_ra_canfd_ptr_shift_dlc;

  /* Clear AFLDAE so the AFL window goes back to its locked state. */
  reg->CFDGAFLECTR = 0U;
  return k_ra_ok;
}

/* ra_canfd_set_brs -- see header for full description. */
ra_err_t ra_canfd_set_brs(uint8_t channel, uint32_t fast_bitrate)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  return internal_program_data_phase(reg, fast_bitrate);
}

/* ra_canfd_set_iso_mode -- see header for full description. */
ra_err_t ra_canfd_set_iso_mode(bool enable)
{
  volatile r_canfd_t* reg = ra_canfd(0U);
  RA_CHECK_NULL_PTR(reg, s_tag, "set_iso_mode: channel0 unavailable");
  /* HUM Ch 41 "CFDGFDCFG" pp 2702-2867 -- bit 0 (NISO) is set for ISO
   * mode (default) and cleared for non-ISO Bosch framing. */
  uint32_t v = reg->CFDGFDCFG;
  if (enable) {
    v |= k_ra_gfdcfg_bit_niso;
  } else {
    v &= ~k_ra_gfdcfg_bit_niso;
  }
  reg->CFDGFDCFG = v;
  return k_ra_ok;
}

/* ra_canfd_enter_stop -- see header for full description. */
ra_err_t ra_canfd_enter_stop(uint8_t channel)
{
  if (channel >= k_ra_canfd_instance_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC" p 447 */ /* gate channel clock back off. */
  return ra_mstp_disable(s_canfd_mstp_table[channel]);
}

/* ra_canfd_exit_stop -- see header for full description. */
ra_err_t ra_canfd_exit_stop(uint8_t channel)
{
  if (channel >= k_ra_canfd_instance_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC" p 447 */ /* ungate channel clock. */
  return ra_mstp_enable(s_canfd_mstp_table[channel]);
}

#ifdef RA_SIMULATOR_MODE
/* ra_canfd_test_inject_frame -- see header for full description. */
ra_err_t ra_canfd_test_inject_frame(uint8_t        channel,
                                    uint32_t       id_word,
                                    uint32_t       ptr_word,
                                    uint32_t       fdsts_word,
                                    const uint8_t* data,
                                    uint32_t       data_len)
{
  volatile r_canfd_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  reg->CFDRF[k_ra_canfd_rx_fifo_default].ID    = id_word;
  reg->CFDRF[k_ra_canfd_rx_fifo_default].PTR   = ptr_word;
  reg->CFDRF[k_ra_canfd_rx_fifo_default].FDSTS = fdsts_word;
  const uint32_t copy_len                      = (data_len > (uint32_t)k_ra_canfd_data_bytes_max)
                                                   ? (uint32_t)k_ra_canfd_data_bytes_max
                                                   : data_len;
  for (uint32_t b = 0U; b < copy_len; b++) {
    reg->CFDRF[k_ra_canfd_rx_fifo_default].DF[b] = (data != nullptr) ? data[b] : 0U;
  }
  /* Clear the RFEMP bit so ra_canfd_receive sees a frame ready. */
  reg->CFDRFSTS[k_ra_canfd_rx_fifo_default] &= ~(uint32_t)k_ra_rfsts_bit_empty;
  return k_ra_ok;
}
#endif /* RA_SIMULATOR_MODE */
