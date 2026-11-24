/**
 * @file longstrip.h
 * @brief Continuous vertical-scroll (longstrip / manhwa) reading mode over an
 *        JOF band-tile atlas.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The third e-reader reading mode, beside reflowable EPUB text (`reflow`)
 * and paged CBZ/manga (`comic`): a chapter is one **continuous vertical
 * strip** -- a sequence of tall image slices stacked seamlessly and read by
 * scrolling, with no page boundaries. On RA8D2/RA8P1 there is no hardware
 * JPEG decoder and a single slice can decode to tens of megabytes, so the
 * strip is normalised at import into the shared **JOF band-tile atlas**
 * (`jof.h`): a longstrip band is simply a JOF tile the full image
 * width (`tile_w == width`, one tile column), so the JOF tile index **is**
 * the band index -- byte offset + exact height per band -- giving O(1) random
 * access to any scroll position with a single bounded read + decode. No
 * parallel format is invented; this module is a thin scroll/geometry engine
 * over JOF, paged through `ra8_tile_cache`.
 *
 * ## What this module owns
 *   - **Virtual-canvas geometry.** Width `W == info.width`, height
 *     `H == info.height`, uniform band height `band_h == info.tile_h`. The
 *     scroll position is a single scalar `scroll_y`. `y -> band` is one
 *     division (`y / band_h`) -- O(1), no cumulative-height scan, because
 *     JOF bands are fixed-height by construction.
 *   - **Scroll state machine + fling.** `scroll_by` for direct drags,
 *     `fling` + `tick` for momentum scrolling with integer deceleration, all
 *     clamped to `[0, H - viewport_h]`. No page snapping.
 *   - **Bounded directional prefetch.** `prefetch` warms the cache with the
 *     bands just beyond the visible window in the current scroll direction,
 *     depth-capped; eviction is the tile cache's own pinned-skip LRU. Constant
 *     resident memory regardless of strip height or fling distance.
 *   - **Composite render.** `render` blits each visible band's on-screen
 *     sub-window to the framebuffer through a caller-supplied blit seam (the
 *     production path binds `ra8_gfx_blit` / `ra8_drw_blit_textured_rect`;
 *     tests bind a recording blit). Bands are contiguous, so a fully-drawn
 *     visible range has zero seams and zero gaps.
 *
 * ## Zero post-init allocation (NASA P10 Rule 3)
 * The engine allocates nothing. Band pixels live in the caller's tile-cache
 * cells (SDRAM); the atlas bytes live behind the caller's `pread` seam; the
 * engine state is one caller-owned ::longstrip_t. The tile cache bounds the
 * resident decoded-pixel set.
 *
 * ## Untrusted content (fail-closed)
 * The atlas arrives from untrusted EPUB/CBZ content. `longstrip_open()`
 * validates the geometry through `jof_parse()` and additionally
 * rejects any atlas that is not a single full-width band column, so the O(1)
 * band math can never index outside the grid.
 *
 *
 * @see jof.h    The band-tile atlas format + reader this rides on.
 * @see ra8_tile_cache.h   The LRU decode-on-miss band cache.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "jof.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"

/**
 * @enum longstrip_limits_t
 * @brief Fixed engine bounds (NASA P10 Rule 2: every loop provably bounded).
 *
 * @details `k_longstrip_max_visible_bands` caps the visible-band loop: a
 *          viewport can straddle at most `ceil(viewport_h / 1) + 1` bands in
 *          the degenerate `band_h == 1` case, but a longstrip band is hundreds
 *          of pixels tall, so this is a comfortable ceiling that also bounds
 *          the render loop independent of the untrusted band height.
 *          `k_longstrip_max_prefetch` caps the prefetch depth per call.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_longstrip_max_visible_bands = 4096U, /**< Visible + render loop bound. */
  k_longstrip_max_prefetch      = 8U,    /**< Max prefetch depth per call. */
} longstrip_limits_t;

/**
 * @typedef longstrip_blit_fn
 * @brief Composite one decoded band sub-window onto the display (DIP seam).
 *
 * @details The engine computes each visible band's destination top-left
 *          (`dst_y` may be negative when the band is partly scrolled off the
 *          top) and hands the caller the whole decoded band; the sink clips to
 *          the framebuffer. Production binds `ra8_gfx_blit` (software) or
 *          `ra8_drw_blit_textured_rect` (DRW-accelerated, zero-copy from
 *          SDRAM); tests bind a recording sink to prove coverage.
 *
 * @param[in] ctx    Sink context (`blit_ctx` from the config).
 * @param[in] pixels Decoded band pixels, tightly packed row-major.
 * @param[in] src_w  Band width, pixels (== canvas width).
 * @param[in] src_h  Band height, pixels (clamped for the last band).
 * @param[in] bpp    Bytes per pixel (1 gray8 / 3 RGB888 / 4 RGBA8888).
 * @param[in] dst_x  Destination left in the framebuffer (may be negative).
 * @param[in] dst_y  Destination top in the framebuffer (may be negative).
 * @return k_ra8_ok on success; any error aborts the render with that code.
 * @since 0.1.0
 */
typedef ra8_err_t (*longstrip_blit_fn)(void*          ctx,
                                       const uint8_t* pixels,
                                       uint16_t       src_w,
                                       uint16_t       src_h,
                                       uint8_t        bpp,
                                       int32_t        dst_x,
                                       int32_t        dst_y);

/**
 * @struct longstrip_decode_ctx_t
 * @brief Context binding a JOF atlas to a ::ra8_tile_cache decode-on-miss.
 *
 * @details Bind ::longstrip_tile_decode as the tile cache's `decode` and a
 *          pointer to this struct as its `decode_ctx`. The cache then pages any
 *          band in with one bounded JOF tile read. `scratch` is the deflate
 *          staging buffer required only for `k_jof_codec_deflate`
 *          atlases (size it with `jof_stored_bound()` over the band
 *          payload); a raw-codec atlas may leave it NULL / zero.
 *
 * @invariant `pread` serves the same atlas `info` was parsed from.
 * @see longstrip_tile_decode()
 * @since 0.1.0
 */
typedef struct {
  jof_pread_fn pread;       /**< Atlas backing read seam.        */
  void*        pread_ctx;   /**< Context for `pread`.            */
  jof_info_t   info;        /**< Parsed atlas geometry.          */
  uint8_t*     scratch;     /**< Deflate staging (codec 1 only). */
  uint32_t     scratch_cap; /**< Capacity of `scratch`.          */
} longstrip_decode_ctx_t;

/**
 * @struct longstrip_cfg_t
 * @brief One-shot configuration handed to ::longstrip_open.
 *
 * @details The atlas is reached through `pread`/`pread_ctx`/`atlas_size`
 *          (same seam ::jof_parse uses). `cache` is a tile cache the
 *          caller has already initialised with ::longstrip_tile_decode over
 *          the SAME atlas; `image_id` is the cache key namespace for this
 *          strip. The viewport is the on-screen window in pixels; `blit`
 *          composites bands into it.
 *
 * @invariant `viewport_w` and `viewport_h` are non-zero.
 * @see longstrip_open()
 * @since 0.1.0
 */
typedef struct {
  jof_pread_fn      pread;      /**< Atlas backing read seam.            */
  void*             pread_ctx;  /**< Context for `pread`.                */
  uint64_t          atlas_size; /**< Atlas byte length (for parse).      */
  ra8_tile_cache_t* cache;      /**< Band cache (decode-on-miss = JOF).  */
  uint32_t          image_id;   /**< Tile-cache key namespace for strip. */
  uint16_t          viewport_w; /**< On-screen viewport width, pixels.   */
  uint16_t          viewport_h; /**< On-screen viewport height, pixels.  */
  longstrip_blit_fn blit;       /**< Band composite sink.                */
  void*             blit_ctx;   /**< Context for `blit`.                 */
} longstrip_cfg_t;

/**
 * @struct longstrip_t
 * @brief Opened longstrip-strip scroll state (caller-owned; treat as private).
 *
 * @details Filled by ::longstrip_open; the geometry fields are read-only
 *          afterwards while `scroll_y`/`velocity` evolve through the scroll
 *          API. `max_scroll == max(0, canvas_h - viewport_h)` is the clamp
 *          ceiling.
 *
 * @invariant `0 <= scroll_y <= max_scroll` after every public call.
 * @invariant `band_count == info.tile_rows` and `band_h == info.tile_h`.
 * @since 0.1.0
 */
typedef struct {
  jof_info_t        info;       /**< Parsed atlas geometry.                 */
  uint32_t          canvas_w;   /**< Strip width, pixels (== info.width).   */
  uint32_t          canvas_h;   /**< Strip height, pixels (== info.height). */
  uint16_t          band_h;     /**< Band height, pixels (== info.tile_h).  */
  uint16_t          band_count; /**< Bands (== info.tile_rows).             */
  uint16_t          viewport_w; /**< Viewport width, pixels.                */
  uint16_t          viewport_h; /**< Viewport height, pixels.               */
  int32_t           scroll_y;   /**< Viewport top on the canvas, clamped.   */
  int32_t           max_scroll; /**< Clamp ceiling (>= 0).                  */
  int32_t           velocity;   /**< Fling velocity, px/tick (signed).      */
  ra8_tile_cache_t* cache;      /**< Band cache.                            */
  uint32_t          image_id;   /**< Tile-cache key namespace.              */
  longstrip_blit_fn blit;       /**< Band composite sink.                   */
  void*             blit_ctx;   /**< Context for `blit`.                    */
} longstrip_t;

/**
 * @struct longstrip_render_stats_t
 * @brief Per-frame render accounting returned by ::longstrip_render.
 *
 * @details `bands_drawn` is how many visible bands composited this frame;
 *          `skipped` is how many visible bands FAILED to composite (a cache /
 *          decode error) -- the seamless-scroll contract requires `skipped`
 *          to stay zero. `covered_rows` is the number of viewport pixel rows
 *          the drawn bands cover; when the strip fills the viewport it equals
 *          `viewport_h`, proving there is no gap band.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t bands_drawn;  /**< Visible bands composited this frame.      */
  uint16_t skipped;      /**< Visible bands that failed to composite.   */
  uint32_t covered_rows; /**< Viewport rows covered by the drawn bands. */
} longstrip_render_stats_t;

/**
 * @brief JOF-backed ::ra8_tile_decode_fn: page one band in on a cache miss.
 *
 * @details Adapts ::ra8_tile_cache's decode-on-miss to `jof_read_tile`:
 *          the tile key's `(tile_x, tile_y)` select the band (`tile_x` is
 *          always 0 for a longstrip column), and the tile is read + decoded into
 *          the cache cell in bounded RAM. Bind this as the cache's `decode`
 *          with a ::longstrip_decode_ctx_t as `decode_ctx`.
 *
 * @param[in]  ctx        A ::longstrip_decode_ctx_t*.
 * @param[in]  key        Band to decode (`tile_y` = band index).
 * @param[out] cell       Destination cell pixels.
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Decoded band width, pixels.
 * @param[out] out_h      Decoded band height, pixels (clamped for last band).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Band decoded into @p cell.
 * @retval k_ra8_err_null_ptr     @p ctx, @p key, @p cell, @p out_w or @p out_h
 *                                is NULL.
 * @retval other                  Propagated from `jof_read_tile()`.
 *
 * @pre @p ctx->info was parsed over @p ctx->pread's atlas.
 * @pre @p cell holds @p cell_bytes writable bytes.
 * @post On success `(*out_w) * (*out_h) * info.bpp` cell bytes are valid.
 * @post On any error the cell contents are unspecified.
 * @note Not thread-safe (shares the decode context's scratch).
 * @see longstrip_open()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_tile_decode(void*                 ctx,
                                              const ra8_tile_key_t* key,
                                              uint8_t*              cell,
                                              uint32_t              cell_bytes,
                                              uint16_t*             out_w,
                                              uint16_t*             out_h);

/**
 * @brief Open a longstrip strip over a parsed + validated JOF atlas.
 *
 * @details
 * Parses the atlas through `jof_parse()` and then fail-closed
 * rejects anything that is not a single full-width band column
 * (`tile_w == width` and `tile_cols == 1`), because the O(1) `y -> band`
 * math relies on one band per row. On success the virtual-canvas geometry is
 * cached and `scroll_y`/`velocity` are zeroed (top of strip, at rest).
 *
 * @param[out] wt  Engine state to populate (need not be pre-zeroed).
 * @param[in]  cfg Configuration (see the struct contract).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Strip opened; positioned at the top.
 * @retval k_ra8_err_null_ptr          @p wt, @p cfg, or a required @p cfg seam
 *                                     (`pread`, `cache`, `blit`) is NULL.
 * @retval k_ra8_err_invalid_arg       `viewport_w` or `viewport_h` is zero.
 * @retval k_ra8_err_not_supported     Atlas is not a full-width band column.
 * @retval other                       Propagated from `jof_parse()`.
 *
 * @pre @p cfg->cache was initialised with ::longstrip_tile_decode over the
 *      SAME atlas @p cfg->pread serves.
 * @pre @p cfg->pread serves `[0, cfg->atlas_size)` of the atlas.
 * @post On success `*wt` satisfies the documented invariants.
 * @post On any error `*wt` must not be used.
 * @note Not thread-safe.
 * @see longstrip_render()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_open(longstrip_t* wt, const longstrip_cfg_t* cfg);

/**
 * @brief Resize the viewport of an open strip and re-derive the scroll clamp.
 *
 * @details
 * `max_scroll` is a function of the viewport height (`canvas_h - viewport_h`),
 * so a viewport that changes after open -- a resized window, or a paginated
 * caller rendering a short final page -- leaves the clamp stale. A stale clamp
 * is not a cosmetic problem: it silently pins `scroll_y` short of the position
 * the caller asked for, and the strip then composites a window it has already
 * shown. This entry point recomputes the clamp and re-applies it to the current
 * `scroll_y` so the invariant `0 <= scroll_y <= max_scroll` still holds.
 *
 * A paginated caller must set the viewport to the height of the page it is
 * about to draw. For the final page of a strip whose height is not a whole
 * multiple of the page height, that content height is exactly
 * `canvas_h - page_index * page_height`, which makes the requested scroll
 * position land exactly on the recomputed `max_scroll` instead of being
 * clamped backwards into the previous page.
 *
 * @param[in,out] wt         Opened strip to resize (non-NULL).
 * @param[in]     viewport_w New viewport width, pixels (non-zero).
 * @param[in]     viewport_h New viewport height, pixels (non-zero).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Viewport applied; `max_scroll` re-derived.
 * @retval k_ra8_err_null_ptr    @p wt is NULL.
 * @retval k_ra8_err_invalid_arg @p viewport_w or @p viewport_h is zero.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre The caller passes the viewport it is about to composite into.
 * @post `wt->max_scroll == max(0, canvas_h - viewport_h)`.
 * @post `0 <= wt->scroll_y <= wt->max_scroll`.
 *
 * @par Example:
 * @code
 * // Paginated draw: page `p` of `page_h`-tall pages over an H-tall strip.
 * const uint16_t content_h = (uint16_t)((H - (p * page_h) < page_h)
 *                                         ? (H - (p * page_h)) : page_h);
 * (void)longstrip_set_viewport(&strip, strip_w, content_h);
 * @endcode
 *
 * @note Not thread-safe (mutates @p wt).
 * @see longstrip_clamp_scroll()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
longstrip_set_viewport(longstrip_t* wt, uint16_t viewport_w, uint16_t viewport_h);

/**
 * @brief Clamp a candidate scroll position to the strip's legal range.
 *
 * @param[in] wt Opened strip.
 * @param[in] y  Candidate viewport-top position, pixels (may be out of range).
 *
 * @return The clamped position in `[0, wt->max_scroll]`.
 * @retval 0             @p y was at/below the top, or the strip fits fully.
 * @retval wt->max_scroll @p y was at/beyond the bottom.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre `wt->max_scroll >= 0` (guaranteed by open).
 * @post The result is in `[0, wt->max_scroll]`.
 * @post No engine state is modified.
 * @note Thread-safe (pure over its inputs). Returns 0 on a NULL @p wt.
 * @since 0.1.0
 */
[[nodiscard]] int32_t longstrip_clamp_scroll(const longstrip_t* wt, int32_t y);

/**
 * @brief Report the band index containing canvas row @p y.
 *
 * @param[in]  wt       Opened strip.
 * @param[in]  y        Canvas row, pixels.
 * @param[out] out_band Receives the band index in `[0, band_count)`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Band reported.
 * @retval k_ra8_err_null_ptr     @p wt or @p out_band is NULL.
 * @retval k_ra8_err_out_of_range @p y is at/beyond `canvas_h`.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre @p out_band is writable.
 * @post On success `*out_band < wt->band_count`.
 * @post On any error `*out_band` is unmodified.
 * @note Thread-safe (pure). O(1): a single division.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_band_at_y(const longstrip_t* wt, uint32_t y, uint16_t* out_band);

/**
 * @brief Compute the inclusive visible band range for a scroll position.
 *
 * @details The viewport `[scroll_y, scroll_y + viewport_h)` intersects bands
 *          `first = scroll_y / band_h` through `last = (bottom - 1) / band_h`,
 *          with `last` clamped to the final band. @p scroll_y is clamped
 *          internally, so an out-of-range value yields the nearest legal range.
 *
 * @param[in]  wt        Opened strip.
 * @param[in]  scroll_y  Viewport-top position, pixels.
 * @param[out] out_first Receives the first visible band index.
 * @param[out] out_last  Receives the last visible band index (>= first).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Range reported.
 * @retval k_ra8_err_null_ptr @p wt, @p out_first or @p out_last is NULL.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre @p out_first and @p out_last are writable.
 * @post `*out_first <= *out_last < wt->band_count`.
 * @post No engine state is modified.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_visible_bands(const longstrip_t* wt,
                                                int32_t            scroll_y,
                                                uint16_t*          out_first,
                                                uint16_t*          out_last);

/**
 * @brief Scroll by a signed pixel delta (a finger drag), clamping at the ends.
 *
 * @details Adds @p delta to `scroll_y` and clamps. When the clamp pins the
 *          position at an end, `velocity` is zeroed so a fling that ran into
 *          the boundary comes to rest instead of pushing further.
 *
 * @param[in,out] wt    Opened strip.
 * @param[in]     delta Signed pixel delta (down positive, up negative).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Position updated.
 * @retval k_ra8_err_null_ptr @p wt is NULL.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre The engine invariant `0 <= scroll_y <= max_scroll` held on entry.
 * @post `0 <= wt->scroll_y <= wt->max_scroll`.
 * @post `velocity` is 0 if the new position pinned at an end.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_scroll_by(longstrip_t* wt, int32_t delta);

/**
 * @brief Start a momentum fling with initial velocity @p v0.
 *
 * @param[in,out] wt Opened strip.
 * @param[in]     v0 Initial velocity, px/tick (down positive, up negative).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Fling armed.
 * @retval k_ra8_err_null_ptr @p wt is NULL.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre A caller ticks the engine to animate the fling.
 * @post `wt->velocity == v0`.
 * @post `wt->scroll_y` is unchanged until the first ::longstrip_tick.
 * @note Not thread-safe.
 * @see longstrip_tick()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_fling(longstrip_t* wt, int32_t v0);

/**
 * @brief Advance the fling one physics step (integer velocity + friction).
 *
 * @details Applies the current velocity to `scroll_y` (clamped), then decays
 *          `|velocity|` by one friction step toward zero. Motion stops when
 *          the velocity reaches zero OR the position pins against the end it is
 *          travelling toward. Call once per display tick until it returns
 *          false.
 *
 * @param[in,out] wt Opened strip.
 *
 * @return true while the strip is still moving; false once it has come to rest
 *         (also false for a NULL @p wt).
 * @retval true  The fling advanced and still has velocity to run.
 * @retval false The fling came to rest this tick, or @p wt is NULL.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre A prior ::longstrip_fling (or scroll) set the velocity.
 * @post `0 <= wt->scroll_y <= wt->max_scroll`.
 * @post `|velocity|` did not increase.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool longstrip_tick(longstrip_t* wt);

/**
 * @brief Warm the cache with bands just beyond the viewport, in scroll order.
 *
 * @details Directional decode-ahead: with a non-negative `velocity` (or at
 *          rest) it warms up to @p depth bands below the visible range; with a
 *          negative velocity it warms above it. Each band is fetched and
 *          immediately released, so it becomes resident + MRU without holding a
 *          pin -- the tile cache's LRU then bounds residency and reclaims stale
 *          prefetches. A reversal simply prefetches the other side on the next
 *          call, so stale-direction bands age out naturally.
 *
 * @param[in,out] wt    Opened strip.
 * @param[in]     depth Bands to warm ahead (clamped to
 *                      ::k_longstrip_max_prefetch).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Prefetch attempted (misses that fail to decode are
 *                            skipped, not fatal -- the visible render still
 *                            decodes on demand).
 * @retval k_ra8_err_null_ptr @p wt is NULL.
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre @p wt->cache has at least one unpinned cell to spare.
 * @post No engine geometry or scroll state changed.
 * @post At most `min(depth, k_longstrip_max_prefetch)` bands were touched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_prefetch(longstrip_t* wt, uint16_t depth);

/**
 * @brief Composite every visible band at the current scroll position.
 *
 * @details For each band in the visible range, fetches it from the cache
 *          (decode-on-miss reads the JOF tile), computes its destination
 *          `(dst_x, dst_y)` -- `dst_x` centres the column in the viewport,
 *          `dst_y = band_top - scroll_y` (negative for the partly-scrolled top
 *          band) -- blits it through the sink, then releases the pin. Because
 *          bands are contiguous and the whole visible range is drawn, the
 *          result is seamless with no gap; `stats->skipped` counts any band
 *          that failed to composite and must be zero for a clean frame.
 *
 * @param[in,out] wt    Opened strip.
 * @param[out]    stats Receives per-frame accounting (may be NULL to ignore).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Frame attempted; `stats->skipped == 0` means every
 *                            visible band composited (a clean, seamless frame),
 *                            `> 0` means a band failed to page in.
 * @retval k_ra8_err_null_ptr @p wt is NULL.
 * @retval other              Propagated from the blit sink or the cache release
 *                            (a genuine I/O failure aborts the frame).
 *
 * @pre @p wt was opened by ::longstrip_open.
 * @pre The blit sink clips to the framebuffer.
 * @post Each visible band was blitted at most once; all pins are released.
 * @post `stats->covered_rows == min(viewport_h, canvas_h - scroll_y)` when
 *       `stats->skipped == 0`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_render(longstrip_t* wt, longstrip_render_stats_t* stats);

/**
 * @struct longstrip_clip_rect_t
 * @brief Viewport-clipped band or tile rectangle and source offsets.
 *
 * @invariant When @c visible is true, @c width > 0 and @c height > 0.
 * @invariant @c dst_x >= 0 and @c dst_y >= 0 when @c visible is true.
 */
typedef struct {
  uint16_t src_x;   /**< Clipped source X offset in texels.           */
  uint16_t src_y;   /**< Clipped source Y offset in texels.           */
  int32_t  dst_x;   /**< Clipped viewport X coordinate.               */
  int32_t  dst_y;   /**< Clipped viewport Y coordinate.               */
  uint16_t width;   /**< Visible clipped width in pixels.             */
  uint16_t height;  /**< Visible clipped height in pixels.            */
  bool     visible; /**< True when clipped intersection is non-empty. */
} longstrip_clip_rect_t;

/**
 * @brief Hardware-accelerated clipped band-blit seam (e.g. RA8 DRW + MVE).
 *
 * @param[in,out] ctx    Caller-owned context pointer.
 * @param[in]     pixels Decoded source pixels.
 * @param[in]     src_w  Source band width in pixels.
 * @param[in]     src_h  Source band height in pixels.
 * @param[in]     bpp    Source bytes per pixel (1, 2, 3, or 4).
 * @param[in]     dst_x  Unclipped viewport X coordinate.
 * @param[in]     dst_y  Unclipped viewport Y coordinate.
 * @param[in]     view_w Viewport width in pixels.
 * @param[in]     view_h Viewport height in pixels.
 * @return ``ra8_err_t`` status code.
 */
typedef ra8_err_t (*longstrip_hw_blit_fn)(void*          ctx,
                                          const uint8_t* pixels,
                                          uint16_t       src_w,
                                          uint16_t       src_h,
                                          uint8_t        bpp,
                                          int32_t        dst_x,
                                          int32_t        dst_y,
                                          uint16_t       view_w,
                                          uint16_t       view_h);

/**
 * @brief Hardware-accelerated solid rectangle fill seam (e.g. DRW fill_rect).
 *
 * @param[in,out] ctx      Caller-owned context pointer.
 * @param[in]     x        Clipped viewport X coordinate.
 * @param[in]     y        Clipped viewport Y coordinate.
 * @param[in]     w        Clipped rectangle width in pixels.
 * @param[in]     h        Clipped rectangle height in pixels.
 * @param[in]     argb8888 Solid fill color in 0xAARRGGBB format.
 * @return ``ra8_err_t`` status code.
 */
typedef ra8_err_t (*longstrip_hw_fill_fn)(void*    ctx,
                                          int32_t  x,
                                          int32_t  y,
                                          uint16_t w,
                                          uint16_t h,
                                          uint32_t argb8888);

/**
 * @struct longstrip_hw_ops_t
 * @brief Optional hardware acceleration callbacks bound behind the compositor.
 */
typedef struct {
  longstrip_hw_blit_fn blit_band; /**< Hardware band-blit backend (required if active). */
  longstrip_hw_fill_fn fill_rect; /**< Optional hardware solid rect backend.            */
  void*                ctx;       /**< Caller context passed to hardware callbacks.     */
} longstrip_hw_ops_t;

/**
 * @struct longstrip_target_t
 * @brief Compositor render target binding HW acceleration and SW RGB565 parity fallback.
 *
 * @details
 * When ``prefer_hw`` is true and ``hw.blit_band`` is non-null, calls dispatch
 * through the hardware callbacks (e.g. ``ra8_drw_mve_blit_band`` and
 * ``ra8_drw_fill_rect``). Otherwise ``longstrip`` composites bit-exact native
 * RGB565 into @c fb565 so host tools and target silicon agree pixel-for-pixel.
 */
typedef struct {
  uint16_t*          fb565;       /**< RGB565 destination buffer (nullable if HW-only). */
  uint16_t           fb_pitch_px; /**< Framebuffer pitch in pixels.                     */
  uint16_t           view_w;      /**< Viewport width in pixels.                        */
  uint16_t           view_h;      /**< Viewport height in pixels.                       */
  int32_t            x_off;       /**< Horizontal offset added to band @c dst_x.        */
  bool               prefer_hw;   /**< Route through @c hw when wired.                  */
  longstrip_hw_ops_t hw;          /**< Optional hardware acceleration backend.          */
} longstrip_target_t;

/**
 * @brief Compute the viewport-clipped sub-rectangle for a band/tile placement.
 *
 * @param[in]  dst_x  Signed viewport X coordinate.
 * @param[in]  dst_y  Signed viewport Y coordinate.
 * @param[in]  src_w  Source band width in pixels (> 0).
 * @param[in]  src_h  Source band height in pixels (> 0).
 * @param[in]  view_w Viewport width in pixels (> 0).
 * @param[in]  view_h Viewport height in pixels (> 0).
 * @param[out] out    Receives clipped coordinates and visibility flag.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Clipped result populated in @p out.
 * @retval k_ra8_err_null_ptr   @p out was ``nullptr``.
 * @retval k_ra8_err_invalid_arg Zero dimension supplied.
 *
 * @pre @p out is non-null.
 * @pre Dimensions are strictly positive.
 * @post When ``out->visible`` is true, ``out->dst_x + out->width <= view_w``.
 * @post When ``out->visible`` is false, ``out->width == 0`` and ``out->height == 0``.
 *
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_clip_band(int32_t                dst_x,
                                            int32_t                dst_y,
                                            uint16_t               src_w,
                                            uint16_t               src_h,
                                            uint16_t               view_w,
                                            uint16_t               view_h,
                                            longstrip_clip_rect_t* out);

/**
 * @brief Software-composite a packed band window directly into an RGB565 buffer.
 *
 * @details
 * Supports @p bpp 1 (Gray8), 2 (LE RGB565), 3 (RGB888), and 4 (RGBA8888),
 * matching the hardware DRW + MVE RGB565 packing formula bit-for-bit.
 *
 * @param[out] fb565       Destination RGB565 framebuffer.
 * @param[in]  fb_pitch_px Destination stride in pixels (>= @p view_w).
 * @param[in]  view_w      Viewport width in pixels.
 * @param[in]  view_h      Viewport height in pixels.
 * @param[in]  pixels      Source pixel buffer.
 * @param[in]  src_w       Source width in pixels.
 * @param[in]  src_h       Source height in pixels.
 * @param[in]  bpp         Source bytes per pixel (1, 2, 3, or 4).
 * @param[in]  dst_x       Signed destination X.
 * @param[in]  dst_y       Signed destination Y.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Blitted (or completely outside viewport).
 * @retval k_ra8_err_null_ptr    @p fb565 or @p pixels was ``nullptr``.
 * @retval k_ra8_err_invalid_arg Invalid dimensions, @p bpp, or pitch.
 *
 * @pre @p fb565 spans ``fb_pitch_px * view_h`` halfwords.
 * @pre @p pixels spans ``src_w * src_h * bpp`` bytes.
 * @post Visible pixels inside ``[0..view_w) x [0..view_h)`` match RGB565 conversion.
 * @post Pixels outside the intersection are left untouched.
 *
 * @note Reentrant and thread-safe on disjoint targets.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_sw_blit_rgb565(uint16_t*      fb565,
                                                 uint16_t       fb_pitch_px,
                                                 uint16_t       view_w,
                                                 uint16_t       view_h,
                                                 const uint8_t* pixels,
                                                 uint16_t       src_w,
                                                 uint16_t       src_h,
                                                 uint8_t        bpp,
                                                 int32_t        dst_x,
                                                 int32_t        dst_y);

/**
 * @brief ``longstrip_blit_fn`` adapter routing bands through ``longstrip_target_t``.
 *
 * @param[in,out] target_ctx Pointer to configured ::longstrip_target_t.
 * @param[in]     pixels     Decoded source band pixels.
 * @param[in]     src_w      Band width in pixels.
 * @param[in]     src_h      Band height in pixels.
 * @param[in]     bpp        Bytes per source pixel.
 * @param[in]     dst_x      Signed viewport X coordinate.
 * @param[in]     dst_y      Signed viewport Y coordinate.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Dispatched cleanly.
 * @retval k_ra8_err_null_ptr @p target_ctx, @p pixels, or needed fallback FB was ``nullptr``.
 *
 * @pre @p target_ctx points to an initialized ::longstrip_target_t.
 * @pre Either @c hw.blit_band (with @c prefer_hw) or @c fb565 is populated.
 * @post Destination pixels reflect the composited band via HW or SW parity path.
 * @post Uncovered pixels remain unchanged.
 *
 * @note Not thread-safe when HW callbacks drive shared DRW hardware.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_target_blit(void*          target_ctx,
                                              const uint8_t* pixels,
                                              uint16_t       src_w,
                                              uint16_t       src_h,
                                              uint8_t        bpp,
                                              int32_t        dst_x,
                                              int32_t        dst_y);

/**
 * @brief Fill a viewport-clipped solid rectangle through ``longstrip_target_t``.
 *
 * @param[in] target   Compositor target (HW ops or SW RGB565 buffer).
 * @param[in] x        Signed destination X coordinate.
 * @param[in] y        Signed destination Y coordinate.
 * @param[in] w        Rectangle width in pixels.
 * @param[in] h        Rectangle height in pixels.
 * @param[in] argb8888 Fill color in 0xAARRGGBB format.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Filled (or clipped outside viewport).
 * @retval k_ra8_err_null_ptr    @p target or needed fallback FB was ``nullptr``.
 * @retval k_ra8_err_invalid_arg Zero dimensions or invalid pitch.
 *
 * @pre @p target is non-null.
 * @pre Either @c hw.fill_rect (with @c prefer_hw) or @c fb565 is populated.
 * @post Clipped rectangle in @p target holds @p argb8888 converted to RGB565.
 * @post Framebuffer pixels outside the rectangle are untouched.
 *
 * @note Not thread-safe when HW callbacks drive shared DRW hardware.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t longstrip_target_fill(const longstrip_target_t* target,
                                              int32_t                   x,
                                              int32_t                   y,
                                              uint16_t                  w,
                                              uint16_t                  h,
                                              uint32_t                  argb8888);

#ifdef __cplusplus
}
#endif
