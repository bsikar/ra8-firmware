/**
 * @file ra_adc_d.c
 * @brief Delta-Sigma A/D Converter (ADC-D) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_adc_d`` peripheral
 * driver. RA8D2 carries ADC-B; this file exposes the FSP-shaped
 * surface so cross-platform examples build cleanly.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_adc_d.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ADC_D";

/**
 * @enum ra_adc_d_limits_t
 * @brief Channel-count and status-bit constants.
 */
typedef enum : uint8_t {
  k_ra_adc_d_channel_count = 8U,
  k_ra_adc_d_status_open   = 0x1U,
  k_ra_adc_d_status_done   = 0x2U,
} ra_adc_d_limits_t;

/**
 * @enum ra_adc_d_seed_t
 * @brief Sample seed values for placeholder readouts.
 */
typedef enum : int32_t {
  k_ra_adc_d_seed_step  = 0x10000,
  k_ra_adc_d_seed_start = 0x100,
} ra_adc_d_seed_t;

/**
 * @struct ra_adc_d_state_t
 * @brief Driver-wide placeholder state.
 */
typedef struct {
  bool                  open;
  bool                  scan_done;
  uint8_t               channel_mask;
  ra_adc_d_resolution_t resolution;
  uint16_t              osr;
  bool                  continuous;
  int32_t               samples[k_ra_adc_d_channel_count];
} ra_adc_d_state_t;

static ra_adc_d_state_t   s_adc_d_state;
static ra_adc_d_scan_fn_t s_adc_d_fn;
static void*              s_adc_d_ctx;

ra_err_t ra_adc_d_open(const ra_adc_d_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if (cfg->channel_mask == 0U) {
    return k_ra_err_invalid_arg;
  }
  s_adc_d_state.open         = true;
  s_adc_d_state.scan_done    = false;
  s_adc_d_state.channel_mask = cfg->channel_mask;
  s_adc_d_state.resolution   = cfg->resolution;
  s_adc_d_state.osr          = cfg->osr;
  s_adc_d_state.continuous   = cfg->continuous;
  for (uint8_t i = 0U; i < k_ra_adc_d_channel_count; ++i) {
    s_adc_d_state.samples[i] = 0;
  }
  ra_log_info_val(s_tag, "open mask", (uint32_t)cfg->channel_mask);
  return k_ra_ok;
}

ra_err_t ra_adc_d_close(void)
{
  s_adc_d_state.open      = false;
  s_adc_d_state.scan_done = false;
  return k_ra_ok;
}

ra_err_t ra_adc_d_scan_start(void)
{
  if (!s_adc_d_state.open) {
    return k_ra_err_invalid_state;
  }
  /* Fabricate a deterministic ramp for tests. */
  int32_t seed = k_ra_adc_d_seed_start;
  for (uint8_t i = 0U; i < k_ra_adc_d_channel_count; ++i) {
    if ((s_adc_d_state.channel_mask & (uint8_t)(1U << i)) != 0U) {
      s_adc_d_state.samples[i] = seed;
      seed += k_ra_adc_d_seed_step;
    }
  }
  s_adc_d_state.scan_done = true;
  return k_ra_ok;
}

ra_err_t ra_adc_d_read(uint8_t channel, int32_t* out_sample)
{
  RA_CHECK_NULL_PTR(out_sample, s_tag, "out_sample must not be nullptr");
  if ((uint16_t)channel >= k_ra_adc_d_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_sample = s_adc_d_state.samples[channel];
  return k_ra_ok;
}

ra_err_t ra_adc_d_get_status(uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  uint8_t mask = 0U;
  if (s_adc_d_state.open) {
    mask |= k_ra_adc_d_status_open;
  }
  if (s_adc_d_state.scan_done) {
    mask |= k_ra_adc_d_status_done;
  }
  *out_status = mask;
  return k_ra_ok;
}

ra_err_t ra_adc_d_attach_handler(ra_adc_d_scan_fn_t fn, void* ctx)
{
  s_adc_d_fn  = fn;
  s_adc_d_ctx = ctx;
  return k_ra_ok;
}

void ra_adc_d_dispatch(void)
{
  s_adc_d_state.scan_done      = true;
  const ra_adc_d_scan_fn_t fn  = s_adc_d_fn;
  void* const              ctx = s_adc_d_ctx;
  if (fn != nullptr) {
    fn(ctx);
  }
}
