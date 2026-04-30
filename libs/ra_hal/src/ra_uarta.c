/**
 * @file ra_uarta.c
 * @brief Legacy UART-A (UARTA) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_uarta`` peripheral
 * driver. RA8D2 uses the SCI block instead of UARTA; this file
 * exposes the FSP-shaped surface so example projects can build
 * cleanly. Bytes written are buffered in a small ring and looped
 * back on read so loopback-style examples behave deterministically.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_uarta.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "UARTA";

/**
 * @enum ra_uarta_limits_t
 * @brief Channel-count, ring-size, and status-bit constants.
 */
typedef enum : uint16_t {
  k_ra_uarta_channel_count = 2U,
  k_ra_uarta_ring_size     = 64U,
  k_ra_uarta_status_open   = 0x1U,
  k_ra_uarta_status_data   = 0x2U,
} ra_uarta_limits_t;

/**
 * @struct ra_uarta_state_t
 * @brief Per-channel placeholder state.
 */
typedef struct {
  bool                    open;
  uint32_t                baud_rate;
  uint8_t                 data_bits;
  uint8_t                 stop_bits;
  bool                    parity_en;
  bool                    parity_odd;
  ra_uarta_clock_source_t clock_source;
  uint8_t                 ring[k_ra_uarta_ring_size];
  uint16_t                ring_count;
  uint16_t                ring_head;
  uint16_t                ring_tail;
} ra_uarta_state_t;

static ra_uarta_state_t    s_channels[k_ra_uarta_channel_count];
static ra_uarta_event_fn_t s_uarta_fn;
static void*               s_uarta_ctx;

ra_err_t ra_uarta_open(const ra_uarta_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->channel >= k_ra_uarta_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->baud_rate == 0U) {
    return k_ra_err_invalid_arg;
  }
  ra_uarta_state_t* st = &s_channels[cfg->channel];
  st->open             = true;
  st->baud_rate        = cfg->baud_rate;
  st->data_bits        = cfg->data_bits;
  st->stop_bits        = cfg->stop_bits;
  st->parity_en        = cfg->parity_en;
  st->parity_odd       = cfg->parity_odd;
  st->clock_source     = cfg->clock_source;
  st->ring_count       = 0U;
  st->ring_head        = 0U;
  st->ring_tail        = 0U;
  ra_log_info_val(s_tag, "open ch", (uint32_t)cfg->channel);
  return k_ra_ok;
}

ra_err_t ra_uarta_close(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_uarta_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].open       = false;
  s_channels[channel].ring_count = 0U;
  s_channels[channel].ring_head  = 0U;
  s_channels[channel].ring_tail  = 0U;
  return k_ra_ok;
}

ra_err_t ra_uarta_write(uint8_t channel, const uint8_t* data, uint16_t len)
{
  RA_CHECK_NULL_PTR((void*)data, s_tag, "data must not be nullptr");
  if ((uint16_t)channel >= k_ra_uarta_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_channels[channel].open) {
    return k_ra_err_invalid_state;
  }
  ra_uarta_state_t* st = &s_channels[channel];
  for (uint16_t i = 0U; i < len; ++i) {
    if (st->ring_count >= k_ra_uarta_ring_size) {
      return k_ra_err_no_mem;
    }
    st->ring[st->ring_head] = data[i];
    st->ring_head           = (uint16_t)((st->ring_head + 1U) % k_ra_uarta_ring_size);
    ++st->ring_count;
  }
  return k_ra_ok;
}

ra_err_t ra_uarta_read(uint8_t channel, uint8_t* data, uint16_t len, uint16_t* out_read)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA_CHECK_NULL_PTR(out_read, s_tag, "out_read must not be nullptr");
  if ((uint16_t)channel >= k_ra_uarta_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_uarta_state_t* st       = &s_channels[channel];
  uint16_t          consumed = 0U;
  while ((consumed < len) && (st->ring_count > 0U)) {
    data[consumed] = st->ring[st->ring_tail];
    st->ring_tail  = (uint16_t)((st->ring_tail + 1U) % k_ra_uarta_ring_size);
    --st->ring_count;
    ++consumed;
  }
  *out_read = consumed;
  return k_ra_ok;
}

ra_err_t ra_uarta_set_baud(uint8_t channel, uint32_t baud_rate)
{
  if ((uint16_t)channel >= k_ra_uarta_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (baud_rate == 0U) {
    return k_ra_err_invalid_arg;
  }
  s_channels[channel].baud_rate = baud_rate;
  return k_ra_ok;
}

ra_err_t ra_uarta_get_status(uint8_t channel, uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  if ((uint16_t)channel >= k_ra_uarta_channel_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_uarta_state_t* st   = &s_channels[channel];
  uint8_t                 mask = 0U;
  if (st->open) {
    mask |= (uint8_t)k_ra_uarta_status_open;
  }
  if (st->ring_count > 0U) {
    mask |= (uint8_t)k_ra_uarta_status_data;
  }
  *out_status = mask;
  return k_ra_ok;
}

ra_err_t ra_uarta_attach_handler(ra_uarta_event_fn_t fn, void* ctx)
{
  s_uarta_fn  = fn;
  s_uarta_ctx = ctx;
  return k_ra_ok;
}

void ra_uarta_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_uarta_channel_count) {
    return;
  }
  const ra_uarta_event_fn_t fn  = s_uarta_fn;
  void* const               ctx = s_uarta_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
