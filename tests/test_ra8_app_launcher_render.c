/**
 * @file test_ra8_app_launcher_render.c
 * @brief Widget-rendered CRC golden of the app-framework chrome + launcher.
 *
 * @details
 * Ties the app framework (`ra8_app`, #146) to the composable widget layer
 * (`ra8_widget`, #145): the "chrome" enumerates the app **registry** and
 * composes a **launcher** -- a header label over one label tile per registered
 * app, tiled by an ::ra8_widget_panel column -- into an in-memory `ra8_gfx`
 * framebuffer, and the result is pinned with a CRC-32 golden. This proves an
 * "app = a launcher tile" composes deterministically and, crucially, that the
 * launcher **reflects the registry**: uninstalling the removable app drops its
 * tile (a different CRC golden), while a refused uninstall of a *core* app
 * leaves the launcher byte-identical -- the run-time "core uninstallable"
 * invariant, observed through pixels.
 *
 * The launcher build is the same enumeration a home screen does on-target
 * (`ra8_app_count` + `ra8_app_at` + `app->name`), so this is a faithful host
 * proxy for the on-panel chrome. The pixels are drawn through the injected
 * ::ra8_widget_paint_t backend bound to `ra8_gfx` -- the same Dependency-Inversion
 * seam the widget unit tests bind to a recording mock. There are no compound
 * boolean decisions under test here (the logic lives in `ra8_app` / `ra8_widget`,
 * each tested in its own TU), so there is no `@par MC/DC` obligation in this
 * file; it asserts the two goldens and the registry-reflects invariant.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_app.h"
#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "unity_minimal.h"

/**
 * @enum app_launcher_render_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_crc32_init           = 0xFFFFFFFFU, /**< CRC-32 initial value, and the final XOR-out. */
  k_crc32_poly_reflected = 0xEDB88320U, /**< The reflected CRC-32 polynomial.             */
} app_launcher_render_uint32_const_t;

/**
 * @enum lr_geom_t
 * @brief Launcher framebuffer + tile geometry (pixels).
 */
typedef enum : int32_t {
  k_lr_fb_w     = 128, /**< Framebuffer width.                    */
  k_lr_fb_h     = 96,  /**< Framebuffer height.                   */
  k_lr_pad      = 4,   /**< Root panel inner padding.             */
  k_lr_gap      = 2,   /**< Gap between the header and the tiles. */
  k_lr_header_h = 18,  /**< Header label fixed height.            */
  k_lr_tile_h   = 16,  /**< Per-app launcher tile fixed height.   */
  k_lr_text_pad = 0,   /**< Label text inset (top-left origin).   */
} lr_geom_t;

/**
 * @enum lr_cap_t
 * @brief Static capacities for the launcher widget tree (no allocation).
 */
typedef enum : uint16_t {
  k_lr_child_cap = 8U, /**< Root child slots (header + up to 7 tiles).     */
  k_lr_box_cap   = 9U, /**< ra8_box layout scratch (child cap + 1 parent). */
} lr_cap_t;

/**
 * @enum lr_color_t
 * @brief Distinct 0x00RRGGBB colours for the launcher scene.
 */
typedef enum : uint32_t {
  k_lr_bg     = 0x00101820U, /**< Root background (panel padding / gaps). */
  k_lr_header = 0x00204060U, /**< Header label fill.                      */
  k_lr_tile   = 0x00303030U, /**< Launcher tile fill.                     */
  k_lr_fg     = 0x00FFFFFFU, /**< Text colour.                            */
} lr_color_t;

/**
 * @enum lr_app_id_t
 * @brief App ids registered into the launcher's registry.
 */
typedef enum : uint16_t {
  k_lr_id_library  = 1U, /**< Core file/book organizer.    */
  k_lr_id_reader   = 2U, /**< Core EPUB reader.            */
  k_lr_id_settings = 3U, /**< Optional/removable settings. */
} lr_app_id_t;

/**
 * @enum lr_golden_t
 * @brief CRC-32 goldens of the composited launcher (baked from a run).
 */
typedef enum : uint32_t {
  k_lr_golden_a = 0xA6AA065AU, /**< Frame A: library + reader + settings tiles. */
  k_lr_golden_b = 0x759DF0ECU, /**< Frame B: settings uninstalled (2 tiles).    */
} lr_golden_t;

/** @brief Framebuffer for frame A (and the reflowed frame B). */
static uint32_t s_lr_fb[k_lr_fb_w * k_lr_fb_h];
/** @brief Second framebuffer for the core-refused recomposite check. */
static uint32_t s_lr_fb2[k_lr_fb_w * k_lr_fb_h];

/** @brief Empty lifecycle vtable: the launcher renders from `name` only. */
static const ra8_app_vtable_t k_lr_vt = {};

/** @brief Root child widgets (the header + one tile per app). */
static ra8_widget_t s_lr_kids[k_lr_child_cap];
/** @brief Label descriptors backing every child widget. */
static ra8_widget_label_t s_lr_labels[k_lr_child_cap];
/** @brief ra8_box layout scratch for the root panel. */
static ra8_box_t s_lr_box[k_lr_box_cap];

/** @brief CRC-32 (reflected, poly 0xEDB88320) over a byte span. */
static uint32_t lr_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_crc32_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= (uint32_t)data[i];
    for (uint32_t b = 0U; b < 8U; ++b) {
      if ((crc & 1U) != 0U) {
        crc = (crc >> 1U) ^ k_crc32_poly_reflected;
      } else {
        crc = crc >> 1U;
      }
    }
  }
  return ~crc;
}

/* ===========================================================================
 * ra8_gfx paint backend (the DI seam the widgets paint through)
 * =========================================================================== */

/** @brief Paint backend: fill a rectangle through ra8_gfx. */
static void lr_fill(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  (void)user;
  (void)ra8_gfx_rect(x, y, w, h, color, true);
}

/** @brief Paint backend: draw ASCII text through ra8_gfx. */
static void lr_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  (void)user;
  (void)ra8_gfx_text_out(x, y, str, &ra8_gfx_font_8x16, fg, bg);
}

/** @brief Paint backend: measure a string's pixel size through ra8_gfx. */
static void lr_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  (void)user;
  uint32_t w = 0U;
  uint32_t h = 0U;
  (void)ra8_gfx_text_size(str, &ra8_gfx_font_8x16, &w, &h);
  *out_w = (int32_t)w;
  *out_h = (int32_t)h;
}

/** @brief The single paint backend the launcher widgets share. */
static const ra8_widget_paint_t k_lr_paint = {
  .user      = nullptr,
  .fill_rect = lr_fill,
  .draw_text = lr_text,
  .text_size = lr_size,
};

/** @brief Build a static app instance the launcher can enumerate + render. */
static ra8_app_t lr_make_app(uint16_t id, const char* name, bool removable)
{
  ra8_app_t a = {};
  a.vt        = &k_lr_vt;
  a.id        = id;
  a.name      = name;
  a.removable = removable;
  return a;
}

/** @brief Bind child slot @p idx to a label tile with @p text over @p bg. */
static void lr_bind_label(uint16_t idx, const char* text, uint32_t bg, int16_t fixed)
{
  s_lr_labels[idx] = (ra8_widget_label_t){.paint = &k_lr_paint,
                                          .text  = text,
                                          .fg    = (uint32_t)k_lr_fg,
                                          .bg    = bg,
                                          .pad   = (int16_t)k_lr_text_pad,
                                          .align = k_ra8_widget_align_left};
  s_lr_kids[idx]   = (ra8_widget_t){};
  (void)ra8_widget_label_init(&s_lr_kids[idx], &s_lr_labels[idx]);
  s_lr_kids[idx].fixed   = fixed;
  s_lr_kids[idx].visible = true;
}

/**
 * @brief Compose the launcher: a header label over one tile label per app.
 *
 * @details Enumerates @p reg the way a home screen does -- `ra8_app_count` then
 * `ra8_app_at` -- binding a label tile carrying each app's `name` under the root
 * column panel. The NULL-slot guard mirrors the on-target chrome
 * (`app_shell_log_menu`). The root panel paints nothing itself, so the frame is
 * pre-cleared to the background colour; the header + tiles overpaint their bands.
 */
static void
lr_build_launcher(ra8_widget_t* root, ra8_widget_panel_t* panel, const ra8_app_registry_t* reg)
{
  uint16_t n = 0U;
  lr_bind_label(n, "Apps", (uint32_t)k_lr_header, (int16_t)k_lr_header_h);
  n++;

  uint16_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_count(reg, &count));
  for (uint16_t i = 0U; (i < count) && (n < (uint16_t)k_lr_child_cap); ++i) {
    ra8_app_t* app = nullptr;
    if (ra8_app_at(reg, i, &app) == k_ra8_ok) {
      if (app != nullptr) {
        lr_bind_label(n, app->name, (uint32_t)k_lr_tile, (int16_t)k_lr_tile_h);
        n++;
      }
    }
  }

  *panel = (ra8_widget_panel_t){.children    = s_lr_kids,
                                .box_scratch = s_lr_box,
                                .count       = n,
                                .box_cap     = (uint16_t)k_lr_box_cap,
                                .gap         = (int16_t)k_lr_gap,
                                .pad         = (int16_t)k_lr_pad,
                                .axis        = k_ra8_widget_axis_col};
  *root  = (ra8_widget_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_init(root, panel));
}

/** @brief Build + full-compose the launcher for @p reg into @p buf; return CRC. */
static uint32_t lr_compose_crc(uint32_t* buf, const ra8_app_registry_t* reg)
{
  ra8_widget_t        root  = {};
  ra8_widget_panel_t  panel = {};
  const ra8_ui_rect_t frame = {0, 0, k_lr_fb_w, k_lr_fb_h};
  lr_build_launcher(&root, &panel, reg);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(buf, (uint16_t)k_lr_fb_w, (uint16_t)k_lr_fb_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_lr_bg));
  for (uint16_t i = 0U; i < panel.count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_invalidate(&s_lr_kids[i], k_ra8_widget_refresh_quality));
  }
  ra8_ui_rect_t        damage = {};
  ra8_widget_refresh_t hint   = k_ra8_widget_refresh_none;
  uint16_t             dirty  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_widget_panel_compose(&root, &frame, &damage, &hint, &dirty));
  return lr_crc32((const uint8_t*)buf, (size_t)(k_lr_fb_w * k_lr_fb_h) * sizeof(uint32_t));
}

/**
 * @test The chrome launcher composites to a CRC golden and reflects the registry.
 *
 * @details
 * Frame A composites a three-app launcher (library, reader, settings) to a CRC
 * golden. A refused uninstall of the core `library` app leaves the launcher
 * byte-identical (the run-time core-uninstallable invariant, seen in pixels).
 * Uninstalling the removable `settings` app drops its tile, yielding the
 * distinct frame-B golden -- the launcher visibly reflects the registry change.
 */
static void test_launcher_render_golden(void)
{
  TEST_BEGIN("ra8_app: widget launcher CRC golden + uninstall reflow");
  ra8_app_t          a_lib = lr_make_app((uint16_t)k_lr_id_library, "library", false);
  ra8_app_t          a_rdr = lr_make_app((uint16_t)k_lr_id_reader, "reader", false);
  ra8_app_t          a_set = lr_make_app((uint16_t)k_lr_id_settings, "settings", true);
  ra8_app_t*         slots[3];
  ra8_app_registry_t reg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg, slots, 3U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a_lib));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a_rdr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a_set));

  /* Frame A: all three tiles. */
  const uint32_t crc_a = lr_compose_crc(s_lr_fb, &reg);

  /* Core uninstall is refused -> the launcher must be byte-identical. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_app_uninstall(&reg, (uint16_t)k_lr_id_library));
  const uint32_t crc_a2 = lr_compose_crc(s_lr_fb2, &reg);
  TEST_ASSERT_EQ(crc_a, crc_a2);

  /* Uninstall the removable app -> its tile disappears (distinct golden). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_uninstall(&reg, (uint16_t)k_lr_id_settings));
  const uint32_t crc_b = lr_compose_crc(s_lr_fb, &reg);

  TEST_ASSERT_EQ(k_lr_golden_a, crc_a);
  TEST_ASSERT_EQ(k_lr_golden_b, crc_b);
  TEST_ASSERT(crc_a != crc_b);
  TEST_END("ra8_app: widget launcher CRC golden + uninstall reflow");
}

int main(void)
{
  test_launcher_render_golden();
  return 0;
}
