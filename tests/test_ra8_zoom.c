/**
 * @file test_ra8_zoom.c
 * @brief Host unit tests for the tap-to-zoom viewport engine (#478).
 *
 * @details
 * Drives ra8_zoom over a synthetic gray8 source whose every pixel encodes its
 * own coordinate, so a render can be checked pixel by pixel against the source
 * rectangle it should be showing rather than against a remembered hash alone.
 * Coverage:
 *
 *   - open / rebind validation, with MC/DC on every compound guard,
 *   - the magnified-plane anchor model: clamping, letterboxing, pan granularity
 *     of one destination pixel at every zoom, and zoom-about-a-fixed-point,
 *   - the flush bookkeeping: fast-during-a-burst / quality-on-settle, and the
 *     "a pan that clamped to nothing owes the panel nothing" rule,
 *   - the strip composite: exact magnification, letterbox fill, source-read
 *     failure propagation, and a one-source-read-per-distinct-row guarantee,
 *   - the pan-stability property the whole coordinate model exists for: the
 *     dithered output of a pixel does not change when the viewport moves.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_dither.h"
#include "ra8_ui.h"
#include "ra8_zoom.h"
#include "ra8_zoom_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_zoom_geom_t
 * @brief Synthetic source and viewport geometry for the whole file.
 * @details A 64x48 source under a 32x16 viewport: small enough to check every
 *          pixel, big enough that the viewport pans on both axes at 1:1 and
 *          letterboxes at 1:1 when the source is made smaller than it.
 */
typedef enum : uint16_t {
  k_t_src_w  = 64U,  /**< Synthetic source width, pixels.         */
  k_t_src_h  = 48U,  /**< Synthetic source height, pixels.        */
  k_t_view_w = 32U,  /**< Viewport width, destination pixels.     */
  k_t_view_h = 16U,  /**< Viewport height, destination pixels.    */
  k_t_fb_w   = 48U,  /**< Host framebuffer width, pixels.         */
  k_t_fb_h   = 32U,  /**< Host framebuffer height, pixels.        */
  k_t_dst_x  = 8U,   /**< Viewport origin column in the FB.       */
  k_t_dst_y  = 4U,   /**< Viewport origin row in the FB.          */
  k_t_strip  = 4U,   /**< Destination rows per composite strip.   */
  k_t_settle = 100U, /**< Settle window used by the timing tests. */
} t_zoom_geom_t;

/**
 * @enum t_zoom_probe_t
 * @brief Recognisable values moved through the code under test.
 */
typedef enum : uint32_t {
  k_t_poison    = 0xA5A5A5A5U, /**< Written into out-params before a call.    */
  k_t_huge_dim  = 1000000U,    /**< A source extent past k_ra8_zoom_dim_max.  */
  k_t_big_delta = 1000000,     /**< A pan delta far past either edge.         */
  k_t_byte_mask = 0xFFU,       /**< Sample / pixel modulus.                   */
  k_t_off_scale = 200U,        /**< A scale_max far past the engine ladder.   */
  k_t_over_max  = 5U,          /**< A scale one rung past a ceiling of 4.     */
  k_t_axis_a    = 10,          /**< A 1:1 anchor inside a larger image.       */
  k_t_axis_b    = 40,          /**< The same view at 4x: anchor 40 => src 10. */
  k_t_axis_off  = 1000,        /**< An anchor entirely past the image.        */
  k_t_narrow_w  = 16,          /**< A source narrower than the viewport.      */
  k_t_narrow_d0 = 8,           /**< Its left letterbox width, 32 - 16 / 2.    */
  k_t_narrow_d1 = 24,          /**< One past its covered span, d0 + 16.       */
} t_zoom_probe_t;

/** @brief Host framebuffer ra8_gfx paints into (ARGB8888: one word per pixel). */
static uint32_t s_fb[(size_t)k_t_fb_h * (size_t)k_t_fb_w];

/** @brief Composite scratch: one source row at the viewport width. */
static uint8_t s_row[k_t_view_w];
/** @brief Composite scratch: the gray8 destination strip. */
static uint8_t s_strip[(size_t)k_t_view_w * (size_t)k_t_strip];
/** @brief Composite scratch: the strip packed to 4 bpp. */
static uint8_t s_packed[((size_t)k_t_view_w * (size_t)k_t_strip) / 2U];

/** @brief Source rows read during the last render, for the read-once check. */
static uint32_t s_reads;
/** @brief When non-zero, the source reader fails on the Nth call (1-based). */
static uint32_t s_fail_on;

/**
 * @brief The synthetic source sample: a pure function of the coordinate.
 * @param[in] x Source column.
 * @param[in] y Source row.
 * @return A gray8 value unique to (x, y) modulo 256.
 */
static uint8_t t_sample(uint32_t x, uint32_t y)
{
  return (uint8_t)(((y * (uint32_t)k_t_src_w) + x) & (uint32_t)k_t_byte_mask);
}

/**
 * @brief ::ra8_zoom_read_fn over the synthetic source, with an injectable fault.
 * @param[in]  ctx        Unused.
 * @param[in]  x          Rectangle left edge.
 * @param[in]  y          Rectangle top edge.
 * @param[in]  w          Rectangle width.
 * @param[in]  h          Rectangle height.
 * @param[out] out        gray8 destination.
 * @param[in]  out_stride Output row stride.
 * @return k_ra8_ok, or k_ra8_err_hw_error on the injected failing call.
 */
static ra8_err_t
t_read(void* ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out, uint32_t out_stride)
{
  (void)ctx;
  s_reads++;
  if (s_reads == s_fail_on) {
    return k_ra8_err_hw_error;
  }
  /* The engine must never ask for a rectangle outside the declared extent. */
  TEST_ASSERT((x + w) <= (uint32_t)k_t_src_w);
  TEST_ASSERT((y + h) <= (uint32_t)k_t_src_h);
  for (uint32_t r = 0U; r < h; ++r) {
    for (uint32_t c = 0U; c < w; ++c) {
      out[(r * out_stride) + c] = t_sample(x + c, y + r);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Build a source descriptor over the synthetic sampler.
 * @param[out] src Descriptor to populate.
 * @param[in]  w   Declared width.
 * @param[in]  h   Declared height.
 */
static void t_make_src(ra8_zoom_source_t* src, uint32_t w, uint32_t h)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_source_init(src, t_read, nullptr, w, h));
}

/**
 * @brief Build a configuration with valid scratch and the standard viewport.
 * @param[out] cfg Configuration to populate.
 * @param[in]  src Bound source.
 */
static void t_make_cfg(ra8_zoom_view_cfg_t* cfg, const ra8_zoom_source_t* src)
{
  const ra8_zoom_view_cfg_t base = {
    .src       = *src,
    .scratch   = {.row        = s_row,
                  .row_cap    = (uint32_t)sizeof(s_row),
                  .strip      = s_strip,
                  .strip_cap  = (uint32_t)sizeof(s_strip),
                  .packed     = s_packed,
                  .packed_cap = (uint32_t)sizeof(s_packed)},
    .dst       = {.x = (int32_t)k_t_dst_x,
                  .y = (int32_t)k_t_dst_y,
                  .w = (int32_t)k_t_view_w,
                  .h = (int32_t)k_t_view_h},
    .scale     = (uint8_t)k_ra8_zoom_scale_min,
    .scale_max = (uint8_t)k_ra8_zoom_scale_max,
    .policy    = k_ra8_zoom_policy_responsive,
    .settle_ms = (uint16_t)k_t_settle,
    .focus_x   = 0,
    .focus_y   = 0,
  };
  *cfg = base;
}

/** @brief Bind ra8_gfx to the host framebuffer, cleared. */
static void t_bind_fb(void)
{
  (void)memset(s_fb, 0, sizeof(s_fb));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_t_fb_w, (uint16_t)k_t_fb_h, k_ra8_gfx_format_argb8888));
}

/**
 * @test source_init_validates
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@ra8_zoom_source_init
 * `if (!internal_dim_ok(width) || !internal_dim_ok(height))` (2 conditions):
 * - V1: width=64,  height=48       -> false (both ok: the source binds)
 * - V2: width=0,   height=48       -> true  (varies width only)
 * - V3: width=64,  height=1000000  -> true  (varies height only)
 * V1+V2 prove width's independent influence; V1+V3 prove height's. N+1 = 3.
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@internal_dim_ok
 * `return (dim != 0U) && (dim <= k_ra8_zoom_dim_max)` (2 conditions):
 * - V1: dim=64        -> true  (both conditions true: accepted)
 * - V2: dim=0         -> false (varies the non-zero condition only)
 * - V3: dim=1000000   -> false (varies the ceiling condition only)
 * V1+V2 and V1+V3 give each condition independent influence. N+1 = 3, reached
 * through the three vectors above (the helper has no other caller shape).
 */
static void t_source_init_validates(void)
{
  TEST_BEGIN("source_init_validates");
  ra8_zoom_source_t src = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_source_init(nullptr, t_read, nullptr, 1U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_source_init(&src, nullptr, nullptr, 1U, 1U));
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_source_init(&src, t_read, nullptr, k_t_src_w, k_t_src_h));
  TEST_ASSERT_EQ(k_t_src_w, src.width);
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_source_init(&src, t_read, nullptr, 0U, k_t_src_h));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_zoom_source_init(&src, t_read, nullptr, k_t_src_w, k_t_huge_dim));
  TEST_END("source_init_validates");
}

/**
 * @brief The scratch-sizing and ladder halves of ::t_view_open_validates.
 * @details Split out only to stay inside the 60-line function cap; the MC/DC
 *          vector labels (V7..V13) are continuous with the parent test's block.
 * @param[in] src A bound source to build candidate configurations over.
 */
static void t_view_open_scratch_and_ladder(const ra8_zoom_source_t* src)
{
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  /* V8 */
  t_make_cfg(&cfg, src);
  cfg.scratch.row_cap = 4U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_zoom_view_open(&v, &cfg));
  /* V9 */
  t_make_cfg(&cfg, src);
  cfg.scratch.strip_cap = 4U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_zoom_view_open(&v, &cfg));
  /* A strip that fits but a packed buffer that cannot hold it. */
  t_make_cfg(&cfg, src);
  cfg.scratch.packed_cap = 1U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_zoom_view_open(&v, &cfg));
  /* V11 */
  t_make_cfg(&cfg, src);
  cfg.scale_max = (uint8_t)k_t_off_scale;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_open(&v, &cfg));
  /* V13 */
  t_make_cfg(&cfg, src);
  cfg.scale     = (uint8_t)k_t_over_max;
  cfg.scale_max = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_open(&v, &cfg));

  /* Zero scale / scale_max take the engine defaults rather than failing. */
  t_make_cfg(&cfg, src);
  cfg.scale     = 0U;
  cfg.scale_max = 0U;
  cfg.settle_ms = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT_EQ(k_ra8_zoom_scale_min, v.scale);
  TEST_ASSERT_EQ(k_ra8_zoom_scale_max, v.scale_max);
  TEST_ASSERT_EQ(k_ra8_zoom_settle_ms_default, v.settle_ms);
}

/**
 * @test view_open_validates
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@internal_cfg_extent_ok
 * `if ((cfg->dst.w <= 0) || (cfg->dst.h <= 0))` (2 conditions):
 * - V1: w=32, h=16 -> false (control: a real viewport)
 * - V2: w=0,  h=16 -> true  (varies w only)
 * - V3: w=32, h=0  -> true  (varies h only)
 * Second decision in the same function,
 * `if (!internal_dim_ok(cfg->src.width) || !internal_dim_ok(cfg->src.height))`:
 * - V4: src 64x48       -> false (control)
 * - V5: src 0x48        -> true  (varies width only)
 * - V6: src 64x1000000  -> true  (varies height only)
 * V1+V2 / V1+V3 and V4+V5 / V4+V6 give each of the four conditions independent
 * influence. N+1 = 3 vectors per decision.
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@internal_cfg_scratch_ok
 * `if ((cfg->scratch.row_cap < width) || (cfg->scratch.strip_cap < width))`:
 * - V7: row_cap=32, strip_cap=128 -> false (control: both fit a 32-px viewport)
 * - V8: row_cap=4,  strip_cap=128 -> true  (varies row_cap only)
 * - V9: row_cap=32, strip_cap=4   -> true  (varies strip_cap only)
 * V7+V8 prove row_cap's influence; V7+V9 prove strip_cap's. N+1 = 3.
 *
 * ::internal_cfg_ladder_ok carries no compound decision: a `< min` half would be
 * structurally unable to vary (the "0 means default" rewrite lifts both values
 * to the minimum on the line above), so each check is the single bound that can
 * actually fail. Both are exercised both ways here:
 * - V10: scale_max=4          -> accepted;  V11: scale_max=200 -> rejected.
 * - V12: scale=2, scale_max=4 -> accepted;  V13: scale=5, scale_max=4 -> rejected.
 */
static void t_view_open_validates(void)
{
  TEST_BEGIN("view_open_validates");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_open(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_open(&v, nullptr));

  /* V1 / V4 / V7 / V10 / V12: the control -- everything valid. */
  t_make_cfg(&cfg, &src);
  cfg.scale     = 2U;
  cfg.scale_max = 4U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT(ra8_zoom_view_active(&v));

  /* A missing scratch pointer is rejected before any sizing. */
  t_make_cfg(&cfg, &src);
  cfg.scratch.strip = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT(!ra8_zoom_view_active(&v));

  /* V2 */
  t_make_cfg(&cfg, &src);
  cfg.dst.w = 0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_open(&v, &cfg));
  /* V3 */
  t_make_cfg(&cfg, &src);
  cfg.dst.h = 0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_open(&v, &cfg));
  /* V5 */
  t_make_cfg(&cfg, &src);
  cfg.src.width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_open(&v, &cfg));
  /* V6 */
  t_make_cfg(&cfg, &src);
  cfg.src.height = (uint32_t)k_t_huge_dim;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_open(&v, &cfg));
  t_view_open_scratch_and_ladder(&src);
  TEST_END("view_open_validates");
}

/**
 * @test view_active_and_close
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@ra8_zoom_view_active
 * `return (v != nullptr) && v->active` (2 conditions):
 * - V1: v=&open   -> true  (both true)
 * - V2: v=NULL    -> false (varies the pointer condition)
 * - V3: v=&closed -> false (varies the active condition)
 * V1+V2 and V1+V3 give each condition independent influence. N+1 = 3.
 */
static void t_view_active_and_close(void)
{
  TEST_BEGIN("view_active_and_close");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT(ra8_zoom_view_active(&v));       /* V1 */
  TEST_ASSERT(!ra8_zoom_view_active(nullptr)); /* V2 */
  /* Invalidate is the "something outside the engine damaged the viewport"
   * seam: the anchor does not move, but a flush becomes owed. A pan of (0, 0)
   * deliberately does NOT do this, which is why the seam has to exist -- see
   * ra8_zoom_view_invalidate's docblock. */
  ra8_zoom_present_t drained = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &drained));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 0, 0, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &drained));
  TEST_ASSERT(!drained.present);
  const int32_t held_x = v.anchor_x;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_invalidate(nullptr, 10U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_invalidate(&v, 10U));
  TEST_ASSERT_EQ(held_x, v.anchor_x);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &drained));
  TEST_ASSERT(drained.present);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_close(&v));
  TEST_ASSERT(!ra8_zoom_view_active(&v)); /* V3 */
  /* Closing twice is not an error, and a closed view refuses every operation. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_close(&v));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_pan(&v, 1, 0, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_pan_dir(&v, k_ra8_zoom_pan_left, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_set_scale(&v, 2U, 0, 0, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_render(&v));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_invalidate(&v, 0U));
  ra8_ui_rect_t      win  = {};
  ra8_zoom_present_t plan = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(!ra8_zoom_view_tick(&v, (uint32_t)k_t_settle * 4U));
  TEST_END("view_active_and_close");
}

/**
 * @test rebind_keeps_zoom
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@ra8_zoom_view_rebind
 * `if (!internal_dim_ok(src->width) || !internal_dim_ok(src->height))` (2 conditions):
 * - V1: 64x48      -> false (control: the rebind takes)
 * - V2: 0x48       -> true  (varies width only)
 * - V3: 64x1000000 -> true  (varies height only)
 * V1+V2 prove width's independent influence; V1+V3 prove height's. N+1 = 3.
 * The extent is written straight into the descriptor by the test rather than
 * through ra8_zoom_source_init(), which would reject V2/V3 first -- the guard
 * under test is rebind's own, and it must hold for a hand-built descriptor.
 */
static void t_rebind_keeps_zoom(void)
{
  TEST_BEGIN("rebind_keeps_zoom");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_set_scale(&v, 4U, 0, 0, 0U));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_rebind(&v, nullptr, 0U));
  ra8_zoom_source_t bad = src;
  bad.read              = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_rebind(&v, &bad, 0U));

  /* V1: a page turn keeps the magnification -- the deliberate product choice. */
  ra8_zoom_source_t next = {};
  t_make_src(&next, k_t_src_w, k_t_src_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_rebind(&v, &next, 1U));
  TEST_ASSERT_EQ(4U, v.scale);

  /* V2 */
  ra8_zoom_source_t zero_w = next;
  zero_w.width             = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_rebind(&v, &zero_w, 1U));
  /* V3 */
  ra8_zoom_source_t huge_h = next;
  huge_h.height            = (uint32_t)k_t_huge_dim;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_zoom_view_rebind(&v, &huge_h, 1U));

  /* A shorter page re-clamps the anchor instead of showing past its bottom. */
  ra8_zoom_source_t shorter = {};
  t_make_src(&shorter, k_t_src_w, (uint32_t)k_t_view_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_rebind(&v, &shorter, 2U));
  ra8_ui_rect_t win = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT((win.y + win.h) <= (int32_t)k_t_view_h);

  /* A closed view cannot be pointed at a new page: the reader must reopen. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_close(&v));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_rebind(&v, &next, 3U));
  TEST_END("rebind_keeps_zoom");
}

/**
 * @test set_scale_keeps_focus
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@ra8_zoom_view_set_scale
 * `if ((scale < k_ra8_zoom_scale_min) || (scale > v->scale_max))` (2 conditions):
 * - V1: scale=2, scale_max=4 -> false (control: accepted)
 * - V2: scale=0, scale_max=4 -> true  (varies the floor condition)
 * - V3: scale=9, scale_max=4 -> true  (varies the ceiling condition)
 * V1+V2 prove the floor's independent influence; V1+V3 the ceiling's. N+1 = 3.
 */
static void t_set_scale_keeps_focus(void)
{
  TEST_BEGIN("set_scale_keeps_focus");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  cfg.scale_max = 4U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_set_scale(nullptr, 2U, 0, 0, 0U));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_zoom_view_set_scale(&v, 0U, 0, 0, 0U));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_zoom_view_set_scale(&v, 9U, 0, 0, 0U));

  /* Park the viewport mid-source so the focus maths is not against a clamp. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 8, 8, 0U));
  ra8_ui_rect_t before = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &before));
  const int32_t focus_x = (int32_t)k_t_dst_x + ((int32_t)k_t_view_w / 2);
  const int32_t focus_y = (int32_t)k_t_dst_y + ((int32_t)k_t_view_h / 2);
  const int32_t held_x  = before.x + ((int32_t)k_t_view_w / 2);
  const int32_t held_y  = before.y + ((int32_t)k_t_view_h / 2);

  /* V1: the source pixel under the focus point stays under it after the zoom. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_set_scale(&v, 2U, focus_x, focus_y, 1U));
  TEST_ASSERT_EQ(2U, v.scale);
  ra8_ui_rect_t after = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &after));
  TEST_ASSERT_EQ(held_x, after.x + ((int32_t)k_t_view_w / 2 / 2));
  TEST_ASSERT_EQ(held_y, after.y + ((int32_t)k_t_view_h / 2 / 2));

  /* Re-selecting the current scale is a no-op and owes no flush. */
  ra8_zoom_present_t drain = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &drain));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_set_scale(&v, 2U, focus_x, focus_y, 2U));
  ra8_zoom_present_t none = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &none));
  TEST_ASSERT(!none.present);
  TEST_END("set_scale_keeps_focus");
}

/**
 * @test scale_cycle_ladder
 */
static void t_scale_cycle_ladder(void)
{
  TEST_BEGIN("scale_cycle_ladder");
  TEST_ASSERT_EQ(2U, ra8_zoom_scale_cycle(1U, 1U, 4U));
  TEST_ASSERT_EQ(4U, ra8_zoom_scale_cycle(2U, 1U, 4U));
  TEST_ASSERT_EQ(1U, ra8_zoom_scale_cycle(4U, 1U, 4U));
  /* A ceiling that is not a power of two still ends exactly on it. */
  TEST_ASSERT_EQ(2U, ra8_zoom_scale_cycle(1U, 1U, 3U));
  TEST_ASSERT_EQ(3U, ra8_zoom_scale_cycle(2U, 1U, 3U));
  TEST_ASSERT_EQ(1U, ra8_zoom_scale_cycle(3U, 1U, 3U));
  /* Degenerate inputs land inside the ladder rather than wrapping. */
  TEST_ASSERT_EQ(k_ra8_zoom_scale_min, ra8_zoom_scale_cycle(0U, 0U, 0U));
  TEST_ASSERT_EQ(4U, ra8_zoom_scale_cycle(0U, 4U, 8U));
  TEST_ASSERT_EQ(5U, ra8_zoom_scale_cycle(9U, 5U, 2U));
  TEST_END("scale_cycle_ladder");
}

/**
 * @test pan_clamps_and_marks_dirty
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@ra8_zoom_view_pan
 * `if ((v->anchor_x != was_x) || (v->anchor_y != was_y))` (2 conditions):
 * - V1: dx=0,  dy=0  from mid-source -> false (neither anchor moved: no flush)
 * - V2: dx=4,  dy=0                  -> true  (varies the x condition)
 * - V3: dx=0,  dy=4                  -> true  (varies the y condition)
 * V1+V2 prove the x anchor's independent influence; V1+V3 the y anchor's.
 * N+1 = 3 vectors for N=2 conditions.
 */
static void t_pan_clamps_and_marks_dirty(void)
{
  TEST_BEGIN("pan_clamps_and_marks_dirty");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_pan(nullptr, 0, 0, 0U));

  /* Park mid-source so a zero pan really is a no-move rather than a clamp. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 8, 8, 0U));
  ra8_zoom_present_t plan = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));

  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 0, 0, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(!plan.present);
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 4, 0, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(plan.present);
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 0, 4, 3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(plan.present);

  /* A pan far past the edge clamps, and panning again into the same edge owes
   * the panel nothing -- the property that stops an e-ink refresh per tap. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, k_t_big_delta, k_t_big_delta, 4U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  ra8_ui_rect_t win = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT_EQ(k_t_src_w, win.x + win.w);
  TEST_ASSERT_EQ(k_t_src_h, win.y + win.h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, k_t_big_delta, k_t_big_delta, 5U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(!plan.present);
  /* ...and the same at the other end, which is the negative saturation arm. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, -k_t_big_delta, -k_t_big_delta, 6U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT_EQ(0, win.x);
  TEST_ASSERT_EQ(0, win.y);
  TEST_END("pan_clamps_and_marks_dirty");
}

/**
 * @test pan_dir_steps_with_overlap
 */
static void t_pan_dir_steps_with_overlap(void)
{
  TEST_BEGIN("pan_dir_steps_with_overlap");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_pan_dir(nullptr, k_ra8_zoom_pan_left, 0U));

  const int32_t before = v.anchor_x;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan_dir(&v, k_ra8_zoom_pan_none, 0U));
  TEST_ASSERT_EQ(before, v.anchor_x);

  /* One step advances by the viewport less a one-eighth overlap. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan_dir(&v, k_ra8_zoom_pan_right, 1U));
  TEST_ASSERT_EQ(k_t_view_w - ((int32_t)k_t_view_w / 8), v.anchor_x);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan_dir(&v, k_ra8_zoom_pan_left, 2U));
  TEST_ASSERT_EQ(0, v.anchor_x);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan_dir(&v, k_ra8_zoom_pan_down, 3U));
  TEST_ASSERT_EQ(k_t_view_h - ((int32_t)k_t_view_h / 8), v.anchor_y);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan_dir(&v, k_ra8_zoom_pan_up, 4U));
  TEST_ASSERT_EQ(0, v.anchor_y);
  TEST_END("pan_dir_steps_with_overlap");
}

/**
 * @test tick_settles_to_quality
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom.c@ra8_zoom_view_tick
 * `if (!v->active || !v->settle_armed)` (2 conditions):
 * - V1: active=true,  armed=true  -> false (control: the settle can fire)
 * - V2: active=false, armed=true  -> true  (varies the active condition)
 * - V3: active=true,  armed=false -> true  (varies the armed condition)
 * V1+V2 prove `active`'s independent influence; V1+V3 prove `settle_armed`'s.
 * N+1 = 3 vectors for N=2 conditions.
 */
static void t_tick_settles_to_quality(void)
{
  TEST_BEGIN("tick_settles_to_quality");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT(!ra8_zoom_view_tick(nullptr, 0U));

  /* Opening owes a QUALITY flush regardless of policy: the first frame a
   * reader sees must be the good one. */
  ra8_zoom_present_t plan = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT_EQ(k_ra8_zoom_refresh_quality, plan.refresh);
  TEST_ASSERT_EQ(k_t_view_w, plan.rect.w);

  /* V3: nothing armed yet, so no settle can be due however long we wait. */
  TEST_ASSERT(!ra8_zoom_view_tick(&v, (uint32_t)k_t_settle * 10U));

  /* An interactive pan under the responsive policy owes a FAST flush... */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 4, 0, 1000U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT_EQ(k_ra8_zoom_refresh_fast, plan.refresh);
  /* ...and nothing more until the settle window has elapsed. */
  TEST_ASSERT(!ra8_zoom_view_tick(&v, 1000U + (uint32_t)k_t_settle - 1U));
  /* V1 */
  TEST_ASSERT(ra8_zoom_view_tick(&v, 1000U + (uint32_t)k_t_settle));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT_EQ(k_ra8_zoom_refresh_quality, plan.refresh);
  /* The settle disarms itself, so a reader repaints once, not per tick. */
  TEST_ASSERT(!ra8_zoom_view_tick(&v, 1000U + ((uint32_t)k_t_settle * 4U)));

  /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 4, 0, 2000U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_close(&v));
  TEST_ASSERT(!ra8_zoom_view_tick(&v, 2000U + ((uint32_t)k_t_settle * 4U)));

  /* Under the quality policy no bi-level frame is ever shown and nothing arms. */
  t_make_cfg(&cfg, &src);
  cfg.policy = k_ra8_zoom_policy_quality;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 4, 0, 3000U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_present(&v, &plan));
  TEST_ASSERT_EQ(k_ra8_zoom_refresh_quality, plan.refresh);
  TEST_ASSERT(!ra8_zoom_view_tick(&v, 3000U + ((uint32_t)k_t_settle * 4U)));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_present(&v, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_present(nullptr, &plan));
  TEST_END("tick_settles_to_quality");
}

/**
 * @test window_reports_visible_source
 */
static void t_window_reports_visible_source(void)
{
  TEST_BEGIN("window_reports_visible_source");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  ra8_ui_rect_t       win = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_window(&v, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_window(nullptr, &win));

  /* At 1:1 the viewport shows exactly its own extent of source. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT_EQ(0, win.x);
  TEST_ASSERT_EQ(k_t_view_w, win.w);
  TEST_ASSERT_EQ(k_t_view_h, win.h);

  /* At 4x it shows a quarter of that on each axis -- the residency claim. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_set_scale(&v, 4U, 0, 0, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT_EQ(k_t_view_w / 4, win.w);
  TEST_ASSERT_EQ(k_t_view_h / 4, win.h);

  /* A source smaller than the viewport is centred, and the window clips to it. */
  ra8_zoom_source_t tiny = {};
  t_make_src(&tiny, 8U, 8U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_set_scale(&v, 1U, 0, 0, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_rebind(&v, &tiny, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_window(&v, &win));
  TEST_ASSERT_EQ(0, win.x);
  TEST_ASSERT_EQ(8, win.w);
  TEST_ASSERT_EQ(8, win.h);
  TEST_ASSERT(v.anchor_x < 0);
  TEST_END("window_reports_visible_source");
}

/**
 * @test axis_resolves_geometry
 *
 * @details Exercises ::priv_zoom_axis directly, including the empty-coverage
 *          arm that a clamped view cannot reach through the public API but that
 *          the render's `count > 0` guard depends on.
 */
static void t_axis_resolves_geometry(void)
{
  TEST_BEGIN("axis_resolves_geometry");
  ra8_zoom_axis_t ax = {};

  /* Viewport inside a larger image at 1:1: fully covered, one source per dest. */
  priv_zoom_axis(k_t_axis_a, (int32_t)k_t_view_w, 1, (int32_t)k_t_src_w, &ax);
  TEST_ASSERT_EQ(0, ax.d0);
  TEST_ASSERT_EQ(k_t_view_w, ax.d1);
  TEST_ASSERT_EQ(k_t_axis_a, ax.s0);
  TEST_ASSERT_EQ(k_t_view_w, ax.count);

  /* Magnified: the same viewport now needs a quarter of the source columns. */
  priv_zoom_axis(k_t_axis_b, (int32_t)k_t_view_w, 4, (int32_t)k_t_src_w, &ax);
  TEST_ASSERT_EQ(k_t_axis_a, ax.s0);
  TEST_ASSERT_EQ(k_t_view_w / 4U, ax.count);

  /* Image narrower than the viewport: centred, with letterbox on both sides. */
  priv_zoom_axis(-(int32_t)k_t_narrow_d0, (int32_t)k_t_view_w, 1, (int32_t)k_t_narrow_w, &ax);
  TEST_ASSERT_EQ(k_t_narrow_d0, ax.d0);
  TEST_ASSERT_EQ(k_t_narrow_d1, ax.d1);
  TEST_ASSERT_EQ(0U, ax.s0);
  TEST_ASSERT_EQ(k_t_narrow_w, ax.count);

  /* Anchor beyond the image entirely: no coverage, and count is zero rather
   * than a wrapped span -- the arm the strip fill's guard relies on. */
  priv_zoom_axis(k_t_axis_off, (int32_t)k_t_view_w, 1, (int32_t)k_t_src_w, &ax);
  TEST_ASSERT_EQ(0, ax.d0);
  TEST_ASSERT_EQ(0, ax.d1);
  TEST_ASSERT_EQ(0U, ax.count);
  TEST_END("axis_resolves_geometry");
}

/**
 * @brief Read one framebuffer pixel as its gray level (all channels are equal).
 * @param[in] x Framebuffer column.
 * @param[in] y Framebuffer row.
 * @return The blue channel, which equals the gray level written by ra8_gfx.
 */
static uint8_t t_fb_gray(int32_t x, int32_t y)
{
  return (uint8_t)(s_fb[((size_t)y * (size_t)k_t_fb_w) + (size_t)x] & (uint32_t)k_t_byte_mask);
}

/**
 * @brief The 2x and letterbox halves of ::t_render_magnifies_and_dithers.
 * @details Split out only to stay inside the 60-line function cap; the MC/DC
 *          vector labels (V2..V4) are continuous with the parent test's block.
 * @param[in,out] v An open view over the standard 64x48 synthetic source.
 */
static void t_render_magnified_and_letterboxed(ra8_zoom_view_t* v)
{
  /* 2x: each source pixel becomes an exact 2x2 block of destination pixels,
   * and the grain is re-rolled per destination pixel (dithered AFTER the
   * magnify), so the four pixels of a block need not be equal. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_zoom_view_set_scale(v, 2U, (int32_t)k_t_dst_x, (int32_t)k_t_dst_y, 0U));
  s_reads = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(v));
  TEST_ASSERT_EQ(k_t_view_h / 2U, s_reads);
  for (int32_t r = 0; r < (int32_t)k_t_view_h; ++r) {
    for (int32_t c = 0; c < (int32_t)k_t_view_w; ++c) {
      const uint8_t sample =
        t_sample((uint32_t)(v->anchor_x + c) / 2U, (uint32_t)(v->anchor_y + r) / 2U);
      const uint8_t level = ra8_gfx_dither_gray4_level(sample, v->anchor_x + c, v->anchor_y + r);
      const uint8_t want  = (uint8_t)((level << 4) | level);
      TEST_ASSERT_EQ(want, t_fb_gray((int32_t)k_t_dst_x + c, (int32_t)k_t_dst_y + r));
    }
  }

  /* V2 / V3 / V4: a source shorter and narrower than the viewport is centred,
   * and every uncovered destination pixel holds the background level rather
   * than whatever the framebuffer happened to contain. */
  ra8_zoom_source_t small = {};
  t_make_src(&small, 8U, 8U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_set_scale(v, 1U, 0, 0, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_rebind(v, &small, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(v));
  const uint8_t bg_level = ra8_gfx_dither_gray4_level((uint8_t)k_ra8_zoom_bg_gray, 0, 0);
  const uint8_t bg       = (uint8_t)((bg_level << 4) | bg_level);
  TEST_ASSERT_EQ(bg, t_fb_gray((int32_t)k_t_dst_x, (int32_t)k_t_dst_y));
  TEST_ASSERT_EQ(bg,
                 t_fb_gray(((int32_t)k_t_dst_x + (int32_t)k_t_view_w) - 1,
                           ((int32_t)k_t_dst_y + (int32_t)k_t_view_h) - 1));

  /* A view whose strip height was never derived refuses to render rather than
   * looping forever on a zero-height strip (NASA P10 Rule 2). */
  ra8_zoom_view_t broken = *v;
  broken.strip_rows      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_zoom_view_render(&broken));

  /* internal_fill_strip's horizontal-coverage guard. No public call can produce it
   * -- every mutator clamps the anchor so the image always intersects the
   * viewport -- so it is reached by mutating the anchor directly, which is the
   * only honest way to exercise a defensive guard that is kept because the
   * source seam's `w > 0` contract depends on it. The whole viewport must come
   * back as background. */
  ra8_zoom_view_t adrift = *v;
  adrift.anchor_x        = k_t_axis_off;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(&adrift));
  const uint8_t off_level = ra8_gfx_dither_gray4_level((uint8_t)k_ra8_zoom_bg_gray, 0, 0);
  TEST_ASSERT_EQ(((off_level << 4) | off_level),
                 t_fb_gray(k_t_dst_x + (k_t_view_w / 2), k_t_dst_y + (k_t_view_h / 2)));
}

/**
 * @test render_magnifies_and_dithers
 *
 * @par MC/DC:
 * Decision libs/ra8_zoom/src/ra8_zoom_render.c@internal_fill_row
 * `const bool covered = (sy >= 0) && (sy < height)` (2 conditions):
 * - V1: sy in [0, height) -> true  (control: the row shows image)
 * - V2: sy = -1, i.e. a plane row ABOVE the image -> false (varies `sy >= 0`;
 *       reached by a source shorter than the viewport, which centres and puts
 *       the top rows above the image)
 * - V3: sy past the last row -> false (varies `sy < height`; the bottom
 *       letterbox rows of that same centred view)
 * V1+V2 prove the first condition's independent influence; V1+V3 the second's.
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * The horizontal-coverage test is deliberately NOT part of this decision: it is
 * constant across a frame, so ::internal_fill_strip settles it once as a
 * single-condition guard (exercised below by a hand-built off-image view)
 * rather than as a third condition here that could never vary row to row.
 *
 * @details Beyond MC/DC this asserts the render itself: at 1:1 every destination
 *          pixel is the dithered value of its own source pixel, at 2x each
 *          source pixel occupies an exact 2x2 block, and the letterbox is the
 *          background level rather than stale framebuffer content.
 */
static void t_render_magnifies_and_dithers(void)
{
  TEST_BEGIN("render_magnifies_and_dithers");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  t_bind_fb();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_zoom_view_render(nullptr));

  /* V1: at 1:1, destination (c, r) must be the dithered source pixel (c, r),
   * with the mask phased on the plane coordinate. */
  s_reads = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(&v));
  for (int32_t r = 0; r < (int32_t)k_t_view_h; ++r) {
    for (int32_t c = 0; c < (int32_t)k_t_view_w; ++c) {
      const uint8_t level = ra8_gfx_dither_gray4_level(t_sample((uint32_t)c, (uint32_t)r), c, r);
      const uint8_t want  = (uint8_t)((level << 4) | level);
      TEST_ASSERT_EQ(want, t_fb_gray((int32_t)k_t_dst_x + c, (int32_t)k_t_dst_y + r));
    }
  }
  /* One source read per distinct source row, and not one more: the strip
   * pipeline must not re-fault a row it already holds, including across the
   * strip boundary at every k_t_strip rows. */
  TEST_ASSERT_EQ(k_t_view_h, s_reads);

  t_render_magnified_and_letterboxed(&v);
  TEST_END("render_magnifies_and_dithers");
}

/**
 * @test render_propagates_source_failure
 */
static void t_render_propagates_source_failure(void)
{
  TEST_BEGIN("render_propagates_source_failure");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  t_bind_fb();

  /* A reader failure aborts the frame and is returned verbatim -- the viewer
   * never paints half an image and calls it success. */
  s_reads   = 0U;
  s_fail_on = 3U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_zoom_view_render(&v));
  s_fail_on = 0U;

  /* The next frame starts from a clean row cache, so the row the failed call
   * abandoned is re-read rather than reused. */
  s_reads = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(&v));
  TEST_ASSERT_EQ(k_t_view_h, s_reads);
  TEST_END("render_propagates_source_failure");
}

/**
 * @test dither_phase_is_pan_stable
 *
 * @details The property the magnified-plane coordinate model exists for. A
 *          source pixel must keep the same dithered level when the viewport
 *          moves, or the grain re-rolls on every pan step and an e-ink panel
 *          shimmers. Renders the same source pixel at two different anchors and
 *          asserts the framebuffer value is identical.
 */
static void t_dither_phase_is_pan_stable(void)
{
  TEST_BEGIN("dither_phase_is_pan_stable");
  ra8_zoom_source_t   src = {};
  ra8_zoom_view_cfg_t cfg = {};
  ra8_zoom_view_t     v   = {};
  t_make_src(&src, k_t_src_w, k_t_src_h);
  t_make_cfg(&cfg, &src);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_open(&v, &cfg));
  t_bind_fb();

  /* Source pixel (20, 10) at anchor 0: destination (20, 10) of the viewport. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(&v));
  const uint8_t before = t_fb_gray((int32_t)k_t_dst_x + 20, (int32_t)k_t_dst_y + 10);

  /* Pan by (7, 3) -- deliberately not a multiple of the 64-px mask period --
   * and the same source pixel lands at a different destination pixel. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_pan(&v, 7, 3, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_zoom_view_render(&v));
  const uint8_t after = t_fb_gray(((int32_t)k_t_dst_x + 20) - 7, ((int32_t)k_t_dst_y + 10) - 3);
  TEST_ASSERT_EQ(before, after);
  TEST_END("dither_phase_is_pan_stable");
}

/**
 * @brief Host test entry point.
 * @return 0 on success; any assertion `exit(1)`s before returning.
 */
int main(void)
{
  t_source_init_validates();
  t_view_open_validates();
  t_view_active_and_close();
  t_rebind_keeps_zoom();
  t_set_scale_keeps_focus();
  t_scale_cycle_ladder();
  t_pan_clamps_and_marks_dirty();
  t_pan_dir_steps_with_overlap();
  t_tick_settles_to_quality();
  t_window_reports_visible_source();
  t_axis_resolves_geometry();
  t_render_magnifies_and_dithers();
  t_render_propagates_source_failure();
  t_dither_phase_is_pan_stable();
  return 0;
}
