/**
 * @file examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c
 * @brief Tap-to-zoom demo scene: page sampler, viewports, chrome, self-check.
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * Implements ez_scene.h. Every pixel this file puts on the panel comes from
 * integer arithmetic -- the procedural page sampler, the ra8_zoom composite, and
 * filled rectangles for the chrome -- so the framebuffer hash it reports is the
 * same number on the unit-test host, in ra8_emulator, and on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ez_scene.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_tile_cache.h"
#include "ra8_ui.h"
#include "ra8_zoom.h"
#include "ra8_zoom_tiles.h"

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "ereader_zoom";

/*
 * ::k_ez_cells is DERIVED from the panel and tile geometry, never hand-picked
 * (#338). At 1:1 the 1024x552 content window over 256x256 tiles straddles at
 * most ceil(1024/256)+1 = 5 columns and ceil(552/256)+1 = 4 rows; the enum
 * carries one more column than that so a horizontal pan step (which is nearly a
 * whole viewport) still finds the union of before/after resident. Sizing the
 * cache to that frame plus one tile of margin on each axis means a pan
 * re-decodes only the newly exposed row or column, never a tile still on screen.
 * The first assert fails the build if the derivation is ever undercut; the
 * second bounds the resident arena so it cannot silently balloon.
 */
static_assert((uint64_t)k_ez_cells >= ((uint64_t)k_ez_view_cols * (uint64_t)k_ez_view_rows),
              "tile cache is smaller than the 1:1 viewport tile demand: re-derive k_ez_cells "
              "from the panel + tile geometry so a single-step pan cannot thrash (#338)");
static_assert((uint64_t)k_ez_cells * (uint64_t)k_ez_cell_bytes <= (uint64_t)k_ez_cell_budget_bytes,
              "resident tile arena exceeds its SDRAM budget");
static_assert((k_ez_page_w % k_ez_tile_edge) == 0U,
              "page width must tile exactly: the demo asserts a full-tile geometry");
static_assert((k_ez_page_h % k_ez_tile_edge) == 0U,
              "page height must tile exactly: the demo asserts a full-tile geometry");
/*
 * The composite scratch is sized for the WIDEST viewport and shared by both
 * views, which is only sound while the packed buffer can hold a full strip of
 * that width. 1024 x 16 rows packed at 2 px/byte is exactly k_ez_packed_bytes.
 */
static_assert((uint64_t)k_ez_packed_bytes >= (((uint64_t)k_ez_strip_bytes + 1U) / 2U),
              "packed scratch cannot hold one dithered strip of the widest viewport");
static_assert((uint64_t)k_ez_lens_edge < (uint64_t)k_ez_row_bytes,
              "the loupe must be narrower than the shared composite row buffer");

/**
 * @enum ez_tone_t
 * @brief Gray8 tones and the shift/mask geometry of the procedural page.
 * @details The background is a triangle wave whose full cycle is
 *          `(1 << k_ez_grad_shift) * (k_ez_grad_wrap + 1)` = 2048 px along the
 *          diagonal -- a 64-level swing across roughly a screen, which a
 *          16-level panel would band visibly without the blue-noise dither, and
 *          which is exactly what the demo is showing. A triangle rather than a
 *          sawtooth because a sawtooth's wrap is a genuine tone discontinuity in
 *          the source and would be mistaken for the artefact. The rules are 3 px
 *          tall on a 32 px pitch with one blank run in eight, so at 1:1 they
 *          read as texture and at 4x as structure.
 * @invariant k_ez_bg_lo + k_ez_grad_mask <= 255.
 * @invariant k_ez_grad_wrap == (2 * k_ez_grad_mask) + 1.
 * @par Example:
 * @code
 * const uint32_t phase = ((x >> k_ez_grad_shift) + (y >> k_ez_grad_shift)) & k_ez_grad_wrap;
 * @endcode
 * @see ez_page_sample
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ez_bg_lo      = 140U, /**< Darkest background tone.                     */
  k_ez_grad_shift = 4U,   /**< Pixels per gradient step (1 << shift).       */
  k_ez_grad_mask  = 63U,  /**< Gradient span above ::k_ez_bg_lo.            */
  k_ez_grad_wrap  = 127U, /**< Triangle-wave period - 1 (2 * span + 1).     */
  k_ez_ink        = 24U,  /**< Rule (text) tone.                            */
  k_ez_rule_mask  = 31U,  /**< Rule pitch - 1, rows.                        */
  k_ez_rule_thick = 3U,   /**< Rule thickness, rows.                        */
  k_ez_word_shift = 5U,   /**< Word run length (1 << shift), columns.       */
  k_ez_word_mask  = 7U,   /**< Words per group - 1 (one blank per group).   */
  k_ez_word_blank = 7U,   /**< Group index left blank (the inter-word gap). */
} ez_tone_t;

/**
 * @enum ez_chrome_t
 * @brief Chrome colours, in the 0x00RRGGBB space ra8_gfx down-converts from.
 * @details Grey levels on the panel's 16-level palette (multiples of 17
 *          replicated across the three channels) so the chrome never dithers.
 * @invariant Every value is an opaque 0x00RRGGBB triple.
 * @par Example:
 * @code
 * (void)ra8_gfx_rect(0, 0, w, k_ez_status_h, (uint32_t)k_ez_col_bar, true);
 * @endcode
 * @see ez_draw_chrome
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ez_col_bar  = 0x00222222U, /**< Status-bar fill.            */
  k_ez_col_ind  = 0x00DDDDDDU, /**< Lit zoom-indicator block.   */
  k_ez_col_dim  = 0x00555555U, /**< Unlit zoom-indicator block. */
  k_ez_col_lens = 0x00000000U, /**< Loupe border.               */
} ez_chrome_t;

/**
 * @enum ez_fnv_t
 * @brief FNV-1a-32 parameters.
 * @details The standard 32-bit offset basis and prime; named so the hash is not
 *          two unexplained constants in the middle of a loop.
 * @invariant k_ez_fnv_prime is the 32-bit FNV prime, 16777619.
 * @par Example:
 * @code
 * uint32_t h = (uint32_t)k_ez_fnv_offset;
 * @endcode
 * @see ez_fnv1a
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ez_fnv_offset = 2166136261U, /**< FNV-1a-32 offset basis. */
  k_ez_fnv_prime  = 16777619U,   /**< FNV-1a-32 prime.        */
} ez_fnv_t;

/**
 * @enum ez_selftest_step_t
 * @brief The scripted self-check's fixed inputs.
 * @details Held as named constants so the app, the host twin and `hil.conf`
 *          cannot drift on "which pan, which focus point": the sequence is part
 *          of the golden, not an incidental of whoever wrote the test.
 * @invariant k_ez_st_focus_x / _y lie inside the content rectangle.
 * @par Example:
 * @code
 * (void)ra8_zoom_view_set_scale(&s->page, k_ez_st_scale, k_ez_st_focus_x, k_ez_st_focus_y, t);
 * @endcode
 * @see ez_scene_selftest
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ez_st_focus_x = 700U, /**< Panel column the 2x step keeps fixed. */
  k_ez_st_focus_y = 400U, /**< Panel row the 2x step keeps fixed.    */
  k_ez_st_scale   = 2U,   /**< Magnification the 2x step selects.    */
  k_ez_st_t0      = 0U,   /**< Timestamp handed to every step (ms).  */
} ez_selftest_step_t;

uint8_t ez_page_sample(uint32_t x, uint32_t y)
{
  /* A TRIANGLE wave, not a sawtooth: a sawtooth's wrap is a real tone
   * discontinuity in the source, and it would read on the panel as exactly the
   * banding artefact this demo exists to show the dither removing. */
  const uint32_t phase = ((x >> (uint32_t)k_ez_grad_shift) + (y >> (uint32_t)k_ez_grad_shift)) &
                         (uint32_t)k_ez_grad_wrap;
  const uint32_t band =
    (phase <= (uint32_t)k_ez_grad_mask) ? phase : ((uint32_t)k_ez_grad_wrap - phase);
  const bool on_rule = ((y & (uint32_t)k_ez_rule_mask) < (uint32_t)k_ez_rule_thick);
  const bool inked =
    (((x >> (uint32_t)k_ez_word_shift) & (uint32_t)k_ez_word_mask) != (uint32_t)k_ez_word_blank);
  if (on_rule && inked) {
    return (uint8_t)k_ez_ink;
  }
  return (uint8_t)((uint32_t)k_ez_bg_lo + band);
}

/**
 * @brief Write one whole tile's pixels from the procedural page sampler.
 * @details Split out of ::ez_tile_decode so the decode entry point is its five
 *          precondition checks and nothing else. The offset is computed in
 *          `size_t` because a tile index times the tile edge is a pointer
 *          offset, and doing that arithmetic in 32 bits before the implicit
 *          widening is how a large image silently wraps.
 * @param[out] cell  Destination cell, at least ::k_ez_cell_bytes writable bytes.
 * @param[in]  org_x Tile origin column in page pixels.
 * @param[in]  org_y Tile origin row in page pixels.
 * @return Nothing.
 * @pre  @p cell addresses at least ::k_ez_cell_bytes writable bytes.
 * @pre  The tile origin lies inside the page.
 * @post Every byte of the tile holds its sampled page value.
 * @post Nothing outside the tile is written.
 * @note Not thread-safe (writes @p cell); otherwise pure.
 * @since 0.1.0
 */
static void ez_fill_tile(uint8_t* cell, uint32_t org_x, uint32_t org_y)
{
  for (uint32_t r = 0U; r < (uint32_t)k_ez_tile_edge; ++r) {
    uint8_t* row = &cell[(size_t)r * (size_t)k_ez_tile_edge];
    for (uint32_t c = 0U; c < (uint32_t)k_ez_tile_edge; ++c) {
      row[c] = ez_page_sample(org_x + c, org_y + r);
    }
  }
}

ra8_err_t ez_tile_decode(void*                 ctx,
                         const ra8_tile_key_t* key,
                         uint8_t*              cell,
                         uint32_t              cell_bytes,
                         uint16_t*             out_w,
                         uint16_t*             out_h)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(key, s_tag, "tile key must not be nullptr");
  RA8_CHECK_NULL_PTR(cell, s_tag, "tile cell must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  if (cell_bytes < (uint32_t)k_ez_cell_bytes) {
    ra8_log_error(s_tag, "tile cell is smaller than one page tile");
    return k_ra8_err_no_mem;
  }
  const uint32_t org_x = (uint32_t)key->tile_x * (uint32_t)k_ez_tile_edge;
  const uint32_t org_y = (uint32_t)key->tile_y * (uint32_t)k_ez_tile_edge;
  if ((org_x >= (uint32_t)k_ez_page_w) || (org_y >= (uint32_t)k_ez_page_h)) {
    ra8_log_error(s_tag, "tile key names a tile outside the page");
    return k_ra8_err_out_of_range;
  }
  ez_fill_tile(cell, org_x, org_y);
  *out_w = (uint16_t)k_ez_tile_edge;
  *out_h = (uint16_t)k_ez_tile_edge;
  return k_ra8_ok;
}

ra8_ui_rect_t ez_content_rect(int32_t fb_w, int32_t fb_h)
{
  const ra8_ui_rect_t r = {
    .x = 0,
    .y = (int32_t)k_ez_status_h,
    .w = fb_w,
    .h = fb_h - (int32_t)k_ez_status_h,
  };
  return r;
}

/**
 * @brief The loupe box: a square centred in the content rectangle.
 * @param[in] content The content rectangle.
 * @return The lens rectangle in framebuffer coordinates.
 * @retval "centred square" Always ::k_ez_lens_edge on a side.
 * @pre  @p content is at least ::k_ez_lens_edge on both axes.
 * @pre  @p content is the rectangle the page view paints.
 * @post The result lies wholly inside @p content.
 * @post No state is modified (pure function).
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static ra8_ui_rect_t ez_lens_rect(ra8_ui_rect_t content)
{
  const ra8_ui_rect_t r = {
    .x = content.x + ((content.w - (int32_t)k_ez_lens_edge) / 2),
    .y = content.y + ((content.h - (int32_t)k_ez_lens_edge) / 2),
    .w = (int32_t)k_ez_lens_edge,
    .h = (int32_t)k_ez_lens_edge,
  };
  return r;
}

/**
 * @brief Build the shared composite-scratch descriptor from a configuration.
 * @param[in] cfg Scene configuration holding the three borrowed buffers.
 * @return The scratch descriptor both viewports use.
 * @retval "descriptor" Always; validation happens in ra8_zoom_view_open().
 * @pre  Every scratch pointer in @p cfg is non-NULL.
 * @pre  The buffers outlive the scene.
 * @post The descriptor's capacities match the ez_scratch_t enum.
 * @post No state is modified.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static ra8_zoom_scratch_t ez_scratch_of(const ez_scene_cfg_t* cfg)
{
  const ra8_zoom_scratch_t sc = {
    .row        = cfg->row,
    .row_cap    = (uint32_t)k_ez_row_bytes,
    .strip      = cfg->strip,
    .strip_cap  = (uint32_t)k_ez_strip_bytes,
    .packed     = cfg->packed,
    .packed_cap = (uint32_t)k_ez_packed_bytes,
  };
  return sc;
}

/**
 * @brief Validate the three composite-scratch pointers.
 * @details The second half of ::ez_cfg_ptrs_ok, split only so each stays inside
 *          the project's function-size bar -- every RA8_CHECK_NULL_PTR is
 *          several statements once expanded, so eight of them in one function
 *          is over the ceiling on their own.
 * @param[in] cfg Candidate configuration (already known non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           All three scratch pointers are present.
 * @retval k_ra8_err_null_ptr One is missing; the log line names it.
 * @pre  @p cfg is non-NULL.
 * @pre  The cache-storage half has already passed.
 * @post No state is modified.
 * @post On k_ra8_ok the composite can be bound to both viewports.
 * @note Not thread-safe (logs).
 * @since 0.1.0
 */
static ra8_err_t ez_cfg_scratch_ok(const ez_scene_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->row, s_tag, "row scratch must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->strip, s_tag, "strip scratch must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->packed, s_tag, "packed scratch must not be nullptr");
  return k_ra8_ok;
}

/**
 * @brief Validate the borrowed storage a scene configuration must carry.
 * @param[in] cfg Candidate configuration (already known non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Every pointer is present.
 * @retval k_ra8_err_null_ptr One is missing; the log line names it.
 * @pre  @p cfg is non-NULL.
 * @pre  Nothing in @p cfg has been published into a scene yet.
 * @post No state is modified.
 * @post On k_ra8_ok all eight storage pointers are non-NULL.
 * @note Not thread-safe (logs).
 * @since 0.1.0
 */
static ra8_err_t ez_cfg_ptrs_ok(const ez_scene_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->fb, s_tag, "framebuffer must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->cell_mem, s_tag, "cell_mem must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->meta, s_tag, "meta must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->keys, s_tag, "keys must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->dims, s_tag, "dims must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->buckets, s_tag, "buckets must not be nullptr");
  return ez_cfg_scratch_ok(cfg);
}

/**
 * @brief Open both viewports over the bound page source.
 * @param[in,out] s   Scene whose cache and source are already wired.
 * @param[in]     cfg Scene configuration (for the scratch buffers).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Both views are open and owe a quality flush.
 * @retval k_ra8_err_* Propagated from ::ra8_zoom_view_open.
 * @pre  `s->src` is bound and `s->content` is set.
 * @pre  The scratch buffers outlive the scene.
 * @post On k_ra8_ok the page view covers `s->content` and the loupe sits inside it.
 * @post On any error neither view is left half-open.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_open_views(ez_scene_t* s, const ez_scene_cfg_t* cfg)
{
  const ra8_zoom_scratch_t  scratch  = ez_scratch_of(cfg);
  const ra8_zoom_view_cfg_t page_cfg = {
    .src       = s->src,
    .scratch   = scratch,
    .dst       = s->content,
    .scale     = (uint8_t)k_ra8_zoom_scale_min,
    .scale_max = (uint8_t)k_ez_page_scale_max,
    .policy    = k_ra8_zoom_policy_responsive,
    .settle_ms = 0U,
    .focus_x   = 0,
    .focus_y   = 0,
  };
  RA8_RETURN_ON_ERROR(ra8_zoom_view_open(&s->page, &page_cfg), s_tag, "page view open");

  const ra8_ui_rect_t       lens     = ez_lens_rect(s->content);
  const ra8_zoom_view_cfg_t lens_cfg = {
    .src       = s->src,
    .scratch   = scratch,
    .dst       = lens,
    .scale     = (uint8_t)k_ez_lens_scale_min,
    .scale_max = (uint8_t)k_ez_lens_scale_max,
    .policy    = k_ra8_zoom_policy_responsive,
    .settle_ms = 0U,
    .focus_x   = 0,
    .focus_y   = 0,
  };
  return ra8_zoom_view_open(&s->lens, &lens_cfg);
}

/**
 * @brief Wire the tile cache over the borrowed storage and bind it as a source.
 * @details Split out of ::ez_scene_init so that function reads as "validate,
 *          lay out, bind the page, open the views". The tile grid is derived by
 *          ::ra8_zoom_tile_src_init from the page extent rather than passed in,
 *          so it cannot disagree with the geometry it is meant to cover.
 * @param[in,out] s   Scene whose layout is already computed.
 * @param[in]     cfg Validated configuration supplying the cache storage.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    `s->src` is a bound, magnifiable page.
 * @retval k_ra8_err_* Propagated from the tile cache or the tiled adapter.
 * @pre  @p cfg passed ::ez_cfg_ptrs_ok.
 * @pre  Every borrowed buffer outlives the scene.
 * @post On k_ra8_ok the cache is empty and every cell is cold.
 * @post On any error @p s holds no usable source.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_bind_page(ez_scene_t* s, const ez_scene_cfg_t* cfg)
{
  const ra8_tile_cache_cfg_t cache_cfg = {
    .cell_mem     = cfg->cell_mem,
    .cell_bytes   = (uint32_t)k_ez_cell_bytes,
    .cell_count   = (uint32_t)k_ez_cells,
    .meta         = cfg->meta,
    .keys         = cfg->keys,
    .dims         = cfg->dims,
    .buckets      = cfg->buckets,
    .bucket_count = (uint32_t)k_ez_buckets,
    .decode       = ez_tile_decode,
    .decode_ctx   = nullptr,
  };
  RA8_RETURN_ON_ERROR(ra8_tile_cache_init(&s->cache, &cache_cfg), s_tag, "tile cache init");
  RA8_RETURN_ON_ERROR(ra8_zoom_tile_src_init(&s->tiles,
                                             &s->cache,
                                             (uint32_t)k_ez_image_id,
                                             (uint32_t)k_ez_page_w,
                                             (uint32_t)k_ez_page_h,
                                             (uint16_t)k_ez_tile_edge,
                                             (uint16_t)k_ez_tile_edge),
                      s_tag,
                      "tiled source init");
  return ra8_zoom_tile_src_bind(&s->tiles, &s->src);
}

ra8_err_t ez_scene_init(ez_scene_t* s, const ez_scene_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  s->lens_on = false;
  RA8_RETURN_ON_ERROR(ez_cfg_ptrs_ok(cfg), s_tag, "cfg pointer check");
  if ((cfg->fb_w <= (int32_t)k_ez_lens_edge) ||
      (cfg->fb_h <= ((int32_t)k_ez_status_h + (int32_t)k_ez_lens_edge))) {
    ra8_log_error(s_tag, "framebuffer is too small for the demo layout");
    return k_ra8_err_invalid_arg;
  }
  s->fb       = cfg->fb;
  s->fb_bytes = cfg->fb_bytes;
  s->fb_w     = cfg->fb_w;
  s->fb_h     = cfg->fb_h;
  s->content  = ez_content_rect(cfg->fb_w, cfg->fb_h);

  RA8_RETURN_ON_ERROR(ez_bind_page(s, cfg), s_tag, "page binding");
  return ez_open_views(s, cfg);
}

ez_zone_t ez_zone_hit(const ez_scene_t* s, int32_t x, int32_t y)
{
  if (s == nullptr) {
    return k_ez_zone_none;
  }
  if (y < (int32_t)k_ez_status_h) {
    return k_ez_zone_toggle;
  }
  if (!ra8_ui_rect_contains(&s->content, x, y)) {
    return k_ez_zone_none;
  }
  const ra8_ui_rect_t lens = ez_lens_rect(s->content);
  if (s->lens_on && ra8_ui_rect_contains(&lens, x, y)) {
    return k_ez_zone_lens;
  }
  if (x < (s->content.x + (int32_t)k_ez_edge_band)) {
    return k_ez_zone_pan_left;
  }
  if (x >= ((s->content.x + s->content.w) - (int32_t)k_ez_edge_band)) {
    return k_ez_zone_pan_right;
  }
  if (y < (s->content.y + (int32_t)k_ez_edge_band)) {
    return k_ez_zone_pan_up;
  }
  if (y >= ((s->content.y + s->content.h) - (int32_t)k_ez_edge_band)) {
    return k_ez_zone_pan_down;
  }
  return k_ez_zone_zoom;
}

/**
 * @brief Apply a pan zone to the page viewport.
 * @param[in,out] s      Initialised scene.
 * @param[in]     zone   A pan zone (any other zone is a no-op).
 * @param[in]     now_ms Current millisecond timestamp.
 * @return Whether the page anchor moved.
 * @retval true  The page panned; a redraw is due.
 * @retval false The pan clamped against an edge, or the zone was not a pan.
 * @pre  @p s was initialised by ::ez_scene_init.
 * @pre  @p now_ms comes from a monotonic millisecond source.
 * @post A true return leaves the page view owing a flush.
 * @post The read-ahead is only issued for a pan that actually moved.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool ez_apply_pan(ez_scene_t* s, ez_zone_t zone, uint32_t now_ms)
{
  ra8_zoom_pan_t dir = k_ra8_zoom_pan_none;
  switch (zone) {
    case k_ez_zone_pan_left:
      dir = k_ra8_zoom_pan_left;
      break;
    case k_ez_zone_pan_right:
      dir = k_ra8_zoom_pan_right;
      break;
    case k_ez_zone_pan_up:
      dir = k_ra8_zoom_pan_up;
      break;
    case k_ez_zone_pan_down:
      dir = k_ra8_zoom_pan_down;
      break;
    default:
      return false;
  }
  ra8_ui_rect_t before = {};
  ra8_ui_rect_t after  = {};
  (void)ra8_zoom_view_window(&s->page, &before);
  if (ra8_zoom_view_pan_dir(&s->page, dir, now_ms) != k_ra8_ok) {
    return false;
  }
  (void)ra8_zoom_view_window(&s->page, &after);
  if ((after.x == before.x) && (after.y == before.y)) {
    return false;
  }
  /* Idle-window read-ahead: warm the tiles the next step in this direction
   * will expose, out of the cache's spare margin (#341). */
  (void)ez_scene_prefetch(s, dir, nullptr);
  return true;
}

bool ez_scene_tap(ez_scene_t* s, int32_t x, int32_t y, uint32_t now_ms)
{
  if (s == nullptr) {
    return false;
  }
  const ez_zone_t zone = ez_zone_hit(s, x, y);
  if (zone == k_ez_zone_toggle) {
    s->lens_on = !s->lens_on;
    /* The loupe covers page pixels, so opening or closing it damages the whole
     * content rectangle -- damage the engine cannot see, because neither the
     * anchor nor the scale moved. Say so explicitly: a pan of (0, 0) would
     * correctly report "nothing moved" and the lens would never be flushed to
     * an e-ink panel (a continuously-scanned LCD would hide the bug entirely). */
    return ra8_zoom_view_invalidate(&s->page, now_ms) == k_ra8_ok;
  }
  if (zone == k_ez_zone_lens) {
    const uint8_t next = ra8_zoom_scale_cycle(s->lens.scale,
                                              (uint8_t)k_ez_lens_scale_min,
                                              (uint8_t)k_ez_lens_scale_max);
    return ra8_zoom_view_set_scale(&s->lens, next, x, y, now_ms) == k_ra8_ok;
  }
  if (zone == k_ez_zone_zoom) {
    const uint8_t next = ra8_zoom_scale_cycle(s->page.scale,
                                              (uint8_t)k_ra8_zoom_scale_min,
                                              (uint8_t)k_ez_page_scale_max);
    return ra8_zoom_view_set_scale(&s->page, next, x, y, now_ms) == k_ra8_ok;
  }
  return ez_apply_pan(s, zone, now_ms);
}

/**
 * @brief Paint the status bar and its block zoom indicator.
 * @param[in] s Initialised scene.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The bar is painted.
 * @retval k_ra8_err_* Propagated from ::ra8_gfx_rect.
 * @pre  ::ra8_gfx_init has bound the framebuffer.
 * @pre  @p s was initialised by ::ez_scene_init.
 * @post The bar shows one lit block per magnification step of the page view.
 * @post No pixel outside the status bar is written.
 * @note Not thread-safe; writes the ra8_gfx framebuffer binding. Deliberately
 *       glyph-free so the framebuffer hash stays toolchain-independent.
 * @since 0.1.0
 */
static ra8_err_t ez_draw_status(const ez_scene_t* s)
{
  RA8_RETURN_ON_ERROR(
    ra8_gfx_rect(0, 0, s->fb_w, (int32_t)k_ez_status_h, (uint32_t)k_ez_col_bar, true),
    s_tag,
    "status bar fill");
  const int32_t top   = ((int32_t)k_ez_status_h - (int32_t)k_ez_ind_block) / 2;
  const int32_t pitch = (int32_t)k_ez_ind_block + (int32_t)k_ez_ind_gap;
  for (int32_t i = 0; i < (int32_t)k_ez_page_scale_max; ++i) {
    const uint32_t colour =
      (i < (int32_t)s->page.scale) ? (uint32_t)k_ez_col_ind : (uint32_t)k_ez_col_dim;
    RA8_RETURN_ON_ERROR(ra8_gfx_rect(top + (i * pitch),
                                     top,
                                     (int32_t)k_ez_ind_block,
                                     (int32_t)k_ez_ind_block,
                                     colour,
                                     true),
                        s_tag,
                        "zoom indicator block");
  }
  return k_ra8_ok;
}

/**
 * @brief Paint the loupe's border so the lens reads as a lens, not a seam.
 * @param[in] lens The lens rectangle.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The border is painted.
 * @retval k_ra8_err_* Propagated from ::ra8_gfx_rect.
 * @pre  ::ra8_gfx_init has bound the framebuffer.
 * @pre  @p lens lies inside the framebuffer.
 * @post Only the ::k_ez_lens_border-thick frame of @p lens is written.
 * @post The magnified pixels inside the frame are left untouched.
 * @note Not thread-safe; writes the ra8_gfx framebuffer binding.
 * @since 0.1.0
 */
static ra8_err_t ez_draw_lens_chrome(ra8_ui_rect_t lens)
{
  for (int32_t i = 0; i < (int32_t)k_ez_lens_border; ++i) {
    RA8_RETURN_ON_ERROR(ra8_gfx_rect(lens.x + i,
                                     lens.y + i,
                                     lens.w - (2 * i),
                                     lens.h - (2 * i),
                                     (uint32_t)k_ez_col_lens,
                                     false),
                        s_tag,
                        "lens border");
  }
  return k_ra8_ok;
}

ra8_err_t ez_scene_render(ez_scene_t* s)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(s->src.read, s_tag, "scene source must not be nullptr");
  RA8_RETURN_ON_ERROR(ra8_zoom_view_render(&s->page), s_tag, "page render");
  if (s->lens_on) {
    RA8_RETURN_ON_ERROR(ra8_zoom_view_render(&s->lens), s_tag, "lens render");
    RA8_RETURN_ON_ERROR(ez_draw_lens_chrome(ez_lens_rect(s->content)), s_tag, "lens chrome");
  }
  return ez_draw_status(s);
}

/**
 * @brief Pick the flush rectangle from the two views' drained plans.
 * @details The page wins when it owes anything, because the loupe is drawn over
 *          page pixels and a page repaint therefore damages the lens box too.
 *          Only when the page owes nothing does a loupe-only change get to ask
 *          for the small rectangle -- which is the partial-update case the demo
 *          exists to show.
 * @param[in]  s         Initialised scene.
 * @param[in]  page_plan The page view's drained plan.
 * @param[in]  lens_plan The loupe's drained plan.
 * @param[out] out       Receives the scene-level plan.
 * @return Nothing.
 * @pre  Both plans were just drained by ::ra8_zoom_view_present.
 * @pre  @p out addresses writable storage.
 * @post `out->present` is false when neither view owed anything.
 * @post When `out->present` is true, `out->rect` is the smallest correct flush.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ez_choose_plan(const ez_scene_t*         s,
                           const ra8_zoom_present_t* page_plan,
                           const ra8_zoom_present_t* lens_plan,
                           ez_present_t*             out)
{
  if (page_plan->present) {
    out->rect    = s->content;
    out->quality = (page_plan->refresh == k_ra8_zoom_refresh_quality);
    out->present = true;
    return;
  }
  if (lens_plan->present && s->lens_on) {
    /* The partial-update case: only the lens box changed, so only the lens box
     * is flushed -- 102400 pixels instead of the content area's 565248. */
    out->rect    = ez_lens_rect(s->content);
    out->quality = (lens_plan->refresh == k_ra8_zoom_refresh_quality);
    out->present = true;
    return;
  }
  out->present = false;
}

ra8_err_t ez_scene_present(ez_scene_t* s, ez_present_t* out)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "present out must not be nullptr");
  ra8_zoom_present_t page_plan = {};
  ra8_zoom_present_t lens_plan = {};
  RA8_RETURN_ON_ERROR(ra8_zoom_view_present(&s->page, &page_plan), s_tag, "page present");
  RA8_RETURN_ON_ERROR(ra8_zoom_view_present(&s->lens, &lens_plan), s_tag, "lens present");
  ez_choose_plan(s, &page_plan, &lens_plan, out);
  return k_ra8_ok;
}

bool ez_scene_tick(ez_scene_t* s, uint32_t now_ms)
{
  if (s == nullptr) {
    return false;
  }
  const bool page_due = ra8_zoom_view_tick(&s->page, now_ms);
  const bool lens_due = ra8_zoom_view_tick(&s->lens, now_ms);
  return page_due || (lens_due && s->lens_on);
}

ra8_err_t ez_scene_prefetch(ez_scene_t* s, ra8_zoom_pan_t dir, uint16_t* out_warmed)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(s->tiles.cache, s_tag, "scene tile source must be bound");
  return ra8_zoom_tiles_prefetch(&s->tiles, &s->page, dir, (uint16_t)k_ez_prefetch_max, out_warmed);
}

uint32_t ez_fnv1a(const void* buf, uint32_t len)
{
  uint32_t hash = (uint32_t)k_ez_fnv_offset;
  if (buf == nullptr) {
    return hash;
  }
  const uint8_t* bytes = (const uint8_t*)buf;
  for (uint32_t i = 0U; i < len; ++i) {
    hash ^= (uint32_t)bytes[i];
    hash *= (uint32_t)k_ez_fnv_prime;
  }
  return hash;
}

/**
 * @brief Render the scene and hash the framebuffer it produced.
 * @param[in,out] s        Initialised scene.
 * @param[out]    out_hash Receives the FNV-1a-32 of the whole framebuffer.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The frame was painted and hashed.
 * @retval k_ra8_err_* Propagated from ::ez_scene_render or ra8_gfx.
 * @pre  ::ra8_gfx_init has bound the framebuffer.
 * @pre  @p out_hash is writable.
 * @post On k_ra8_ok the framebuffer holds the rendered frame.
 * @post The pending flush is consumed, so the next step starts clean.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_render_and_hash(ez_scene_t* s, uint32_t* out_hash)
{
  RA8_RETURN_ON_ERROR(ez_scene_render(s), s_tag, "selftest render");
  ez_present_t plan = {};
  RA8_RETURN_ON_ERROR(ez_scene_present(s, &plan), s_tag, "selftest present");
  *out_hash = ez_fnv1a(s->fb, s->fb_bytes);
  return k_ra8_ok;
}

/**
 * @brief Self-check stage 2: one right-pan step at 1:1, with read-ahead.
 * @details The pan is a full discrete step rather than a nudge, so it is the
 *          case the tile cache is sized for: the newly exposed column decodes
 *          and nothing still on screen is evicted.
 * @param[in,out] s   Initialised scene.
 * @param[out]    out Receives `crc_pan` and `warmed`.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The stage ran and both fields are set.
 * @retval k_ra8_err_* Propagated from the viewport, the prefetch or the render.
 * @pre  Stage 1 has already rendered the opening view.
 * @pre  ::ra8_gfx_init has bound the framebuffer the hash covers.
 * @post The page viewport has advanced by one step.
 * @post `out->warmed` is at most ::k_ez_prefetch_max.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_selftest_pan(ez_scene_t* s, ez_selftest_t* out)
{
  RA8_RETURN_ON_ERROR(ra8_zoom_view_pan_dir(&s->page, k_ra8_zoom_pan_right, (uint32_t)k_ez_st_t0),
                      s_tag,
                      "selftest pan");
  RA8_RETURN_ON_ERROR(ez_scene_prefetch(s, k_ra8_zoom_pan_right, &out->warmed),
                      s_tag,
                      "selftest prefetch");
  return ez_render_and_hash(s, &out->crc_pan);
}

/**
 * @brief Self-check stage 3: 2x about a fixed panel point.
 * @details The focus point is a named constant, not an incidental of whoever
 *          wrote the test, because the resulting hash is part of the golden.
 * @param[in,out] s   Initialised scene.
 * @param[out]    out Receives `crc_2x`.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The stage ran and the hash is set.
 * @retval k_ra8_err_* Propagated from the viewport or the render.
 * @pre  Stage 2 has already run.
 * @pre  ::ra8_gfx_init has bound the framebuffer the hash covers.
 * @post The page view is at ::k_ez_st_scale.
 * @post The point at (::k_ez_st_focus_x, ::k_ez_st_focus_y) is unmoved.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_selftest_zoom(ez_scene_t* s, ez_selftest_t* out)
{
  RA8_RETURN_ON_ERROR(ra8_zoom_view_set_scale(&s->page,
                                              (uint8_t)k_ez_st_scale,
                                              (int32_t)k_ez_st_focus_x,
                                              (int32_t)k_ez_st_focus_y,
                                              (uint32_t)k_ez_st_t0),
                      s_tag,
                      "selftest zoom");
  return ez_render_and_hash(s, &out->crc_2x);
}

/**
 * @brief Self-check stage 4: open the loupe over the magnified page.
 * @details Both views are invalidated explicitly. Opening the loupe moves no
 *          anchor and changes no scale, so neither view would otherwise know
 *          its pixels had been damaged -- the seam this stage also regression-
 *          tests.
 * @param[in,out] s   Initialised scene.
 * @param[out]    out Receives `crc_lens`.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The stage ran and the hash is set.
 * @retval k_ra8_err_* Propagated from the viewport or the render.
 * @pre  Stage 3 has already run.
 * @pre  ::ra8_gfx_init has bound the framebuffer the hash covers.
 * @post The loupe is open and drawn.
 * @post Both views owe nothing (the render drained their plans).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_selftest_lens(ez_scene_t* s, ez_selftest_t* out)
{
  s->lens_on = true;
  RA8_RETURN_ON_ERROR(ra8_zoom_view_invalidate(&s->lens, (uint32_t)k_ez_st_t0),
                      s_tag,
                      "selftest lens dirty");
  RA8_RETURN_ON_ERROR(ra8_zoom_view_invalidate(&s->page, (uint32_t)k_ez_st_t0),
                      s_tag,
                      "selftest page dirty");
  return ez_render_and_hash(s, &out->crc_lens);
}

/**
 * @brief Run the four scripted self-check stages in order.
 * @details The order IS the golden: each stage starts from the state the last
 *          one left, so re-ordering them changes every hash after the first.
 *          Split from ::ez_scene_selftest so that entry point is its argument
 *          checks, this call, and the counter read.
 * @param[in,out] s   Freshly initialised scene.
 * @param[out]    out Receives all four hashes and the prefetch count.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    All four stages rendered and hashed.
 * @retval k_ra8_err_* The first failing stage's code, verbatim.
 * @pre  ::ra8_gfx_init has bound the framebuffer the hashes cover.
 * @pre  @p s was freshly initialised, so the cache counters start at zero.
 * @post On k_ra8_ok the framebuffer holds the final (loupe) state.
 * @post On failure the earlier stages' hashes are still valid.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ez_selftest_stages(ez_scene_t* s, ez_selftest_t* out)
{
  RA8_RETURN_ON_ERROR(ez_render_and_hash(s, &out->crc_1x), s_tag, "selftest 1:1");
  RA8_RETURN_ON_ERROR(ez_selftest_pan(s, out), s_tag, "selftest pan stage");
  RA8_RETURN_ON_ERROR(ez_selftest_zoom(s, out), s_tag, "selftest zoom stage");
  return ez_selftest_lens(s, out);
}

ra8_err_t ez_scene_selftest(ez_scene_t* s, ez_selftest_t* out)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "selftest out must not be nullptr");

  RA8_RETURN_ON_ERROR(ez_selftest_stages(s, out), s_tag, "selftest stages");
  return ra8_tile_cache_stats(&s->cache, &out->hits, &out->misses, &out->evictions);
}
