/**
 * @file test_ra_app_launcher_render.c
 * @brief Compositor-rendered CRC golden of the app-framework chrome + launcher.
 *
 * @details
 * Ties the app framework (`ra_app`, #146) to the framebuffer compositor
 * (`ra_compositor`, #145): the "chrome" enumerates the app **registry** and
 * renders a **launcher** -- a header bar plus one tile per registered app --
 * into an in-memory `ra_gfx` framebuffer, and the result is pinned with a
 * CRC-32 golden. This proves an "app = a launcher tile" composes deterministically
 * and, crucially, that the launcher **reflects the registry**: uninstalling the
 * removable app drops its tile (a different CRC golden), while a refused
 * uninstall of a *core* app leaves the launcher byte-identical -- the run-time
 * "core uninstallable" invariant, observed through pixels.
 *
 * The launcher build is the same enumeration a home screen does on-target
 * (`ra_app_count` + `ra_app_at` + `app->name`), so this is a faithful host proxy
 * for the on-panel chrome. There are no compound boolean decisions under test
 * here (the logic lives in `ra_app` / `ra_compositor`, each tested in its own
 * TU), so there is no `@par MC/DC` obligation in this file; it asserts the two
 * goldens and the registry-reflects invariant.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_app.h"
#include "ra_compositor.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "unity_minimal.h"

/**
 * @enum lr_geom_t
 * @brief Launcher framebuffer + tile geometry (pixels).
 */
typedef enum : int32_t {
  k_lr_fb_w     = 128, /**< Framebuffer width.                       */
  k_lr_fb_h     = 96,  /**< Framebuffer height.                      */
  k_lr_pad      = 4,   /**< Root container inner padding.            */
  k_lr_gap      = 2,   /**< Gap between the header and the tiles.    */
  k_lr_header_h = 18,  /**< Header bar fixed height.                 */
  k_lr_tile_h   = 16,  /**< Per-app launcher tile fixed height.      */
  k_lr_pool_cap = 8,   /**< Scene node pool (root + header + tiles). */
} lr_geom_t;

/**
 * @enum lr_color_t
 * @brief Distinct 0x00RRGGBB colours for the launcher scene.
 */
typedef enum : uint32_t {
  k_lr_bg     = 0x00101820U, /**< Root background.    */
  k_lr_header = 0x00204060U, /**< Header bar fill.    */
  k_lr_tile   = 0x00303030U, /**< Launcher tile fill. */
  k_lr_fg     = 0x00FFFFFFU, /**< Text colour.        */
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
static const ra_app_vtable_t k_lr_vt = {};

/** @brief CRC-32 (reflected, poly 0xEDB88320) over a byte span. */
static uint32_t lr_crc32(const uint8_t* data, size_t len)
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

/** @brief Build a static app instance the launcher can enumerate + render. */
static ra_app_t lr_make_app(uint16_t id, const char* name, bool removable)
{
  ra_app_t a  = {};
  a.vt        = &k_lr_vt;
  a.id        = id;
  a.name      = name;
  a.removable = removable;
  return a;
}

/**
 * @brief Compose the launcher: a bg root, a header bar, and one tile per app.
 *
 * @details Enumerates @p reg the way a home screen does -- `ra_app_count` then
 * `ra_app_at` -- adding a label tile carrying each app's `name` under the root
 * column. The NULL-slot guard mirrors the on-target chrome (`app_shell_log_menu`).
 */
static void lr_build_launcher(ra_compositor_t*         s,
                              ra_compositor_widget_t*  pool,
                              uint16_t                 cap,
                              const ra_app_registry_t* reg)
{
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_init(s, pool, cap));
  uint16_t idx = 0U;

  ra_compositor_widget_t root = {.kind    = k_ra_compositor_kind_rect,
                                 .color   = (uint32_t)k_lr_bg,
                                 .visible = true,
                                 .axis    = k_ra_compositor_axis_col,
                                 .pad     = (int16_t)k_lr_pad,
                                 .gap     = (int16_t)k_lr_gap};
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_compositor_add(s, &root, (uint16_t)k_ra_compositor_no_parent, &idx));

  ra_compositor_widget_t header = {.kind    = k_ra_compositor_kind_label,
                                   .color   = (uint32_t)k_lr_header,
                                   .fg      = (uint32_t)k_lr_fg,
                                   .text    = "Apps",
                                   .visible = true,
                                   .fixed   = (int16_t)k_lr_header_h};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(s, &header, 0U, &idx));

  uint16_t count = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_count(reg, &count));
  for (uint16_t i = 0U; i < count; ++i) {
    ra_app_t* app = nullptr;
    if (ra_app_at(reg, i, &app) == k_ra_ok) {
      if (app != nullptr) {
        ra_compositor_widget_t tile = {.kind    = k_ra_compositor_kind_label,
                                       .color   = (uint32_t)k_lr_tile,
                                       .fg      = (uint32_t)k_lr_fg,
                                       .text    = app->name,
                                       .visible = true,
                                       .fixed   = (int16_t)k_lr_tile_h};
        uint16_t               ti   = 0U;
        TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_add(s, &tile, 0U, &ti));
      }
    }
  }
}

/** @brief Build + full-compose the launcher for @p reg into @p buf; return CRC. */
static uint32_t lr_compose_crc(uint32_t* buf, const ra_app_registry_t* reg)
{
  ra_compositor_widget_t     pool[k_lr_pool_cap] = {};
  ra_compositor_t            s                   = {};
  const ra_compositor_rect_t frame               = {0, 0, k_lr_fb_w, k_lr_fb_h};
  lr_build_launcher(&s, pool, (uint16_t)k_lr_pool_cap, reg);
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_gfx_init(buf, (uint16_t)k_lr_fb_w, (uint16_t)k_lr_fb_h, k_ra_gfx_format_argb8888));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_compositor_compose_full(&s, &frame));
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
  TEST_BEGIN("ra_app: compositor launcher CRC golden + uninstall reflow");
  ra_app_t          a_lib = lr_make_app((uint16_t)k_lr_id_library, "library", false);
  ra_app_t          a_rdr = lr_make_app((uint16_t)k_lr_id_reader, "reader", false);
  ra_app_t          a_set = lr_make_app((uint16_t)k_lr_id_settings, "settings", true);
  ra_app_t*         slots[3];
  ra_app_registry_t reg = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_registry_init(&reg, slots, 3U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_register(&reg, &a_lib));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_register(&reg, &a_rdr));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_register(&reg, &a_set));

  /* Frame A: all three tiles. */
  const uint32_t crc_a = lr_compose_crc(s_lr_fb, &reg);

  /* Core uninstall is refused -> the launcher must be byte-identical. */
  TEST_ASSERT_EQ((int)k_ra_err_not_supported,
                 (int)ra_app_uninstall(&reg, (uint16_t)k_lr_id_library));
  const uint32_t crc_a2 = lr_compose_crc(s_lr_fb2, &reg);
  TEST_ASSERT_EQ((int64_t)crc_a, (int64_t)crc_a2);

  /* Uninstall the removable app -> its tile disappears (distinct golden). */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_app_uninstall(&reg, (uint16_t)k_lr_id_settings));
  const uint32_t crc_b = lr_compose_crc(s_lr_fb, &reg);

  TEST_ASSERT_EQ((int64_t)(uint32_t)k_lr_golden_a, (int64_t)crc_a);
  TEST_ASSERT_EQ((int64_t)(uint32_t)k_lr_golden_b, (int64_t)crc_b);
  TEST_ASSERT(crc_a != crc_b);
  TEST_END("ra_app: compositor launcher CRC golden + uninstall reflow");
}

int main(void)
{
  test_launcher_render_golden();
  return 0;
}
