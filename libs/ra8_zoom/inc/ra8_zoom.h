/**
 * @file ra8_zoom.h
 * @brief Tap-to-zoom image viewer: viewport state machine + tiled magnifying render (#478).
 * @ingroup grp_ereader
 *
 * @details
 * The reader half of the "keep the pixels, magnify into them" decision: `.rabook`
 * import deliberately does NOT downscale (#210-213), so the reader must be able to
 * magnify a retained full-resolution figure or manga panel instead. This module is
 * that magnifier -- one viewport engine serving both presentations the reader needs:
 *
 *  - **full-screen zoom**, where @ref ra8_zoom_view_t::dst is the whole panel and a
 *    tapped EPUB figure or comic page opens into it; and
 *  - **loupe / lens**, where @c dst is a small box composited over the page. Only the
 *    lens rectangle changes, so dragging it costs one small partial e-ink update
 *    instead of a full-panel refresh.
 *
 * Both are the same code; the mode is entirely a matter of how big @c dst is.
 *
 * @par The source seam (Dependency Inversion)
 * The engine never knows what it is magnifying. It pulls through one function
 * pointer, @ref ra8_zoom_read_fn -- "give me this source rectangle as gray8" -- so a
 * `.rabook` image pool (@ref ra8_zoom_book.h), a JOF atlas paged through an
 * `ra8_tile_cache` (@ref ra8_zoom_tiles.h), and a unit-test pattern generator are
 * interchangeable behind it (Liskov). gray8 is the one depth every retained source
 * can produce losslessly: gray4 quantisation is not reversible, so re-dithering must
 * start from continuous tone.
 *
 * @par Coordinate model -- the magnified plane
 * The viewport anchor is NOT a source pixel. It is a coordinate in the *magnified
 * image plane*: the source scaled by @ref ra8_zoom_view_t::scale, so the plane is
 * `width * scale` by `height * scale` and destination column @c c shows plane column
 * `anchor_x + c`. Two properties fall out of that choice, and both matter:
 *
 *  1. **Pan granularity stays one destination pixel at every zoom.** An anchor held
 *     in source pixels would quantise panning to @c scale destination pixels, so a
 *     4x view could only pan in 4-pixel jumps.
 *  2. **The blue-noise dither phase is stable under pan.** The mask (#477) is indexed
 *     at plane coordinates, so a given image pixel gets the same threshold no matter
 *     where the viewport sits. Phasing on *panel* coordinates -- what
 *     ::ra8_gfx_blit_gray8_dither does, correctly, for static chrome -- would re-roll
 *     the grain on every pan step and shimmer.
 *
 * An anchor may be negative: when the image is smaller than the viewport at this
 * zoom it is centred, and the letterbox is filled with @ref k_ra8_zoom_bg_gray.
 *
 * @par Memory -- why there is no big buffer
 * A naive zoom viewer decodes the visible window and holds it: 1024x600 gray8 is
 * 600 KiB, and it cannot be malloc'd (NASA P10 Rule 3, zero dynamic allocation after
 * init). This engine never holds the window. It composites in horizontal **strips**
 * of at most @ref k_ra8_zoom_strip_rows_max destination rows, so the resident cost is
 * `O(dst.w * strip_rows)`, not `O(dst.w * dst.h)` -- about 25 KiB for a full-screen
 * 1024-wide viewport at 16 rows, small enough to sit in SRAM. Every buffer is
 * supplied by the caller in @ref ra8_zoom_scratch_t; the module allocates nothing and
 * owns nothing. The large working set stays where it already is and is already
 * budgeted: the tile cache in SDRAM.
 *
 * @par Refresh behaviour on a 16-level e-ink panel
 * A pan moves every pixel in the viewport, so there is no "small dirty region" to
 * exploit -- the viewport must be flushed whole. What can be exploited is the
 * *waveform*: under @ref k_ra8_zoom_policy_responsive an interactive burst flushes
 * with @ref k_ra8_zoom_refresh_fast (the panel's A2 waveform, ~10x faster than GC16
 * but bi-level), and @ref ra8_zoom_view_tick promotes the view to
 * @ref k_ra8_zoom_refresh_quality once the gesture has been still for
 * @ref ra8_zoom_view_t::settle_ms, repainting the same rectangle in all 16 levels.
 * @ref k_ra8_zoom_policy_quality opts out and pays full GC16 per step. The engine
 * emits the *decision* (@ref ra8_zoom_present_t) rather than calling the display PAL,
 * so it stays a pure integer module: identical on host, ra8_emulator and silicon.
 *
 * @note No entry point in this module is thread-safe or ISR-safe;
 *       ::ra8_zoom_view_render additionally writes through the single ra8_gfx
 *       framebuffer binding.
 *
 * @see ra8_zoom_book.h  Bind a `.rabook` image pool as a source.
 * @see ra8_zoom_tiles.h Bind an `ra8_tile_cache` tiled atlas as a source.
 * @see ra8_gfx_dither.h The blue-noise quantiser this render re-dithers through.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ui.h"

/**
 * @enum ra8_zoom_limits_t
 * @brief Magnification ladder bounds, strip geometry and the letterbox fill.
 *
 * @details
 * @ref k_ra8_zoom_scale_max is a *hard* ceiling on the engine, not a policy: a
 * caller narrows it further through @ref ra8_zoom_view_cfg_t::scale_max. The
 * ladder is integer-only because every magnifying primitive in this tree is
 * (nearest-neighbour replication); a fractional scale would need a resampler
 * that does not exist and would show uneven pixel blocks on a 16-level panel.
 *
 * @ref k_ra8_zoom_bg_gray is 255 (white) rather than a mid grey because 255 is
 * exactly `15 * 17` -- an on-palette level -- so the letterbox dithers to a flat
 * level 15 with no grain, which is what a page margin should look like.
 *
 * @invariant k_ra8_zoom_scale_min <= k_ra8_zoom_scale_max.
 * @invariant k_ra8_zoom_bg_gray is an exact multiple of the 16-level palette step.
 *
 * @par Example:
 * @code
 * cfg.scale     = k_ra8_zoom_scale_min;   // open at full source resolution
 * cfg.scale_max = 4U;                     // this reader stops at 4x
 * @endcode
 *
 * @see ra8_zoom_view_cfg_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_zoom_scale_min      = 1U,   /**< 1:1 -- one source pixel per panel pixel. */
  k_ra8_zoom_scale_max      = 8U,   /**< Engine ceiling on integer magnification. */
  k_ra8_zoom_strip_rows_max = 32U,  /**< Destination rows composited per strip.   */
  k_ra8_zoom_bg_gray        = 255U, /**< Letterbox fill level (on-palette white). */
} ra8_zoom_limits_t;

/**
 * @enum ra8_zoom_dim_t
 * @brief Bound on a source dimension the engine will magnify.
 *
 * @details Caps `width * k_ra8_zoom_scale_max` (the widest magnified plane) far
 *          inside `int32_t`, so every plane-coordinate expression in the engine
 *          is overflow-free by construction rather than by inspection. The JOF
 *          atlas format already caps an edge at 32768 px, so this is not a
 *          practical restriction on any retained source.
 *
 * @invariant k_ra8_zoom_dim_max * k_ra8_zoom_scale_max < INT32_MAX.
 * @see ra8_zoom_source_init
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_zoom_dim_max = 65535U, /**< Largest accepted source width or height, pixels. */
} ra8_zoom_dim_t;

/**
 * @enum ra8_zoom_timing_t
 * @brief Default gesture-settle delay, in milliseconds.
 *
 * @details The interval of input silence after which
 *          @ref ra8_zoom_view_tick promotes a fast (bi-level) view to a full
 *          16-level repaint. Long enough that a multi-step pan is not
 *          interrupted by a 450 ms GC16 mid-gesture; short enough that a
 *          reader who has stopped sees the clean image essentially at once.
 *
 * @invariant k_ra8_zoom_settle_ms_default > 0.
 * @see ra8_zoom_view_tick
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_zoom_settle_ms_default = 350U, /**< Default input-silence settle window, ms. */
} ra8_zoom_timing_t;

/**
 * @enum ra8_zoom_refresh_t
 * @brief Waveform class the next flush of the viewport should use.
 *
 * @details Deliberately *not* ::display_refresh_hint_t: this module owes the
 *          display PAL nothing and must stay linkable into a host unit test
 *          with no panel. The mapping the reader applies is one switch --
 *          fast -> @c k_display_refresh_fast (A2), quality ->
 *          @c k_display_refresh_quality (GC16).
 *
 * @invariant Exactly one of the two values is present in @ref ra8_zoom_present_t.
 * @see ra8_zoom_view_present
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_zoom_refresh_fast    = 0U, /**< Interactive burst: panel A2 (bi-level, fast). */
  k_ra8_zoom_refresh_quality = 1U, /**< Settled: panel GC16 (all 16 levels, slow).    */
} ra8_zoom_refresh_t;

/**
 * @enum ra8_zoom_policy_t
 * @brief How a viewport change is scheduled against the panel's waveforms.
 *
 * @details
 * The one genuine product fork in this module, so both halves are available
 * rather than one being chosen silently. @ref k_ra8_zoom_policy_responsive
 * trades tone for latency during a gesture and pays the quality repaint once,
 * on settle; @ref k_ra8_zoom_policy_quality never shows a bi-level frame and
 * pays a full GC16 (~450 ms) for every pan step. A photographic figure wants
 * the second on a slow panel; text and line art want the first.
 *
 * @invariant Under @ref k_ra8_zoom_policy_quality the engine never emits
 *            @ref k_ra8_zoom_refresh_fast, and @ref ra8_zoom_view_tick never
 *            arms a settle repaint.
 *
 * @par Example:
 * @code
 * cfg.policy = k_ra8_zoom_policy_responsive;  // A2 while panning, GC16 on settle
 * @endcode
 *
 * @see ra8_zoom_view_tick
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_zoom_policy_responsive = 0U, /**< Fast during a burst, quality once settled.  */
  k_ra8_zoom_policy_quality    = 1U, /**< Always quality; no bi-level frame is shown. */
} ra8_zoom_policy_t;

/**
 * @enum ra8_zoom_pan_t
 * @brief Discrete pan direction for tap-band navigation.
 *
 * @details The reader's touch path (GT911) reports contacts, not drags, so the
 *          shipped interaction is edge tap-bands rather than a fling. The
 *          direction names the way the *content* travels under the viewport,
 *          matching ::ra8_tile_pan_dir_t so the tiled adapter can forward it to
 *          the cache's read-ahead without a translation table.
 *
 * @invariant k_ra8_zoom_pan_none names "no movement" and is the zero value.
 * @see ra8_zoom_view_pan_dir
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_zoom_pan_none  = 0U, /**< No movement.                */
  k_ra8_zoom_pan_left  = 1U, /**< Viewport travels toward -x. */
  k_ra8_zoom_pan_right = 2U, /**< Viewport travels toward +x. */
  k_ra8_zoom_pan_up    = 3U, /**< Viewport travels toward -y. */
  k_ra8_zoom_pan_down  = 4U, /**< Viewport travels toward +y. */
} ra8_zoom_pan_t;

/**
 * @brief Read a sub-rectangle of the magnified source as gray8 (the DIP seam).
 *
 * @details
 * The single seam through which the engine sees pixels. An implementation must
 * write exactly @p w bytes at each of @p h successive @p out_stride offsets, one
 * gray8 sample per source pixel, and must fail rather than truncate if the
 * rectangle leaves the image -- the engine only ever asks for rectangles inside
 * the declared @ref ra8_zoom_source_t::width x @ref ra8_zoom_source_t::height, so
 * an out-of-range request is a defect, not a clamp.
 *
 * The engine requests exactly one row at a time (`h == 1`), and never re-requests
 * a row it already holds, so an implementation backed by a cache sees each source
 * row faulted once per frame.
 *
 * @param[in]  ctx        Implementation context supplied at bind time (may be NULL).
 * @param[in]  x          Left edge of the requested rectangle, source pixels.
 * @param[in]  y          Top edge of the requested rectangle, source pixels.
 * @param[in]  w          Width in source pixels (`> 0`).
 * @param[in]  h          Height in source pixels (`> 0`).
 * @param[out] out        Destination gray8 buffer of at least `out_stride * h` bytes.
 * @param[in]  out_stride Bytes between successive output rows (`>= w`).
 *
 * @return Error code; anything but @c k_ra8_ok aborts the frame and is returned
 *         verbatim to the caller of ::ra8_zoom_view_render.
 * @retval k_ra8_ok The rectangle was written.
 *
 * @pre  The rectangle lies wholly inside the declared source extent.
 * @pre  @p out addresses at least `out_stride * h` writable bytes.
 * @post On success every `out[r * out_stride + c]` is the gray8 value of source
 *       pixel `(x + c, y + r)`.
 * @post On failure @p out content is unspecified and the frame is abandoned.
 *
 * @note Called from ::ra8_zoom_view_render only; inherits its (non-)thread-safety.
 * @see ra8_zoom_source_init
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_zoom_read_fn)(void*    ctx,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t w,
                                      uint32_t h,
                                      uint8_t* out,
                                      uint32_t out_stride);

/**
 * @struct ra8_zoom_source_t
 * @brief A magnifiable full-resolution image behind the gray8 sub-rect seam.
 *
 * @details Plain data: the reader function, its context, and the extent the
 *          engine may address. Populate it with ::ra8_zoom_source_init or with
 *          one of the adapter binders, never by hand -- the initialiser is where
 *          the extent bound (@ref k_ra8_zoom_dim_max) is enforced.
 *
 * @invariant `width` and `height` are both in `[1, k_ra8_zoom_dim_max]`.
 * @invariant `read` is non-NULL for any source bound into a view.
 *
 * @par Example:
 * @code
 * ra8_zoom_source_t src = {};
 * (void)ra8_zoom_source_init(&src, my_read, &my_ctx, 4096U, 3072U);
 * @endcode
 *
 * @see ra8_zoom_read_fn
 * @since 0.1.0
 */
typedef struct {
  ra8_zoom_read_fn read;   /**< gray8 sub-rectangle reader (never NULL once bound). */
  void*            ctx;    /**< Opaque context handed back to @c read.              */
  uint32_t         width;  /**< Full-resolution source width, pixels.               */
  uint32_t         height; /**< Full-resolution source height, pixels.              */
} ra8_zoom_source_t;

/**
 * @struct ra8_zoom_scratch_t
 * @brief Caller-owned composite buffers -- the module's entire memory footprint.
 *
 * @details
 * Three buffers, all supplied by the caller and none allocated: one source row,
 * one destination strip in gray8, and the same strip packed to 4 bpp. Sizing is
 * a function of the viewport width alone, never its height, which is the whole
 * point of the strip pipeline (see the file docblock):
 *
 *  - `row_cap    >= dst.w`                      -- the widest source row a frame reads
 *  - `strip_cap  >= dst.w`                      -- at least one destination row
 *  - `packed_cap >= (dst.w * strip_rows + 1)/2` -- with `strip_rows = strip_cap / dst.w`
 *
 * ::ra8_zoom_view_open derives @c strip_rows from @c strip_cap and rejects a
 * @c packed_cap that cannot hold it, so an under-sized buffer is a startup error
 * and never a partial frame.
 *
 * @invariant No pointer is NULL and no capacity is zero in an opened view.
 * @invariant The three buffers do not overlap.
 *
 * @par Example:
 * @code
 * static uint8_t s_row[1024], s_strip[1024 * 16], s_packed[(1024 * 16) / 2];
 * cfg.scratch = (ra8_zoom_scratch_t){ .row = s_row,       .row_cap    = sizeof(s_row),
 *                                     .strip = s_strip,   .strip_cap  = sizeof(s_strip),
 *                                     .packed = s_packed, .packed_cap = sizeof(s_packed) };
 * @endcode
 *
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
typedef struct {
  uint8_t* row;        /**< One source row, `>= dst.w` bytes.              */
  uint32_t row_cap;    /**< Capacity of @c row, bytes.                     */
  uint8_t* strip;      /**< gray8 destination strip, `dst.w * strip_rows`. */
  uint32_t strip_cap;  /**< Capacity of @c strip, bytes.                   */
  uint8_t* packed;     /**< The strip packed to 4 bpp (2 px/byte).         */
  uint32_t packed_cap; /**< Capacity of @c packed, bytes.                  */
} ra8_zoom_scratch_t;

/**
 * @struct ra8_zoom_present_t
 * @brief What the reader must flush, and with which waveform.
 *
 * @details Produced by ::ra8_zoom_view_present and consumed by the reader's
 *          display call. @c present is false when nothing changed since the last
 *          present, which is the common case in a polling main loop and is what
 *          keeps an idle reader from refreshing an e-ink panel forever.
 *
 * @invariant When `present` is false, `rect` and `refresh` are unspecified.
 * @invariant `rect` always equals the view's destination rectangle: a pan moves
 *            every pixel in the viewport, so there is no smaller true dirty region.
 *
 * @par Example:
 * @code
 * ra8_zoom_present_t plan = {};
 * if ((ra8_zoom_view_present(&view, &plan) == k_ra8_ok) && plan.present) {
 *   const display_rect_t r = { .x = (uint16_t)plan.rect.x, .y = (uint16_t)plan.rect.y,
 *                              .w = (uint16_t)plan.rect.w, .h = (uint16_t)plan.rect.h };
 *   (void)display_flush(d, r, (plan.refresh == k_ra8_zoom_refresh_fast)
 *                               ? k_display_refresh_fast : k_display_refresh_quality);
 * }
 * @endcode
 *
 * @see ra8_zoom_view_present
 * @since 0.1.0
 */
typedef struct {
  ra8_ui_rect_t      rect;    /**< Panel rectangle to flush.                    */
  ra8_zoom_refresh_t refresh; /**< Waveform class for that flush.               */
  bool               present; /**< False: nothing changed, do not flush at all. */
} ra8_zoom_present_t;

/**
 * @struct ra8_zoom_view_cfg_t
 * @brief Everything ::ra8_zoom_view_open needs; nothing it does not.
 *
 * @details The focus fields are in **source** pixels and name the point the view
 *          opens centred on -- the panel coordinate of the tap that opened the
 *          viewer, mapped back through whatever laid the image out. Zero/zero is
 *          the image's top-left corner, which after clamping is simply
 *          "top-left of the image", the natural place to open a comic page.
 *
 * @invariant `dst.w > 0` and `dst.h > 0`.
 * @invariant `scale` lies in `[k_ra8_zoom_scale_min, effective scale_max]`.
 *
 * @par Example:
 * @code
 * const ra8_zoom_view_cfg_t cfg = {
 *   .src = src, .scratch = scratch,
 *   .dst = { .x = 0, .y = 0, .w = 1024, .h = 552 },
 *   .scale = k_ra8_zoom_scale_min, .scale_max = 4U,
 *   .policy = k_ra8_zoom_policy_responsive,
 *   .focus_x = (int32_t)tap_src_x, .focus_y = (int32_t)tap_src_y,
 * };
 * @endcode
 *
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
typedef struct {
  ra8_zoom_source_t  src;       /**< Bound source (see ::ra8_zoom_source_init).    */
  ra8_zoom_scratch_t scratch;   /**< Caller-owned composite buffers.               */
  ra8_ui_rect_t      dst;       /**< Viewport in framebuffer pixels.               */
  uint8_t            scale;     /**< Opening magnification (0 => scale_min).       */
  uint8_t            scale_max; /**< Ladder ceiling (0 => ::k_ra8_zoom_scale_max). */
  ra8_zoom_policy_t  policy;    /**< Waveform scheduling policy.                   */
  uint16_t           settle_ms; /**< Settle window (0 => the module default).      */
  int32_t            focus_x;   /**< Opening centre, source pixels (x).            */
  int32_t            focus_y;   /**< Opening centre, source pixels (y).            */
} ra8_zoom_view_cfg_t;

/**
 * @struct ra8_zoom_view_t
 * @brief Live viewport: where it is looking, how magnified, and what it owes the panel.
 *
 * @details
 * Opaque by convention -- read it through the accessors, never write it. The two
 * anchor fields are the state that matters and they are in *magnified-plane*
 * coordinates (see the file docblock), so `anchor_x + c` is the plane column shown
 * at destination column @c c and `(anchor_x + c) / scale` is the source column.
 *
 * @invariant `scale` lies in `[k_ra8_zoom_scale_min, scale_max]` whenever `active`.
 * @invariant `anchor_x` is clamped to `[0, width*scale - dst.w]` when the image is
 *            wider than the viewport, and is the negative centring offset otherwise.
 * @invariant `strip_rows >= 1` and `strip_rows <= k_ra8_zoom_strip_rows_max`.
 * @invariant `settle_armed` is false under ::k_ra8_zoom_policy_quality.
 *
 * @par State transitions
 * @dot
 * digraph ra8_zoom_view {
 *   rankdir=LR;
 *   node [shape=ellipse, fontsize=10];
 *   closed  [label="closed\n(active=false)"];
 *   clean   [label="shown\n(pending=false)"];
 *   fastp   [label="fast pending\n(A2 owed)"];
 *   qualp   [label="quality pending\n(GC16 owed)"];
 *   closed -> qualp [label="view_open()"];
 *   qualp  -> clean [label="render(); present()"];
 *   fastp  -> clean [label="render(); present()"];
 *   clean  -> fastp [label="pan/set_scale\n(responsive)"];
 *   clean  -> qualp [label="pan/set_scale\n(quality policy)"];
 *   fastp  -> fastp [label="pan/set_scale\n(responsive)"];
 *   clean  -> qualp [label="tick() after settle_ms"];
 *   clean  -> closed [label="view_close()"];
 *   qualp  -> closed [label="view_close()"];
 *   fastp  -> closed [label="view_close()"];
 * }
 * @enddot
 *
 * @par State transition table
 * | State | Event | Next state | Action |
 * |---|---|---|---|
 * | closed | `view_open` | quality pending | anchor from focus, clamp |
 * | any pending | `view_present` | shown | hand the rect + waveform to the caller |
 * | shown | `view_pan` / `view_set_scale` | fast (responsive) or quality pending | re-clamp anchor, arm settle |
 * | shown | `view_tick` past `settle_ms` | quality pending | disarm settle |
 * | any | `view_close` | closed | nothing owed |
 *
 * @par Example:
 * @code
 * static ra8_zoom_view_t s_view;
 * (void)ra8_zoom_view_open(&s_view, &cfg);
 * @endcode
 *
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
typedef struct {
  ra8_zoom_source_t  src;           /**< Bound source.                                  */
  ra8_zoom_scratch_t scratch;       /**< Caller-owned composite buffers.                */
  ra8_ui_rect_t      dst;           /**< Viewport in framebuffer pixels.                */
  int32_t            anchor_x;      /**< Plane column shown at `dst.x`.                 */
  int32_t            anchor_y;      /**< Plane row shown at `dst.y`.                    */
  uint32_t           last_input_ms; /**< Timestamp of the last viewport change.         */
  uint16_t           settle_ms;     /**< Input silence before the quality repaint.      */
  uint16_t           strip_rows;    /**< Destination rows per composite strip.          */
  uint8_t            scale;         /**< Current integer magnification.                 */
  uint8_t            scale_max;     /**< Ladder ceiling for this view.                  */
  ra8_zoom_policy_t  policy;        /**< Waveform scheduling policy.                    */
  ra8_zoom_refresh_t pending_kind;  /**< Waveform the owed flush should use.            */
  bool               pending;       /**< A flush is owed.                               */
  bool               settle_armed;  /**< A quality repaint is still due after settling. */
  bool               active;        /**< The view is open.                              */
} ra8_zoom_view_t;

/**
 * @brief Bind a gray8 sub-rectangle reader and its extent as a magnifiable source.
 *
 * @details Validates the reader and the extent once, here, so nothing downstream
 *          has to re-check them: the engine's plane arithmetic is overflow-free
 *          precisely because @p width and @p height passed
 *          @ref k_ra8_zoom_dim_max at this seam.
 *
 * @param[out] out    Source descriptor to populate.
 * @param[in]  read   gray8 sub-rectangle reader (non-NULL).
 * @param[in]  ctx    Opaque context handed back to @p read (may be NULL).
 * @param[in]  width  Full-resolution source width, `[1, k_ra8_zoom_dim_max]`.
 * @param[in]  height Full-resolution source height, `[1, k_ra8_zoom_dim_max]`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              @p out describes the source.
 * @retval k_ra8_err_null_ptr    @p out or @p read is NULL.
 * @retval k_ra8_err_invalid_arg @p width or @p height is 0 or exceeds the cap.
 *
 * @pre  @p out addresses writable storage for one ::ra8_zoom_source_t.
 * @pre  @p read remains valid for the lifetime of every view bound to this source.
 * @post On k_ra8_ok every field of @p out is set and `out->read != NULL`.
 * @post On any error @p out is left untouched.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_source_init(ra8_zoom_source_t* out,
                                             ra8_zoom_read_fn   read,
                                             void*              ctx,
                                             uint32_t           width,
                                             uint32_t           height);

/**
 * @brief Open a viewport onto a source, centred on a source-pixel focus.
 *
 * @details
 * Derives everything the engine needs and never re-derives it: the strip height
 * from the scratch capacity, the effective ladder ceiling, the settle window, and
 * the initial anchor from @p cfg->focus_x / @c focus_y clamped to the image. The
 * view opens owing a @ref k_ra8_zoom_refresh_quality flush regardless of policy --
 * the first frame a reader sees must be the good one.
 *
 * @param[out] v   View to open (overwritten wholesale).
 * @param[in]  cfg Configuration; see ::ra8_zoom_view_cfg_t.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The view is open and owes a quality flush.
 * @retval k_ra8_err_null_ptr    @p v, @p cfg, `cfg->src.read`, or a scratch pointer is NULL.
 * @retval k_ra8_err_invalid_arg Empty viewport, out-of-range scale, or a bad source extent.
 * @retval k_ra8_err_no_mem      A scratch buffer is too small for this viewport width.
 *
 * @pre  @p cfg->src was populated by ::ra8_zoom_source_init or an adapter binder.
 * @pre  Every scratch buffer in @p cfg outlives the view.
 * @post On k_ra8_ok `v->active` is true, `v->pending` is true and
 *       `v->pending_kind == k_ra8_zoom_refresh_quality`.
 * @post On any error @p v is not left half-open: `v->active` is false.
 *
 * @note Not thread-safe. Does not touch the framebuffer -- call
 *       ::ra8_zoom_view_render for that.
 *
 * @par Example:
 * @code
 * if (ra8_zoom_view_open(&s_view, &cfg) == k_ra8_ok) { (void)ra8_zoom_view_render(&s_view); }
 * @endcode
 *
 * @see ra8_zoom_view_close
 * @see ra8_zoom_view_render
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_open(ra8_zoom_view_t* v, const ra8_zoom_view_cfg_t* cfg);

/**
 * @brief Point an open view at a different source, keeping zoom and position.
 *
 * @details
 * The page-turn path, and a deliberate product choice: turning the page while
 * magnified keeps the magnification and the viewport anchor rather than dropping
 * back to the whole page. A reader who zoomed in because the text is small wants
 * the next page at the same magnification; the alternative (a turn exits zoom) is
 * strictly less capable and is recoverable by calling ::ra8_zoom_view_close
 * instead. The anchor is re-clamped to the new extent, so a shorter page simply
 * pins to its bottom edge rather than showing past it.
 *
 * @param[in,out] v      Open view.
 * @param[in]     src    New source (validated as at open).
 * @param[in]     now_ms Current millisecond timestamp for the settle timer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The view now reads @p src.
 * @retval k_ra8_err_null_ptr      @p v or @p src is NULL, or `src->read` is NULL.
 * @retval k_ra8_err_invalid_arg   @p src has a zero or oversized extent.
 * @retval k_ra8_err_invalid_state @p v is not open.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  @p src outlives the view.
 * @post On k_ra8_ok `v->scale` is unchanged and the anchor is clamped to @p src.
 * @post On k_ra8_ok a flush is owed (`v->pending` is true).
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_zoom_view_rebind(ra8_zoom_view_t* v, const ra8_zoom_source_t* src, uint32_t now_ms);

/**
 * @brief Declare the viewport's pixels stale for a reason the engine cannot see.
 *
 * @details
 * The engine marks itself dirty when *it* changes the view, and deliberately does
 * not when nothing moved -- a pan clamped against an edge owes the panel nothing,
 * which is what stops an e-ink refresh per tap. That is exactly right for the
 * engine's own state and exactly wrong for everything a compositor does around
 * it: drawing a loupe over the viewport, lifting one off it, or repainting chrome
 * that overlaps it all invalidate pixels the engine has no way to know about.
 *
 * Without this the reader hits a silent class of bug that a continuously-scanned
 * LCD hides completely: the framebuffer is correct, no flush is ever requested,
 * and on an e-ink panel the change simply never appears. Call it whenever
 * something outside the engine has damaged the viewport.
 *
 * @param[in,out] v      Open view.
 * @param[in]     now_ms Current millisecond timestamp for the settle timer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                A flush of the whole viewport is now owed.
 * @retval k_ra8_err_null_ptr      @p v is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  The caller genuinely damaged (or is about to repaint) the viewport.
 * @post `v->pending` is true and `v->pending_kind` follows `v->policy`.
 * @post The anchor and the scale are untouched -- only the flush bookkeeping moves.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * s->lens_on = !s->lens_on;                                 // chrome over the page
 * (void)ra8_zoom_view_invalidate(&s->page, ra8_time_ms());  // ...so repaint it
 * @endcode
 *
 * @see ra8_zoom_view_present
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_invalidate(ra8_zoom_view_t* v, uint32_t now_ms);

/**
 * @brief Close a view; it stops owing the panel anything.
 *
 * @param[in,out] v View to close (may already be closed).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The view is closed.
 * @retval k_ra8_err_null_ptr @p v is NULL.
 *
 * @pre  @p v addresses writable storage for one ::ra8_zoom_view_t.
 * @pre  The caller has already flushed anything it intends the panel to keep.
 * @post `v->active` is false and `v->pending` is false.
 * @post The caller's scratch buffers and source are untouched and may be reused.
 *
 * @note Not thread-safe. Closing twice is not an error.
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_close(ra8_zoom_view_t* v);

/**
 * @brief Whether a view is open.
 *
 * @param[in] v View to query (NULL reads as closed).
 *
 * @return Openness of the view.
 * @retval true  @p v is non-NULL and open.
 * @retval false @p v is NULL or closed.
 *
 * @pre  None -- NULL is an accepted input and answers false.
 * @pre  @p v, if non-NULL, addresses a ::ra8_zoom_view_t.
 * @post No state is modified (pure query).
 * @post The result is exactly `v != NULL && v->active`.
 *
 * @note Thread-safe with respect to this module: reads one bool.
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_zoom_view_active(const ra8_zoom_view_t* v);

/**
 * @brief Set the magnification, keeping a panel point fixed under the finger.
 *
 * @details
 * Tap-to-zoom, literally: the plane point currently displayed at
 * (@p focus_x, @p focus_y) -- framebuffer coordinates, i.e. where the user tapped
 * -- is the point that stays put. The plane coordinate is rescaled by
 * `new_scale / old_scale` and the anchor is recomputed so that the same image
 * feature lands under the same pixel, then clamped. A focus outside the viewport
 * still works and simply pulls that off-screen feature toward the tapped edge.
 *
 * Setting the scale it already has is a no-op and owes no flush, so a reader can
 * call this unconditionally from a tap handler.
 *
 * @param[in,out] v       Open view.
 * @param[in]     scale   Requested magnification, `[k_ra8_zoom_scale_min, v->scale_max]`.
 * @param[in]     focus_x Framebuffer column to keep fixed.
 * @param[in]     focus_y Framebuffer row to keep fixed.
 * @param[in]     now_ms  Current millisecond timestamp for the settle timer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The scale is now @p scale (or already was).
 * @retval k_ra8_err_null_ptr      @p v is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 * @retval k_ra8_err_out_of_range  @p scale is outside the ladder.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  @p scale is on the integer ladder (fractional zoom is not representable).
 * @post On k_ra8_ok `v->scale == scale` and the anchor is clamped to the new plane.
 * @post On k_ra8_ok with a changed scale, a flush is owed.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * const uint8_t next = ra8_zoom_scale_cycle(view.scale, k_ra8_zoom_scale_min, view.scale_max);
 * (void)ra8_zoom_view_set_scale(&view, next, tap_x, tap_y, ra8_time_ms());
 * @endcode
 *
 * @see ra8_zoom_scale_cycle
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_set_scale(ra8_zoom_view_t* v,
                                                uint8_t          scale,
                                                int32_t          focus_x,
                                                int32_t          focus_y,
                                                uint32_t         now_ms);

/**
 * @brief Next magnification on the doubling ladder, wrapping back to the minimum.
 *
 * @details The tap-to-zoom cycle every reader in this tree uses:
 *          `min -> 2*min -> 4*min -> ... -> max -> min`. Doubling rather than
 *          incrementing because a 1x -> 2x step is a visible change and a
 *          7x -> 8x step is not. The last rung is @p max exactly even when @p max
 *          is not a power of two, so a ceiling of 3 gives `1 -> 2 -> 3 -> 1`.
 *
 * @param[in] scale Current magnification.
 * @param[in] min   Ladder floor (0 is read as ::k_ra8_zoom_scale_min).
 * @param[in] max   Ladder ceiling (below @p min is read as @p min).
 *
 * @return The next magnification on the ladder.
 * @retval min                 @p scale was already at or above @p max.
 * @retval "2 * scale or max"  Otherwise, whichever is smaller.
 *
 * @pre  None -- every input combination has a defined answer.
 * @pre  @p min and @p max are magnifications, not plane coordinates.
 * @post The result is in `[min, max]`.
 * @post No state is modified (pure function).
 *
 * @note Thread-safe: pure integer arithmetic on its arguments.
 * @see ra8_zoom_view_set_scale
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra8_zoom_scale_cycle(uint8_t scale, uint8_t min, uint8_t max);

/**
 * @brief Pan the viewport by a destination-pixel delta.
 *
 * @details Moves the anchor and re-clamps it. Because the anchor lives in the
 *          magnified plane the delta is in *destination* pixels at every zoom, so
 *          a one-pixel pan is one pixel of panel movement whether the view is at
 *          1:1 or 8x. A delta that would leave the image is clamped rather than
 *          rejected, and a clamped-to-no-movement pan owes no flush -- panning
 *          into an edge does not cost an e-ink refresh.
 *
 * @param[in,out] v      Open view.
 * @param[in]     dx     Horizontal movement in destination pixels (+ moves content left).
 * @param[in]     dy     Vertical movement in destination pixels (+ moves content up).
 * @param[in]     now_ms Current millisecond timestamp for the settle timer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The anchor moved, or was already against the edge.
 * @retval k_ra8_err_null_ptr      @p v is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  @p dx and @p dy are destination pixels, not source pixels.
 * @post The anchor satisfies the clamp invariant of ::ra8_zoom_view_t.
 * @post A flush is owed if and only if the anchor actually changed.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_pan_dir
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_zoom_view_pan(ra8_zoom_view_t* v, int32_t dx, int32_t dy, uint32_t now_ms);

/**
 * @brief Pan by a discrete step in one direction (the tap-band interaction).
 *
 * @details One step is @ref ra8_zoom_view_t::dst minus a one-eighth overlap on
 *          the moving axis, so consecutive taps keep a strip of the previous view
 *          on screen and the reader never loses their place. The overlap is a
 *          fraction of the viewport rather than a constant so it scales with the
 *          lens size in loupe mode.
 *
 * @param[in,out] v      Open view.
 * @param[in]     dir    Direction; ::k_ra8_zoom_pan_none is a no-op.
 * @param[in]     now_ms Current millisecond timestamp for the settle timer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The step was applied (possibly clamped to nothing).
 * @retval k_ra8_err_null_ptr      @p v is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  @p dir is a ::ra8_zoom_pan_t enumerator.
 * @post The anchor satisfies the clamp invariant of ::ra8_zoom_view_t.
 * @post ::k_ra8_zoom_pan_none leaves the view byte-identical.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_pan
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_zoom_view_pan_dir(ra8_zoom_view_t* v, ra8_zoom_pan_t dir, uint32_t now_ms);

/**
 * @brief The source rectangle currently visible, in source pixels.
 *
 * @details The residency question in one call: these are exactly the pixels the
 *          next ::ra8_zoom_view_render will read, so a tiled adapter can turn it
 *          into a tile rectangle and prove that nothing outside it is pinned.
 *          The rectangle is clipped to the image, so a letterboxed view reports
 *          the image's own extent rather than the viewport's.
 *
 * @param[in]  v   Open view.
 * @param[out] out Receives the visible source rectangle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                @p out holds the visible source rectangle.
 * @retval k_ra8_err_null_ptr      @p v or @p out is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  @p out addresses writable storage for one ::ra8_ui_rect_t.
 * @post `out->x + out->w <= (int32_t)v->src.width` and likewise for y/h.
 * @post `out->w >= 1` and `out->h >= 1` (a view always shows at least one pixel).
 *
 * @note Thread-safe with respect to this module: reads only.
 * @see ra8_zoom_tiles_prefetch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_window(const ra8_zoom_view_t* v, ra8_ui_rect_t* out);

/**
 * @brief Advance the settle timer; promote a bi-level view to full quality.
 *
 * @details Call once per main-loop iteration. Under
 *          ::k_ra8_zoom_policy_responsive an interactive change leaves the view
 *          armed; once @ref ra8_zoom_view_t::settle_ms of input silence have
 *          passed this arms a @ref k_ra8_zoom_refresh_quality repaint of the same
 *          rectangle and disarms itself, so the reader repaints once and not per
 *          tick. The elapsed-time comparison is unsigned, so a millisecond
 *          counter that wraps produces one early settle rather than a hang.
 *
 * @param[in,out] v      View (NULL is accepted and answers false).
 * @param[in]     now_ms Current millisecond timestamp.
 *
 * @return Whether a quality repaint just became due.
 * @retval true  The caller should re-render and present.
 * @retval false Nothing to do this tick.
 *
 * @pre  None -- NULL, closed and disarmed views all answer false.
 * @pre  @p now_ms comes from a monotonic millisecond source.
 * @post On true, `v->pending` is true and `v->settle_armed` is false.
 * @post On false, @p v is unmodified.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_present
 * @since 0.1.0
 */
bool ra8_zoom_view_tick(ra8_zoom_view_t* v, uint32_t now_ms);

/**
 * @brief Take the pending flush plan, clearing it.
 *
 * @details The consume half of the dirty-tracking pair: it reports what the panel
 *          owes and immediately marks it paid, so a caller that ignores the plan
 *          simply drops a frame rather than flushing the same rectangle forever.
 *          Call it after ::ra8_zoom_view_render, not before.
 *
 * @param[in,out] v   Open view.
 * @param[out]    out Receives the plan; `out->present` is false when none is owed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                @p out holds the plan (possibly `present == false`).
 * @retval k_ra8_err_null_ptr      @p v or @p out is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 *
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @pre  The framebuffer already holds the pixels this plan describes.
 * @post `v->pending` is false.
 * @post A second immediate call reports `present == false`.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_render
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_present(ra8_zoom_view_t* v, ra8_zoom_present_t* out);

/**
 * @brief Composite the visible window into the bound ra8_gfx framebuffer.
 *
 * @details
 * The render pipeline, one horizontal strip at a time (see the file docblock for
 * why strips): magnify the source into the gray8 strip by nearest-neighbour
 * replication, blue-noise dither the strip at *magnified-plane* phase so the grain
 * belongs to the image rather than the panel, pack it to 4 bpp, and blit it. Every
 * destination pixel of @ref ra8_zoom_view_t::dst is written -- image where the
 * source covers it, @ref k_ra8_zoom_bg_gray in the letterbox -- so the caller
 * never has to pre-clear the viewport.
 *
 * The whole path is integer arithmetic over the source bytes and a `const` mask,
 * so the output is byte-identical on the unit-test host, in ra8_emulator and on
 * silicon. That is what makes a framebuffer hash a usable golden.
 *
 * @param[in,out] v Open view.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The viewport was painted.
 * @retval k_ra8_err_null_ptr      @p v is NULL.
 * @retval k_ra8_err_invalid_state @p v is not open.
 * @retval k_ra8_err_*             Propagated verbatim from the source reader or ra8_gfx.
 *
 * @pre  ::ra8_gfx_init has bound a framebuffer.
 * @pre  @p v was opened by ::ra8_zoom_view_open.
 * @post On k_ra8_ok every pixel of `v->dst` inside the framebuffer has been written.
 * @post The ra8_gfx clip rectangle is left exactly as the caller set it.
 *
 * @note Not thread-safe; writes through the single ra8_gfx framebuffer binding.
 *       Honours the caller's clip, so a lens can be masked to a rounded box.
 *
 * @par Example:
 * @code
 * if (ra8_zoom_view_render(&view) != k_ra8_ok) { er_show_error(); }
 * @endcode
 *
 * @see ra8_zoom_view_present
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_zoom_view_render(ra8_zoom_view_t* v);
