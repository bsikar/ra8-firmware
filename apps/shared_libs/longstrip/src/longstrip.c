/**
 * @file longstrip.c
 * @brief Continuous vertical-scroll (longstrip) engine over a JOF atlas (#289).
 *
 * @details Implements longstrip.h: virtual-canvas geometry, the scroll +
 *          fling state machine, bounded directional prefetch and the visible
 *          band composite. The whole file is pure integer arithmetic over the
 *          parsed atlas geometry plus calls into `ra8_tile_cache` (band paging)
 *          and `jof` (per-band decode) -- no MMIO, so it runs
 *          identically on the target, in ra8_emulator and on the unit-test host.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "longstrip.h"

#include <stdint.h>

#include "jof.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "longstrip";

/**
 * @enum wt_consts_t
 * @brief Fixed engine constants (no magic numbers).
 *
 * @details `k_wt_fling_num` / `k_wt_fling_den` are the per-tick velocity
 *          retention ratio: integer `v = v * num / den` decays magnitude
 *          monotonically to zero (truncation toward zero snaps small speeds
 *          to rest), so a fling always terminates in a bounded number of
 *          ticks. `k_wt_tile_x` is the single full-width band column; a
 *          longstrip strip has exactly one.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_wt_fling_num = 7U, /**< Velocity retained per tick (numerator).      */
  k_wt_fling_den = 8U, /**< Velocity retention denominator.              */
  k_wt_tile_x    = 0U, /**< Longstrip band column index (single column). */
  k_wt_zoom      = 0U, /**< Native zoom / mip level.                     */
} wt_consts_t;

ra8_err_t longstrip_tile_decode(void*                 ctx,
                                const ra8_tile_key_t* key,
                                uint8_t*              cell,
                                uint32_t              cell_bytes,
                                uint16_t*             out_w,
                                uint16_t*             out_h)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "decode ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(cell, s_tag, "cell must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  longstrip_decode_ctx_t* dc = (longstrip_decode_ctx_t*)ctx;
  return jof_read_tile(dc->pread,
                       dc->pread_ctx,
                       &dc->info,
                       key->tile_x,
                       key->tile_y,
                       dc->scratch,
                       dc->scratch_cap,
                       cell,
                       cell_bytes,
                       out_w,
                       out_h);
}

/**
 * @brief Bind the parsed atlas geometry and config into the engine state.
 *
 * @details The state-population half of ::longstrip_open, split out to keep
 *          the entry point under the statement-complexity threshold. Homes the
 *          scroll to the top and derives the max-scroll clamp from the canvas
 *          height against the viewport (a strip no taller than the viewport
 *          cannot scroll, so the clamp floors at zero).
 *
 * @param[out] wt   Engine state to populate (every field written).
 * @param[in]  cfg  Validated open configuration.
 * @param[in]  info Parsed, shape-checked JOF atlas geometry.
 *
 * @pre @p wt, @p cfg and @p info are non-NULL (the caller validated them).
 * @pre @p info describes a single full-width band column.
 * @post Every field of @p wt is initialised; the scroll is homed to the top.
 * @post `wt->max_scroll >= 0`.
 *
 * @note Not thread-safe (mutates @p wt).
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_bind(longstrip_t* wt, const longstrip_cfg_t* cfg, const jof_info_t* info)
{
  wt->info       = *info;
  wt->canvas_w   = info->width;
  wt->canvas_h   = info->height;
  wt->band_h     = info->tile_h;
  wt->band_count = info->tile_rows;
  wt->viewport_w = cfg->viewport_w;
  wt->viewport_h = cfg->viewport_h;
  wt->scroll_y   = 0;
  wt->max_scroll = (info->height > cfg->viewport_h)
                     ? (int32_t)((uint32_t)info->height - (uint32_t)cfg->viewport_h)
                     : 0;
  wt->velocity   = 0;
  wt->cache      = cfg->cache;
  wt->image_id   = cfg->image_id;
  wt->blit       = cfg->blit;
  wt->blit_ctx   = cfg->blit_ctx;
}

/**
 * @brief Reject a NULL engine, config, or any NULL callback / cache it carries.
 *
 * @details The null-guard half of ::longstrip_open, split out so the entry
 *          point stays under the statement-complexity threshold. Validates the
 *          three seams the engine must hold before it can page or composite.
 *
 * @param[in] wt  Engine state handle to validate.
 * @param[in] cfg Open configuration to validate.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `wt`, `cfg` and every required callback are non-NULL.
 * @retval k_ra8_err_null_ptr One of them is NULL.
 *
 * @pre `s_tag` is initialised (always true for this TU).
 * @pre The caller propagates a non-OK result unchanged.
 * @post No state is modified.
 * @post A `k_ra8_ok` result guarantees `cfg->pread`/`cache`/`blit` are non-NULL.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_ptrs(const longstrip_t* wt, const longstrip_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->pread, s_tag, "cfg->pread must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->cache, s_tag, "cfg->cache must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->blit, s_tag, "cfg->blit must not be nullptr");
  return k_ra8_ok;
}

ra8_err_t longstrip_open(longstrip_t* wt, const longstrip_cfg_t* cfg)
{
  const ra8_err_t ptr_err = internal_check_ptrs(wt, cfg);
  if (ptr_err != k_ra8_ok) {
    return ptr_err;
  }
  /* Decision: reject a zero-area viewport (2 conditions -- MC/DC in tests). */
  if ((cfg->viewport_w == 0U) || (cfg->viewport_h == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  jof_info_t      info = {};
  const ra8_err_t err  = jof_parse(cfg->pread, cfg->pread_ctx, cfg->atlas_size, &info);
  if (err != k_ra8_ok) {
    return err;
  }
  /* A longstrip strip is one full-width band column; anything else is not a
   * scrollable strip. `jof_parse` derives tile_cols as
   * ceil(width / tile_w), so `tile_w == width` already implies tile_cols == 1
   * -- the full-width test alone is necessary and sufficient. */
  if (info.tile_w != info.width) {
    return k_ra8_err_not_supported;
  }
  internal_bind(wt, cfg, &info);
  return k_ra8_ok;
}

ra8_err_t longstrip_set_viewport(longstrip_t* wt, uint16_t viewport_w, uint16_t viewport_h)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  /* Decision: reject a zero-area viewport (2 conditions -- MC/DC in tests). */
  if ((viewport_w == 0U) || (viewport_h == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  wt->viewport_w = viewport_w;
  wt->viewport_h = viewport_h;
  wt->max_scroll =
    (wt->canvas_h > (uint32_t)viewport_h) ? (int32_t)(wt->canvas_h - (uint32_t)viewport_h) : 0;
  wt->scroll_y = longstrip_clamp_scroll(wt, wt->scroll_y);
  return k_ra8_ok;
}

int32_t longstrip_clamp_scroll(const longstrip_t* wt, int32_t y)
{
  if (wt == nullptr) {
    return 0;
  }
  const int32_t max = wt->max_scroll;
  if (y < 0) {
    y = 0;
  } else if (y > max) {
    y = max;
  }
  return y;
}

ra8_err_t longstrip_band_at_y(const longstrip_t* wt, uint32_t y, uint16_t* out_band)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  RA8_CHECK_NULL_PTR(out_band, s_tag, "out_band must not be nullptr");
  if (y >= wt->canvas_h) {
    return k_ra8_err_out_of_range;
  }
  *out_band = (uint16_t)(y / (uint32_t)wt->band_h);
  return k_ra8_ok;
}

ra8_err_t longstrip_visible_bands(const longstrip_t* wt,
                                  int32_t            scroll_y,
                                  uint16_t*          out_first,
                                  uint16_t*          out_last)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  RA8_CHECK_NULL_PTR(out_first, s_tag, "out_first must not be nullptr");
  RA8_CHECK_NULL_PTR(out_last, s_tag, "out_last must not be nullptr");
  const int32_t  top    = longstrip_clamp_scroll(wt, scroll_y);
  const uint32_t bh     = (uint32_t)wt->band_h;
  const uint32_t first  = (uint32_t)top / bh;
  uint32_t       bottom = (uint32_t)top + (uint32_t)wt->viewport_h - 1U;
  if (bottom >= wt->canvas_h) {
    bottom = wt->canvas_h - 1U;
  }
  uint32_t last = bottom / bh;
  if (last >= (uint32_t)wt->band_count) {
    last = (uint32_t)wt->band_count - 1U;
  }
  *out_first = (uint16_t)first;
  *out_last  = (uint16_t)last;
  return k_ra8_ok;
}

/**
 * @brief Saturating signed 32-bit add: pins at the int32 limits, never wraps.
 * @details Widens to int64 for the sum so a hostile scroll delta cannot
 *          overflow `scroll_y + delta` before it is clamped to the strip.
 * @param[in] a First addend.
 * @param[in] b Second addend.
 * @return The clamped sum in `[INT32_MIN, INT32_MAX]`.
 * @retval INT32_MAX The true sum exceeded the signed 32-bit range (positive).
 * @retval INT32_MIN The true sum fell below the signed 32-bit range.
 * @pre @p a and @p b are valid signed integers.
 * @pre The result is only ever used as an argument to the range clamp.
 * @post No state is mutated.
 * @post The result never wraps around the int32 boundary.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_sat_add(int32_t a, int32_t b)
{
  const int64_t sum = (int64_t)a + (int64_t)b;
  if (sum > (int64_t)INT32_MAX) {
    return INT32_MAX;
  }
  if (sum < (int64_t)INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)sum;
}

ra8_err_t longstrip_scroll_by(longstrip_t* wt, int32_t delta)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  wt->scroll_y = longstrip_clamp_scroll(wt, internal_sat_add(wt->scroll_y, delta));
  /* Decision: pinned at either end stops a running fling (2 conditions). */
  if ((wt->scroll_y == 0) || (wt->scroll_y == wt->max_scroll)) {
    wt->velocity = 0;
  }
  return k_ra8_ok;
}

ra8_err_t longstrip_fling(longstrip_t* wt, int32_t v0)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  wt->velocity = v0;
  return k_ra8_ok;
}

/**
 * @brief Decay a velocity one friction step toward zero (magnitude down).
 * @details Integer proportional decay `v * num / den` (7/8) with truncation
 *          toward zero, so `|v|` strictly decreases and small speeds snap to
 *          rest -- a fling always terminates in a bounded number of ticks.
 * @param[in] v Current velocity, px/tick (signed).
 * @return The decayed velocity, same sign, `|result| <= |v|`.
 * @retval 0 The input magnitude was small enough to snap to rest.
 * @pre @p v is the engine's current velocity.
 * @pre `num < den` so the magnitude cannot grow.
 * @post No state is mutated.
 * @post `|result| <= |v|` (magnitude never increases).
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_apply_friction(int32_t v)
{
  return (int32_t)((v * (int32_t)k_wt_fling_num) / (int32_t)k_wt_fling_den);
}

/**
 * @brief Report whether the viewport is pinned at the top of the strip.
 * @details A pure boundary predicate over `scroll_y`; the clamp keeps
 *          `scroll_y >= 0`, so equality is the pinned case.
 * @param[in] wt Opened strip (non-NULL by caller contract).
 * @return `true` when `scroll_y <= 0`, else `false`.
 * @retval true  The viewport top is at the strip top.
 * @retval false The viewport has scrolled below the top.
 * @pre @p wt was opened by ::longstrip_open.
 * @pre The clamp invariant `scroll_y >= 0` holds.
 * @post No state is mutated.
 * @post The result depends only on `scroll_y`.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_at_top(const longstrip_t* wt)
{
  return wt->scroll_y <= 0;
}

/**
 * @brief Report whether the viewport is pinned at the bottom of the strip.
 * @details Mirror of ::internal_at_top over the `max_scroll` end; the clamp keeps
 *          `scroll_y <= max_scroll`, so equality is the pinned case.
 * @param[in] wt Opened strip (non-NULL by caller contract).
 * @return `true` when `scroll_y >= max_scroll`, else `false`.
 * @retval true  The viewport is at the strip bottom.
 * @retval false The viewport is above the bottom.
 * @pre @p wt was opened by ::longstrip_open.
 * @pre The clamp invariant `scroll_y <= max_scroll` holds.
 * @post No state is mutated.
 * @post The result depends only on `scroll_y` and `max_scroll`.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_at_bottom(const longstrip_t* wt)
{
  return wt->scroll_y >= wt->max_scroll;
}

/**
 * @brief True when a fling has hit the boundary it is travelling toward.
 *
 * @details The caller (::longstrip_tick) only reaches this with a non-zero
 *          velocity -- a zeroed velocity already short-circuits the tick to
 *          rest -- so the sign alone decides which end halts the fling: an
 *          upward (`velocity < 0`) fling stops only at the top, a downward
 *          (`velocity > 0`) fling only at the bottom; a fling moving away from
 *          a boundary keeps going. Testing the sign once (not twice) keeps both
 *          arms independently reachable rather than folding the mutually
 *          exclusive sign tests into one compound decision.
 *
 * @param[in] wt Opened strip (non-NULL by caller contract).
 * @return `true` if the fling should come to rest this tick, else `false`.
 * @retval true  The velocity is driving into the boundary it has reached.
 * @retval false The fling can keep moving (away from, or not yet at, a bound).
 * @pre @p wt was opened by ::longstrip_open.
 * @pre `wt->velocity` is the non-zero post-friction velocity for this tick.
 * @post No state is mutated.
 * @post The result depends only on the velocity sign and the pinned ends.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_fling_should_stop(const longstrip_t* wt)
{
  return (wt->velocity < 0) ? internal_at_top(wt) : internal_at_bottom(wt);
}

bool longstrip_tick(longstrip_t* wt)
{
  if (wt == nullptr) {
    return false;
  }
  if (wt->velocity == 0) {
    return false;
  }
  wt->scroll_y = longstrip_clamp_scroll(wt, internal_sat_add(wt->scroll_y, wt->velocity));
  wt->velocity = internal_apply_friction(wt->velocity);
  /* Decision: come to rest when the speed hits zero or a boundary is reached. */
  if ((wt->velocity == 0) || internal_fling_should_stop(wt)) {
    wt->velocity = 0;
    return false;
  }
  return true;
}

/**
 * @brief Fetch a band into the cache then release it (warm, no held pin).
 * @details Directional prefetch primitive: a get + immediate put makes the band
 *          resident and MRU without holding a pin, so the tile cache's LRU still
 *          bounds residency. A decode miss is best-effort -- the visible render
 *          decodes it on demand -- so any error is intentionally swallowed.
 * @param[in,out] wt   Opened strip whose cache is warmed.
 * @param[in]     band Band index to warm (caller keeps it in range).
 * @pre @p wt was opened by ::longstrip_open.
 * @pre @p band is a valid band index for @p wt.
 * @post No engine geometry or scroll state is mutated.
 * @post No cache pin is held on return.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_warm_band(longstrip_t* wt, uint16_t band)
{
  ra8_tile_key_t key = {};
  key.image_id       = wt->image_id;
  key.tile_x         = (uint16_t)k_wt_tile_x;
  key.tile_y         = band;
  key.zoom           = (uint16_t)k_wt_zoom;
  ra8_tile_t tile    = {};
  if (ra8_tile_cache_get(wt->cache, &key, &tile) == k_ra8_ok) {
    (void)ra8_tile_cache_put(wt->cache, tile.pixels);
  }
}

ra8_err_t longstrip_prefetch(longstrip_t* wt, uint16_t depth)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  uint16_t        first = 0;
  uint16_t        last  = 0;
  const ra8_err_t err   = longstrip_visible_bands(wt, wt->scroll_y, &first, &last);
  if (err != k_ra8_ok) {
    return err;
  }
  if (depth > (uint16_t)k_longstrip_max_prefetch) {
    depth = (uint16_t)k_longstrip_max_prefetch;
  }
  const uint16_t visible_span = (uint16_t)((last - first) + 1U);
  /* Decision: nothing to prefetch with zero depth or a strip that fits (2). */
  if ((depth == 0U) || (wt->band_count <= visible_span)) {
    return k_ra8_ok;
  }
  const bool downward = (wt->velocity >= 0);
  for (uint16_t i = 1U; i <= depth; i++) { /* bounded by k_longstrip_max_prefetch */
    uint16_t target = 0;
    if (downward) {
      const uint32_t t = (uint32_t)last + (uint32_t)i;
      if (t >= (uint32_t)wt->band_count) {
        break;
      }
      target = (uint16_t)t;
    } else {
      if (first < i) {
        break;
      }
      target = (uint16_t)(first - i);
    }
    internal_warm_band(wt, target);
  }
  return k_ra8_ok;
}

/**
 * @brief Add the on-screen row span of one blitted band to the coverage.
 * @details Intersects the band's destination rows `[dst_y, dst_y + band_h)`
 *          with the viewport `[0, viewport_h)` and adds the overlap to
 *          `stats->covered_rows`. Bands are contiguous and non-overlapping, so
 *          summing per-band intersections yields the exact covered-row total
 *          with no double counting -- the seamless-frame check.
 * @param[in]     wt     Opened strip (for the viewport height).
 * @param[in]     dst_y  Band destination top in the viewport (may be negative).
 * @param[in]     band_h Band height, pixels.
 * @param[in,out] stats  Frame accounting whose `covered_rows` is advanced.
 * @pre @p wt was opened by ::longstrip_open.
 * @pre @p stats is a live per-frame accumulator.
 * @post `stats->covered_rows` grew by the clipped on-screen row count (>= 0).
 * @post No other field of @p stats or @p wt is mutated.
 * @note Not thread-safe (mutates @p stats).
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_accumulate_coverage(const longstrip_t*        wt,
                                         int32_t                   dst_y,
                                         uint16_t                  band_h,
                                         longstrip_render_stats_t* stats)
{
  int32_t       top = (dst_y > 0) ? dst_y : 0;
  int32_t       bot = dst_y + (int32_t)band_h;
  const int32_t vh  = (int32_t)wt->viewport_h;
  if (bot > vh) {
    bot = vh;
  }
  if (bot > top) {
    stats->covered_rows += (uint32_t)(bot - top);
  }
}

/**
 * @brief Fetch, composite and release one visible band; update stats.
 * @details Gets the band from the tile cache (decode-on-miss reads the JOF
 *          tile), computes `dst_y = band_top - scroll_y`, blits it through the
 *          sink, accumulates coverage and releases the pin. A cache/decode miss
 *          is recorded as a skip and the frame continues (returns k_ra8_ok); a
 *          blit-sink or cache-release failure is fatal and propagates.
 * @param[in,out] wt    Opened strip.
 * @param[in]     band  Visible band index to draw.
 * @param[in]     dst_x Destination left (viewport-centred column origin).
 * @param[in,out] stats Frame accounting (bands_drawn / skipped / covered_rows).
 * @return Result code.
 * @retval k_ra8_ok The band drew, or a miss was recorded as a skip.
 * @retval other    The blit sink or the cache release failed (fatal).
 * @pre @p wt was opened by ::longstrip_open.
 * @pre @p stats is a live per-frame accumulator.
 * @post The band was blitted at most once and no pin remains held.
 * @post Exactly one of `bands_drawn` / `skipped` advanced for this band.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_draw_band(longstrip_t* wt, uint16_t band, int32_t dst_x, longstrip_render_stats_t* stats)
{
  ra8_tile_key_t key = {};
  key.image_id       = wt->image_id;
  key.tile_x         = (uint16_t)k_wt_tile_x;
  key.tile_y         = band;
  key.zoom           = (uint16_t)k_wt_zoom;
  ra8_tile_t tile    = {};
  if (ra8_tile_cache_get(wt->cache, &key, &tile) != k_ra8_ok) {
    stats->skipped++; /* record a gap; the frame still completes */
    return k_ra8_ok;
  }
  const int32_t   dst_y = (int32_t)((uint32_t)band * (uint32_t)wt->band_h) - wt->scroll_y;
  const ra8_err_t berr =
    wt->blit(wt->blit_ctx, tile.pixels, tile.width, tile.height, wt->info.bpp, dst_x, dst_y);
  if (berr == k_ra8_ok) {
    stats->bands_drawn++;
    internal_accumulate_coverage(wt, dst_y, tile.height, stats);
  } else {
    stats->skipped++;
  }
  const ra8_err_t perr = ra8_tile_cache_put(wt->cache, tile.pixels);
  return (berr != k_ra8_ok) ? berr : perr;
}

ra8_err_t longstrip_render(longstrip_t* wt, longstrip_render_stats_t* stats)
{
  RA8_CHECK_NULL_PTR(wt, s_tag, "wt must not be nullptr");
  longstrip_render_stats_t local = {};
  wt->scroll_y                   = longstrip_clamp_scroll(wt, wt->scroll_y);
  uint16_t  first                = 0;
  uint16_t  last                 = 0;
  ra8_err_t err                  = longstrip_visible_bands(wt, wt->scroll_y, &first, &last);
  if (err != k_ra8_ok) {
    return err;
  }
  const int32_t dst_x = ((int32_t)wt->viewport_w - (int32_t)wt->canvas_w) / 2;
  for (uint32_t b = (uint32_t)first; b <= (uint32_t)last; b++) { /* bounded by band_count */
    err = internal_draw_band(wt, (uint16_t)b, dst_x, &local);
    if (err != k_ra8_ok) {
      break;
    }
  }
  if (stats != nullptr) {
    *stats = local;
  }
  return err;
}
