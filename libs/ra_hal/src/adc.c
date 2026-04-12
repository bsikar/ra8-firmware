/**
 * @file adc.c
 * @brief ADC_B polling driver stub
 *
 * @details
 * The ADC_B block is much more complex than the classic ADC found
 * on RX72N -- it has per-unit configuration, calibration sequences,
 * and digital-filter options. The full driver will land once the
 * register header is expanded. Until then, `ra_adc_read_channel()`
 * returns `k_ra_err_not_supported` so callers can detect that the
 * ADC layer is not yet live.
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

ra_err_t ra_adc_init(void)
{
  ra_log_info(s_tag, "adc_init (stub until ra8d2_adc_b_regs.h is expanded)");
  return k_ra_ok;
}

ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  (void)channel;
  RA_CHECK_NULL_PTR(out_raw, s_tag, "out_raw must not be nullptr");
  *out_raw = 0U;
  return k_ra_err_not_supported;
}
