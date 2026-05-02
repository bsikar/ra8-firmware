/**
 * @file ra_ulpt.c
 * @brief Ultra-Low-Power Timer driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 ULPT block (two channels, ULPT0
 * and ULPT1). The ULPT clocks from the sub-clock so it keeps
 * counting in software-standby mode, which makes it the right
 * source for low-power wake-up. This driver covers init, start,
 * stop, deinit, runtime period change, status read, IRQ
 * dispatch, and power transition. Every register access carries
 * a HUM Ch 25 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ulpt.h"

#include <stdint.h>

#include "ra8d2_ulpt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ULPT";

/**
 * @var s_ulpt_mstp_table
 * @brief Channel-index -> MSTP id lookup. ULPT0/1 share MSTPCRE
 * bits E9/E8 per HUM Ch 11.2.10 p 449.
 */
static const ra_mstp_t s_ulpt_mstp_table[] = {
  k_ra_mstp_ulpt0,
  k_ra_mstp_ulpt1,
};

[[nodiscard]] ra_err_t ra_ulpt_init(void)
{
  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_ulpt_channel_count; ++ch) {
    volatile r_ulpt_regs_t* reg = ra_ulpt(ch);
    if (reg == nullptr) {
      return k_ra_err_hw_init_failed;
    }
    /* HUM Ch 11.2.10 "MSTPCRE : Module Stop Control Register E" p 449 */
    const ra_err_t mst_err = ra_mstp_enable(s_ulpt_mstp_table[ch]);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "ulpt_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 */
    reg->ULPTCR = 0U;
    /* HUM Ch 25.2.2 "ULPTMR1 : ULPT Mode Register 1" p 1192 */
    reg->ULPTMR1 = 0U;
    /* HUM Ch 25.2.3 "ULPTMR2 : ULPT Mode Register 2" p 1194 */
    reg->ULPTMR2 = 0U;
    /* HUM Ch 25.2.4 "ULPTMR3 : ULPT Mode Register 3" p 1195 */
    reg->ULPTMR3 = 0U;
    /* HUM Ch 25.2.5 "ULPTIOC : ULPT I/O Control Register" p 1196 */
    reg->ULPTIOC = 0U;
    /* HUM Ch 25.2.6 "ULPT : ULPT Counter Register" p 1198 */
    reg->ULPTCNT = 0U;
  }
  ra_log_info(s_tag, "ulpt_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ulpt_start(uint8_t channel, uint32_t period)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 */
  reg->ULPTCR = 0U;
  /* HUM Ch 25.2.2 "ULPTMR1 : ULPT Mode Register 1" p 1192 -- free-running,
   * sub-clock source. */
  reg->ULPTMR1 = 0U;
  /* HUM Ch 25.2.3 "ULPTMR2 : ULPT Mode Register 2" p 1194 */
  reg->ULPTMR2 = 0U;
  /* HUM Ch 25.2.4 "ULPTMR3 : ULPT Mode Register 3" p 1195 */
  reg->ULPTMR3 = 0U;
  /* HUM Ch 25.2.6 "ULPT : ULPT Counter Register" p 1198 */
  reg->ULPTCNT = period;
  /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 */
  /* TSTART = 1. */
  reg->ULPTCR = k_ra_ulpt_mask_tstart;

  ra_log_info_val(s_tag, "start channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ulpt_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 */
  /* TSTOP toggle. */
  reg->ULPTCR = k_ra_ulpt_mask_tstop;
  reg->ULPTCR = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

static ra_ulpt_event_fn_t s_ulpt_fn;
static void*              s_ulpt_ctx;

/**
 * @brief Tear down one ULPT channel and drop its MSTP reference.
 *
 * @details
 * Clears ULPTCR (HUM Ch 25.2.1 "ULPTCR : ULPT Control Register",
 * p 1190) so the counter stops, then releases the matching MSTP gate.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Channel stopped, MSTP released.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 2.
 * @retval k_ra_err_invalid_state  ``ra_mstp_disable`` underflow.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 * @pre ``ra_ulpt_init`` (or the start API) was previously called.
 *
 * @post ULPTCR for the channel reads as 0 (counter stopped).
 * @post MSTP reference for the channel has been decremented.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_ulpt_deinit(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->ULPTCR = 0U;
  return ra_mstp_disable(s_ulpt_mstp_table[channel]);
}

/**
 * @brief Update the 32-bit reload value for a running ULPT channel.
 *
 * @details
 * Writes ``period`` into ULPTCNT (HUM Ch 25.2.4 "ULPTCNT : ULPT
 * Counter", p 1192). Use on a stopped channel or immediately after
 * a counter refresh.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 * @param[in] period  32-bit reload value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Counter rewritten.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 2.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Channel has been brought up via ``ra_ulpt_init``.
 *
 * @post Counter holds ``period``.
 * @post Channel state (running / stopped) is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_ulpt_set_period(uint8_t channel, uint32_t period)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ulpt_regs_t* reg = ra_ulpt(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");
  reg->ULPTCNT = period;
  return k_ra_ok;
}

/**
 * @brief Read the ULPTCR control / status register for ``channel``.
 *
 * @details
 * Returns the raw 8-bit ULPTCR value (HUM Ch 25.2.1, p 1190) so
 * callers can inspect TSTART / TCSTF / TUNDF / TEDGF without leaking
 * the register layout.
 *
 * @param[in]  channel  ULPT channel index (0 or 1).
 * @param[out] out_mask Receives the ULPTCR snapshot.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Status read into ``*out_mask``.
 * @retval k_ra_err_null_ptr       ``out_mask`` was NULL.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 2.
 *
 * @pre ``out_mask`` non-NULL.
 * @pre Channel is powered or has previously been started.
 *
 * @post ``*out_mask`` reflects live ULPTCR contents.
 * @post Hardware state is unchanged.
 *
 * @note Read-only; safe under simple races.
 * @since 0.1.0
 */
ra_err_t ra_ulpt_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  *out_mask = ra_ulpt(channel)->ULPTCR;
  return k_ra_ok;
}

/**
 * @brief Register the global ULPT ISR callback (NULL detaches).
 *
 * @details
 * Stores ``(fn, ctx)`` in the shared slot consulted by every
 * ``ra_ulpt_dispatch`` call, regardless of channel index. Passing
 * ``fn == NULL`` detaches.
 *
 * @param[in] fn  Callback function or NULL to detach.
 * @param[in] ctx Opaque value forwarded verbatim to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Slot updated.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Lifetime of ``ctx`` outlives the next ``ra_ulpt_dispatch`` call.
 *
 * @post Shared callback slot reflects ``(fn, ctx)``.
 * @post No ULPT register state is mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_ulpt_attach_handler(ra_ulpt_event_fn_t fn, void* ctx)
{
  s_ulpt_fn  = fn;
  s_ulpt_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Fire the registered ULPT callback for ``channel`` (ISR helper).
 *
 * @details
 * Out-of-range ``channel`` values and detached slots are silently
 * dropped so spurious IRQs are harmless.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @return None.
 * @retval None
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``channel`` < 2 (out-of-range silently dropped).
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post No ULPT register state is mutated.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra_ulpt_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return;
  }
  const ra_ulpt_event_fn_t fn  = s_ulpt_fn;
  void* const              ctx = s_ulpt_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}

/**
 * @brief Drop the MSTP reference for one ULPT channel (low-power gate).
 *
 * @details
 * Releases the per-channel MSTP gate so the channel stops consuming
 * clock during sleep / standby. Counter state is preserved by the
 * always-on ULPT clock domain.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 MSTP reference dropped.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 2.
 * @retval k_ra_err_invalid_state  ``ra_mstp_disable`` underflow.
 *
 * @pre Channel is in a quiescent state.
 * @pre Caller manages MSTP reference balance per-channel.
 *
 * @post MSTP gate for the channel is closed.
 * @post Sibling-channel MSTP state is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_ulpt_enter_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_ulpt_mstp_table[channel]);
}

/**
 * @brief Re-enable the MSTP gate for one ULPT channel after stop.
 *
 * @details
 * Re-takes the per-channel MSTP gate so the channel resumes clocking
 * with its prior counter / control state intact.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 MSTP reference taken.
 * @retval k_ra_err_invalid_arg    ``channel`` >= 2.
 * @retval k_ra_err_invalid_state  ``ra_mstp_enable`` over-took limit.
 *
 * @pre ``ra_ulpt_enter_stop`` was previously called for ``channel``.
 * @pre Caller manages MSTP reference balance per-channel.
 *
 * @post MSTP gate for the channel is open.
 * @post Channel resumes from its previously-cached state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_ulpt_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_ulpt_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_ulpt_mstp_table[channel]);
}
