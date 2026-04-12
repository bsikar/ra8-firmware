/**
 * @file adc.c
 * @brief ADC_B polling driver
 *
 * @details
 * Minimum-viable single-channel polling ADC driver using the ADC_B
 * peripheral on the RA8D2. `ra_adc_init()` programmes 14-bit
 * resolution, right-aligned result format, software trigger, and
 * leaves ADCSR.ADST at 0. `ra_adc_read_channel()` enables the given
 * channel, sets ADST=1, busy-waits for ADST to auto-clear, then
 * reads `ADDRxx`.
 *
 * Not covered yet: DMA, group scans, hardware trigger routing,
 * sample-and-hold tuning, comparator mode, PGA. Those extensions
 * grow `ra8d2_adc_b_regs.h` and this file incrementally.
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
  /* Leave ADST clear and programme the extended control register. */
  *ra_adc_b_adcsr() = (uint16_t)k_ra_adcsr_reset_val;
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

  /* Programme sample time and the channel-select bitmap for ADANSA0
   * / ADANSA1 so only the requested channel is sampled. */
  *adsstr             = (uint8_t)k_ra_adsstr_default;
  *ra_adc_b_adansa0() = (uint16_t)(1U << (channel & (uint8_t)k_ra_adc_ansa0_channel_mask));

  /* Kick: ADCSR.ADST = 1. */
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
