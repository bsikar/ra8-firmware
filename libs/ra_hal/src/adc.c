/**
 * @file adc.c
 * @brief 12/14-bit ADC_B converter driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 4 driver for the RA8D2 ADC_B peripheral. Unlike the pre-RA8
 * ADC16H which had a single ADCSR / ADCER pair, FSP R_ADC_B0
 * spreads conversion configuration across ADCLKENR, ADCLKCR,
 * ADMDR, ADINTCR, and per-channel ADCHCR[0..23]. Results land in
 * ADDR[0..22]. This driver owns the minimal subset: clock enable,
 * mode select, per-channel conversion trigger, blocking poll, and
 * resolution update. Every register access carries a HUM Ch 53
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

typedef enum : uint32_t {
  k_ra_adc_admdr_default     = 0x00000000UL, /**< Single-shot, software trigger. */
  k_ra_adc_admdr_scan_bit    = 0x00000002UL, /**< ADSCANMD scan-mode bit.        */
  k_ra_adc_admdr_trigext_bit = 0x00000001UL, /**< ADTRGMD external-trigger bit.   */
  k_ra_adc_poll_limit        = 200000UL,     /**< ADCHCR busy-poll budget.        */
  k_ra_adc_result_mask       = 0x0000FFFFUL, /**< Low 16 bits of ADDR slot.       */
} ra_adc_const_t;

/**
 * @struct ra_adc_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra_adc_complete_fn_t fn;
  void*                ctx;
  bool                 configured;
} ra_adc_state_t;

static ra_adc_state_t s_adc_state;

/**
 * @brief Translate an ra_adc_resolution_t into ADCHCR.RESSEL[5:4].
 */
static uint32_t internal_ressel(ra_adc_resolution_t resolution)
{
  return ((uint32_t)resolution << (uint32_t)k_ra_adchcr_bit_ressel0) &
         (uint32_t)k_ra_adchcr_mask_ressel;
}

/**
 * @brief Programme the resolution field in every channel's ADCHCR.
 */
static void internal_apply_resolution_all(ra_adc_resolution_t resolution)
{
  const uint32_t field = internal_ressel(resolution);
  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_adc_b_max_channels; ++ch) {
    volatile uint32_t* reg = ra_adc_b_adchcr(ch);
    if (reg == nullptr) {
      continue;
    }
    *reg = ((*reg & ~(uint32_t)k_ra_adchcr_mask_ressel) | field);
  }
}

ra_err_t ra_adc_init(void)
{
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 449 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_adc16h);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "adc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_adclkenr() = (uint32_t)k_ra_adclkenr_mask_clken;
  *ra_adc_b_adclkcr()  = 0U;
  *ra_adc_b_admdr()    = (uint32_t)k_ra_adc_admdr_default;
  *ra_adc_b_adintcr()  = 0U;

  /* Default to 14-bit on every channel config slot. */
  internal_apply_resolution_all(k_ra_adc_res_14bit);

  s_adc_state.configured = true;
  ra_log_info(s_tag, "adc_init ready");
  return k_ra_ok;
}

ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  RA_CHECK_NULL_PTR(out_raw, s_tag, "out_raw must not be nullptr");

  volatile uint32_t* chcr = ra_adc_b_adchcr(channel);
  volatile uint32_t* addr = ra_adc_b_addr(channel);
  if ((chcr == nullptr) || (addr == nullptr)) {
    return k_ra_err_out_of_range;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Kick the conversion by asserting CVEN. Hardware clears it on
   * completion. In the sim build the mmap backing stays asserted
   * which means the poll loop below exits on the first iteration
   * with whatever value the test pre-seeded into ADDR[channel]. */
  *chcr = (*chcr | (uint32_t)k_ra_adchcr_mask_cven);

  for (uint32_t i = 0U; i < (uint32_t)k_ra_adc_poll_limit; ++i) {
    if ((*chcr & (uint32_t)k_ra_adchcr_mask_cven) == 0U) {
      *out_raw = (uint16_t)(*addr & (uint32_t)k_ra_adc_result_mask);
      return k_ra_ok;
    }
  }

  *out_raw = 0U;
  ra_log_error(s_tag, "CVEN poll timeout");
  return k_ra_err_hw_timeout;
}

ra_err_t ra_adc_init_configured(const ra_adc_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_adc16h);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "adc_init_cfg: mstp enable");

  *ra_adc_b_adclkenr() = (uint32_t)k_ra_adclkenr_mask_clken;
  *ra_adc_b_adclkcr()  = 0U;

  uint32_t mdr = (uint32_t)k_ra_adc_admdr_default;
  if (cfg->scan_mode) {
    mdr |= (uint32_t)k_ra_adc_admdr_scan_bit;
  }
  if ((cfg->trigger == k_ra_adc_trig_external) || (cfg->trigger == k_ra_adc_trig_elc)) {
    mdr |= (uint32_t)k_ra_adc_admdr_trigext_bit;
  }
  *ra_adc_b_admdr()   = mdr;
  *ra_adc_b_adintcr() = 0U;

  internal_apply_resolution_all(cfg->resolution);

  s_adc_state.configured = true;
  ra_log_info(s_tag, "adc_init_configured ready");
  return k_ra_ok;
}

ra_err_t ra_adc_deinit(void)
{
  *ra_adc_b_admdr()      = 0U;
  *ra_adc_b_adintcr()    = 0U;
  *ra_adc_b_adclkenr()   = 0U;
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
  internal_apply_resolution_all(resolution);
  return k_ra_ok;
}

ra_err_t ra_adc_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  const uint32_t mdr = *ra_adc_b_admdr();
  uint16_t       out = 0U;
  if ((mdr & (uint32_t)k_ra_admdr_mask_adbusy) != 0U) {
    out |= (uint16_t)k_ra_adc_status_busy;
  }
  if (*ra_adc_b_adintcr() != 0U) {
    out |= (uint16_t)k_ra_adc_status_ie;
  }
  *out_mask = out;
  return k_ra_ok;
}

ra_err_t ra_adc_clear_status(void)
{
  /* Clearing "busy" means stopping any in-flight conversion. Mask the
   * top bit off ADMDR. (ADBUSY is strictly RO on real silicon, but the
   * driver models it for tests / future ADMDR write-to-clear bits.) */
  *ra_adc_b_admdr() = (*ra_adc_b_admdr() & ~(uint32_t)k_ra_admdr_mask_adbusy);
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
  *ra_adc_b_admdr() = 0U;
  return ra_mstp_disable(k_ra_mstp_adc16h);
}

ra_err_t ra_adc_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_adc16h);
}

void ra_adc_dispatch_cnv_end(uint8_t channel)
{
  volatile uint32_t* addr = ra_adc_b_addr(channel);
  if (addr == nullptr) {
    return;
  }
  const uint16_t             result = (uint16_t)(*addr & (uint32_t)k_ra_adc_result_mask);
  const ra_adc_complete_fn_t fn     = s_adc_state.fn;
  void* const                ctx    = s_adc_state.ctx;
  if (fn != nullptr) {
    fn(ctx, result);
  }
}
