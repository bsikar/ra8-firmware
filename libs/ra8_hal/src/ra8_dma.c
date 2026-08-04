/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dma.c
 * @brief Generic DMA transfer substrate implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * See ``ra8_dma.h`` for the API contract. This file owns the
 * channel-allocation table and the completion dispatch table.
 * Per-channel register writes go through ``ra8_dmac_start`` /
 * ``ra8_dmac_stop``. Callers that need the DTC's vector-table
 * transfer model use ``ra8_dtc_*`` directly.
 */

#include "ra8_dma.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* s_tag = "DMA";

/* =============================================================================
 * State
 * =============================================================================
 */

/**
 * @struct ra8_dma_channel_state_t
 * @brief Per-channel dispatch-table entry.
 */
typedef struct {
  ra8_dma_complete_fn_t on_complete; /**< Callback or NULL.     */
  void*                 ctx;         /**< Stored context.       */
  bool                  in_use;      /**< True while allocated. */
} ra8_dma_channel_state_t;

/**
 * @var s_channels
 * @brief Channel allocation + dispatch table.
 *
 * @details
 * Indexed by DMAC channel number. Updated by
 * ``ra8_dma_request / release`` and read by
 * ``ra8_dma_dispatch_complete``.
 *
 * @note Static so the table is zero-initialized at boot.
 */
static ra8_dma_channel_state_t s_channels[k_ra8_dma_channel_count];

#ifdef RA8_OFF_TARGET
/**
 * @var s_fake_requests
 * @brief Host-only copy of every submitted ``ra8_dma_request_t``.
 *
 * @details
 * The real DMSAR / DMDAR registers are 32-bit and cannot round-
 * trip a 64-bit host pointer. This side table stores the full
 * ``ra8_dma_request_t`` so test helpers like ``ra8_fake_dma_memcpy``
 * can walk the real ``uintptr_t`` addresses instead of relying on
 * the truncated MMIO copy.
 *
 * Dropped by ``#ifdef`` on the target build.
 */
static ra8_dma_request_t s_fake_requests[k_ra8_dma_channel_count];
#endif

/**
 * @var s_initialized
 * @brief ``true`` while the substrate is live (between
 *        ``ra8_dma_init`` and ``ra8_dma_deinit``).
 */
static bool s_initialized = false;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Find the first free channel.
 *
 * @details
 * Linear scan over the static ``s_channels[]`` table looking for the
 * first slot whose ``in_use`` flag is ``false``. Used by
 * ``ra8_dma_request()`` to allocate a channel; tests may also call it
 * to assert table consistency. Loop is bounded by
 * ``k_ra8_dma_channel_count``.
 *
 * @return Channel index on success, ``k_ra8_dma_channel_none`` when
 *         every channel is allocated.
 * @retval 0..k_ra8_dma_channel_count-1 First free channel index.
 * @retval k_ra8_dma_channel_none Every channel slot is currently in use.
 *
 * @pre ``s_channels[]`` has been initialized (``ra8_dma_init()`` called).
 * @pre Caller holds an implicit lock against concurrent allocation.
 * @post No state mutated by the scan itself.
 * @post Returned index, if valid, references a free slot.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
static uint8_t internal_find_free(void)
{
  for (uint8_t ch = 0U; ch < k_ra8_dma_channel_count; ++ch) {
    if (!s_channels[ch].in_use) {
      return ch;
    }
  }
  return k_ra8_dma_channel_none;
}

/**
 * @brief Copy a high-level ``ra8_dma_request_t`` into the lower-level
 *        ``ra8_dmac_config_t`` the DMAC driver expects.
 *
 * @details
 * Translates the user-facing request descriptor (typed addresses,
 * width enum, increment direction) into the register-level config
 * struct consumed by ``ra8_dmac_start()``. No validation -- caller
 * has already invoked ``internal_validate_request()``.
 *
 * @param[in]  req      Source request descriptor (non-NULL).
 * @param[out] out_cfg  Destination DMAC config struct (non-NULL).
 *
 * @pre ``req != NULL`` and ``out_cfg != NULL``.
 * @pre ``internal_validate_request(req) == k_ra8_ok``.
 * @post ``*out_cfg`` populated with values copied from ``*req``.
 * @post No state mutated outside ``*out_cfg``.
 *
 * @note Not thread-safe; caller must own ``out_cfg``.
 * @since 0.1.0
 */
static void internal_pack_dmac_cfg(const ra8_dma_request_t* req, ra8_dmac_config_t* out_cfg)
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
 * @details
 * Bounds-checks the byte count and width enum so a malformed request
 * is rejected before a single DMAC register write. Width must be
 * within the ``k_ra8_dmac_width_*`` range (HUM Ch 16.2.4 "DMTMD : DMA
 * Transfer Mode Register", p 608).
 *
 * @param[in] req Request descriptor (non-NULL).
 *
 * @return ``k_ra8_ok`` on valid, otherwise the specific rejection code.
 * @retval k_ra8_ok Request is well-formed.
 * @retval k_ra8_err_invalid_arg ``count == 0`` or ``width`` out of range.
 *
 * @pre ``req != NULL``.
 * @pre Caller has not yet armed the DMAC channel.
 * @post No state mutated.
 * @post On success, all fields of ``*req`` are within driver range.
 *
 * @note Pure function over the input pointer; thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_request(const ra8_dma_request_t* req)
{
  if (req->count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)req->width > k_ra8_dmac_width_word) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Bring up the generic DMA substrate.
 *
 * @details
 * Enables MSTPA22 (shared with DTC0, HUM Ch 11.2.6 "MSTPCRA : Module
 * Stop Control Register A", p 443) and clears every entry in the
 * channel-allocation / dispatch table. After this returns
 * ``ra8_dma_request()`` may allocate channels.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Channel table reset and MSTP gate opened.
 * @retval k_ra8_err_hw_init_failed Underlying ``ra8_mstp_enable()`` failed.
 *
 * @pre ``ra8_mstp_init()`` previously succeeded.
 * @pre Single-threaded init context (or IRQs masked).
 * @post All channels report ``in_use == false``.
 * @post DMAC0 + DTC0 MSTP reference held by this driver.
 *
 * @note Not thread-safe; call once during system init.
 * @since 0.1.0
 */
ra8_err_t ra8_dma_init(void)
{
  ra8_log_info(s_tag, "ra8_dma_init");

  /* HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443
   *  -- DMAC0 + DTC0 share MSTPA22. */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0);
  if (mst_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "mstp enable failed", (uint32_t)mst_err);
    return k_ra8_err_hw_init_failed;
  }

  for (uint8_t ch = 0U; ch < k_ra8_dma_channel_count; ++ch) {
    s_channels[ch].on_complete = nullptr;
    s_channels[ch].ctx         = nullptr;
    s_channels[ch].in_use      = false;
  }
  s_initialized = true;
  return k_ra8_ok;
}

/**
 * @brief Tear down the generic DMA substrate.
 *
 * @details
 * Stops every in-use channel via ``ra8_dmac_stop()``, clears the
 * dispatch table, and releases the MSTPA22 reference (HUM Ch 11.2.6,
 * p 443). After this returns ``ra8_dma_request()`` rejects with
 * ``k_ra8_err_not_initialized``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All channels stopped and MSTP released.
 * @retval k_ra8_err_hw_error Underlying ``ra8_mstp_disable()`` failed.
 *
 * @pre ``ra8_dma_init()`` previously succeeded.
 * @pre No outstanding interrupt expected from any channel.
 * @post All channels report ``in_use == false``.
 * @post Driver re-enters uninitialized state.
 *
 * @note Not thread-safe; call once during system shutdown.
 * @since 0.1.0
 */
ra8_err_t ra8_dma_deinit(void)
{
  /* Tear down every in-use channel before releasing the MSTP
   * reference so no pending transfer can wedge the DMAC block. */
  for (uint8_t ch = 0U; ch < k_ra8_dma_channel_count; ++ch) {
    if (s_channels[ch].in_use) {
      /* Ignore dmac_stop errors -- the goal is to get every
       * channel into a safe state before releasing MSTP. */
      (void)ra8_dmac_stop(ch);
      s_channels[ch].on_complete = nullptr;
      s_channels[ch].ctx         = nullptr;
      s_channels[ch].in_use      = false;
    }
  }

  const ra8_err_t mst_err = ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0);
  if (mst_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "mstp disable failed", (uint32_t)mst_err);
    return k_ra8_err_hw_error;
  }
  s_initialized = false;
  return k_ra8_ok;
}

/**
 * @brief Allocate a channel and arm a DMAC transfer.
 *
 * @details
 * Validates ``*req``, finds a free channel via ``internal_find_free()``,
 * packs a ``ra8_dmac_config_t`` and calls ``ra8_dmac_start()``. On success
 * the channel index is written through ``out_channel`` and the
 * caller-supplied completion callback is stashed for later dispatch.
 *
 * @param[in]  req         Transfer descriptor (non-NULL).
 * @param[out] out_channel Allocated channel index (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Channel allocated and DMAC armed.
 * @retval k_ra8_err_invalid_arg ``req`` malformed.
 * @retval k_ra8_err_not_initialized ``ra8_dma_init()`` was not called.
 * @retval k_ra8_err_no_mem No free channel.
 * @retval k_ra8_err_hw_error ``ra8_dmac_start()`` failed.
 *
 * @pre ``ra8_dma_init()`` previously succeeded.
 * @pre ``req != NULL`` and ``out_channel != NULL``.
 * @post On success ``*out_channel`` references an in-use channel.
 * @post On failure no channel is allocated and no DMAC register changed.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
ra8_err_t ra8_dma_request(const ra8_dma_request_t* req, uint8_t* out_channel)
{
  RA8_CHECK_NULL_PTR(req, s_tag, "request must not be NULL");
  RA8_CHECK_NULL_PTR(out_channel, s_tag, "out_channel must not be NULL");
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }

  const ra8_err_t verr = internal_validate_request(req);
  if (verr != k_ra8_ok) {
    return verr;
  }

  const uint8_t ch = internal_find_free();
  if (ch == k_ra8_dma_channel_none) {
    ra8_log_error(s_tag, "no free channel");
    return k_ra8_err_no_mem;
  }

  ra8_dmac_config_t cfg = {};
  internal_pack_dmac_cfg(req, &cfg);

  const ra8_err_t derr = ra8_dmac_start(ch, &cfg);
  if (derr != k_ra8_ok) {
    ra8_log_error_val(s_tag, "dmac_start failed", (uint32_t)derr);
    return k_ra8_err_hw_error;
  }

  s_channels[ch].on_complete = req->on_complete;
  s_channels[ch].ctx         = req->ctx;
  s_channels[ch].in_use      = true;
#ifdef RA8_OFF_TARGET
  s_fake_requests[ch] = *req;
#endif
  *out_channel = ch;
  return k_ra8_ok;
}

/**
 * @brief Release a previously allocated channel.
 *
 * @details
 * Calls ``ra8_dmac_stop()`` to disable the channel at the hardware
 * level, then clears the dispatch entry. The MSTP reference held by
 * the substrate root is preserved -- only ``ra8_dma_deinit()`` releases it.
 *
 * @param[in] channel Channel previously returned by ``ra8_dma_request()``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Channel released.
 * @retval k_ra8_err_invalid_arg ``channel >= k_ra8_dma_channel_count``.
 * @retval k_ra8_err_invalid_state Channel was not in use.
 * @retval k_ra8_err_hw_error ``ra8_dmac_stop()`` failed.
 *
 * @pre ``ra8_dma_init()`` previously succeeded.
 * @pre Caller has accepted any pending IRQs may still latch.
 * @post Channel slot reports ``in_use == false``.
 * @post DMAC channel is disabled at the register level.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
ra8_err_t ra8_dma_release(uint8_t channel)
{
  if (channel >= k_ra8_dma_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_channels[channel].in_use) {
    return k_ra8_err_invalid_state;
  }

  /* ra8_dmac_stop itself drops one MSTP reference -- the bit
   * stays cleared because ra8_dma_init holds the root reference. */
  const ra8_err_t stop_err = ra8_dmac_stop(channel);
  if (stop_err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "dmac_stop failed", (uint32_t)stop_err);
    return k_ra8_err_hw_error;
  }

  s_channels[channel].on_complete = nullptr;
  s_channels[channel].ctx         = nullptr;
  s_channels[channel].in_use      = false;
  return k_ra8_ok;
}

#ifdef RA8_OFF_TARGET
/**
 * @brief Host-only peek at the cached request for a channel.
 *
 * @details
 * Returns a pointer to the side-table copy of the original
 * ``ra8_dma_request_t`` so fake code can inspect the full 64-bit
 * host buffer pointers (the DMAC mirrors only hold 32-bit truncations).
 *
 * @param[in] channel Channel previously returned by ``ra8_dma_request()``.
 *
 * @return Pointer to the cached request, or ``nullptr`` on miss.
 * @retval non-NULL Pointer to the cached descriptor.
 * @retval nullptr  ``channel`` out of range or slot not in use.
 *
 * @pre Built with ``RA8_OFF_TARGET`` defined.
 * @pre ``ra8_dma_request()`` previously returned ``k_ra8_ok`` for ``channel``.
 * @post No firmware state mutated.
 * @post Returned pointer aliases the static side table.
 *
 * @note Test-only; not present on the target build.
 * @since 0.1.0
 */
const ra8_dma_request_t* ra8_dma_fake_peek_request(uint8_t channel)
{
  if (channel >= k_ra8_dma_channel_count) {
    return nullptr;
  }
  if (!s_channels[channel].in_use) {
    return nullptr;
  }
  return &s_fake_requests[channel];
}
#endif

ra8_err_t ra8_dma_channel_is_busy(uint8_t channel, bool* out_busy)
{
  RA8_CHECK_NULL_PTR(out_busy, s_tag, "out_busy must not be NULL");
  if (channel >= k_ra8_dma_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_busy = s_channels[channel].in_use;
  return k_ra8_ok;
}

/**
 * @brief Fire the per-channel completion callback.
 *
 * @details
 * Looks up the stashed callback / context for ``channel`` and invokes
 * the callback if non-NULL. Called from the DMAC IRQ trampoline (or
 * from a unit-test driver in off-target mode). Out-of-range channels
 * are silently ignored.
 *
 * @param[in] channel Channel whose completion just fired.
 *
 * @pre ``ra8_dma_init()`` previously succeeded.
 * @pre Called from ISR context or a test driver.
 * @post Registered callback invoked at most once with the stored context.
 * @post No state mutated by the dispatch itself.
 *
 * @note Thread safety: ISR context only; not re-entrant per channel.
 * @since 0.1.0
 */
RA8_ISR_SAFE
void ra8_dma_dispatch_complete(uint8_t channel)
{
  if (channel >= k_ra8_dma_channel_count) {
    return;
  }
  const ra8_dma_complete_fn_t cb  = s_channels[channel].on_complete;
  void* const                 ctx = s_channels[channel].ctx;
  if (cb != nullptr) {
    cb(ctx);
  }
}
