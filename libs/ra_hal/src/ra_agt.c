/**
 * @file ra_agt.c
 * @brief Low Power Asynchronous General Purpose Timer driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 AGT block (10 channels total; only
 * AGT0 / AGT1 have dedicated MSTPD bits, the rest share the
 * sub-clock path). Programmes an AGT channel as a free-running
 * 16-bit down-counter clocked from PCLKB. Used as a coarse tick
 * source on boards where SysTick is not desirable. Every register
 * access carries a HUM Ch 24 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_agt.h"

#include <stdint.h>

#include "ra8d2_agt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "AGT";

/**
 * @enum ra_agt_mstp_limit_t
 * @brief Number of AGT channels that have dedicated MSTPD bits.
 *
 * @details
 * HUM Ch 11.2.9 p 448 only lists MSTPD4 / MSTPD5 for AGT1 / AGT0.
 * The chip lays out 10 AGT channels total but only the first two
 * are ref-counted through ra_mstp; additional channels inherit
 * their clock from the shared LOCO / sub-clock path.
 */
typedef enum : uint8_t {
  k_ra_agt_mstp_id_count = 2U,
} ra_agt_mstp_limit_t;

/**
 * @var s_agt_mstp_table
 * @brief Channel-index -> MSTP id lookup for AGT0 / AGT1.
 */
static const ra_mstp_t s_agt_mstp_table[k_ra_agt_mstp_id_count] = {
  k_ra_mstp_agt0,
  k_ra_mstp_agt1,
};

[[nodiscard]] ra_err_t ra_agt_start_free_run(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  if (channel < k_ra_agt_mstp_id_count) {
    /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
    const ra_err_t mst_err = ra_mstp_enable(s_agt_mstp_table[channel]);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "agt_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */
  }

  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  /* HUM Ch 24.2.2 "AGTMR1 : AGT Mode Register 1" p 1169 -- timer mode,
   * PCLKB source. */
  reg->AGTMR1 = 0U;
  /* HUM Ch 24.2.3 "AGTMR2 : AGT Mode Register 2" p 1170 */
  reg->AGTMR2 = 0U;
  /* HUM Ch 24.2.4 "AGT : AGT Counter" p 1170 */
  reg->AGT = reload;
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  /* TSTART = 1. */
  reg->AGTCR = 0x01U;

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_agt_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

static ra_agt_event_fn_t s_agt_fn;
static void*             s_agt_ctx;

/**
 * @brief Tear down one AGT channel.
 *
 * @details
 * Clears AGTCR (HUM Ch 24.2.1 "AGTCR : AGT Control Register",
 * p 1167) so the counter stops, then drops the matching MSTP
 * reference for AGT0 / AGT1. Channels 2..9 share the LOCO /
 * sub-clock path, so no MSTP work is required for them.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Channel stopped, MSTP released.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 10 (no register window).
 * @retval k_ra_err_invalid_state  ``ra_mstp_disable`` underflow.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_agt_start_free_run`` (or sibling start) was previously
 *      called for ``channel``.
 *
 * @post AGTCR for the channel reads as 0 (counter stopped).
 * @post MSTP reference for AGT0/AGT1 has been decremented when the
 *      channel index falls inside the dedicated MSTP window.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_agt_deinit(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  if (channel < k_ra_agt_mstp_id_count) {
    return ra_mstp_disable(s_agt_mstp_table[channel]);
  }
  return k_ra_ok;
}

/**
 * @brief Change the AGT reload value at runtime.
 *
 * @details
 * Writes ``reload`` into the AGT counter register (HUM Ch 24.2.4
 * "AGT : AGT Counter", p 1170). Valid only on a stopped channel
 * or while the counter has just been refreshed.
 *
 * @param[in] channel AGT channel index (0..9).
 * @param[in] reload  16-bit reload value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Counter rewritten.
 * @retval k_ra_err_invalid_arg ``channel`` >= 10.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_agt_start_free_run`` was previously called for ``channel``
 *      or the channel has been initialized by a sibling driver.
 *
 * @post Counter holds ``reload``.
 * @post Channel state (running / stopped) is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_agt_set_reload(uint8_t channel, uint16_t reload)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.4 "AGT : AGT Counter" p 1170 */
  reg->AGT = reload;
  return k_ra_ok;
}

/**
 * @brief Read the AGTCR status register.
 *
 * @details
 * Returns the raw 8-bit AGTCR value (HUM Ch 24.2.1 "AGTCR : AGT
 * Control Register", p 1167) so callers can inspect the TSTART /
 * TCSTF / TUNDF / TEDGF flags without leaking the register layout.
 *
 * @param[in]  channel  AGT channel index (0..9).
 * @param[out] out_mask Receives the AGTCR snapshot.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Status read into ``*out_mask``.
 * @retval k_ra_err_null_ptr    ``out_mask`` was NULL.
 * @retval k_ra_err_invalid_arg ``channel`` >= 10.
 *
 * @pre ``out_mask`` non-NULL.
 * @pre Caller has previously powered or started the channel.
 *
 * @post ``*out_mask`` reflects live AGTCR contents.
 * @post No AGT register state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_agt_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  *out_mask = reg->AGTCR;
  return k_ra_ok;
}

/**
 * @brief Attach an AGT event callback (shared across channels).
 *
 * @details
 * Stores ``fn`` / ``ctx`` in module-static storage. The same handler
 * fires for every AGT channel that is dispatched through
 * ``ra_agt_dispatch``; the dispatcher passes the channel index back
 * to the handler so multi-channel callers can route on it.
 *
 * @param[in] fn  Callback (NULL clears the slot).
 * @param[in] ctx Opaque pointer passed back to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Slot updated.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller is happy to share the single global slot with other
 *      AGT users.
 *
 * @post Subsequent ``ra_agt_dispatch`` calls invoke ``fn(ctx, ch)``.
 * @post Previous callback (if any) is overwritten.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_agt_attach_handler(ra_agt_event_fn_t fn, void* ctx)
{
  s_agt_fn  = fn;
  s_agt_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Dispatch an AGT event to the registered shared callback.
 *
 * @details
 * Called from the AGT compare-match / underflow ISR (HUM Ch 24
 * "Asynchronous General Purpose Timer", p 1167). Silently no-ops
 * when ``channel`` is out of range or no handler is attached.
 *
 * @param[in] channel AGT channel (0..9) whose ISR fired.
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``channel`` < 10.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post No AGT register state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra_agt_dispatch(uint8_t channel)
{
  if (ra_agt(channel) == nullptr) {
    return;
  }
  const ra_agt_event_fn_t fn  = s_agt_fn;
  void* const             ctx = s_agt_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}

/**
 * @brief Put one AGT channel into MSTP-gated stop.
 *
 * @details
 * Clears AGTCR (HUM Ch 24.2.1 "AGTCR : AGT Control Register",
 * p 1167) so the counter halts, then for AGT0 / AGT1 drops the
 * dedicated MSTPD bit (HUM Ch 11.2.9 "MSTPCRD", p 448) so the
 * peripheral clock can gate.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Channel halted, MSTP gated.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 10.
 * @retval k_ra_err_invalid_state  ``ra_mstp_disable`` underflow on the
 *                                 shared MSTP id.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Channel was previously started via ``ra_agt_start_free_run``.
 *
 * @post AGTCR for the channel reads as 0.
 * @post For AGT0/AGT1 the MSTPCRD bit has been ref-decremented.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_agt_enter_stop(uint8_t channel)
{
  volatile r_agt_regs_t* reg = ra_agt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  /* HUM Ch 24.2.1 "AGTCR : AGT Control Register" p 1167 */
  reg->AGTCR = 0U;
  if (channel < k_ra_agt_mstp_id_count) {
    return ra_mstp_disable(s_agt_mstp_table[channel]);
  }
  return k_ra_ok;
}

/**
 * @brief Exit MSTP-gated stop for one AGT channel.
 *
 * @details
 * Re-asserts the MSTP reference for AGT0 / AGT1 (HUM Ch 11.2.9
 * "MSTPCRD", p 448) so the channel sees its peripheral clock again.
 * Channels 2..9 share the LOCO / sub-clock path and become a no-op
 * here. The caller is expected to reissue a start request next.
 *
 * @param[in] channel AGT channel index (0..9).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              MSTP re-enabled (or no-op for shared).
 * @retval k_ra_err_invalid_arg ``channel`` >= 10.
 * @retval k_ra_err_hw_timeout  ``ra_mstp_enable`` read-back failure.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Channel was previously placed in stop via ``ra_agt_enter_stop``.
 *
 * @post For AGT0/AGT1 the matching MSTPCRD bit has been ref-incremented.
 * @post Channel is ready to be restarted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_agt_exit_stop(uint8_t channel)
{
  if (ra_agt(channel) == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (channel < k_ra_agt_mstp_id_count) {
    return ra_mstp_enable(s_agt_mstp_table[channel]);
  }
  return k_ra_ok;
}
