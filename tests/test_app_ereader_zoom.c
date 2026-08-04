/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_ereader_zoom.c
 * @brief Host twin of the `ereader_zoom` tap-to-zoom demo: its golden (#478).
 *
 * @details
 * Compiles the app's own `ez_scene.c` -- the production presentation model, not
 * a re-implementation -- against a host framebuffer and drives the identical
 * `ez_scene_selftest()` sequence the firmware runs at boot. The four framebuffer
 * hashes and the three tile-cache counters it produces must equal the numbers
 * pinned in `examples/.../ereader_zoom/hil.conf`, which is what makes that
 * banner a golden rather than a printout.
 *
 * The equality is only meaningful because the whole path is integer: the page
 * sampler is shifts and masks, the composite is nearest-neighbour magnification
 * plus a `const` blue-noise mask, and the chrome is filled rectangles rather
 * than antialiased glyphs. A hash over float-antialiased text would be
 * toolchain-bound (the same board prints different values from a 13.3- and a
 * 14.3-built image), and this app deliberately renders none.
 *
 * Beyond the golden this asserts the two claims the app exists to make:
 *   - **bounded residency** -- the whole four-state sequence decodes far fewer
 *     tiles than the page holds, and evicts none (#338);
 *   - **partial update** -- a loupe-only change asks for a flush of the lens box
 *     rather than the content area.
 */

#include <stdint.h>
#include <string.h>

#include "ez_scene.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_tile_cache.h"
#include "ra8_ui.h"
#include "ra8_zoom.h"
#include "unity_minimal.h"

/**
 * @enum t_ez_fb_t
 * @brief Host framebuffer geometry -- the EK-RA8D2 panel, RGB565.
 * @details The same surface the firmware binds, so the hash covers the same
 *          bytes in the same order.
 */
typedef enum : uint16_t {
  k_t_ez_fb_w = 1024U, /**< Framebuffer width, pixels.  */
  k_t_ez_fb_h = 600U,  /**< Framebuffer height, pixels. */
} t_ez_fb_t;

/**
 * @enum t_ez_golden_t
 * @brief The pinned golden numbers -- these ARE hil.conf's HIL_EXPECT.
 * @details Changing any of them without a stated reason is a silently
 *          rebaselined golden, which is indistinguishable from a regression.
 *          They were minted from the ra8_emulator run of the same firmware and
 *          are re-derived here from the same production render.
 */
typedef enum : uint32_t {
  k_t_ez_crc_1x   = 0xEA5CD9B4U, /**< Opening 1:1 render.                    */
  k_t_ez_crc_pan  = 0xD5BD08D3U, /**< After one right-pan step at 1:1.       */
  k_t_ez_crc_2x   = 0x6C68D8BFU, /**< After 2x about panel point (700,400).  */
  k_t_ez_crc_lens = 0x8B3C5733U, /**< After the 4x loupe is opened.          */
  k_t_ez_hits     = 6680U,       /**< Tile-cache hits over the sequence.     */
  k_t_ez_misses   = 27U,         /**< Tile-cache decodes over the sequence.  */
  k_t_ez_evicts   = 0U,          /**< Tile-cache evictions: none, by design. */
  k_t_ez_warmed   = 3U,          /**< Tiles warmed by the pan read-ahead.    */
  k_t_ez_tiny_fb  = 64U,         /**< A panel too small for the layout.      */
  k_t_ez_off_tile = 999U,        /**< A tile column past the page's grid.    */
  k_t_ez_tile_x   = 2U,          /**< Decode-test tile column.               */
  k_t_ez_tile_y   = 3U,          /**< Decode-test tile row.                  */
  k_t_ez_rule_y   = 8U,          /**< A page row between two ink rules.      */
  k_t_ez_burst_ms = 200U,        /**< Timestamp of the simulated gesture.    */
} t_ez_golden_t;

/** @brief Host framebuffer: RGB565, one halfword per pixel. */
static uint16_t s_fb[(size_t)k_t_ez_fb_h * (size_t)k_t_ez_fb_w];
/** @brief Tile-cache cell storage (the app's SDRAM arena, here in .bss). */
static uint8_t s_cell_mem[(size_t)k_ez_cells * (size_t)k_ez_cell_bytes];
/** @brief Tile-cache link metadata. */
static ra8_keycache_cell_t s_meta[k_ez_cells];
/** @brief Tile-cache key storage. */
static ra8_tile_key_t s_keys[k_ez_cells];
/** @brief Tile-cache decoded-extent descriptors. */
static ra8_tile_dims_t s_dims[k_ez_cells];
/** @brief Tile-cache hash-bucket heads. */
static int32_t s_buckets[k_ez_buckets];
/** @brief Composite scratch: one source row at the widest viewport. */
static uint8_t s_row[k_ez_row_bytes];
/** @brief Composite scratch: the gray8 destination strip. */
static uint8_t s_strip[k_ez_strip_bytes];
/** @brief Composite scratch: the strip packed to 4 bpp. */
static uint8_t s_packed[k_ez_packed_bytes];
/** @brief The scene under test. */
static ez_scene_t s_scene;

/**
 * @brief Bind ra8_gfx to the host framebuffer and wire a fresh scene over it.
 */
static void t_ez_open(void)
{
  (void)memset(s_fb, 0, sizeof(s_fb));
  (void)memset(&s_scene, 0, sizeof(s_scene));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_t_ez_fb_w, (uint16_t)k_t_ez_fb_h, k_ra8_gfx_format_rgb565));
  const ez_scene_cfg_t cfg = {
    .fb       = s_fb,
    .fb_bytes = (uint32_t)sizeof(s_fb),
    .fb_w     = (int32_t)k_t_ez_fb_w,
    .fb_h     = (int32_t)k_t_ez_fb_h,
    .cell_mem = s_cell_mem,
    .meta     = s_meta,
    .keys     = s_keys,
    .dims     = s_dims,
    .buckets  = s_buckets,
    .row      = s_row,
    .strip    = s_strip,
    .packed   = s_packed,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_init(&s_scene, &cfg));
}

/**
 * @test scene_init_validates
 */
static void t_scene_init_validates(void)
{
  TEST_BEGIN("scene_init_validates");
  ez_scene_cfg_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_init(&s_scene, nullptr));
  /* Every borrowed buffer is required; the framebuffer most of all, because the
   * golden hashes it. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_init(&s_scene, &cfg));
  cfg.fb       = s_fb;
  cfg.cell_mem = s_cell_mem;
  cfg.meta     = s_meta;
  cfg.keys     = s_keys;
  cfg.dims     = s_dims;
  cfg.buckets  = s_buckets;
  cfg.row      = s_row;
  cfg.strip    = s_strip;
  cfg.packed   = s_packed;
  /* A panel too small for the loupe layout is refused rather than clipped. */
  cfg.fb_w = (int32_t)k_t_ez_tiny_fb;
  cfg.fb_h = (int32_t)k_t_ez_tiny_fb;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ez_scene_init(&s_scene, &cfg));
  TEST_ASSERT(!s_scene.lens_on);
  TEST_END("scene_init_validates");
}

/**
 * @test page_sampler_is_pure_and_bounded
 */
static void t_page_sampler_is_pure_and_bounded(void)
{
  TEST_BEGIN("page_sampler_is_pure_and_bounded");
  /* The sampler is a pure function of the coordinate, which is what lets the
   * tile decoder stand in for a container without changing the cache's
   * behaviour. Two calls at the same point agree; the rules are darker than the
   * background everywhere. */
  const uint8_t a = ez_page_sample(1234U, 567U);
  TEST_ASSERT_EQ(a, ez_page_sample(1234U, 567U));
  /* Row 0 is on a rule and column 0 is inside an inked run, so it is ink; row 8
   * is between rules, so it is background and strictly lighter. */
  TEST_ASSERT(ez_page_sample(0U, 0U) < ez_page_sample(0U, (uint32_t)k_t_ez_rule_y));

  /* A tile decode fills exactly one tile and reports the declared geometry. */
  static uint8_t s_cell[k_ez_cell_bytes];
  ra8_tile_key_t key = {.image_id = (uint32_t)k_ez_image_id,
                        .tile_x   = (uint16_t)k_t_ez_tile_x,
                        .tile_y   = (uint16_t)k_t_ez_tile_y};
  uint16_t       tw  = 0U;
  uint16_t       th  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ez_tile_decode(nullptr, nullptr, s_cell, sizeof(s_cell), &tw, &th));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ez_tile_decode(nullptr, &key, s_cell, 1U, &tw, &th));
  TEST_ASSERT_EQ(k_ra8_ok, ez_tile_decode(nullptr, &key, s_cell, sizeof(s_cell), &tw, &th));
  TEST_ASSERT_EQ(k_ez_tile_edge, tw);
  TEST_ASSERT_EQ(k_ez_tile_edge, th);
  TEST_ASSERT_EQ(ez_page_sample((uint32_t)k_t_ez_tile_x * (uint32_t)k_ez_tile_edge,
                                (uint32_t)k_t_ez_tile_y * (uint32_t)k_ez_tile_edge),
                 s_cell[0]);
  /* A key naming a tile off the page is refused rather than wrapped. */
  key.tile_x = (uint16_t)k_t_ez_off_tile;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ez_tile_decode(nullptr, &key, s_cell, sizeof(s_cell), &tw, &th));
  TEST_END("page_sampler_is_pure_and_bounded");
}

/**
 * @test zones_route_taps
 */
static void t_zones_route_taps(void)
{
  TEST_BEGIN("zones_route_taps");
  t_ez_open();
  const ra8_ui_rect_t c = s_scene.content;
  TEST_ASSERT_EQ(k_ez_zone_none, ez_zone_hit(nullptr, 0, 0));
  TEST_ASSERT_EQ(k_ez_zone_toggle, ez_zone_hit(&s_scene, 10, 10));
  TEST_ASSERT_EQ(k_ez_zone_pan_left, ez_zone_hit(&s_scene, c.x + 4, c.y + (c.h / 2)));
  TEST_ASSERT_EQ(k_ez_zone_pan_right, ez_zone_hit(&s_scene, (c.x + c.w) - 4, c.y + (c.h / 2)));
  TEST_ASSERT_EQ(k_ez_zone_pan_up, ez_zone_hit(&s_scene, c.x + (c.w / 2), c.y + 4));
  TEST_ASSERT_EQ(k_ez_zone_pan_down, ez_zone_hit(&s_scene, c.x + (c.w / 2), (c.y + c.h) - 4));
  /* The centre cycles the page zoom while the loupe is closed... */
  TEST_ASSERT_EQ(k_ez_zone_zoom, ez_zone_hit(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2)));
  /* ...and belongs to the loupe once it is open. */
  s_scene.lens_on = true;
  TEST_ASSERT_EQ(k_ez_zone_lens, ez_zone_hit(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2)));
  s_scene.lens_on = false;
  /* Below the panel is no zone at all. */
  TEST_ASSERT_EQ(k_ez_zone_none, ez_zone_hit(&s_scene, c.x + (c.w / 2), c.y + c.h + 10));

  /* Taps mutate the scene: the centre cycles 1 -> 2 -> 4 -> 1. */
  TEST_ASSERT(!ez_scene_tap(nullptr, 0, 0, 0U));
  TEST_ASSERT_EQ(1U, s_scene.page.scale);
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2), 1U));
  TEST_ASSERT_EQ(2U, s_scene.page.scale);
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2), 2U));
  TEST_ASSERT_EQ(k_ez_page_scale_max, s_scene.page.scale);
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2), 3U));
  TEST_ASSERT_EQ(1U, s_scene.page.scale);
  /* The status bar opens the loupe; a tap inside it cycles the lens ladder. */
  TEST_ASSERT(ez_scene_tap(&s_scene, 10, 10, 4U));
  TEST_ASSERT(s_scene.lens_on);
  TEST_ASSERT_EQ(k_ez_lens_scale_min, s_scene.lens.scale);
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2), 5U));
  TEST_ASSERT_EQ(k_ez_lens_scale_max, s_scene.lens.scale);
  /* A pan into the top-left corner clamps to nothing and reports no change. */
  TEST_ASSERT(!ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + 4, 6U));
  TEST_END("zones_route_taps");
}

/**
 * @brief The loupe-only half of ::t_present_narrows_to_the_lens.
 * @details Split out only to stay inside the function-size bar. This is the
 *          partial-update measurement itself: a change confined to the loupe
 *          must narrow the flush to the lens box, and the interactive burst
 *          must arrive bi-level and settle to 16 levels.
 * @param[in] content The scene's content rectangle, for the tap coordinates.
 */
static void t_present_lens_only(ra8_ui_rect_t content)
{
  ez_present_t plan = {};
  TEST_ASSERT(ez_scene_tap(&s_scene,
                           content.x + (content.w / 2),
                           content.y + (content.h / 2),
                           (uint32_t)k_t_ez_burst_ms));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_render(&s_scene));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT_EQ(k_ez_lens_edge, plan.rect.w);
  TEST_ASSERT_EQ(k_ez_lens_edge, plan.rect.h);
  /* An interactive burst is bi-level; the settle promotes it to 16 levels. */
  TEST_ASSERT(!plan.quality);
  TEST_ASSERT(!ez_scene_tick(nullptr, 0U));
  TEST_ASSERT(
    ez_scene_tick(&s_scene, (uint32_t)k_t_ez_burst_ms + (uint32_t)k_ra8_zoom_settle_ms_default));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT(plan.quality);
}

/**
 * @test scene_decisions_are_covered
 *
 * @par MC/DC:
 * Decision examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c@ez_apply_pan
 * `if ((after.x == before.x) && (after.y == before.y))` -> "nothing moved"
 * (2 conditions):
 * - V1: a right-pan -> x moved, y did not -> false (varies condition 1)
 * - V2: a down-pan  -> y moved, x did not -> false (varies condition 2)
 * - V3: an up-pan at the top edge -> neither moved -> true (control)
 * V3+V1 prove the x term's independent influence; V3+V2 the y term's. N+1 = 3.
 *
 * @par MC/DC:
 * Decision examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c@ez_scene_tick
 * `return page_due || (lens_due && s->lens_on)` (3 conditions):
 * - V4: the page's own settle is due            -> true  (varies page_due)
 * - V5: loupe OPEN, only the loupe settle due   -> true  (varies lens_due)
 * - V6: loupe CLOSED, only the loupe settle due -> false (varies lens_on; a
 *       closed loupe's timer must not repaint the page)
 * Plus a quiet tick with nothing armed as the all-false control. V6+V5 give
 * lens_on independent influence, control+V4 page_due, and V6 vs the quiet tick
 * lens_due. N+1 = 4 vectors for N=3.
 *
 * @details These are the app's own compound decisions. `examples/` sits outside
 *          the enforcing MC/DC gate, but a decision left uncovered here is still
 *          reported debt in docs/MCDC_GAPS.csv, and covering it is cheaper than
 *          explaining it.
 */
static void t_scene_decisions_are_covered(void)
{
  TEST_BEGIN("scene_decisions_are_covered");
  t_ez_open();
  const ra8_ui_rect_t c = s_scene.content;

  /* V3: the view opens at the top-left, so an up-pan clamps to no movement. */
  TEST_ASSERT(!ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + 4, 1U));
  /* V1: a right-pan moves x only. */
  TEST_ASSERT(ez_scene_tap(&s_scene, (c.x + c.w) - 4, c.y + (c.h / 2), 2U));
  /* V2: a down-pan moves y only. */
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), (c.y + c.h) - 4, 3U));

  /* A tick with nothing armed is the all-false control. */
  ez_present_t plan = {};
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  /* V4: the page's own settle is due. */
  TEST_ASSERT(ez_scene_tick(&s_scene, 3U + (uint32_t)k_ra8_zoom_settle_ms_default));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(!ez_scene_tick(&s_scene, 3U + (uint32_t)k_ra8_zoom_settle_ms_default));

  /* V5: with the loupe open, its own settle is enough to ask for a repaint. */
  TEST_ASSERT(ez_scene_tap(&s_scene, 10, 10, 10U));
  TEST_ASSERT(s_scene.lens_on);
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2), 20U));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(ez_scene_tick(&s_scene, 20U + (uint32_t)k_ra8_zoom_settle_ms_default));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));

  /* V6: the same armed loupe with the lens CLOSED must not ask for a repaint --
   * its pixels are not on screen. */
  TEST_ASSERT(ez_scene_tap(&s_scene, c.x + (c.w / 2), c.y + (c.h / 2), 30U));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  s_scene.lens_on = false;
  TEST_ASSERT(!ez_scene_tick(&s_scene, 30U + (uint32_t)k_ra8_zoom_settle_ms_default));
  TEST_END("scene_decisions_are_covered");
}

/**
 * @test present_narrows_to_the_lens
 *
 * @details The partial-update claim, measured: a loupe-only change must ask for
 *          a flush of the lens box, not the content area. On the shipping e-ink
 *          panel that is 102400 pixels instead of 565248.
 */
static void t_present_narrows_to_the_lens(void)
{
  TEST_BEGIN("present_narrows_to_the_lens");
  t_ez_open();
  const ra8_ui_rect_t c    = s_scene.content;
  ez_present_t        plan = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_present(nullptr, &plan));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_present(&s_scene, nullptr));

  /* Opening owes a full-content, full-quality flush. */
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_render(&s_scene));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT(plan.quality);
  TEST_ASSERT_EQ(c.w, plan.rect.w);
  TEST_ASSERT_EQ(c.h, plan.rect.h);

  /* Nothing changed since: no flush at all, so an idle reader never refreshes. */
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(!plan.present);

  /* Open the loupe. The lens covers page pixels but neither view's anchor nor
   * scale moved, so this only asks for a flush because ez_scene_tap declares
   * the damage explicitly -- assert `present`, not just the rectangle, or the
   * stale `plan` from the previous call would satisfy the check and the bug
   * (an e-ink panel never showing the lens) would pass. */
  TEST_ASSERT(ez_scene_tap(&s_scene, 10, 10, 100U));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_render(&s_scene));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT_EQ(c.w, plan.rect.w);
  /* Closing it damages the same rectangle, for the same reason. */
  TEST_ASSERT(ez_scene_tap(&s_scene, 10, 10, 110U));
  TEST_ASSERT(!s_scene.lens_on);
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_render(&s_scene));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));
  TEST_ASSERT(plan.present);
  TEST_ASSERT_EQ(c.w, plan.rect.w);
  TEST_ASSERT(ez_scene_tap(&s_scene, 10, 10, 120U));
  TEST_ASSERT(s_scene.lens_on);
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_render(&s_scene));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_present(&s_scene, &plan));

  t_present_lens_only(c);
  TEST_END("present_narrows_to_the_lens");
}

/**
 * @test selftest_matches_the_golden
 *
 * @details The golden itself. If this fails after an intentional render change,
 *          re-mint BOTH ::t_ez_golden_t and hil.conf's HIL_EXPECT in the same
 *          commit and say in the message what changed and why -- a rebaselined
 *          golden with no stated reason is indistinguishable from a regression.
 */
static void t_selftest_matches_the_golden(void)
{
  TEST_BEGIN("selftest_matches_the_golden");
  t_ez_open();
  ez_selftest_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_selftest(nullptr, &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_selftest(&s_scene, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_selftest(&s_scene, &st));

  TEST_ASSERT_EQ(k_t_ez_crc_1x, st.crc_1x);
  TEST_ASSERT_EQ(k_t_ez_crc_pan, st.crc_pan);
  TEST_ASSERT_EQ(k_t_ez_crc_2x, st.crc_2x);
  TEST_ASSERT_EQ(k_t_ez_crc_lens, st.crc_lens);
  TEST_ASSERT_EQ(k_t_ez_hits, st.hits);
  TEST_ASSERT_EQ(k_t_ez_misses, st.misses);
  TEST_ASSERT_EQ(k_t_ez_evicts, st.evictions);
  TEST_ASSERT_EQ(k_t_ez_warmed, st.warmed);

  /* Each stage must actually change the picture, or a hash that never moved
   * would "pass" a broken zoom. */
  TEST_ASSERT(st.crc_1x != st.crc_pan);
  TEST_ASSERT(st.crc_pan != st.crc_2x);
  TEST_ASSERT(st.crc_2x != st.crc_lens);

  /* Residency: the whole sequence decodes a small fraction of the page's tiles
   * and evicts none, which is the #338 no-thrash property. */
  const uint32_t page_tiles = ((uint32_t)k_ez_page_w / (uint32_t)k_ez_tile_edge) *
                              ((uint32_t)k_ez_page_h / (uint32_t)k_ez_tile_edge);
  TEST_ASSERT(st.misses < page_tiles);
  TEST_ASSERT(st.misses <= (uint32_t)k_ez_cells);
  TEST_ASSERT_EQ(0U, st.evictions);
  TEST_END("selftest_matches_the_golden");
}

/**
 * @test prefetch_is_bounded
 */
static void t_prefetch_is_bounded(void)
{
  TEST_BEGIN("prefetch_is_bounded");
  t_ez_open();
  uint16_t warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ez_scene_prefetch(nullptr, k_ra8_zoom_pan_right, &warmed));
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_prefetch(&s_scene, k_ra8_zoom_pan_none, &warmed));
  TEST_ASSERT_EQ(0U, warmed);
  TEST_ASSERT_EQ(k_ra8_ok, ez_scene_prefetch(&s_scene, k_ra8_zoom_pan_right, &warmed));
  TEST_ASSERT(warmed <= (uint16_t)k_ez_prefetch_max);
  TEST_END("prefetch_is_bounded");
}

/**
 * @test fnv1a_matches_the_reference
 */
static void t_fnv1a_matches_the_reference(void)
{
  TEST_BEGIN("fnv1a_matches_the_reference");
  /* The published FNV-1a-32 vectors: the empty input is the offset basis and
   * "a" is 0xE40C292C. A hash that drifted would move every golden at once. */
  TEST_ASSERT_EQ(2166136261U, ez_fnv1a(nullptr, 0U));
  TEST_ASSERT_EQ(2166136261U, ez_fnv1a("", 0U));
  TEST_ASSERT_EQ(0xE40C292CU, ez_fnv1a("a", 1U));
  TEST_ASSERT_EQ(0xBF9CF968U, ez_fnv1a("foobar", 6U));
  TEST_END("fnv1a_matches_the_reference");
}

/**
 * @brief Host test entry point.
 * @return 0 on success; any assertion `exit(1)`s before returning.
 */
int32_t main(void)
{
  t_scene_init_validates();
  t_page_sampler_is_pure_and_bounded();
  t_zones_route_taps();
  t_present_narrows_to_the_lens();
  t_scene_decisions_are_covered();
  t_selftest_matches_the_golden();
  t_prefetch_is_bounded();
  t_fnv1a_matches_the_reference();
  (void)fprintf(stderr, "[PASS] test_app_ereader_zoom: all cases passed\n");
  return 0;
}
