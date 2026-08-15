/**
 * @file ra8_pdm.c
 * @brief Pulse Density Modulation Interface (PDM-IF) capture driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode capture driver for the RA8D2 PDM-IF block. Implements the
 * HUM Ch 49.4.1 "Start Flow": configure a channel's mode + decimation +
 * FIR coefficients, activate the filter, prime the FIFO and drain 20-bit
 * signed PCM from the data-read register. The hardware performs the
 * PDM->PCM decimation; this driver only sequences and reads it. Every
 * register access carries a HUM Ch 49 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_pdm.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_pdm_regs.h"

static const char* s_tag = "PDM";

/** @brief Per-channel interrupt callback binding. */
typedef struct {
  ra8_pdm_data_callback_t callback; /**< Caller callback. */
  void*                   ctx;      /**< Caller context.  */
} pdm_stream_state_t;

static pdm_stream_state_t s_streams[k_ra8_pdm_ch_count];

static const ra8_elc_event_t s_pdm_data_events[k_ra8_pdm_ch_count] = {
  k_ra8_elc_event_pdm_dat0,
  k_ra8_elc_event_pdm_dat1,
  k_ra8_elc_event_pdm_dat2,
};

/**
 * @enum pdm_impl_const_t
 * @brief Implementation-local bounds (HUM Ch 49 "Normal Processing Flow").
 */
typedef enum : uint32_t {
  k_pdm_prime_discards = 30U,   /**< FIFO depth (32) minus 2 priming reads. */
  k_pdm_stop_poll_max  = 1000U, /**< Bounded stop-poll retry budget.        */
  k_pdm_field_3bit     = 0x7U,  /**< 3-bit field mask (SFMD).               */
} pdm_impl_const_t;

/**
 * @brief Write a channel's PDMDSR + PDSFCR mode/decimation words.
 *
 * @details
 * Packs the mode-setting fields (sinc order, filter input shifts, data
 * shift, input edge) into PDMDSR and the clock divider + sinc decimation
 * + clip range into PDSFCR.
 *
 * @param[in,out] reg Channel register bank (non-NULL).
 * @param[in]     cfg Channel configuration (non-NULL).
 *
 * @return Nothing.
 *
 * @pre ``reg`` points at a valid PDM channel bank.
 * @pre ``cfg->sinc_order`` is in 1..4.
 * @post PDMDSR and PDSFCR hold the packed configuration.
 * @post No other channel register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdm_write_mode(volatile r_pdm_ch_regs_t*    reg,
                                                 const ra8_pdm_channel_cfg_t* cfg)
{
  uint32_t pdmdsr = (uint32_t)(cfg->edge & (uint8_t)k_ra8_pdm_pdmdsr_inpsel);
  pdmdsr |= ((uint32_t)cfg->sinc_order & k_pdm_field_3bit) << k_ra8_pdm_pdmdsr_sfmd_pos;
  pdmdsr |= (uint32_t)cfg->hpf_shift << k_ra8_pdm_pdmdsr_hfis_pos;
  pdmdsr |= (uint32_t)cfg->cf_shift << k_ra8_pdm_pdmdsr_cfis_pos;
  pdmdsr |= (uint32_t)cfg->lpf_shift << k_ra8_pdm_pdmdsr_lfis_pos;
  pdmdsr |= (uint32_t)cfg->data_shift << k_ra8_pdm_pdmdsr_dbis_pos;
  /* HUM Ch 49.2.19 "PDMDSRCHn : Mode Setting Register" p 3208 */
  reg->PDMDSR = pdmdsr;

  uint32_t pdsfcr = (uint32_t)cfg->clock_div << k_ra8_pdm_pdsfcr_ckdiv_pos;
  pdsfcr |= (uint32_t)cfg->sinc_dec << k_ra8_pdm_pdsfcr_sincdec_pos;
  pdsfcr |= (uint32_t)cfg->sinc_range << k_ra8_pdm_pdsfcr_sincrng_pos;
  /* HUM Ch 49.2.20 "PDSFCRCHn : Sinc Filter Control Register" p 3210 */
  reg->PDSFCR = pdsfcr;
}

/**
 * @brief Write a channel's high-pass, compensation and low-pass coefficients.
 *
 * @details
 * Loads the three FIR banks (HPF s0/k1/h[2], compensation h[11], low-pass
 * h0/h1[20]) that the sinc chain feeds. The values are the microphone/rate
 * filter set carried by @p cfg.
 *
 * @param[in,out] reg Channel register bank (non-NULL).
 * @param[in]     cfg Channel configuration (non-NULL).
 *
 * @return Nothing.
 *
 * @pre ``reg`` points at a valid PDM channel bank.
 * @pre ``cfg`` carries a complete coefficient set.
 * @post All HPF/compensation/low-pass coefficient registers hold ``cfg``.
 * @post No mode or trigger register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdm_write_coeffs(volatile r_pdm_ch_regs_t*    reg,
                                                   const ra8_pdm_channel_cfg_t* cfg)
{
  /* HUM Ch 49.2 "Filter coefficient registers" p 3210 */
  reg->PDHFCS0R = (uint32_t)cfg->hpf_s0;
  reg->PDHFCK1R = (uint32_t)cfg->hpf_k1;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_pdm_hpf_h_count; ++i) {
    /* HUM Ch 49.2 "PDHFCHnRCHn : High-pass filter Coefficient" p 3211 */
    reg->PDHFCHR[i] = (uint32_t)cfg->hpf_h[i];
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_pdm_comp_h_count; ++i) {
    /* HUM Ch 49.2 "PDCFCHnnRCHn : Compensation filter Coefficient" p 3213 */
    reg->PDCFCHR[i] = (uint32_t)cfg->comp_h[i];
  }
  /* HUM Ch 49.2 "PDLFCH010RCHn : Low-pass filter Coefficient h0(10)" p 3216 */
  reg->PDLFCH010R = (uint32_t)cfg->lpf_h0;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_pdm_lpf_h1_count; ++i) {
    /* HUM Ch 49.2 "PDLFCH1nnRCHn : Low-pass filter Coefficient h1" p 3218 */
    reg->PDLFCH1R[i] = (uint32_t)cfg->lpf_h1[i];
  }
}

/**
 * @brief Sign-extend one 20-bit PDDRR sample to a full ``int32_t``.
 *
 * @details Masks ``raw`` to its low 20 significant bits, then, if bit 19 (the
 * two's-complement sign bit) is set, fills the upper 12 bits with ones so the
 * value reads as a correctly-signed ``int32_t``; otherwise returns the masked
 * value unchanged. Pure bit manipulation, no hardware access.
 *
 * @param[in] raw Raw PDDRR read (only the low 20 bits are significant).
 *
 * @return Signed 32-bit PCM sample.
 * @retval <0 The 20-bit sign bit (bit 19) was set; upper bits filled with ones.
 * @retval >=0 The 20-bit sign bit was clear; the masked low-20-bit value.
 *
 * @pre ``raw`` came from a PDDRR read.
 * @pre The channel is producing 20-bit data (DBIS 20-bit mode).
 * @post The result equals ``raw`` interpreted as a 20-bit two's-complement.
 * @post No hardware state is touched.
 *
 * @note Pure helper, thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static int32_t internal_pdm_sign_extend20(uint32_t raw)
{
  const uint32_t dat = raw & (uint32_t)k_ra8_pdm_pddrr_dat_mask;
  if ((dat & (uint32_t)k_ra8_pdm_pddrr_sign_bit) != 0U) {
    return (int32_t)(dat | (uint32_t)k_ra8_pdm_pddrr_sign_ext);
  }
  return (int32_t)dat;
}

/**
 * @brief Drain one PDM FIFO threshold and dispatch its signed samples.
 * @details Bounds the hardware-reported fill level to the FIFO depth, converts
 *          each 20-bit word, and invokes the registered channel callback once.
 * @param[in] ctx Encoded channel index.
 * @pre Registered only by ::ra8_pdm_stream_enable.
 * @pre The channel callback remains valid until stream disable completes.
 * @post Available FIFO samples are consumed and delivered once.
 * @post Invalid channels and unregistered callbacks return without MMIO access.
 * @note Runs in interrupt context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdm_data_isr(void* ctx)
{
  const uint8_t channel = (uint8_t)(uintptr_t)ctx;
  if (channel >= (uint8_t)k_ra8_pdm_ch_count) {
    return;
  }
  pdm_stream_state_t* stream = &s_streams[channel];
  if (stream->callback == nullptr) {
    return;
  }
  volatile r_pdm_ch_regs_t* reg = ra8_pdm_ch(channel);
  /* HUM Ch 49.2.66 "PDDSRCHn : Data Status Register" p 3228 */
  uint32_t count = reg->PDDSR & (uint32_t)k_ra8_pdm_pddsr_num_mask;
  if (count > (uint32_t)k_ra8_pdm_fifo_depth) {
    count = (uint32_t)k_ra8_pdm_fifo_depth;
  }
  int32_t samples[k_ra8_pdm_fifo_depth];
  for (uint32_t i = 0U; i < count; ++i) {
    /* HUM Ch 49.2.65 "PDDRRCHn : Data Read Register" p 3227 */
    samples[i] = internal_pdm_sign_extend20(reg->PDDRR);
  }
  if (count != 0U) {
    stream->callback(stream->ctx, samples, count);
  }
}

ra8_err_t ra8_pdm_init(void)
{
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_pdmif);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "pdm_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_pdm_regs_t* reg = ra8_pdm();
  /* HUM Ch 49.2.4 "PDCICR : Channel Interrupt Control Register" p 3198 */
  reg->PDCICR = 0U;
  /* HUM Ch 49.2.9 "PDCDRCR : Channel Data Read Control Register" p 3202 */
  reg->PDCDRCR = 0U;
  ra8_log_info(s_tag, "pdm_init");
  return k_ra8_ok;
}

ra8_err_t ra8_pdm_deinit(void)
{
  volatile r_pdm_regs_t* reg = ra8_pdm();
  /* HUM Ch 49.2.9 "PDCDRCR : Channel Data Read Control Register" p 3202 */
  reg->PDCDRCR = 0U;
  return ra8_mstp_disable(k_ra8_mstp_pdmif);
}

ra8_err_t ra8_pdm_configure(uint8_t ch, const ra8_pdm_channel_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);

  volatile r_pdm_ch_regs_t* reg = ra8_pdm_ch(ch);
  internal_pdm_write_mode(reg, cfg);
  internal_pdm_write_coeffs(reg, cfg);
  /* HUM Ch 49.2.59 "PDDBCRCHn : Data Buffer Control Register" p 3225 */
  reg->PDDBCR = (uint32_t)cfg->rx_threshold;
  return k_ra8_ok;
}

ra8_err_t ra8_pdm_start(uint8_t ch)
{
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);
  /* HUM Ch 49.2.2 "PDCSTRTR : Channel Software Start Trigger Register" p 3196 */
  ra8_pdm()->PDCSTRTR = (uint32_t)k_ra8_pdm_pdstrtr_strt << ch;
  return k_ra8_ok;
}

ra8_err_t ra8_pdm_read_enable(uint8_t ch)
{
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);

  volatile r_pdm_ch_regs_t* reg = ra8_pdm_ch(ch);
  /* HUM Ch 49.2.18 "PDSCRCHn : Status Clear Register" p 3208 */
  reg->PDSCR = (uint32_t)k_ra8_pdm_pdscr_clr_all;
  /* HUM Ch 49.2.63 "PDDRCRCHn : Data Read Control Register" p 3227 */
  reg->PDDRCR = (uint32_t)k_ra8_pdm_pddrcr_datre;
  /* Prime the FIFO: discard the first (depth - 2) reads before valid data. */
  for (uint32_t i = 0U; i < (uint32_t)k_pdm_prime_discards; ++i) {
    /* HUM Ch 49.2.65 "PDDRRCHn : Data Read Register" p 3227 */
    (void)reg->PDDRR;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_pdm_read(uint8_t ch, int32_t* out, uint32_t max, uint32_t* out_count)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);
  if (max == 0U) {
    ra8_log_error(s_tag, "read: max must be > 0");
    return k_ra8_err_invalid_arg;
  }

  volatile r_pdm_ch_regs_t* reg = ra8_pdm_ch(ch);
  /* HUM Ch 49.2.66 "PDDSRCHn : Data Status Register" p 3228 */
  const uint32_t avail = reg->PDDSR & (uint32_t)k_ra8_pdm_pddsr_num_mask;
  const uint32_t n     = (avail < max) ? avail : max;
  for (uint32_t i = 0U; i < n; ++i) {
    /* HUM Ch 49.2.65 "PDDRRCHn : Data Read Register" p 3227 */
    out[i] = internal_pdm_sign_extend20(reg->PDDRR);
  }
  *out_count = n;
  return k_ra8_ok;
}

ra8_err_t
ra8_pdm_stream_enable(uint8_t ch, ra8_pdm_data_callback_t callback, void* ctx, uint8_t priority)
{
  RA8_CHECK_NULL_PTR(callback, s_tag, "callback must not be nullptr");
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);
  if (s_streams[ch].callback != nullptr) {
    return k_ra8_err_exists;
  }
  s_streams[ch]       = (pdm_stream_state_t){.callback = callback, .ctx = ctx};
  const ra8_err_t err = ra8_isr_register(s_pdm_data_events[ch],
                                         internal_pdm_data_isr,
                                         (void*)(uintptr_t)ch,
                                         priority,
                                         nullptr);
  if (err != k_ra8_ok) {
    s_streams[ch] = (pdm_stream_state_t){};
    return err;
  }
  /* HUM Ch 49.2.15 "PDICRCHn : Interrupt Control Register" p 3205 */
  ra8_pdm_ch(ch)->PDICR |= (uint32_t)k_ra8_pdm_pdicr_idre;
  return k_ra8_ok;
}

ra8_err_t ra8_pdm_stream_disable(uint8_t ch)
{
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);
  if (s_streams[ch].callback == nullptr) {
    return k_ra8_err_not_initialized;
  }
  /* HUM Ch 49.2.15 "PDICRCHn : Interrupt Control Register" p 3205 */
  ra8_pdm_ch(ch)->PDICR &= ~(uint32_t)k_ra8_pdm_pdicr_idre;
  const ra8_err_t err = ra8_isr_unregister(s_pdm_data_events[ch]);
  if (err != k_ra8_ok) {
    return err;
  }
  s_streams[ch] = (pdm_stream_state_t){};
  return k_ra8_ok;
}

ra8_err_t ra8_pdm_stop(uint8_t ch)
{
  RA8_CHECK_RANGE_TAG(ch, 0, (uint8_t)k_ra8_pdm_ch_count - 1U, k_ra8_err_invalid_arg, s_tag);

  if (s_streams[ch].callback != nullptr) {
    const ra8_err_t stream_err = ra8_pdm_stream_disable(ch);
    RA8_RETURN_ON_ERROR(stream_err, s_tag, "pdm_stop: stream disable");
  }

  /* HUM Ch 49.2.63 "PDDRCRCHn : Data Read Control Register" p 3227 */
  ra8_pdm_ch(ch)->PDDRCR = 0U;
  /* HUM Ch 49.2.3 "PDCSTPTR : Channel Software Stop Trigger Register" p 3197 */
  ra8_pdm()->PDCSTPTR = (uint32_t)k_ra8_pdm_pdstptr_stp << ch;

  const uint32_t running = (uint32_t)k_ra8_pdm_pdsr_state << ch;
  for (uint32_t i = 0U; i < (uint32_t)k_pdm_stop_poll_max; ++i) {
    /* HUM Ch 49.2.6 "PDCSR : Channel Status Register" p 3199 */
    if ((ra8_pdm()->PDCSR & running) == 0U) {
      return k_ra8_ok;
    }
  }
  ra8_log_error(s_tag, "stop: channel did not halt");
  return k_ra8_err_hw_timeout;
}
