/**
 * @file ra_acmphs_b.c
 * @brief Analog Comparator High-Speed B (ACMPHS-B) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_acmphs_b`` peripheral
 * driver. The RA8D2 ships the original ACMPHS block (see
 * ``ra_acmphs.c``); this file exposes the FSP-shaped public API for
 * cross-compatibility with example projects that target other RA
 * SKUs.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. No
 *          register writes are issued -- internal state is kept in
 *          a small static table so the open / close / status path
 *          exercises symmetrically with siblings.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_acmphs_b.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ACMPHS_B";

/**
 * @enum ra_acmphs_b_limits_t
 * @brief Channel-count and status-bit constants.
 */
typedef enum : uint8_t {
  k_ra_acmphs_b_channel_count = 4U, /**< Logical channels modelled.        */
  k_ra_acmphs_b_status_open   = 0x1U,
  k_ra_acmphs_b_status_high   = 0x2U,
  k_ra_acmphs_b_edge_shift    = 2U,
  k_ra_acmphs_b_edge_mask     = 0x0CU,
} ra_acmphs_b_limits_t;

/**
 * @struct ra_acmphs_b_state_t
 * @brief Per-channel placeholder state.
 */
typedef struct {
  bool               open;
  bool               output_high;
  ra_acmphs_b_edge_t edge;
  uint8_t            ivpsel;
  uint8_t            ivrefsel;
  bool               filter_en;
  bool               invert_out;
} ra_acmphs_b_state_t;

static ra_acmphs_b_state_t    s_channels[k_ra_acmphs_b_channel_count];
static ra_acmphs_b_event_fn_t s_acmphs_b_fn;
static void*                  s_acmphs_b_ctx;

ra_err_t ra_acmphs_b_open(const ra_acmphs_b_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->channel >= k_ra_acmphs_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_acmphs_b_state_t* st = &s_channels[cfg->channel];
  st->open                = true;
  st->edge                = cfg->edge;
  st->ivpsel              = cfg->ivpsel;
  st->ivrefsel            = cfg->ivrefsel;
  st->filter_en           = cfg->filter_en;
  st->invert_out          = cfg->invert_out;
  st->output_high         = false;
  ra_log_info_val(s_tag, "open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_acmphs_b_close(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_acmphs_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_acmphs_b_state_t* st = &s_channels[channel];
  st->open                = false;
  st->edge                = k_ra_acmphs_b_edge_none;
  st->output_high         = false;
  return k_ra_ok;
}

ra_err_t ra_acmphs_b_read(uint8_t channel, bool* out_high)
{
  RA_CHECK_NULL_PTR(out_high, s_tag, "out_high must not be nullptr");
  if ((uint16_t)channel >= k_ra_acmphs_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_high = s_channels[channel].output_high;
  return k_ra_ok;
}

ra_err_t ra_acmphs_b_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= k_ra_acmphs_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_acmphs_b_state_t* st   = &s_channels[channel];
  uint8_t                    mask = 0U;
  if (st->open) {
    mask |= k_ra_acmphs_b_status_open;
  }
  if (st->output_high) {
    mask |= k_ra_acmphs_b_status_high;
  }
  mask |= (uint8_t)(((uint8_t)st->edge << k_ra_acmphs_b_edge_shift) & k_ra_acmphs_b_edge_mask);
  *out_mask = mask;
  return k_ra_ok;
}

ra_err_t ra_acmphs_b_attach_handler(ra_acmphs_b_event_fn_t fn, void* ctx)
{
  s_acmphs_b_fn  = fn;
  s_acmphs_b_ctx = ctx;
  return k_ra_ok;
}

void ra_acmphs_b_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_acmphs_b_channel_count) {
    return;
  }
  const ra_acmphs_b_event_fn_t fn  = s_acmphs_b_fn;
  void* const                  ctx = s_acmphs_b_ctx;
  s_channels[channel].output_high  = !s_channels[channel].output_high;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
