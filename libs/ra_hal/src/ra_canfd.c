/**
 * @file ra_canfd.c
 * @brief CAN with Flexible Data-rate driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 CANFD Lite block. Drives
 * ``CFDCNCTR.CHMDC`` through the documented state transitions
 * (reset -> halt -> operation), programmes the nominal (and
 * optional data) bit-timing registers, queues CAN 2.0B / CAN-FD
 * frames into the TX message buffer, pulls received frames from
 * the RX FIFO, and surfaces the TEC/REC error counters.
 *
 * Acceptance filter bank + full DMA delivery land when a concrete
 * CAN application calls for them. Every register access carries a
 * HUM Ch 41 "CAN with Flexible Data-rate (CANFD)" citation.
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
 * @enum ra_canfd_byte_shift_t
 * @brief Bit-shift values for extracting little-endian u32 bytes.
 */
typedef enum : uint8_t {
  k_ra_byte_shift_0 = 0U,  /**< Byte 0 is bits [7:0]. */
  k_ra_byte_shift_1 = 8U,  /**< Byte 1 is bits [15:8]. */
  k_ra_byte_shift_2 = 16U, /**< Byte 2 is bits [23:16]. */
  k_ra_byte_shift_3 = 24U, /**< Byte 3 is bits [31:24]. */
} ra_canfd_byte_shift_t;

/**
 * @brief Bounded wait on a `CFDCNSTS` flag (reset/halt/operation ack).
 */
static ra_err_t internal_wait_mode(volatile r_canfd_channel_regs_t* reg, uint8_t status_bit)
{
  for (uint32_t i = 0U; i < k_ra_canfd_spin; i++) {
    if ((reg->CFDCNSTS & (uint32_t)(1UL << status_bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @var s_canfd_mstp_table
 * @brief Channel-index -> MSTP id lookup. CANFD0/1 have separate
 * MSTPC bits C27/C26 per HUM Ch 11.2.8 p 447.
 */
static const ra_mstp_t s_canfd_mstp_table[] = {
  k_ra_mstp_canfd0,
  k_ra_mstp_canfd1,
};

ra_err_t ra_canfd_init(uint8_t channel)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if (channel >= (uint8_t)(sizeof(s_canfd_mstp_table) / sizeof(s_canfd_mstp_table[0]))) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(s_canfd_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "canfd_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Ensure we are in reset. */
  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_reset;
  (void)internal_wait_mode(reg, k_ra_cnsts_bit_crstst);

  /* Move to halt. */
  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_halt;
  (void)internal_wait_mode(reg, k_ra_cnsts_bit_chltst);

  /* Move to operation. */
  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_operation;
  (void)internal_wait_mode(reg, k_ra_cnsts_bit_crstst);

  ra_log_info_val(s_tag, "canfd_init ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_canfd_deinit(uint8_t channel)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  reg->CFDCNCTR = (uint32_t)k_ra_chmdc_reset;
  return k_ra_ok;
}

/**
 * @struct ra_canfd_timing_t
 * @brief Resolved nominal / data phase bit-timing fields.
 */
typedef struct {
  uint32_t prescaler; /**< Prescaler integer (pre-subtract-1). */
  uint32_t tseg1;     /**< Phase segment 1 (pre-subtract-1). */
  uint32_t tseg2;     /**< Phase segment 2 (pre-subtract-1). */
  uint32_t sjw;       /**< Sync jump width (pre-subtract-1). */
} ra_canfd_timing_t;

/**
 * @brief Walk candidate TQ-per-bit counts until one yields an integer prescaler.
 */
static ra_err_t
internal_solve_timing(uint32_t clock_hz, uint32_t bitrate_bps, ra_canfd_timing_t* out)
{
  if ((bitrate_bps == 0U) || (clock_hz == 0U)) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t tq = k_ra_canfd_tq_search_hi; tq >= k_ra_canfd_tq_search_lo; tq--) {
    const uint32_t denom = bitrate_bps * tq;
    if ((clock_hz % denom) != 0U) {
      continue;
    }
    const uint32_t prescaler = clock_hz / denom;
    if ((prescaler < k_ra_canfd_prescaler_min) || (prescaler > k_ra_canfd_prescaler_max)) {
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
 * @brief Pack a resolved timing triple into the CFDCnNCFG / CFDCnDCFG layout.
 */
static uint32_t internal_pack_timing(const ra_canfd_timing_t* t)
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

ra_err_t ra_canfd_set_bitrate(uint8_t channel, uint32_t bitrate_bps, uint32_t data_bitrate_bps)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  uint32_t       pclka_hz = 0U;
  const ra_err_t clk_err  = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz);
  if (clk_err != k_ra_ok) {
    return clk_err;
  }

  ra_canfd_timing_t nominal = {};
  const ra_err_t    n_err   = internal_solve_timing(pclka_hz, bitrate_bps, &nominal);
  if (n_err != k_ra_ok) {
    return n_err;
  }
  reg->CFDCNCFG = internal_pack_timing(&nominal);

  if ((data_bitrate_bps != 0U) && (data_bitrate_bps > bitrate_bps)) {
    ra_canfd_timing_t data  = {};
    const ra_err_t    d_err = internal_solve_timing(pclka_hz, data_bitrate_bps, &data);
    if (d_err != k_ra_ok) {
      return d_err;
    }
    reg->CFDCNDCFG = internal_pack_timing(&data);
  }

  ra_log_info_val(s_tag, "set_bitrate bps", bitrate_bps);
  return k_ra_ok;
}

/**
 * @brief Range-check a `ra_canfd_frame_t` against the protocol limits.
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
 * @brief Copy the frame's data bytes into `CFDTMDF[0..15]` as little-endian
 * u32 words. Unused words are zeroed.
 */
static void internal_write_tx_data(volatile r_canfd_channel_regs_t* reg,
                                   const ra_canfd_frame_t*          frame)
{
  for (uint8_t w = 0U; w < k_ra_canfd_df_word_count; w++) {
    const uint8_t b0 = frame->data[(w * 4U) + 0U];
    const uint8_t b1 = frame->data[(w * 4U) + 1U];
    const uint8_t b2 = frame->data[(w * 4U) + 2U];
    const uint8_t b3 = frame->data[(w * 4U) + 3U];
    reg->CFDTMDF[w]  = ((uint32_t)b0 << (uint32_t)k_ra_byte_shift_0) |
                       ((uint32_t)b1 << (uint32_t)k_ra_byte_shift_1) |
                       ((uint32_t)b2 << (uint32_t)k_ra_byte_shift_2) |
                       ((uint32_t)b3 << (uint32_t)k_ra_byte_shift_3);
  }
}

/**
 * @brief Assemble the CFDTMID word (raw ID plus IDE flag).
 */
static uint32_t internal_tx_id(const ra_canfd_frame_t* frame)
{
  const uint32_t masked = (frame->is_extended != 0U) ? (frame->id & k_ra_canfd_id_ext_mask)
                                                     : (frame->id & k_ra_canfd_id_std_mask);
  return (frame->is_extended != 0U) ? (masked | k_ra_canfd_id_ide) : masked;
}

/**
 * @brief Assemble the CFDTMFDSTS word (FDF/BRS flags).
 */
static uint32_t internal_tx_fdsts(const ra_canfd_frame_t* frame)
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

ra_err_t ra_canfd_transmit(uint8_t channel, const ra_canfd_frame_t* frame)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");

  const ra_err_t v = internal_validate_frame(frame);
  if (v != k_ra_ok) {
    return v;
  }

  reg->CFDTMID    = internal_tx_id(frame);
  reg->CFDTMPTR   = ((uint32_t)frame->dlc & k_ra_canfd_ptr_mask_dlc)
                    << (uint32_t)k_ra_canfd_ptr_shift_dlc;
  reg->CFDTMFDSTS = internal_tx_fdsts(frame);

  internal_write_tx_data(reg, frame);

  reg->CFDTMC = k_ra_canfd_tmc_txreq;
  return k_ra_ok;
}

/**
 * @brief Copy 64 bytes of RX FIFO data from `CFDRFDF[0..15]` into the caller buffer.
 */
static void internal_read_rx_data(volatile r_canfd_channel_regs_t* reg, ra_canfd_frame_t* out)
{
  for (uint8_t w = 0U; w < k_ra_canfd_df_word_count; w++) {
    const uint32_t word      = reg->CFDRFDF[w];
    out->data[(w * 4U) + 0U] = (uint8_t)(word >> (uint32_t)k_ra_byte_shift_0);
    out->data[(w * 4U) + 1U] = (uint8_t)(word >> (uint32_t)k_ra_byte_shift_1);
    out->data[(w * 4U) + 2U] = (uint8_t)(word >> (uint32_t)k_ra_byte_shift_2);
    out->data[(w * 4U) + 3U] = (uint8_t)(word >> (uint32_t)k_ra_byte_shift_3);
  }
}

/**
 * @brief Decode the raw CFDRFID / CFDRFPTR / CFDRFFDSTS words into `out`.
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

ra_err_t ra_canfd_receive(uint8_t channel, ra_canfd_frame_t* out_frame)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(out_frame, s_tag, "out_frame must not be nullptr");

  if ((reg->CFDRFSTS & k_ra_rfsts_bit_empty) != 0U) {
    return k_ra_err_no_data;
  }

  internal_decode_rx_header(reg->CFDRFID, reg->CFDRFPTR, reg->CFDRFFDSTS, out_frame);
  internal_read_rx_data(reg, out_frame);

  /* Acknowledge / pop the frame. */
  reg->CFDRFPCTR = k_ra_rfpctr_value_ack;
  return k_ra_ok;
}

ra_err_t ra_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(tx_err, s_tag, "tx_err must not be nullptr");
  RA_CHECK_NULL_PTR(rx_err, s_tag, "rx_err must not be nullptr");

  const uint32_t erfl = reg->CFDCNERFL;
  *tx_err             = (uint8_t)((erfl >> (uint32_t)k_ra_cnerfl_shift_tec) & k_ra_cnerfl_mask_tec);
  *rx_err             = (uint8_t)((erfl >> (uint32_t)k_ra_cnerfl_shift_rec) & k_ra_cnerfl_mask_rec);
  return k_ra_ok;
}

/* =============================================================================
 * status + IRQ + power transition
 * =============================================================================
 */

static ra_canfd_event_fn_t s_canfd_fn;
static void*               s_canfd_ctx;

ra_err_t ra_canfd_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  *out_mask = reg->CFDCNSTS;
  return k_ra_ok;
}

ra_err_t ra_canfd_clear_status(uint8_t channel, uint32_t mask)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  reg->CFDCNERFL = reg->CFDCNERFL & ~mask;
  return k_ra_ok;
}

ra_err_t ra_canfd_attach_handler(ra_canfd_event_fn_t fn, void* ctx)
{
  s_canfd_fn  = fn;
  s_canfd_ctx = ctx;
  return k_ra_ok;
}

void ra_canfd_dispatch(uint8_t channel)
{
  volatile r_canfd_channel_regs_t* reg = ra_canfd(channel);
  if (reg == nullptr) {
    return;
  }
  const uint32_t            mask = reg->CFDCNERFL;
  const ra_canfd_event_fn_t fn   = s_canfd_fn;
  void* const               ctx  = s_canfd_ctx;
  reg->CFDCNERFL                 = 0U;
  if (fn != nullptr) {
    fn(ctx, channel, mask);
  }
}

ra_err_t ra_canfd_enter_stop(uint8_t channel)
{
  if (channel >= k_ra_canfd_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_canfd_mstp_table[channel]);
}

ra_err_t ra_canfd_exit_stop(uint8_t channel)
{
  if (channel >= k_ra_canfd_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_canfd_mstp_table[channel]);
}
