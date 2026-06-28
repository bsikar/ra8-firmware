/**
 * @file test_ra_compositor.c
 * @brief Host unit tests + demo for the ra_compositor framebuffer compositor.
 *
 * @details
 * Pure host tests for the zero-heap framebuffer compositor (issue #145). They
 * build sample widget trees, run the tiling layout, composite into an in-memory
 * ra_gfx framebuffer, and assert:
 *
 *   - the scene-construction / layout / damage guard contracts of every entry
 *     point (return codes on bad inputs, capacity overflow, bad parents);
 *   - the tiling layout math (fixed + flex split, padding, gap, row vs column,
 *     overflow clamping, invisible-child skipping);
 *   - a CRC golden of a composited e-reader-style sample tree (the demo);
 *   - the partial-update invariant the issue asks for: compositing only the
 *     damaged region yields a framebuffer byte-identical to a full recomposite,
 *     while pixels outside the damage rectangle are left untouched.
 *
 * ra_compositor has no compound boolean decisions, so there is no `@par MC/DC`
 * obligation here; the tests instead drive both sides of every branch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_compositor.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "unity_minimal.h"

/**
 * @enum t_geom_t
 * @brief Test framebuffer + sample-tree geometry (pixels).
 */
typedef enum : int32_t {
  k_t_fb_w     = 96, /**< Framebuffer width.            */
  k_t_fb_h     = 72, /**< Framebuffer height.           */
  k_t_pad      = 4,  /**< Root container inner padding. */
  k_t_gap      = 2,  /**< Gap between stacked children. */
  k_t_status_h = 20, /**< Status bar fixed height.      */
  k_t_footer_h = 18, /**< Footer fixed height.          */
  k_t_pool_cap = 8,  /**< Sample-tree node pool size.   */
} t_geom_t;

/**
 * @enum t_color_t
 * @brief Distinct 0x00RRGGBB colours for the sample tree.
 */
typedef enum : uint32_t {
  k_t_bg      = 0x00202020U, /**< Root background.            */
  k_t_status  = 0x004040C0U, /**< Status bar fill.            */
  k_t_body    = 0x00FFFFFFU, /**< Body fill.                  */
  k_t_footer  = 0x00C04040U, /**< Footer fill (frame 1).      */
  k_t_footer2 = 0x00C0C040U, /**< Footer fill (frame 2).      */
  k_t_fg      = 0x00000000U, /**< Label text colour.          */
} t_color_t;

/**
 * @enum t_node_idx_t
 * @brief Node indices inside the sample tree built by ::t_build_demo.
 */
typedef enum : uint16_t {
  k_t_root   = 0U, /**< Background rect + column container. */
  k_t_status_i = 1U, /**< Status bar label.                */
  k_t_body_i = 2U, /**< Body rect.                         */
  k_t_footer_i = 3U, /**< Footer label.                    */
} t_node_idx_t;

/**
 * @enum t_golden_t
 * @brief CRC-32 goldens of the composited sample tree (baked from a run).
 */
typedef enum : uint32_t {
  k_golden_initial = 0x4D417F0FU, /**< Frame 1 (footer "p.1"). */
  k_golden_changed = 0xDF20FCBDU, /**< Frame 2 (footer "p.2"). */
} t_golden_t;

/** @brief First framebuffer (incremental / frame-1 target). */
static uint32_t s_fb[k_t_fb_w * k_t_fb_h];
/** @brief Second framebuffer (full recomposite of frame 2). */
static uint32_t s_fb2[k_t_fb_w * k_t_fb_h];

/** @brief CRC-32 (reflected, poly 0xEDB88320) over a byte span. */
static uint32_t t_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= (uint32_t)data[i];
    for (uint32_t b = 0U; b < 8U; ++b) {
      if ((crc & 1U) != 0U) {
        crc = (crc >> 1U) ^ 0xEDB88320U;
      } else {
        crc = crc >> 1U;
      }
    }
  }
  return ~crc;
}

/** @brief Read one framebuffer pixel from the first buffer. */
static uint32_t t_px(int32_t x, int32_t y)
{
  return s_fb[(size_t)(y * k_t_fb_w + x)];
}

/** @brief Build the e-reader-style sample tree (bg + status / body / footer). */
static void t_build_demo(ra_compositor_t*        s,
                         ra_compositor_widget_t* pool,
                         uint16_t                cap,
                         uint32_t                footer_color,
                         const char*             footer_text)
{
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(s, pool, cap));
  uint16_t idx = 0U;

  ra_compositor_widget_t root = {.kind    = k_ra_compositor_kind_rect,
                                 .color   = (uint32_t)k_t_bg,
                                 .visible = true,
                                 .axis    = k_ra_compositor_axis_col,
                                 .pad     = (int16_t)k_t_pad,
                                 .gap     = (int16_t)k_t_gap};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_compositor_add(s, &root, (uint16_t)k_ra_compositor_no_parent, &idx));

  ra_compositor_widget_t status = {.kind    = k_ra_compositor_kind_label,
                                   .color   = (uint32_t)k_t_status,
                                   .fg      = (uint32_t)k_t_fg,
                                   .text    = "RA8D2",
                                   .visible = true,
                                   .fixed   = (int16_t)k_t_status_h};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(s, &status, k_t_root, &idx));

  ra_compositor_widget_t body = {.kind    = k_ra_compositor_kind_rect,
                                 .color   = (uint32_t)k_t_body,
                                 .visible = true,
                                 .flex    = 1U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(s, &body, k_t_root, &idx));

  ra_compositor_widget_t footer = {.kind    = k_ra_compositor_kind_label,
                                   .color   = footer_color,
                                   .fg      = (uint32_t)k_t_fg,
                                   .text    = footer_text,
                                   .visible = true,
                                   .fixed   = (int16_t)k_t_footer_h};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(s, &footer, k_t_root, &idx));
}

/** @brief Full-compose the sample tree (given footer) into @p buf; return CRC. */
static uint32_t t_full_compose(uint32_t* buf, uint32_t footer_color, const char* footer_text)
{
  const ra_compositor_rect_t frame                = {0, 0, k_t_fb_w, k_t_fb_h};
  ra_compositor_widget_t     pool[k_t_pool_cap]   = {};
  ra_compositor_t            s                     = {};
  t_build_demo(&s, pool, (uint16_t)k_t_pool_cap, footer_color, footer_text);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_gfx_init(buf, (uint16_t)k_t_fb_w, (uint16_t)k_t_fb_h,
                                                k_ra_gfx_format_argb8888));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_compose_full(&s, &frame));
  return t_crc32((const uint8_t*)buf, (size_t)(k_t_fb_w * k_t_fb_h) * sizeof(uint32_t));
}

/* ===========================================================================
 * Scene construction
 * ===========================================================================
 */

/** @brief ra_compositor_init guards + success. */
static void test_init_guards(void)
{
  TEST_BEGIN("ra_compositor init guards");
  ra_compositor_widget_t pool[2] = {};
  ra_compositor_t        s       = {};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_init(nullptr, pool, 2U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_init(&s, nullptr, 2U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_init(&s, pool, 0U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(&s, pool, 2U));
  TEST_ASSERT_EQ(0, (int)s.count);
  TEST_ASSERT_EQ(2, (int)s.cap);
  TEST_END("ra_compositor init guards");
}

/** @brief ra_compositor_add guards: null, no-mem, bad parent, success. */
static void test_add_guards(void)
{
  TEST_BEGIN("ra_compositor add guards");
  ra_compositor_widget_t pool[2] = {};
  ra_compositor_t        s       = {};
  ra_compositor_widget_t w       = {.kind = k_ra_compositor_kind_rect, .visible = true};
  uint16_t               idx     = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(&s, pool, 2U));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_add(nullptr, &w, (uint16_t)k_ra_compositor_no_parent, &idx));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_add(&s, nullptr, (uint16_t)k_ra_compositor_no_parent, &idx));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_add(&s, &w, (uint16_t)k_ra_compositor_no_parent, nullptr));

  /* First node must be the root (no-parent sentinel). */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_add(&s, &w, 0U, &idx));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_compositor_add(&s, &w, (uint16_t)k_ra_compositor_no_parent, &idx));
  TEST_ASSERT_EQ(0, (int)idx);

  /* Second node must name an existing parent. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_add(&s, &w, 5U, &idx));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(&s, &w, 0U, &idx));
  TEST_ASSERT_EQ(1, (int)idx);

  /* Pool now full (cap 2): the next add is rejected. */
  TEST_ASSERT_EQ((int)k_ra_err_no_mem, (int)ra_compositor_add(&s, &w, 0U, &idx));
  TEST_END("ra_compositor add guards");
}

/* ===========================================================================
 * Layout
 * ===========================================================================
 */

/** @brief Column tiling: fixed status/footer, flex body, padding, gap. */
static void test_layout_column(void)
{
  TEST_BEGIN("ra_compositor layout column math");
  ra_compositor_widget_t pool[k_t_pool_cap] = {};
  ra_compositor_t        s                  = {};
  t_build_demo(&s, pool, (uint16_t)k_t_pool_cap, (uint32_t)k_t_footer, "p.1");

  const ra_compositor_rect_t frame = {0, 0, k_t_fb_w, k_t_fb_h};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_layout(&s, &frame));

  /* content = frame inset by pad(4) = (4,4,88,64); gaps total = 2*2 = 4. */
  /* status fixed 20 at top; footer fixed 18 at bottom; body flex = 64-20-18-4 = 22. */
  TEST_ASSERT_EQ(4, (int)s.nodes[k_t_status_i].bounds.x);
  TEST_ASSERT_EQ(4, (int)s.nodes[k_t_status_i].bounds.y);
  TEST_ASSERT_EQ(88, (int)s.nodes[k_t_status_i].bounds.w);
  TEST_ASSERT_EQ(20, (int)s.nodes[k_t_status_i].bounds.h);

  TEST_ASSERT_EQ(26, (int)s.nodes[k_t_body_i].bounds.y);
  TEST_ASSERT_EQ(22, (int)s.nodes[k_t_body_i].bounds.h);

  TEST_ASSERT_EQ(50, (int)s.nodes[k_t_footer_i].bounds.y);
  TEST_ASSERT_EQ(18, (int)s.nodes[k_t_footer_i].bounds.h);
  TEST_END("ra_compositor layout column math");
}

/** @brief Row tiling with flex weights 1:3 and an all-fixed row (no flex). */
static void test_layout_row_and_fixed(void)
{
  TEST_BEGIN("ra_compositor layout row + flex split");
  ra_compositor_widget_t pool[4] = {};
  ra_compositor_t        s       = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(&s, pool, 4U));
  uint16_t               idx     = 0U;
  ra_compositor_widget_t row     = {.kind    = k_ra_compositor_kind_container,
                                    .visible = true,
                                    .axis    = k_ra_compositor_axis_row};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_compositor_add(&s, &row, (uint16_t)k_ra_compositor_no_parent, &idx));
  ra_compositor_widget_t a = {.kind = k_ra_compositor_kind_rect, .visible = true, .flex = 1U};
  ra_compositor_widget_t b = {.kind = k_ra_compositor_kind_rect, .visible = true, .flex = 3U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(&s, &a, 0U, &idx));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(&s, &b, 0U, &idx));

  const ra_compositor_rect_t frame = {0, 0, 80, 40};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_layout(&s, &frame));
  /* width 80 split 1:3 -> 20 and 60; cross fills height 40. */
  TEST_ASSERT_EQ(0, (int)s.nodes[1].bounds.x);
  TEST_ASSERT_EQ(20, (int)s.nodes[1].bounds.w);
  TEST_ASSERT_EQ(40, (int)s.nodes[1].bounds.h);
  TEST_ASSERT_EQ(20, (int)s.nodes[2].bounds.x);
  TEST_ASSERT_EQ(60, (int)s.nodes[2].bounds.w);

  /* Now make both children fixed (no flex weight at all -> s_flex_extent=0). */
  s.nodes[1].fixed = 10;
  s.nodes[1].flex  = 0U;
  s.nodes[2].fixed = 25;
  s.nodes[2].flex  = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_layout(&s, &frame));
  TEST_ASSERT_EQ(10, (int)s.nodes[1].bounds.w);
  TEST_ASSERT_EQ(25, (int)s.nodes[2].bounds.w);
  TEST_END("ra_compositor layout row + flex split");
}

/** @brief Layout guards, invisible-child skip, and overflow clamping. */
static void test_layout_edges(void)
{
  TEST_BEGIN("ra_compositor layout guards + clamps");
  ra_compositor_widget_t pool[4] = {};
  ra_compositor_t        s       = {};
  const ra_compositor_rect_t frame = {0, 0, 40, 40};

  /* Guards: null scene / frame, empty scene. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_layout(nullptr, &frame));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(&s, pool, 4U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_layout(&s, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_layout(&s, &frame));

  /* Root with a huge pad (content clamps to 0) plus an invisible child. */
  uint16_t               idx  = 0U;
  ra_compositor_widget_t root = {.kind    = k_ra_compositor_kind_container,
                                 .visible = true,
                                 .axis    = k_ra_compositor_axis_col,
                                 .pad     = 100};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_compositor_add(&s, &root, (uint16_t)k_ra_compositor_no_parent, &idx));
  ra_compositor_widget_t big = {.kind = k_ra_compositor_kind_rect, .visible = true, .fixed = 999};
  ra_compositor_widget_t flx = {.kind = k_ra_compositor_kind_rect, .visible = true, .flex = 1U};
  ra_compositor_widget_t hid = {.kind = k_ra_compositor_kind_rect, .visible = false, .flex = 1U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(&s, &big, 0U, &idx));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(&s, &flx, 0U, &idx));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(&s, &hid, 0U, &idx));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_layout(&s, &frame));
  /* Pad 100 on a 40x40 frame -> content clamped, flex space clamped to 0. */
  TEST_ASSERT_EQ(0, (int)s.nodes[2].bounds.h); /* flex child gets nothing  */
  TEST_ASSERT_EQ(0, (int)s.nodes[3].bounds.w); /* invisible child untouched */
  TEST_ASSERT_EQ(0, (int)s.nodes[3].bounds.h);
  TEST_END("ra_compositor layout guards + clamps");
}

/* ===========================================================================
 * Damage + invalidate
 * ===========================================================================
 */

/** @brief Damage union over visible+dirty nodes; guards; invisible skip. */
static void test_damage_and_invalidate(void)
{
  TEST_BEGIN("ra_compositor damage + invalidate");
  ra_compositor_widget_t pool[k_t_pool_cap] = {};
  ra_compositor_t        s                  = {};
  t_build_demo(&s, pool, (uint16_t)k_t_pool_cap, (uint32_t)k_t_footer, "p.1");
  const ra_compositor_rect_t frame = {0, 0, k_t_fb_w, k_t_fb_h};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_layout(&s, &frame));

  ra_compositor_rect_t rect = {1, 2, 3, 4};
  uint16_t             n    = 9U;

  /* Guards. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_damage(nullptr, &rect, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_damage(&s, nullptr, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_damage(&s, &rect, nullptr));

  /* Nothing dirty -> empty rect, count 0. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_damage(&s, &rect, &n));
  TEST_ASSERT_EQ(0, (int)n);
  TEST_ASSERT_EQ(0, (int)rect.w);
  TEST_ASSERT_EQ(0, (int)rect.h);

  /* invalidate guards. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_invalidate(nullptr, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_invalidate(&s, 99U));

  /* One dirty node -> damage equals its bounds. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_invalidate(&s, k_t_status_i));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_damage(&s, &rect, &n));
  TEST_ASSERT_EQ(1, (int)n);
  TEST_ASSERT_EQ((int)s.nodes[k_t_status_i].bounds.y, (int)rect.y);
  TEST_ASSERT_EQ((int)s.nodes[k_t_status_i].bounds.h, (int)rect.h);

  /* Two dirty nodes -> union spans status top to footer bottom. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_invalidate(&s, k_t_footer_i));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_damage(&s, &rect, &n));
  TEST_ASSERT_EQ(2, (int)n);
  TEST_ASSERT_EQ(4, (int)rect.y); /* status top */
  TEST_ASSERT_EQ(64, (int)rect.h); /* down to footer bottom (50+18-4) */

  /* An invisible dirty node is ignored by damage. */
  s.nodes[k_t_body_i].visible = false;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_invalidate(&s, k_t_body_i));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_damage(&s, &rect, &n));
  TEST_ASSERT_EQ(2, (int)n); /* body skipped despite being dirty */

  /* clear_dirty guards + effect. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_clear_dirty(nullptr));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_clear_dirty(&s));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_damage(&s, &rect, &n));
  TEST_ASSERT_EQ(0, (int)n);
  TEST_END("ra_compositor damage + invalidate");
}

/* ===========================================================================
 * Render guards
 * ===========================================================================
 */

/** @brief Render + compose guard contracts (null, empty scene). */
static void test_render_guards(void)
{
  TEST_BEGIN("ra_compositor render/compose guards");
  ra_compositor_widget_t pool[2] = {};
  ra_compositor_t        s       = {};
  const ra_compositor_rect_t frame = {0, 0, 8, 8};
  ra_compositor_rect_t       dmg   = {};
  uint16_t                   n     = 0U;

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_gfx_init(s_fb, (uint16_t)k_t_fb_w, (uint16_t)k_t_fb_h,
                                  k_ra_gfx_format_argb8888));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_render(nullptr, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_compose_full(nullptr, &frame));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_compositor_compose_full(&s, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_compose_dirty(nullptr, &frame, &dmg, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_compose_dirty(&s, nullptr, &dmg, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_compose_dirty(&s, &frame, nullptr, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_compositor_compose_dirty(&s, &frame, &dmg, nullptr));

  /* Empty scene -> render / compose forward invalid_arg from the count check. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(&s, pool, 2U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_render(&s, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_compositor_compose_full(&s, &frame));
  TEST_END("ra_compositor render/compose guards");
}

/* ===========================================================================
 * The headline test: dirty-region == full recomposite + CRC golden
 * ===========================================================================
 */

/**
 * @brief Demo + proof: composite a sample tree, change the footer, and show the
 *        dirty-region recomposite is byte-identical to a full recomposite.
 */
static void test_dirty_equals_full_and_crc(void)
{
  TEST_BEGIN("ra_compositor dirty==full recomposite + CRC golden");
  const ra_compositor_rect_t frame = {0, 0, k_t_fb_w, k_t_fb_h};

  /* Frame 1: full composite of the sample tree into s_fb. */
  ra_compositor_widget_t pool[k_t_pool_cap] = {};
  ra_compositor_t        s                  = {};
  t_build_demo(&s, pool, (uint16_t)k_t_pool_cap, (uint32_t)k_t_footer, "p.1");
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_gfx_init(s_fb, (uint16_t)k_t_fb_w, (uint16_t)k_t_fb_h,
                                  k_ra_gfx_format_argb8888));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_compose_full(&s, &frame));

  const uint32_t crc_initial = t_crc32((const uint8_t*)s_fb, sizeof s_fb);
  (void)fprintf(stderr, "[CRC ] initial = 0x%08X\n", crc_initial);

  /* Structural spot checks (no dependence on the exact colour encoding). */
  TEST_ASSERT(t_px(0, 0) == t_px(2, 2));           /* pad band is uniform bg     */
  const uint32_t status_pre = t_px(80, 14);        /* status bar fill            */
  const uint32_t footer_pre = t_px(80, 59);        /* footer fill (frame 1)      */
  TEST_ASSERT(status_pre != t_px(0, 0));           /* status painted over bg     */
  TEST_ASSERT(t_px(48, 37) != status_pre);         /* body differs from status   */

  /* Change only the footer, invalidate it, and composite just the damage. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_invalidate(&s, k_t_footer_i));
  s.nodes[k_t_footer_i].color = (uint32_t)k_t_footer2;
  s.nodes[k_t_footer_i].text  = "p.2";

  ra_compositor_rect_t dmg = {};
  uint16_t             dn  = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_compose_dirty(&s, &frame, &dmg, &dn));
  TEST_ASSERT_EQ(1, (int)dn);
  TEST_ASSERT_EQ((int)s.nodes[k_t_footer_i].bounds.x, (int)dmg.x);
  TEST_ASSERT_EQ((int)s.nodes[k_t_footer_i].bounds.y, (int)dmg.y);
  TEST_ASSERT_EQ((int)s.nodes[k_t_footer_i].bounds.w, (int)dmg.w);
  TEST_ASSERT_EQ((int)s.nodes[k_t_footer_i].bounds.h, (int)dmg.h);

  /* Inside the damage the footer changed; outside it the status is untouched. */
  TEST_ASSERT(t_px(80, 59) != footer_pre);
  TEST_ASSERT(t_px(80, 14) == status_pre);

  const uint32_t crc_incremental = t_crc32((const uint8_t*)s_fb, sizeof s_fb);

  /* Frame 2 reference: full recomposite of the changed tree into s_fb2. */
  const uint32_t crc_changed = t_full_compose(s_fb2, (uint32_t)k_t_footer2, "p.2");
  (void)fprintf(stderr, "[CRC ] changed = 0x%08X  incremental = 0x%08X\n", crc_changed,
                crc_incremental);

  /* The proof: the dirty-region update equals a full recomposite, byte for byte. */
  TEST_ASSERT(memcmp(s_fb, s_fb2, sizeof s_fb) == 0);
  TEST_ASSERT_EQ((int64_t)crc_changed, (int64_t)crc_incremental);

  /* CRC goldens pin the exact composited output. */
  TEST_ASSERT_EQ((int64_t)(uint32_t)k_golden_initial, (int64_t)crc_initial);
  TEST_ASSERT_EQ((int64_t)(uint32_t)k_golden_changed, (int64_t)crc_changed);
  TEST_END("ra_compositor dirty==full recomposite + CRC golden");
}

int main(void)
{
  test_init_guards();
  test_add_guards();
  test_layout_column();
  test_layout_row_and_fixed();
  test_layout_edges();
  test_damage_and_invalidate();
  test_render_guards();
  test_dirty_equals_full_and_crc();
  return 0;
}
