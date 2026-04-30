/**
 * @file ra_acmplp.c
 * @brief Low-Power Analog Comparator (ACMPLP) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_acmplp`` peripheral
 * driver. RA8D2 instead ships the ACMPHS block. This file keeps a
 * tiny per-channel state table so example projects can call the
 * FSP-shaped open / close / status API and link cleanly.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_acmplp.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ACMPLP";

/**
 * @enum ra_acmplp_limits_t
 * @brief Channel-count and status-bit constants.
 */
typedef enum : uint8_t {
  k_ra_acmplp_channel_count = 2U,
  k_ra_acmplp_status_open   = 0x1U,
  k_ra_acmplp_status_high   = 0x2U,
  k_ra_acmplp_status_speed  = 0x4U,
} ra_acmplp_limits_t;

/**
 * @struct ra_acmplp_state_t
 * @brief Per-channel placeholder state.
 */
typedef struct {
  bool              open;
  bool              output_high;
  ra_acmplp_speed_t speed;
  uint8_t           ivpsel;
  uint8_t           ivrefsel;
  bool              filter_en;
  bool              invert_out;
} ra_acmplp_state_t;

static ra_acmplp_state_t    s_channels[k_ra_acmplp_channel_count];
static ra_acmplp_event_fn_t s_acmplp_fn;
static void*                s_acmplp_ctx;

ra_err_t ra_acmplp_open(const ra_acmplp_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->channel >= k_ra_acmplp_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_acmplp_state_t* st = &s_channels[cfg->channel];
  st->open              = true;
  st->speed             = cfg->speed;
  st->ivpsel            = cfg->ivpsel;
  st->ivrefsel          = cfg->ivrefsel;
  st->filter_en         = cfg->filter_en;
  st->invert_out        = cfg->invert_out;
  st->output_high       = false;
  ra_log_info_val(s_tag, "open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_acmplp_close(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_acmplp_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].open        = false;
  s_channels[channel].output_high = false;
  return k_ra_ok;
}

ra_err_t ra_acmplp_read(uint8_t channel, bool* out_high)
{
  RA_CHECK_NULL_PTR(out_high, s_tag, "out_high must not be nullptr");
  if ((uint16_t)channel >= k_ra_acmplp_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_high = s_channels[channel].output_high;
  return k_ra_ok;
}

ra_err_t ra_acmplp_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= k_ra_acmplp_channel_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_acmplp_state_t* st   = &s_channels[channel];
  uint8_t                  mask = 0U;
  if (st->open) {
    mask |= k_ra_acmplp_status_open;
  }
  if (st->output_high) {
    mask |= k_ra_acmplp_status_high;
  }
  if (st->speed == k_ra_acmplp_speed_high) {
    mask |= k_ra_acmplp_status_speed;
  }
  *out_mask = mask;
  return k_ra_ok;
}

ra_err_t ra_acmplp_attach_handler(ra_acmplp_event_fn_t fn, void* ctx)
{
  s_acmplp_fn  = fn;
  s_acmplp_ctx = ctx;
  return k_ra_ok;
}

void ra_acmplp_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_acmplp_channel_count) {
    return;
  }
  const ra_acmplp_event_fn_t fn   = s_acmplp_fn;
  void* const                ctx  = s_acmplp_ctx;
  s_channels[channel].output_high = !s_channels[channel].output_high;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
