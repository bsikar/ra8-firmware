/**
 * @file test_ra8_app_launcher_render.c
 * @brief Widget-rendered CRC golden of the app-framework chrome + launcher.
 *
 * @details
 * Ties the app framework (`ra8_app`) to the composable widget layer
 * (`ra8_widget`): the "chrome" enumerates the app **registry** and
 * composes a **launcher** -- a header label over one label tile per registered
 * app, tiled by an ::ra8_widget_panel column -- into an in-memory `ra8_gfx`
 * framebuffer, and the result is pinned with a CRC-32 golden. This proves an
 * "app = a launcher tile" composes deterministically and, crucially, that the
 * launcher **reflects the registry**: uninstalling the removable app drops its
 * tile (a different CRC golden), while a refused uninstall of a *core* app
 * leaves the launcher byte-identical -- the run-time "core uninstallable"
 * invariant, observed through pixels.
 *
 * Additionally verifies the launcher icon consumer (RA8FW-531): reads .ra8app
 * format v2 16x16 2-bit packed grayscale icon metadata, unpacks and plots
 * pixels onto the tile, asserting deterministic composite and rejection of
 * format v1 or malformed icon fields.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_app.h"
#include "ra8_appimg.h"
#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "unity_minimal.h"

/**
 * @enum app_launcher_render_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint32_t {
  k_crc32_init           = 0xFFFFFFFFU, /**< CRC-32 initial value, and the final XOR-out. */
  k_crc32_poly_reflected = 0xEDB88320U, /**< The reflected CRC-32 polynomial.             */
} app_launcher_render_fixture_t;

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
  k_lr_golden_a    = 0xA6AA065AU, /**< Frame A: library + reader + settings tiles. */
  k_lr_golden_b    = 0x759DF0ECU, /**< Frame B: settings uninstalled (2 tiles).    */
  k_lr_golden_icon = 0x0380436BU, /**< Frame with format v2 16x16 icon rendered.   */
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
 * @brief Unpack and blit a 16x16 2-bit grayscale .ra8app icon into the framebuffer.
 *
 * @details Reads the icon metadata and payload from an in-memory .ra8app image
 * using ra8_appimg_parse and ra8_appimg_get_icon, unpacks the 2-bit grayscale
 * pixels (4 pixels per byte, 64 bytes total for 16x16), and plots each pixel
 * onto the active ra8_gfx framebuffer at (dst_x, dst_y).
 *
 * @param[in] img_bytes Raw bytes of the .ra8app file.
 * @param[in] img_len   Length in bytes of img_bytes.
 * @param[in] dst_x     X coordinate on the framebuffer for the top-left of the icon.
 * @param[in] dst_y     Y coordinate on the framebuffer for the top-left of the icon.
 *
 * @return ra8_err_t k_ra8_ok on success, or error from header/icon parsing.
 */
static ra8_err_t lr_render_appimg_icon(const uint8_t* img_bytes,
                                       size_t         img_len,
                                       int32_t        dst_x,
                                       int32_t        dst_y)
{
  if (img_bytes == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_appimg_header_t hdr = {};
  ra8_err_t err = ra8_appimg_parse(img_bytes, img_len, &hdr);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint8_t* icon_data = nullptr;
  size_t         icon_len  = 0U;
  err = ra8_appimg_get_icon(&hdr, img_bytes, img_len, &icon_data, &icon_len);
  if (err != k_ra8_ok) {
    return err;
  }
  if (icon_data == nullptr || icon_len == 0U) {
    return k_ra8_err_not_found;
  }

  /* Unpack 16x16 2-bit grayscale pixels (64 bytes, 4 pixels/byte) */
  for (int32_t y = 0; y < (int32_t)k_ra8_appimg_icon_height; ++y) {
    for (int32_t x = 0; x < (int32_t)k_ra8_appimg_icon_width; ++x) {
      const size_t byte_idx = (size_t)(y * (int32_t)k_ra8_appimg_icon_width + x) / 4U;
      const uint32_t shift = (uint32_t)(6 - 2 * (x % 4));
      const uint8_t level2 = (uint8_t)((icon_data[byte_idx] >> shift) & 0x03U);
      /* 2-bit grayscale mapping to ARGB8888 */
      static const uint32_t tone_lut[4] = {
        0x00000000U, /* 00: Black      */
        0x00555555U, /* 01: Dark gray  */
        0x00AAAAAAU, /* 10: Light gray */
        0x00FFFFFFU  /* 11: White      */
      };
      const uint32_t pixel_color = tone_lut[level2];
      (void)ra8_gfx_pixel(dst_x + x, dst_y + y, pixel_color);
    }
  }
  return k_ra8_ok;
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

/**
 * @test The launcher reads and renders .ra8app format v2 icons (RA8FW-531).
 *
 * @details
 * Proves the launcher acts as a real consumer of .ra8app icon metadata:
 * - A format v2 image with a 64-byte 16x16 2-bit grayscale icon is parsed and
 *   its icon unpacked and rendered onto the launcher framebuffer.
 * - An image without an icon (format v1 or format v2 icon_length == 0) returns
 *   k_ra8_err_not_found.
 * - The composite CRC with the icon rendered is deterministic.
 */
static void test_launcher_render_with_appimg_icon(void)
{
  TEST_BEGIN("ra8_app: launcher reads and renders .ra8app v2 icon");
  ra8_app_t          a_lib = lr_make_app((uint16_t)k_lr_id_library, "library", false);
  ra8_app_t*         slots[1];
  ra8_app_registry_t reg = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_registry_init(&reg, slots, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_app_register(&reg, &a_lib));

  /* Compose initial launcher frame */
  const uint32_t crc_base = lr_compose_crc(s_lr_fb, &reg);
  TEST_ASSERT(crc_base != 0U);

  /* Build format v2 .ra8app fixture with 16x16 icon */
  const uint32_t code_len = 64U;
  const uint32_t data_len = 32U;
  const uint32_t icon_len = (uint32_t)k_ra8_appimg_icon_bytes;
  const size_t v2_size = sizeof(ra8_appimg_header_t) + code_len + data_len + icon_len;
  uint8_t v2_file[sizeof(ra8_appimg_header_t) + 64U + 32U + 64U];
  (void)memset(v2_file, 0, v2_size);

  ra8_appimg_header_t* hdr = (ra8_appimg_header_t*)v2_file;
  hdr->magic           = (uint32_t)k_ra8_appimg_magic;
  hdr->format_version  = (uint32_t)k_ra8_appimg_format_version_v2;
  hdr->entry_offset    = 0U;
  hdr->code_size       = code_len;
  hdr->data_size       = data_len;
  hdr->stack_size      = 2048U;
  hdr->min_api_version = (uint32_t)k_ra8_appimg_api_version_current;
  hdr->capabilities    = (uint32_t)k_ra8_appimg_cap_display;
  hdr->icon_offset     = code_len + data_len;
  hdr->icon_length     = icon_len;
  (void)strcpy(hdr->app_id, "com.ra8.reader");
  (void)strcpy(hdr->display_name, "Reader");

  /* Populate 64-byte 2-bit icon pattern (concentric border / gradient) */
  uint8_t* icon_data = v2_file + sizeof(ra8_appimg_header_t) + hdr->icon_offset;
  for (uint32_t y = 0U; y < 16U; ++y) {
    for (uint32_t x = 0U; x < 16U; ++x) {
      const size_t b_idx = (size_t)(y * 16U + x) / 4U;
      const uint32_t sh = (uint32_t)(6 - 2 * (x % 4));
      uint8_t tone = 1U;
      if (x == 0U || x == 15U || y == 0U || y == 15U) {
        tone = 3U; /* White border */
      } else if (x == y || (x + y) == 15U) {
        tone = 2U; /* Light gray X */
      }
      icon_data[b_idx] |= (uint8_t)(tone << sh);
    }
  }

  /* Render icon at tile position (x=108, y=24) */
  TEST_ASSERT_EQ(k_ra8_ok, lr_render_appimg_icon(v2_file, v2_size, 108, 24));
  const uint32_t crc_with_icon = lr_crc32((const uint8_t*)s_lr_fb,
                                          (size_t)(k_lr_fb_w * k_lr_fb_h) * sizeof(uint32_t));
  /* Icon modified framebuffer pixels deterministically */
  TEST_ASSERT(crc_with_icon != crc_base);
  TEST_ASSERT_EQ(k_lr_golden_icon, crc_with_icon);

  /* Build format v1 fixture (no icon) and verify launcher refuses icon render */
  uint8_t v1_file[160U + 64U];
  (void)memset(v1_file, 0, sizeof(v1_file));
  uint32_t* v1_words = (uint32_t*)v1_file;
  v1_words[0] = (uint32_t)k_ra8_appimg_magic;
  v1_words[1] = (uint32_t)k_ra8_appimg_format_version_v1;
  v1_words[3] = 64U;
  v1_words[5] = 2048U;
  v1_words[6] = (uint32_t)k_ra8_appimg_api_version_current;
  (void)strcpy((char*)(v1_file + 32U), "com.ra8.v1app");
  (void)strcpy((char*)(v1_file + 64U), "V1App");
  (void)memset(v1_file + 96U, 0x55, 64U);

  TEST_ASSERT_EQ(k_ra8_err_not_found, lr_render_appimg_icon(v1_file, sizeof(v1_file), 108, 24));

  /* Null and invalid file checks */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, lr_render_appimg_icon(nullptr, v2_size, 108, 24));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, lr_render_appimg_icon(v2_file, 32U, 108, 24));

  TEST_END("ra8_app: launcher reads and renders .ra8app v2 icon");
}

int main(void)
{
  test_launcher_render_golden();
  test_launcher_render_with_appimg_icon();
  return 0;
}
