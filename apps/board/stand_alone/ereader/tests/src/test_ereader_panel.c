/**
 * @file test_ereader_panel.c
 * @brief Host unit tests for apps/board/stand_alone/ereader panel bring-up and refresh modes.
 *
 * @details
 * Exercises ``er_panel_init``, ``display_bind_gfx``, ``er_panel_set_refresh_mode``,
 * ``er_panel_cycle_refresh_mode``, and ``er_panel_render_frame`` against a fake
 * ``display_backend_iface_t`` backed by a host framebuffer so real ``ra8_display_pal``
 * dispatch, ``ra8_gfx`` drawing, and ``display_policy_decide`` transitions run together.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "er_panel.h"
#include "ra8_attributes.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_internal.h"
#include "ra8_display_pal_policy.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @enum t_er_panel_dims_t @brief Host test panel geometry. */
typedef enum : uint16_t {
  k_t_panel_w     = 320U, /**< Synthetic test framebuffer width, px.  */
  k_t_panel_h     = 180U, /**< Synthetic test framebuffer height, px. */
  k_t_clean_every = 3U,   /**< Periodic full-clean cadence for tests. */
} t_er_panel_dims_t;

/** @brief Host mock backend context tracking flush rectangles and hints. */
typedef struct {
  display_fb_t           fb;
  display_caps_t         caps;
  display_refresh_hint_t flushes[16];
  display_rect_t         last_rect;
  uint32_t               flush_count;
  uint32_t               clear_count;
  bool                   deinit_called;
  bool                   fail_init;
} t_mock_backend_ctx_t;

static uint16_t             s_test_fb[(size_t)k_t_panel_w * (size_t)k_t_panel_h];
static t_mock_backend_ctx_t s_mock_ctx;

RA8_INTERNAL
static ra8_err_t mock_init(const display_cfg_t* cfg, void** out_ctx)
{
  if ((cfg == nullptr) || (out_ctx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (s_mock_ctx.fail_init) {
    return k_ra8_err_hw_init_failed;
  }
  s_mock_ctx.caps = (display_caps_t){
    .width_px                = cfg->width_px,
    .height_px               = cfg->height_px,
    .pixfmt                  = cfg->pixfmt,
    .stride_bytes            = (uint32_t)cfg->width_px * 2U,
    .refresh_latency_us_typ  = 0U,
    .supports_partial_update = true,
    .continuous_refresh      = false,
  };
  s_mock_ctx.fb = (display_fb_t){
    .pixels       = cfg->framebuffer,
    .width_px     = cfg->width_px,
    .height_px    = cfg->height_px,
    .stride_bytes = (uint32_t)cfg->width_px * 2U,
    .pixfmt       = cfg->pixfmt,
  };
  *out_ctx = &s_mock_ctx;
  return k_ra8_ok;
}

RA8_INTERNAL
static ra8_err_t mock_deinit(void* ctx)
{
  t_mock_backend_ctx_t* m = (t_mock_backend_ctx_t*)ctx;
  if (m == nullptr) {
    return k_ra8_err_null_ptr;
  }
  m->deinit_called = true;
  return k_ra8_ok;
}

RA8_INTERNAL
static ra8_err_t mock_get_caps(const void* ctx, display_caps_t* out)
{
  const t_mock_backend_ctx_t* m = (const t_mock_backend_ctx_t*)ctx;
  if ((m == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = m->caps;
  return k_ra8_ok;
}

RA8_INTERNAL
static ra8_err_t mock_get_framebuffer(void* ctx, display_fb_t* out)
{
  t_mock_backend_ctx_t* m = (t_mock_backend_ctx_t*)ctx;
  if ((m == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = m->fb;
  return k_ra8_ok;
}

RA8_INTERNAL
static ra8_err_t mock_flush(void* ctx, display_rect_t rect, display_refresh_hint_t hint)
{
  t_mock_backend_ctx_t* m = (t_mock_backend_ctx_t*)ctx;
  if (m == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (m->flush_count < (uint32_t)(sizeof(m->flushes) / sizeof(m->flushes[0]))) {
    m->flushes[m->flush_count] = hint;
  }
  m->last_rect = rect;
  m->flush_count++;
  return k_ra8_ok;
}

RA8_INTERNAL
static ra8_err_t mock_clear(void* ctx, uint32_t color)
{
  (void)color;
  t_mock_backend_ctx_t* m = (t_mock_backend_ctx_t*)ctx;
  if (m == nullptr) {
    return k_ra8_err_null_ptr;
  }
  m->clear_count++;
  return k_ra8_ok;
}

static const display_backend_iface_t k_mock_iface = {
  .init            = mock_init,
  .deinit          = mock_deinit,
  .get_caps        = mock_get_caps,
  .get_framebuffer = mock_get_framebuffer,
  .flush           = mock_flush,
  .clear           = mock_clear,
};

RA8_INTERNAL
static void reset_mock(void)
{
  (void)memset(s_test_fb, 0, sizeof(s_test_fb));
  s_mock_ctx = (t_mock_backend_ctx_t){};
}

RA8_INTERNAL
static display_cfg_t make_valid_cfg(void)
{
  return (display_cfg_t){
    .iface             = &k_mock_iface,
    .framebuffer       = s_test_fb,
    .framebuffer_bytes = (uint32_t)sizeof(s_test_fb),
    .width_px          = k_t_panel_w,
    .height_px         = k_t_panel_h,
    .pixfmt            = k_display_pixfmt_rgb565,
    .panel_timing      = nullptr,
  };
}

RA8_INTERNAL
static uint32_t fb_checksum(void)
{
  uint32_t h = 2166136261U;
  for (size_t i = 0U; i < (sizeof(s_test_fb) / sizeof(s_test_fb[0])); ++i) {
    h = (h ^ (uint32_t)s_test_fb[i]) * 16777619U;
  }
  return h;
}

/** @test Init binds display PAL + ra8_gfx and initial render draws non-zero pixels and flushes INIT. */
RA8_INTERNAL
static void test_init_bind_and_render(void)
{
  TEST_BEGIN("er_panel: init + display_bind_gfx + initial render");
  reset_mock();
  er_panel_state_t    panel = {};
  const display_cfg_t cfg   = make_valid_cfg();

  TEST_ASSERT_EQ(k_ra8_ok, er_panel_init(&panel, &cfg, k_er_refresh_fast_clean, k_t_clean_every));
  TEST_ASSERT(panel.initialized);
  TEST_ASSERT_EQ(k_er_refresh_fast_clean, panel.mode);

  const uint32_t before = fb_checksum();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    er_panel_render_frame(&panel, k_display_event_open, "RA8D2 E-Reader", "Display PAL test"));
  const uint32_t after = fb_checksum();
  TEST_ASSERT(after != before);
  TEST_ASSERT_EQ(1U, s_mock_ctx.flush_count);
  TEST_ASSERT_EQ(k_display_refresh_init, s_mock_ctx.flushes[0]);
  TEST_ASSERT_EQ(k_t_panel_w, s_mock_ctx.last_rect.w);
  TEST_ASSERT_EQ(k_t_panel_h, s_mock_ctx.last_rect.h);
  TEST_ASSERT_EQ(1U, panel.frame_count);

  TEST_ASSERT_EQ(k_ra8_ok, er_panel_deinit(&panel));
  TEST_ASSERT(s_mock_ctx.deinit_called);
  TEST_ASSERT(!panel.initialized);
  TEST_END("er_panel: init + display_bind_gfx + initial render");
}

/** @test Switching selectable refresh modes changes policy flush hints cleanly. */
RA8_INTERNAL
static void test_refresh_mode_switching(void)
{
  TEST_BEGIN("er_panel: selectable full vs fast refresh mode switching");
  reset_mock();
  er_panel_state_t    panel = {};
  const display_cfg_t cfg   = make_valid_cfg();

  TEST_ASSERT_EQ(k_ra8_ok, er_panel_init(&panel, &cfg, k_er_refresh_fast, k_t_clean_every));
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_render_frame(&panel, k_display_event_turn, "Title", "Fast 1"));
  TEST_ASSERT_EQ(k_display_refresh_fast, panel.last_hint);

  /* Cycle fast -> full: every subsequent turn emits quality (full) refresh. */
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_cycle_refresh_mode(&panel));
  TEST_ASSERT_EQ(k_er_refresh_full, panel.mode);
  const uint32_t fast_crc = fb_checksum();
  TEST_ASSERT_EQ(k_ra8_ok,
                 er_panel_render_frame(&panel, k_display_event_turn, "Title", "Full mode"));
  TEST_ASSERT_EQ(k_display_refresh_quality, panel.last_hint);
  TEST_ASSERT(fb_checksum() != fast_crc);

  /* Cycle full -> fast+clean: emits clean_every cadence (fast, fast, periodic quality). */
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_cycle_refresh_mode(&panel));
  TEST_ASSERT_EQ(k_er_refresh_fast_clean, panel.mode);
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_render_frame(&panel, k_display_event_turn, "Title", "Turn 1"));
  TEST_ASSERT_EQ(k_display_refresh_fast, panel.last_hint);
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_render_frame(&panel, k_display_event_turn, "Title", "Turn 2"));
  TEST_ASSERT_EQ(k_display_refresh_fast, panel.last_hint);
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_render_frame(&panel, k_display_event_turn, "Title", "Turn 3"));
  TEST_ASSERT_EQ(k_display_refresh_quality, panel.last_hint);

  /* Cycle fast+clean -> fast wraps cleanly back to fast. */
  TEST_ASSERT_EQ(k_ra8_ok, er_panel_cycle_refresh_mode(&panel));
  TEST_ASSERT_EQ(k_er_refresh_fast, panel.mode);

  TEST_ASSERT_EQ(k_ra8_ok, er_panel_deinit(&panel));
  TEST_END("er_panel: selectable full vs fast refresh mode switching");
}

/** @test Null, uninitialized, out-of-range, and backend failure guards fail closed. */
RA8_INTERNAL
static void test_error_guards(void)
{
  TEST_BEGIN("er_panel: negative error guards and state safety");
  reset_mock();
  er_panel_state_t    panel = {};
  const display_cfg_t cfg   = make_valid_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, er_panel_init(nullptr, &cfg, k_er_refresh_fast, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, er_panel_init(&panel, nullptr, k_er_refresh_fast, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, er_panel_init(&panel, &cfg, k_er_refresh_mode_count, 0U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 er_panel_set_refresh_mode(&panel, k_er_refresh_full, 0U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 er_panel_render_frame(&panel, k_display_event_turn, "A", "B"));

  s_mock_ctx.fail_init = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, er_panel_init(&panel, &cfg, k_er_refresh_fast, 0U));
  TEST_ASSERT(!panel.initialized);
  s_mock_ctx.fail_init = false;

  TEST_ASSERT_EQ(k_ra8_ok, er_panel_init(&panel, &cfg, k_er_refresh_fast, 0U));
  TEST_ASSERT_EQ(k_ra8_err_busy, er_panel_init(&panel, &cfg, k_er_refresh_full, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 er_panel_set_refresh_mode(&panel, (er_refresh_mode_t)99U, 0U));
  TEST_ASSERT_EQ(k_er_refresh_fast, panel.mode);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 er_panel_render_frame(&panel, k_display_event_turn, nullptr, "B"));

  TEST_ASSERT_EQ(k_ra8_ok, er_panel_deinit(&panel));
  TEST_END("er_panel: negative error guards and state safety");
}

int main(void)
{
  test_init_bind_and_render();
  test_refresh_mode_switching();
  test_error_guards();
  return 0;
}
