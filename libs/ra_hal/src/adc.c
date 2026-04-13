/**
 * @file adc.c
 * @brief 16-bit A/D Converter (ADC16H / ADC_B) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 4 driver for the RA8D2 ADC_B peripheral. Programmes
 * 14-bit resolution, right-aligned result format, software
 * trigger, and exposes init / configured-init / deinit / polling
 * read / runtime resolution change / status / async dispatch /
 * power transition. Every register access carries a HUM Ch 53
 * citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_adc_b_regs.h"
#include "ra_adc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ADC";

/**
 * @enum ra_adc_init_val_t
 * @brief Magic register values used during ADC_B init.
 */
typedef enum : uint16_t {
  k_ra_adcsr_reset_val = 0x0000U,
  /* ADCER: right-aligned 16-bit result, 14-bit resolution. */
  k_ra_adcer_14bit_right_aligned = (uint16_t)((1U << 15U) | (2U << 1U)),
  k_ra_adsstr_default            = 0x20U,   /**< ~32 ADCLK sample cycles. */
  k_ra_adc_ansa0_channel_mask    = 0x000FU, /**< Low 4 bits of channel -> ANSA0 slot. */
} ra_adc_init_val_t;

ra_err_t ra_adc_init(void)
{
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 449 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_adc16h);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "adc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 53.2.1 "ADCSR : A/D Control/Status Register" p 3309 */
  *ra_adc_b_adcsr() = (uint16_t)k_ra_adcsr_reset_val;
  /* HUM Ch 53.2.4 "ADCER : A/D Control Extended Register" p 3315 */
  *ra_adc_b_adcer() = (uint16_t)k_ra_adcer_14bit_right_aligned;
  ra_log_info(s_tag, "adc_init ready");
  return k_ra_ok;
}

ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  RA_CHECK_NULL_PTR(out_raw, s_tag, "out_raw must not be nullptr");

  volatile uint16_t* addr   = ra_adc_b_addr(channel);
  volatile uint8_t*  adsstr = ra_adc_b_adsstr(channel);
  if (addr == nullptr || adsstr == nullptr) {
    return k_ra_err_out_of_range;
  }

  /* HUM Ch 53.2.5 "ADSSTRn : A/D Sampling State Register n" p 3317 */
  *adsstr = (uint8_t)k_ra_adsstr_default;
  /* HUM Ch 53.2.2 "ADANSA0 : A/D Channel Select Register A0" p 3312 */
  *ra_adc_b_adansa0() = (uint16_t)(1U << (channel & (uint8_t)k_ra_adc_ansa0_channel_mask));

  /* HUM Ch 53.2.1 "ADCSR : A/D Control/Status Register" p 3309 */
  /* ADST = 1 kicks the conversion. */
  volatile uint16_t* adcsr = ra_adc_b_adcsr();
  *adcsr                   = (uint16_t)((uint16_t)*adcsr | (uint16_t)(1U << k_ra_adcsr_bit_adst));

  /* Poll ADST until hardware clears it. Bounded so a stuck channel
   * cannot block the caller forever. */
  enum : uint32_t { k_ra_adc_poll_limit = 200000U };
  for (uint32_t i = 0U; i < k_ra_adc_poll_limit; i++) {
    if ((*adcsr & (uint16_t)(1U << k_ra_adcsr_bit_adst)) == 0U) {
      *out_raw = *addr;
      return k_ra_ok;
    }
  }

  *out_raw = 0U;
  ra_log_error(s_tag, "ADST poll timeout");
  return k_ra_err_hw_timeout;
}

/* =============================================================================
 * Wave 4.1 -- full build-out
 * =============================================================================
 */

/**
 * @enum ra_adc_w41_bits_t
 * @brief Bit shifts and masks used by Wave 4.1 helpers.
 */
typedef enum : uint16_t {
  k_ra_adc_adcs_scan_shift = 13U,     /**< ADCSR.ADCS bit.        */
  k_ra_adc_trge_shift      = 9U,      /**< ADCSR.TRGE bit.        */
  k_ra_adc_exce_shift      = 10U,     /**< ADCSR.EXCE bit.        */
  k_ra_adc_adprc_shift     = 1U,      /**< ADCER.ADPRC bit0.      */
  k_ra_adc_adrfmt_shift    = 15U,     /**< ADCER.ADRFMT bit.      */
  k_ra_adc_status_mask_all = 0x9000U, /**< ADST|ADIE bits.    */
} ra_adc_w41_bits_t;

/**
 * @struct ra_adc_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra_adc_complete_fn_t fn;         /**< Registered callback.     */
  void*                ctx;        /**< Callback context.        */
  bool                 configured; /**< Set after init.          */
} ra_adc_state_t;

static ra_adc_state_t s_adc_state;

static uint16_t internal_cfg_to_adcsr(const ra_adc_cfg_t* cfg)
{
  uint16_t v = 0U;
  if (cfg->scan_mode) {
    v |= (uint16_t)(1U << k_ra_adc_adcs_scan_shift);
  }
  if (cfg->trigger == k_ra_adc_trig_external) {
    v |= (uint16_t)(1U << k_ra_adc_trge_shift);
  } else if (cfg->trigger == k_ra_adc_trig_elc) {
    v |= (uint16_t)((1U << k_ra_adc_trge_shift) | (1U << k_ra_adc_exce_shift));
  }
  return v;
}

static uint16_t internal_cfg_to_adcer(const ra_adc_cfg_t* cfg)
{
  uint16_t v = (uint16_t)((uint16_t)cfg->resolution << k_ra_adc_adprc_shift);
  if (cfg->right_aligned) {
    v |= (uint16_t)(1U << k_ra_adc_adrfmt_shift);
  }
  return v;
}

ra_err_t ra_adc_init_configured(const ra_adc_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_adc16h);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "adc_init_cfg: mstp enable");

  /* HUM Ch 53.2.1 "ADCSR : A/D Control/Status Register" p 3309 */
  *ra_adc_b_adcsr() = internal_cfg_to_adcsr(cfg);
  /* HUM Ch 53.2.4 "ADCER : A/D Control Extended Register" p 3315 */
  *ra_adc_b_adcer()      = internal_cfg_to_adcer(cfg);
  s_adc_state.configured = true;
  ra_log_info(s_tag, "adc_init_configured ready");
  return k_ra_ok;
}

ra_err_t ra_adc_deinit(void)
{
  /* HUM Ch 53.2.1 "ADCSR : A/D Control/Status Register" p 3309 */
  *ra_adc_b_adcsr() = 0U;
  /* HUM Ch 53.2.4 "ADCER : A/D Control Extended Register" p 3315 */
  *ra_adc_b_adcer()      = 0U;
  s_adc_state.fn         = nullptr;
  s_adc_state.ctx        = nullptr;
  s_adc_state.configured = false;
  return ra_mstp_disable(k_ra_mstp_adc16h);
}

ra_err_t ra_adc_set_resolution(ra_adc_resolution_t resolution)
{
  if ((uint8_t)resolution > (uint8_t)k_ra_adc_res_14bit) {
    return k_ra_err_invalid_arg;
  }
  volatile uint16_t* adcer = ra_adc_b_adcer();
  const uint16_t     mask  = (uint16_t)(3U << k_ra_adc_adprc_shift);
  *adcer                   = (uint16_t)(((uint16_t)*adcer & ~mask) |
                                        (uint16_t)((uint16_t)resolution << k_ra_adc_adprc_shift));
  return k_ra_ok;
}

ra_err_t ra_adc_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint16_t)((uint16_t)*ra_adc_b_adcsr() & (uint16_t)k_ra_adc_status_mask_all);
  return k_ra_ok;
}

ra_err_t ra_adc_clear_status(void)
{
  volatile uint16_t* adcsr = ra_adc_b_adcsr();
  *adcsr = (uint16_t)((uint16_t)*adcsr & (uint16_t)~(uint16_t)k_ra_adc_status_busy);
  return k_ra_ok;
}

ra_err_t ra_adc_attach_handler(ra_adc_complete_fn_t fn, void* ctx)
{
  s_adc_state.fn  = fn;
  s_adc_state.ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_adc_enter_stop(void)
{
  *ra_adc_b_adcsr() =
    (uint16_t)((uint16_t)*ra_adc_b_adcsr() & (uint16_t)~(uint16_t)k_ra_adc_status_busy);
  return ra_mstp_disable(k_ra_mstp_adc16h);
}

ra_err_t ra_adc_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_adc16h);
}

void ra_adc_dispatch_cnv_end(uint8_t channel)
{
  volatile uint16_t* addr = ra_adc_b_addr(channel);
  if (addr == nullptr) {
    return;
  }
  const uint16_t             result = *addr;
  const ra_adc_complete_fn_t fn     = s_adc_state.fn;
  void* const                ctx    = s_adc_state.ctx;
  if (fn != nullptr) {
    fn(ctx, result);
  }
}
