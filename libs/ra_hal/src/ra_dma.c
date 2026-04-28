/**
 * @file ra_dma.c
 * @brief Generic DMA transfer substrate implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * See ``ra_dma.h`` for the API contract. This file owns the
 * channel-allocation table and the completion dispatch table.
 * Per-channel register writes go through ``ra_dmac_start`` /
 * ``ra_dmac_stop``. Callers that need the DTC's vector-table
 * transfer model use ``ra_dtc_*`` directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dma.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "DMA";

/* =============================================================================
 * State
 * =============================================================================
 */

/**
 * @struct ra_dma_channel_state_t
 * @brief Per-channel dispatch-table entry.
 */
typedef struct {
  ra_dma_complete_fn_t on_complete; /**< Callback or NULL.          */
  void*                ctx;         /**< Stored context.            */
  bool                 in_use;      /**< True while allocated.      */
} ra_dma_channel_state_t;

/**
 * @var s_channels
 * @brief Channel allocation + dispatch table.
 *
 * @details
 * Indexed by DMAC channel number. Updated by
 * ``ra_dma_request / release`` and read by
 * ``ra_dma_dispatch_complete``.
 *
 * @note Static so the table is zero-initialised at boot.
 */
static ra_dma_channel_state_t s_channels[k_ra_dma_channel_count];

#ifdef RA_SIMULATOR_MODE
/**
 * @var s_sim_requests
 * @brief Host-only copy of every submitted ``ra_dma_request_t``.
 *
 * @details
 * The real DMSAR / DMDAR registers are 32-bit and cannot round-
 * trip a 64-bit host pointer. This side table stores the full
 * ``ra_dma_request_t`` so test helpers like ``ra_sim_dma_memcpy``
 * can walk the real ``uintptr_t`` addresses instead of relying on
 * the truncated MMIO copy.
 *
 * Dropped by ``#ifdef`` on the target build.
 */
static ra_dma_request_t s_sim_requests[k_ra_dma_channel_count];
#endif

/**
 * @var s_initialized
 * @brief ``true`` while the substrate is live (between
 *        ``ra_dma_init`` and ``ra_dma_deinit``).
 */
static bool s_initialized = false;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Find the first free channel.
 *
 * @return Channel index on success, ``k_ra_dma_channel_none`` when
 *         every channel is allocated.
 */
static uint8_t internal_find_free(void)
{
  for (uint8_t ch = 0U; ch < k_ra_dma_channel_count; ++ch) {
    if (!s_channels[ch].in_use) {
      return ch;
    }
  }
  return k_ra_dma_channel_none;
}

/**
 * @brief Copy a high-level ``ra_dma_request_t`` into the lower-level
 *        ``ra_dmac_config_t`` the DMAC driver expects.
 */
static void internal_pack_dmac_cfg(const ra_dma_request_t* req, ra_dmac_config_t* out_cfg)
{
  out_cfg->src     = (uint32_t)req->src_addr;
  out_cfg->dst     = (uint32_t)req->dst_addr;
  out_cfg->count   = req->count;
  out_cfg->width   = req->width;
  out_cfg->src_inc = req->src_inc;
  out_cfg->dst_inc = req->dst_inc;
}

/**
 * @brief Validate a request descriptor before touching hardware.
 *
 * @return ``k_ra_ok`` on valid, otherwise the specific rejection code.
 */
static ra_err_t internal_validate_request(const ra_dma_request_t* req)
{
  if (req->count == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)req->width > k_ra_dmac_width_word) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_dma_init(void)
{
  ra_log_info(s_tag, "ra_dma_init");

  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443
   *  -- DMAC0 + DTC0 share MSTPA22. */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dmac0_dtc0);
  if (mst_err != k_ra_ok) {
    ra_log_error_val(s_tag, "mstp enable failed", (uint32_t)mst_err);
    return k_ra_err_hw_init_failed;
  }

  for (uint8_t ch = 0U; ch < k_ra_dma_channel_count; ++ch) {
    s_channels[ch].on_complete = nullptr;
    s_channels[ch].ctx         = nullptr;
    s_channels[ch].in_use      = false;
  }
  s_initialized = true;
  return k_ra_ok;
}

ra_err_t ra_dma_deinit(void)
{
  /* Tear down every in-use channel before releasing the MSTP
   * reference so no pending transfer can wedge the DMAC block. */
  for (uint8_t ch = 0U; ch < k_ra_dma_channel_count; ++ch) {
    if (s_channels[ch].in_use) {
      /* Ignore dmac_stop errors -- the goal is to get every
       * channel into a safe state before releasing MSTP. */
      (void)ra_dmac_stop(ch);
      s_channels[ch].on_complete = nullptr;
      s_channels[ch].ctx         = nullptr;
      s_channels[ch].in_use      = false;
    }
  }

  const ra_err_t mst_err = ra_mstp_disable(k_ra_mstp_dmac0_dtc0);
  if (mst_err != k_ra_ok) {
    ra_log_error_val(s_tag, "mstp disable failed", (uint32_t)mst_err);
    return k_ra_err_hw_error;
  }
  s_initialized = false;
  return k_ra_ok;
}

ra_err_t ra_dma_request(const ra_dma_request_t* req, uint8_t* out_channel)
{
  RA_CHECK_NULL_PTR(req, s_tag, "request must not be NULL");
  RA_CHECK_NULL_PTR(out_channel, s_tag, "out_channel must not be NULL");
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }

  const ra_err_t verr = internal_validate_request(req);
  if (verr != k_ra_ok) {
    return verr;
  }

  const uint8_t ch = internal_find_free();
  if (ch == k_ra_dma_channel_none) {
    ra_log_error(s_tag, "no free channel");
    return k_ra_err_no_mem;
  }

  ra_dmac_config_t cfg = {};
  internal_pack_dmac_cfg(req, &cfg);

  const ra_err_t derr = ra_dmac_start(ch, &cfg);
  if (derr != k_ra_ok) {
    ra_log_error_val(s_tag, "dmac_start failed", (uint32_t)derr);
    return k_ra_err_hw_error;
  }

  s_channels[ch].on_complete = req->on_complete;
  s_channels[ch].ctx         = req->ctx;
  s_channels[ch].in_use      = true;
#ifdef RA_SIMULATOR_MODE
  s_sim_requests[ch] = *req;
#endif
  *out_channel = ch;
  return k_ra_ok;
}

ra_err_t ra_dma_release(uint8_t channel)
{
  if (channel >= k_ra_dma_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_channels[channel].in_use) {
    return k_ra_err_invalid_state;
  }

  /* ra_dmac_stop itself drops one MSTP reference -- the bit
   * stays cleared because ra_dma_init holds the root reference. */
  const ra_err_t stop_err = ra_dmac_stop(channel);
  if (stop_err != k_ra_ok) {
    ra_log_error_val(s_tag, "dmac_stop failed", (uint32_t)stop_err);
    return k_ra_err_hw_error;
  }

  s_channels[channel].on_complete = nullptr;
  s_channels[channel].ctx         = nullptr;
  s_channels[channel].in_use      = false;
  return k_ra_ok;
}

#ifdef RA_SIMULATOR_MODE
const ra_dma_request_t* ra_dma_sim_peek_request(uint8_t channel)
{
  if (channel >= k_ra_dma_channel_count) {
    return nullptr;
  }
  if (!s_channels[channel].in_use) {
    return nullptr;
  }
  return &s_sim_requests[channel];
}
#endif

ra_err_t ra_dma_channel_is_busy(uint8_t channel, bool* out_busy)
{
  RA_CHECK_NULL_PTR(out_busy, s_tag, "out_busy must not be NULL");
  if (channel >= k_ra_dma_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_busy = s_channels[channel].in_use;
  return k_ra_ok;
}

void ra_dma_dispatch_complete(uint8_t channel)
{
  if (channel >= k_ra_dma_channel_count) {
    return;
  }
  const ra_dma_complete_fn_t cb  = s_channels[channel].on_complete;
  void* const                ctx = s_channels[channel].ctx;
  if (cb != nullptr) {
    cb(ctx);
  }
}
