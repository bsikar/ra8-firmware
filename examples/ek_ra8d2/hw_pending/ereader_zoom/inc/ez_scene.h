/**
 * @file examples/ek_ra8d2/hw_pending/ereader_zoom/inc/ez_scene.h
 * @brief Tap-to-zoom demo scene: tiled 12 MP page, zoom viewport, loupe (#478).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * The presentation model of the `ereader_zoom` app, kept out of `main.c` so the
 * host twin (`tests/test_app_ereader_zoom.c`) compiles and drives the *same*
 * code the board runs. Nothing here touches MMIO: it is `ra8_zoom` over
 * `ra8_tile_cache` over a procedural page, painting through `ra8_gfx`.
 *
 * @par What the demo proves
 * -# **No downscale.** The page is 4096x3072 -- 12 MB of gray8, which cannot be
 *    resident on a 1.6 MB-SRAM part -- and is magnified at 1:1, 2x and 4x with
 *    no resampling of the source anywhere in the path.
 * -# **Bounded residency.** The page is served through an ::ra8_tile_cache sized
 *    to the viewport tile demand plus a one-tile pan margin (::k_ez_cells), so
 *    the resident set is a screenful of tiles and never the page. The banner
 *    reports the cache's own hit/miss/eviction counters, so the claim is
 *    measured rather than asserted.
 * -# **Partial e-ink update.** Cycling the loupe's magnification changes only the
 *    lens rectangle, so ::ez_scene_present asks for a flush of that box --
 *    102400 of the content area's 565248 pixels -- instead of a full refresh.
 * -# **Pan-stable blue-noise tone.** The whole composite is integer, and the
 *    dither phase is locked to the magnified image plane, so the framebuffer
 *    hash is reproducible on host, ra8_emulator and silicon.
 *
 * @par Why the chrome carries no text
 * The status bar is filled rectangles and a block zoom indicator, not glyphs.
 * A framebuffer hash over antialiased text is toolchain-bound -- the same board
 * prints a different value from a 13.3-built and a 14.3-built image -- which
 * makes it useless as a cross-host golden. Every pixel this scene writes comes
 * from integer arithmetic, so the hash means the same thing everywhere.
 *
 * @note Not thread-safe; single-threaded reader loop only.
 * @see ra8_zoom.h
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_ui.h"
#include "ra8_zoom.h"
#include "ra8_zoom_tiles.h"

/**
 * @enum ez_page_t
 * @brief The procedural source page and its tiling.
 * @details 4096x3072 gray8 is 12 MiB -- deliberately far past what the part can
 *          hold -- so "only the visible tiles are resident" is the only way the
 *          app can work at all. Every dimension is a power of two so the sampler
 *          is shifts and masks rather than divides, which matters under the
 *          instruction-accurate emulator.
 * @invariant k_ez_page_w / k_ez_tile_edge and k_ez_page_h / k_ez_tile_edge are
 *            both exact, so no partial edge tile exists.
 * @par Example:
 * @code
 * const uint8_t sample = ez_page_sample(x, y);
 * @endcode
 * @see ez_page_sample
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ez_page_w    = 4096U, /**< Source page width, pixels.                */
  k_ez_page_h    = 3072U, /**< Source page height, pixels.               */
  k_ez_tile_edge = 256U,  /**< Tile edge, pixels (gray8 => 64 KiB/tile). */
  k_ez_image_id  = 1U,    /**< Tile-cache key image id for the page.     */
} ez_page_t;

/**
 * @enum ez_layout_t
 * @brief Panel layout: status bar, content viewport and the loupe box.
 * @details The content viewport is the panel less the status bar; the loupe is a
 *          square box centred in it. Both are compile-time constants because the
 *          tile-cache budget below is derived from them.
 * @invariant k_ez_lens_edge is smaller than the content viewport on both axes.
 * @par Example:
 * @code
 * const ra8_ui_rect_t content = ez_content_rect(fb_w, fb_h);
 * @endcode
 * @see ez_content_rect
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ez_status_h    = 48U,  /**< Status bar height, pixels.            */
  k_ez_lens_edge   = 320U, /**< Loupe box edge, pixels.               */
  k_ez_lens_border = 3U,   /**< Loupe chrome thickness, pixels.       */
  k_ez_edge_band   = 176U, /**< Pan tap-band depth at each edge.      */
  k_ez_ind_block   = 20U,  /**< Zoom-indicator block edge, pixels.    */
  k_ez_ind_gap     = 8U,   /**< Gap between indicator blocks, pixels. */
} ez_layout_t;

/**
 * @enum ez_scale_t
 * @brief The magnification ladders the page view and the loupe walk.
 * @details The page ladder tops out at 4x -- past that a 1024-wide viewport shows
 *          fewer than 256 source pixels and the reader has lost all context. The
 *          loupe starts where the page ladder ends because that is its whole
 *          purpose: a small window magnified further than the page around it.
 * @invariant k_ez_lens_scale_min >= k_ez_page_scale_max.
 * @par Example:
 * @code
 * const uint8_t next = ra8_zoom_scale_cycle(v->scale, k_ra8_zoom_scale_min, k_ez_page_scale_max);
 * @endcode
 * @see ra8_zoom_scale_cycle
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ez_page_scale_max = 4U, /**< Page-view ladder ceiling. */
  k_ez_lens_scale_min = 4U, /**< Loupe ladder floor.       */
  k_ez_lens_scale_max = 8U, /**< Loupe ladder ceiling.     */
} ez_scale_t;

/**
 * @enum ez_zone_t
 * @brief Tap zones of the demo's discrete touch scheme.
 * @details The GT911 path reports contacts, not drags, so navigation is tap
 *          bands rather than a fling -- the same scheme the sibling comic and
 *          manga demos use.
 * @invariant k_ez_zone_none is the zero value and does nothing.
 * @par Example:
 * @code
 * const ez_zone_t z = ez_zone_hit(&scene, x, y);
 * @endcode
 * @see ez_scene_tap
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ez_zone_none      = 0U, /**< Nothing under the tap.                     */
  k_ez_zone_pan_left  = 1U, /**< Left band: page travels left.              */
  k_ez_zone_pan_right = 2U, /**< Right band: page travels right.            */
  k_ez_zone_pan_up    = 3U, /**< Top band: page travels up.                 */
  k_ez_zone_pan_down  = 4U, /**< Bottom band: page travels down.            */
  k_ez_zone_zoom      = 5U, /**< Centre: cycle the page magnification.      */
  k_ez_zone_lens      = 6U, /**< Inside the loupe: cycle its magnification. */
  k_ez_zone_toggle    = 7U, /**< Status bar: open / close the loupe.        */
} ez_zone_t;

/**
 * @enum ez_cache_t
 * @brief Tile-cache budget, DERIVED from the viewport and tile geometry (#338).
 * @details At 1:1 the content viewport straddles at most ::k_ez_view_cols x
 *          ::k_ez_view_rows tiles; sizing the cache to that frame plus one tile
 *          of margin on each axis means a single pan step re-decodes only the
 *          newly exposed row or column and never a tile still on screen. The
 *          static_asserts in `ez_scene.c` fail the build if the derivation is
 *          ever undercut or grows past its SDRAM budget.
 * @invariant k_ez_cells >= k_ez_view_cols * k_ez_view_rows.
 * @par Example:
 * @code
 * static uint8_t s_cells[(size_t)k_ez_cells * (size_t)k_ez_cell_bytes];
 * @endcode
 * @see ez_scene_init
 * @since 0.1.0
 */
typedef enum : uint32_t {
  /** One gray8 tile, bytes. */
  k_ez_cell_bytes = k_ez_tile_edge * k_ez_tile_edge,
  k_ez_view_cols  = 6U, /**< 1:1 tile columns a frame straddles, plus one. */
  k_ez_view_rows  = 4U, /**< 1:1 tile rows a frame straddles, plus one.    */
  /** Cells. */
  k_ez_cells             = (k_ez_view_cols + 1U) * (k_ez_view_rows + 1U),
  k_ez_buckets           = 64U,                /**< Cache hash buckets (>= cells). */
  k_ez_cell_budget_bytes = 4U * 1024U * 1024U, /**< SDRAM the cache may claim.     */
  k_ez_prefetch_max      = 6U,                 /**< Lead-edge tiles per pan.       */
} ez_cache_t;

/**
 * @enum ez_scratch_t
 * @brief The zoom engine's entire memory footprint, in bytes.
 * @details Sized for the *widest* viewport (the content area), and shared by both
 *          views because only one renders at a time. The strip height falls out
 *          of the division: 16384 / 1024 = 16 rows for the page view, capped at
 *          ::k_ra8_zoom_strip_rows_max = 32 for the narrower loupe. This is the
 *          whole cost of the viewer -- 25 KiB of SRAM -- because the composite
 *          never holds the visible window.
 * @invariant k_ez_packed_bytes >= (k_ez_row_bytes * 16) / 2.
 * @par Example:
 * @code
 * static uint8_t s_strip[k_ez_strip_bytes];
 * @endcode
 * @see ra8_zoom_scratch_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ez_row_bytes    = 1024U,              /**< One source row at the widest viewport. */
  k_ez_strip_bytes  = 1024U * 16U,        /**< gray8 destination strip (16 rows).     */
  k_ez_packed_bytes = (1024U * 16U) / 2U, /**< The strip packed to 4 bpp.             */
} ez_scratch_t;

/**
 * @struct ez_scene_t
 * @brief The whole demo: one tiled page, one zoom viewport, one loupe.
 *
 * @details Owns no memory. The tile cache's cells, the composite scratch and the
 *          framebuffer are all supplied by `main.c` (or by the host twin), which
 *          is what lets the identical scene run on the board out of SDRAM and in
 *          a unit test out of a static array.
 *
 * @invariant `page.dst` is the content rectangle and `lens.dst` is inside it.
 * @invariant `lens` is only rendered and presented while `lens_on` is true.
 *
 * @par Example:
 * @code
 * static ez_scene_t s_scene;
 * (void)ez_scene_init(&s_scene, &cfg);
 * @endcode
 *
 * @see ez_scene_init
 * @since 0.1.0
 */
typedef struct {
  ra8_tile_cache_t    cache;    /**< Page cache (decode-on-miss = the sampler). */
  ra8_zoom_tile_src_t tiles;    /**< Tiled-source binding over that cache.      */
  ra8_zoom_source_t   src;      /**< The page as a zoom source.                 */
  ra8_zoom_view_t     page;     /**< Full-content zoom viewport.                */
  ra8_zoom_view_t     lens;     /**< Loupe over the same page, magnified more.  */
  ra8_ui_rect_t       content;  /**< The content rectangle (page.dst).          */
  const void*         fb;       /**< Framebuffer base, for the golden hash.     */
  uint32_t            fb_bytes; /**< Framebuffer size, bytes.                   */
  int32_t             fb_w;     /**< Framebuffer width, pixels.                 */
  int32_t             fb_h;     /**< Framebuffer height, pixels.                */
  bool                lens_on;  /**< The loupe is open.                         */
} ez_scene_t;

/**
 * @struct ez_scene_cfg_t
 * @brief Everything ::ez_scene_init needs: storage and framebuffer geometry.
 * @details Every pointer is borrowed and must outlive the scene. The cache
 *          arrays are the six ::ra8_tile_cache_cfg_t needs; the three scratch
 *          buffers are the composite's. The framebuffer is carried here (as
 *          well as being bound into ra8_gfx) because the scene hashes it for
 *          the golden, and ra8_gfx exposes no accessor for the binding.
 * @invariant Every pointer is non-NULL and every capacity matches its enum.
 * @par Example:
 * @code
 * const ez_scene_cfg_t cfg = { .fb_w = 1024, .fb_h = 600, .cell_mem = s_cells, ... };
 * @endcode
 * @see ez_scene_init
 * @since 0.1.0
 */
typedef struct {
  void*                fb;       /**< Framebuffer base (hashed by the golden). */
  uint32_t             fb_bytes; /**< Framebuffer size, bytes.                 */
  int32_t              fb_w;     /**< Framebuffer width, pixels.               */
  int32_t              fb_h;     /**< Framebuffer height, pixels.              */
  uint8_t*             cell_mem; /**< `k_ez_cells * k_ez_cell_bytes` bytes.    */
  ra8_keycache_cell_t* meta;     /**< `k_ez_cells` link-metadata entries.      */
  ra8_tile_key_t*      keys;     /**< `k_ez_cells` key entries.                */
  ra8_tile_dims_t*     dims;     /**< `k_ez_cells` dimension entries.          */
  int32_t*             buckets;  /**< `k_ez_buckets` hash-bucket heads.        */
  uint8_t*             row;      /**< `k_ez_row_bytes` composite row.          */
  uint8_t*             strip;    /**< `k_ez_strip_bytes` composite strip.      */
  uint8_t*             packed;   /**< `k_ez_packed_bytes` packed strip.        */
} ez_scene_cfg_t;

/**
 * @struct ez_present_t
 * @brief What the scene asks the panel to flush, and how well.
 * @details The scene's join of its two views: a page change dirties the whole
 *          content rectangle, a loupe-only change dirties just the lens box.
 *          `quality` maps to @c k_display_refresh_quality (GC16) and its
 *          negation to @c k_display_refresh_fast (A2).
 * @invariant When `present` is false the other members are unspecified.
 * @par Example:
 * @code
 * ez_present_t plan = {};
 * if ((ez_scene_present(&scene, &plan) == k_ra8_ok) && plan.present) { ez_flush(&plan); }
 * @endcode
 * @see ez_scene_present
 * @since 0.1.0
 */
typedef struct {
  ra8_ui_rect_t rect;    /**< Panel rectangle to flush.                 */
  bool          quality; /**< True: full 16-level GC16. False: fast A2. */
  bool          present; /**< False: nothing changed; do not flush.     */
} ez_present_t;

/**
 * @struct ez_selftest_t
 * @brief Deterministic boot self-check results (the app's golden numbers).
 * @details Four framebuffer hashes taken after four scripted viewport states,
 *          plus the tile cache's own counters. Because the render is pure
 *          integer these are identical on the unit-test host, in ra8_emulator
 *          and on silicon -- which is what makes them a usable golden.
 * @invariant All four hashes differ (each stage changes visible pixels).
 * @par Example:
 * @code
 * ez_selftest_t st = {};
 * (void)ez_scene_selftest(&scene, &st);
 * @endcode
 * @see ez_scene_selftest
 * @since 0.1.0
 */
typedef struct {
  uint32_t crc_1x;    /**< Hash after the opening 1:1 render.         */
  uint32_t crc_pan;   /**< Hash after one right-pan step at 1:1.      */
  uint32_t crc_2x;    /**< Hash after zooming to 2x at a fixed point. */
  uint32_t crc_lens;  /**< Hash after opening the 4x loupe.           */
  uint32_t hits;      /**< Tile-cache hits over the whole sequence.   */
  uint32_t misses;    /**< Tile-cache misses (decodes).               */
  uint32_t evictions; /**< Tile-cache evictions.                      */
  uint16_t warmed;    /**< Tiles warmed by the pan read-ahead.        */
} ez_selftest_t;

/**
 * @brief Sample the procedural full-resolution page at one pixel.
 *
 * @details A page of "text": a repeating smooth gradient (which only a dither
 *          can render on a 16-level panel without banding) overlaid with 3 px
 *          rules broken into word-like runs (which only full-resolution
 *          magnification can resolve). Every operation is a shift or a mask, so
 *          decoding a 256x256 tile is cheap enough to stay inside the emulator's
 *          boot budget.
 *
 * @param[in] x Source column, `< k_ez_page_w`.
 * @param[in] y Source row, `< k_ez_page_h`.
 *
 * @return The gray8 sample.
 * @retval "ink"        The pixel falls on an inked rule (a dark constant).
 * @retval "background" Otherwise, the gradient value at `(x, y)`.
 *
 * @pre  @p x and @p y are inside the page (the caller is the tile decoder).
 * @pre  No global state is consulted.
 * @post The result is a valid gray8 value.
 * @post No state is modified (pure function).
 *
 * @note Pure; thread-safe.
 * @see ez_tile_decode
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ez_page_sample(uint32_t x, uint32_t y);

/**
 * @brief ::ra8_tile_decode_fn that materialises one page tile from the sampler.
 *
 * @details The decode-on-miss seam. A real reader inflates a JOF tile here; the
 *          demo computes it, which keeps the app free of a multi-megabyte baked
 *          fixture while exercising the identical cache, eviction and residency
 *          behaviour.
 *
 * @param[in]  ctx        Unused (the page is a pure function).
 * @param[in]  key        The tile being decoded.
 * @param[out] cell       Destination cell, `cell_bytes` writable bytes.
 * @param[in]  cell_bytes Capacity of @p cell.
 * @param[out] out_w      Receives the decoded tile width.
 * @param[out] out_h      Receives the decoded tile height.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The tile was written.
 * @retval k_ra8_err_null_ptr     @p key, @p cell, @p out_w or @p out_h is NULL.
 * @retval k_ra8_err_out_of_range The key names a tile outside the page.
 * @retval k_ra8_err_no_mem       @p cell_bytes is smaller than one tile.
 *
 * @pre  @p cell addresses at least @p cell_bytes writable bytes.
 * @pre  The cache was configured with `cell_bytes >= k_ez_cell_bytes`.
 * @post On k_ra8_ok `*out_w == *out_h == k_ez_tile_edge`.
 * @post On any error nothing is written past the failing check.
 *
 * @note Not thread-safe (writes @p cell); otherwise pure.
 * @see ez_page_sample
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ez_tile_decode(void*                 ctx,
                                       const ra8_tile_key_t* key,
                                       uint8_t*              cell,
                                       uint32_t              cell_bytes,
                                       uint16_t*             out_w,
                                       uint16_t*             out_h);

/**
 * @brief The content rectangle: the panel less the status bar.
 *
 * @param[in] fb_w Framebuffer width, pixels.
 * @param[in] fb_h Framebuffer height, pixels.
 *
 * @return The content rectangle in framebuffer coordinates.
 * @retval "{0, k_ez_status_h, fb_w, fb_h - k_ez_status_h}" Always.
 *
 * @pre  @p fb_h exceeds ::k_ez_status_h.
 * @pre  @p fb_w is positive.
 * @post The result's height is positive.
 * @post No state is modified (pure function).
 *
 * @note Pure; thread-safe.
 * @see ez_scene_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_ui_rect_t ez_content_rect(int32_t fb_w, int32_t fb_h);

/**
 * @brief Wire the tile cache, the tiled source and both viewports.
 *
 * @param[out] s   Scene to populate.
 * @param[in]  cfg Borrowed storage and framebuffer geometry.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The scene is ready to render.
 * @retval k_ra8_err_null_ptr    @p s, @p cfg, or a required storage pointer is NULL.
 * @retval k_ra8_err_invalid_arg The framebuffer is too small for the layout.
 * @retval k_ra8_err_*           Propagated from the cache or the zoom engine.
 *
 * @pre  Every pointer in @p cfg outlives the scene.
 * @pre  ::ra8_gfx_init has bound the same framebuffer the scene will paint.
 * @post On k_ra8_ok both views are open and owe a quality flush.
 * @post On any error the scene is not left half-built (`lens_on` is false).
 *
 * @note Not thread-safe.
 * @see ez_scene_render
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ez_scene_init(ez_scene_t* s, const ez_scene_cfg_t* cfg);

/**
 * @brief Classify a tap by the zone it lands in.
 *
 * @param[in] s Initialised scene.
 * @param[in] x Framebuffer column of the tap.
 * @param[in] y Framebuffer row of the tap.
 *
 * @return The zone under the point.
 * @retval k_ez_zone_none The point is outside every zone.
 * @retval "a zone"       Otherwise, the zone that owns the point.
 *
 * @pre  @p s was initialised by ::ez_scene_init (NULL answers none).
 * @pre  The coordinates are framebuffer pixels, not page-local.
 * @post No state is modified (pure query).
 * @post The loupe zone is only returned while the loupe is open.
 *
 * @note Pure with respect to @p s; thread-safe.
 * @see ez_scene_tap
 * @since 0.1.0
 */
[[nodiscard]] ez_zone_t ez_zone_hit(const ez_scene_t* s, int32_t x, int32_t y);

/**
 * @brief Apply a tap to the scene.
 *
 * @param[in,out] s      Initialised scene.
 * @param[in]     x      Framebuffer column of the tap.
 * @param[in]     y      Framebuffer row of the tap.
 * @param[in]     now_ms Current millisecond timestamp.
 *
 * @return Whether anything changed and a redraw is due.
 * @retval true  The scene changed; re-render and present.
 * @retval false The tap hit nothing, or clamped to no movement.
 *
 * @pre  @p s was initialised by ::ez_scene_init (NULL answers false).
 * @pre  @p now_ms comes from a monotonic millisecond source.
 * @post A true return leaves at least one view owing a flush.
 * @post A false return leaves the scene byte-identical.
 *
 * @note Not thread-safe.
 * @see ez_zone_hit
 * @since 0.1.0
 */
[[nodiscard]] bool ez_scene_tap(ez_scene_t* s, int32_t x, int32_t y, uint32_t now_ms);

/**
 * @brief Repaint the content area, the loupe (when open) and the status bar.
 *
 * @param[in,out] s Initialised scene.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The framebuffer holds the current scene.
 * @retval k_ra8_err_null_ptr @p s is NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_zoom_view_render or ra8_gfx.
 *
 * @pre  ::ra8_gfx_init has bound the framebuffer.
 * @pre  @p s was initialised by ::ez_scene_init.
 * @post On k_ra8_ok every pixel of the panel has been written.
 * @post The ra8_gfx clip rectangle is reset to the whole framebuffer.
 *
 * @note Not thread-safe; writes the single ra8_gfx framebuffer binding.
 * @see ez_scene_present
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ez_scene_render(ez_scene_t* s);

/**
 * @brief Take the pending flush plan for whichever view changed.
 *
 * @details A page change dirties the whole content rectangle; a loupe-only
 *          change dirties only the lens box, which is the partial-update case
 *          this demo exists to show. A view that owes nothing contributes
 *          nothing, so an idle reader never refreshes the panel.
 *
 * @param[in,out] s   Initialised scene.
 * @param[out]    out Receives the plan; `out->present` is false when none is owed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           @p out holds the plan.
 * @retval k_ra8_err_null_ptr @p s or @p out is NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_zoom_view_present.
 *
 * @pre  The framebuffer already holds the pixels this plan describes.
 * @pre  @p s was initialised by ::ez_scene_init.
 * @post Neither view still owes the flush this call reported.
 * @post A second immediate call reports `present == false`.
 *
 * @note Not thread-safe.
 * @see ez_scene_render
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ez_scene_present(ez_scene_t* s, ez_present_t* out);

/**
 * @brief Advance both views' settle timers; report whether a repaint is due.
 *
 * @param[in,out] s      Initialised scene.
 * @param[in]     now_ms Current millisecond timestamp.
 *
 * @return Whether a full-quality repaint just became due.
 * @retval true  Re-render and present.
 * @retval false Nothing to do this tick.
 *
 * @pre  @p s was initialised by ::ez_scene_init (NULL answers false).
 * @pre  @p now_ms comes from a monotonic millisecond source.
 * @post A true return leaves a view owing a quality flush.
 * @post A false return leaves the scene unmodified.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_tick
 * @since 0.1.0
 */
bool ez_scene_tick(ez_scene_t* s, uint32_t now_ms);

/**
 * @brief Warm the tiles one pan step ahead of the page viewport.
 *
 * @param[in,out] s          Initialised scene.
 * @param[in]     dir        Direction of travel.
 * @param[out]    out_warmed Tiles warmed (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The read-ahead sweep ran.
 * @retval k_ra8_err_null_ptr @p s is NULL.
 * @retval k_ra8_err_*        Propagated from ::ra8_zoom_tiles_prefetch.
 *
 * @pre  @p s was initialised by ::ez_scene_init.
 * @pre  The caller is in an idle window (read-ahead is not free).
 * @post No on-screen tile is evicted (::k_ez_prefetch_max is the spare margin).
 * @post `*out_warmed` (when given) is at most ::k_ez_prefetch_max.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_tiles_prefetch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ez_scene_prefetch(ez_scene_t* s, ra8_zoom_pan_t dir, uint16_t* out_warmed);

/**
 * @brief Drive the scene through the scripted boot self-check.
 *
 * @details Four states -- opening 1:1, one right-pan step, 2x about a fixed
 *          panel point, and the 4x loupe -- each rendered and hashed. This is
 *          the app's golden: `tests/test_app_ereader_zoom.c` calls this same
 *          function over a host framebuffer and asserts the identical numbers,
 *          and `hil.conf` pins them for the headless emulator gate.
 *
 * @param[in,out] s   Initialised scene (left in the loupe state on return).
 * @param[out]    out Receives the four hashes and the cache counters.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The sequence ran and @p out is populated.
 * @retval k_ra8_err_null_ptr @p s or @p out is NULL.
 * @retval k_ra8_err_*        Propagated from a render or a viewport call.
 *
 * @pre  ::ra8_gfx_init has bound the framebuffer the hashes cover.
 * @pre  @p s was freshly initialised (the counters start from zero).
 * @post On k_ra8_ok the framebuffer holds the final (loupe) state.
 * @post On k_ra8_ok every counter in @p out reflects the whole sequence.
 *
 * @note Not thread-safe.
 * @see ez_fnv1a
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ez_scene_selftest(ez_scene_t* s, ez_selftest_t* out);

/**
 * @brief FNV-1a-32 over a byte range (the framebuffer hash).
 *
 * @param[in] buf Bytes to hash (NULL hashes nothing).
 * @param[in] len Byte count.
 *
 * @return The FNV-1a-32 digest.
 * @retval 2166136261 @p buf is NULL or @p len is 0 (the FNV offset basis).
 * @retval "digest"   Otherwise, the hash of the range.
 *
 * @pre  @p buf addresses at least @p len readable bytes.
 * @pre  The range is deterministic (integer-rendered pixels only).
 * @post No state is modified (pure function).
 * @post The result depends only on the bytes, not on their address.
 *
 * @note Pure; thread-safe.
 * @see ez_scene_selftest
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ez_fnv1a(const void* buf, uint32_t len);
