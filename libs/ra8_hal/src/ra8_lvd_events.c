/**
 * @file ra8_lvd_events.c
 * @brief PVD / LVD security attribution, n-channel lock, ELC / standby
 *        helpers, filter-timing math, and callback registration / ISR demux.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Companion translation unit to `ra8_lvd.c`. Owns the PVDSAR security
 * attribution write, the PVDLR n-channel lock / unlock pair, the ELC
 * event-line and standby reconfiguration helpers, the pure
 * filter-settle delay computation, and the callback registration plus
 * ISR demux path (which holds the per-driver callback state private to
 * this TU). The shared channel-map descriptor table and the cross-TU
 * register helpers it calls live in `ra8_lvd_internal.h`; every register
 * access keeps its HUM Ch 8 citation (R01UH1065EJ rev 1.30).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_lvd.h"
#include "ra8_lvd_internal.h"
#include "ra8_lvd_regs.h"

/**
 * @var s_tag
 * @brief Logging tag used by every error path in this TU.
 *
 * @details Private per-TU copy of the driver-wide "LVD" log tag.
 * @note PRIVATE.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
static const char* s_tag = "LVD";

/* =============================================================================
 * Callback storage
 * =============================================================================
 */

/**
 * @var s_lvd_fn
 * @brief Shared crossing callback (set via `ra8_lvd_attach_handler`).
 */
static ra8_lvd_event_fn_t s_lvd_fn;

/**
 * @var s_lvd_ctx
 * @brief Context passed to ``s_lvd_fn``.
 */
static void* s_lvd_ctx;

/**
 * @var s_lvd_chan_fn
 * @brief Per-channel override callbacks for PVD1 / PVD2.
 *
 * @details
 * Indexed by ``ra8_lvd_map_idx_t``; only entries [0] and [1] are used
 * (the n channels have no IRQ path). When non-null, takes precedence
 * over ``s_lvd_fn`` in `ra8_lvd_dispatch`.
 */
static ra8_lvd_event_fn_t s_lvd_chan_fn[k_ra8_lvd_map_idx_count];

/**
 * @var s_lvd_chan_ctx
 * @brief Per-channel callback contexts paired with ``s_lvd_chan_fn``.
 */
static void* s_lvd_chan_ctx[k_ra8_lvd_map_idx_count];

/* =============================================================================
 * Security attribution + n-channel lock
 * =============================================================================
 */

/**
 * @brief Program PVDSAR security attribution for PVD channels.
 *
 * @details
 * Writes the security attribution mask (HUM Ch 12.2.1 "PVDSAR"
 * p 594) so each PVD channel can be claimed by Secure or Non-secure.
 *
 * @param[in] mask Bitwise-OR of ``k_ra8_lvd_pvdsar_*`` constants.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Mask written.
 * @retval k_ra8_err_invalid_arg   ``mask`` includes undefined bits.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller is in Secure world (PVDSAR is S-only).
 * @post PVDSAR reflects ``mask``.
 * @post Subsequent PVD register accesses honour the new attribution.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_set_security(uint32_t mask)
{
  /* HUM Ch 8.2.1 "PVDSAR : Programmable Voltage Detection Security
   * Attribution Register" p 302 */
  if ((mask & ~k_ra8_lvd_pvdsar_mask_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  *ra8_lvd_reg32(k_ra8_lvd_pvdsar_off) = mask;
  return k_ra8_ok;
}

/**
 * @brief Unlock PVDLR so PVDn channels accept further writes.
 *
 * @details
 * Writes the documented unlock pattern to PVDLR (HUM Ch 12.2.10
 * "PVDLR" p 601) so subsequent ``ra8_lvd_*`` calls on PVDn channels
 * are honoured.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Lock released.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller plans to update PVDn state next.
 * @post PVDLR.LOCK reads as 0.
 * @post PVDn channel writes are accepted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_unlock_n_channels(void)
{
  /* HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock Register" p 309 */
  *ra8_lvd_reg8(k_ra8_lvd_pvdlr_off) = k_ra8_lvd_pvdlr_value_unlock;
  return k_ra8_ok;
}

/**
 * @brief Relock PVDLR after a PVDn maintenance window.
 *
 * @details
 * Writes any non-unlock pattern to PVDLR (HUM Ch 12.2.10 p 601 --
 * "if you write an arbitrary value to the LOCK, the LOCK bit is
 * fixed to 1.").
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Lock re-engaged.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre PVDn updates are complete and safe to freeze.
 * @post PVDLR.LOCK reads as 1.
 * @post Further PVDn writes are silently ignored.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_relock_n_channels(void)
{
  /* HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock Register" p 309 --
   * "if you write an arbitrary value to the LOCK, the LOCK bit is
   * fixed to 1." */
  *ra8_lvd_reg8(k_ra8_lvd_pvdlr_off) = k_ra8_lvd_pvdlr_value_relock;
  return k_ra8_ok;
}

/* =============================================================================
 * ELC + standby helpers
 * =============================================================================
 */

/**
 * @brief Arm the ELC event line for a PVDm channel.
 *
 * @details
 * Per HUM Ch 12.7 "Event Link Controller (ELC) Output" p 606 the
 * event line tracks CMPE; this helper acknowledges any latched DET
 * before raising CMPE so the ELC consumer sees a clean transition.
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  ELC line armed.
 * @retval k_ra8_err_invalid_arg     Channel mapping failed.
 * @retval k_ra8_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post SR.DET reads as 0; CR0.CMPE reads as 1.
 * @post ELC consumer receives subsequent crossings as events.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_enable_elc_event(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = priv_ra8_lvd_internal_channel_to_idx(channel, &idx);
  /* GCOVR_EXCL_BR_START -- channel_to_idx() error edge; enable_elc_event pre-validates it */
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_enable_elc_event: bad channel");
  /* GCOVR_EXCL_BR_STOP */

  const ra8_lvd_channel_map_t map = g_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }

  /* HUM Ch 8.7 "Event Link Controller (ELC) Output" p 315 -- the event
   * line tracks the comparator output enable: clear DET first, then
   * raise CMPE. */
  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307 */
  const uint8_t sr      = *ra8_lvd_reg8(map.sr);
  *ra8_lvd_reg8(map.sr) = (uint8_t)(sr & (uint8_t)~k_ra8_lvd_sr_mask_det);
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  priv_ra8_lvd_internal_cr0_rmw(&map, 0U, k_ra8_lvd_cr0_mask_cmpe);
  return k_ra8_ok;
}

/**
 * @brief Disarm the ELC event line for a PVDm channel.
 *
 * @details
 * Per HUM Ch 12.7 p 606, clears CMPE so the ELC line goes inactive.
 *
 * @param[in] channel PVDm channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  ELC line disarmed.
 * @retval k_ra8_err_invalid_arg     Channel mapping failed.
 * @retval k_ra8_err_not_supported   Channel has no IRQ path (PVDn).
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre PRCR.PRC3 unlocked.
 * @post CR0.CMPE reads as 0.
 * @post ELC consumer no longer receives crossings as events.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_disable_elc_event(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = priv_ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_disable_elc_event: bad channel");

  const ra8_lvd_channel_map_t map = g_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 -- clear CMPE so the ELC line goes
   * inactive (HUM 8.7 p 315). */
  priv_ra8_lvd_internal_cr0_rmw(&map, k_ra8_lvd_cr0_mask_cmpe, 0U);
  return k_ra8_ok;
}

/**
 * @brief Reconfigure a PVD channel for entry into Software / Deep Standby.
 *
 * @details
 * Per HUM Ch 12.5 (p 602-603), the digital filter must be disabled
 * and (for m channels) RI / RN cleared before entering standby.
 *
 * @param[in] channel PVD channel id.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Channel reconfigured for standby.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 *
 * @pre Channel previously brought up via ``ra8_lvd_channel_init``.
 * @pre Caller is preparing the SoC for standby entry.
 * @post CR0.DFDIS reads as 1.
 * @post For m channels CR0.RI and CR0.RN read as 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_configure_for_standby(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = priv_ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_configure_for_standby: bad channel");

  const ra8_lvd_channel_map_t map = g_lvd_map[idx];

  /* HUM Ch 8.5(1) "Setting in Software Standby mode" p 311 +
   * HUM Ch 8.5(2) "Settings in Deep Software Standby mode" p 312 --
   * disable the digital filter and (for m channels) clear RI + RN. */
  uint8_t set_bits = k_ra8_lvd_cr0_mask_dfdis;
  uint8_t clr_bits = 0U;
  if (map.has_irq) {
    clr_bits |= k_ra8_lvd_cr0_mask_ri;
    clr_bits |= k_ra8_lvd_cr0_mask_rn;
  }
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  priv_ra8_lvd_internal_cr0_rmw(&map, clr_bits, set_bits);
  return k_ra8_ok;
}

/**
 * @brief Drop CR0.RI on every NMI-capable channel (deep-standby exit).
 *
 * @details
 * Per HUM Ch 12.2.4 (p 597), prepares the PVD subsystem for normal
 * operation after exiting Deep Standby by clearing the per-channel
 * RI bit (which is documented to be UD-only across DPSTBY).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Path cleared on every NMI channel.
 *
 * @pre PRCR.PRC3 unlocked (caller-managed).
 * @pre Caller has just exited Deep Software Standby.
 * @post CR0.RI reads as 0 on every NMI-capable channel.
 * @post Subsequent crossings will not auto-reset the SoC.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_cancel_deep_standby_path(void)
{
  /* HUM Ch 8.2.4 "PVDmCR0" p 305 */
  for (uint8_t i = 0U; i < k_ra8_lvd_nmi_channel_count; ++i) {
    priv_ra8_lvd_internal_cr0_rmw(&g_lvd_map[i], k_ra8_lvd_cr0_mask_ri, 0U);
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Filter timing helper (pure)
 * =============================================================================
 */

/**
 * @brief Compute the digital-filter settle delay in microseconds.
 *
 * @details
 * Implements the "2s + 3 cycles of the LOCO" formula from HUM
 * Table 12.4 / 12.6 step 8 (s = 2^(div+1)). Matches FSP's
 * ``r_lvd_filter_delay`` rounded-up microseconds.
 *
 * @param[in] div     LOCO divider used by the channel.
 * @param[in] loco_hz LOCO frequency in Hz (0 -> default 32_768 Hz).
 *
 * @return Settle delay in microseconds (rounded up by +1 us).
 * @retval >=1 Microseconds the caller must wait after FSAMP changes.
 *
 * @pre None.
 * @pre Caller will block / sleep at least the returned amount.
 * @post Hardware state is unchanged.
 * @post Return value is in microseconds.
 *
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
uint32_t ra8_lvd_filter_delay_us(ra8_lvd_loco_div_t div, uint32_t loco_hz)
{
  /* HUM Table 8.4 step 8 (p 312) / Table 8.6 step 8 (p 315): wait for
   * "2s + 3 cycles of the LOCO" where s = 2^(div+1). */
  uint8_t safe_div = (uint8_t)div;
  if (safe_div > k_ra8_lvd_loco_div_max) {
    safe_div = k_ra8_lvd_loco_div_max;
  }
  const uint32_t local_factor = (uint32_t)1U << (safe_div + 1U);
  const uint32_t loco_cycles  = (k_ra8_lvd_filter_factor * local_factor) + k_ra8_lvd_filter_extra;
  const uint32_t hz           = (loco_hz != 0U) ? loco_hz : k_ra8_lvd_loco_hz_default;
  /* +1 us round-up matches FSP's r_lvd_filter_delay computation. */
  return ((loco_cycles * k_ra8_lvd_us_per_sec) / hz) + 1U;
}

/* =============================================================================
 * Callback registration + ISR demux
 * =============================================================================
 */

/**
 * @brief Register the shared LVD ISR callback (NULL detaches).
 *
 * @details
 * Stores ``(fn, ctx)`` in the shared slot consulted by every
 * ``ra8_lvd_dispatch`` call when no per-channel handler is attached.
 *
 * @param[in] fn  Callback function or NULL to detach.
 * @param[in] ctx Opaque value forwarded verbatim to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Slot updated.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Lifetime of ``ctx`` outlives the next ``ra8_lvd_dispatch`` call.
 * @post Shared callback slot reflects ``(fn, ctx)``.
 * @post Per-channel slots are unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_lvd_attach_handler(ra8_lvd_event_fn_t fn, void* ctx)
{
  s_lvd_fn  = fn;
  s_lvd_ctx = ctx;
  return k_ra8_ok;
}

/**
 * @brief Register a per-channel LVD callback for a PVDm channel.
 *
 * @details
 * Per-channel handlers take precedence over the shared callback set
 * by ``ra8_lvd_attach_handler``. Available only on PVDm channels.
 *
 * @param[in] channel PVDm channel id.
 * @param[in] fn      Callback function or NULL to detach.
 * @param[in] ctx     Opaque value forwarded verbatim to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Slot updated.
 * @retval k_ra8_err_invalid_arg   Channel mapping failed.
 * @retval k_ra8_err_not_supported Channel has no IRQ path (PVDn).
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Lifetime of ``ctx`` outlives the next ``ra8_lvd_dispatch`` call.
 * @post Per-channel slot for ``channel`` reflects ``(fn, ctx)``.
 * @post Shared callback slot is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t
ra8_lvd_attach_channel_handler(ra8_lvd_channel_t channel, ra8_lvd_event_fn_t fn, void* ctx)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = priv_ra8_lvd_internal_channel_to_idx(channel, &idx);
  RA8_RETURN_ON_ERROR(map_err, s_tag, "lvd_attach_channel_handler: bad channel");

  const ra8_lvd_channel_map_t map = g_lvd_map[idx];
  if (!map.has_irq) {
    return k_ra8_err_not_supported;
  }
  s_lvd_chan_fn[idx]  = fn;
  s_lvd_chan_ctx[idx] = ctx;
  return k_ra8_ok;
}

/**
 * @brief Demux a PVD ISR -- ack DET, fire per-channel + shared callbacks.
 *
 * @details
 * Snapshots PVDmSR.DET; if no crossing is latched the call is a
 * no-op. Otherwise invokes the per-channel callback (if any), then
 * the shared callback (if any), then clears DET via the W0C
 * sequence (HUM Ch 12.2.7 "PVDmSR" p 600).
 *
 * @param[in] channel PVD channel id that fired the IRQ / NMI.
 *
 * @pre Called from PVD ISR or NMI context.
 * @pre PRCR.PRC3 unlocked (caller-managed during ack).
 *
 * @post If a callback was attached, it has been invoked exactly once.
 * @post PVDmSR.DET is 0 on return.
 *
 * @note Thread safety: ISR-context only.
 * @since 0.1.0
 */
void ra8_lvd_dispatch(ra8_lvd_channel_t channel)
{
  uint8_t         idx     = 0U;
  const ra8_err_t map_err = priv_ra8_lvd_internal_channel_to_idx(channel, &idx);
  if (map_err != k_ra8_ok) {
    return;
  }
  const ra8_lvd_channel_map_t map = g_lvd_map[idx];

  /* Only m channels have a status register / IRQ path. */
  if (!map.has_irq) {
    return;
  }

  /* HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register" p 307
   * -- only fire the callback if a crossing was actually latched. */
  const uint8_t sr = *ra8_lvd_reg8(map.sr);
  if ((sr & k_ra8_lvd_sr_mask_det) == 0U) {
    return;
  }

  /* Per-channel callback wins; otherwise fall back to the shared one. */
  const ra8_lvd_event_fn_t chan_fn  = s_lvd_chan_fn[idx];
  void* const              chan_ctx = s_lvd_chan_ctx[idx];
  const ra8_lvd_event_fn_t fn       = (chan_fn != nullptr) ? chan_fn : s_lvd_fn;
  void* const              ctx      = (chan_fn != nullptr) ? chan_ctx : s_lvd_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }

  /* Write 0 to DET so the next crossing can latch (HUM 8.2.7 Note 1 p 307). */
  const uint8_t cleared = (uint8_t)(*ra8_lvd_reg8(map.sr) & (uint8_t)~k_ra8_lvd_sr_mask_det);
  *ra8_lvd_reg8(map.sr) = cleared;
}
