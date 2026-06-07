/**
 * @file main.c
 * @brief Host graphics demo -- tabbed screens on the macOS display PAL backend
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * A 3-tab UI drawn entirely with ``ra_gfx`` (no GUIX): a tab bar across
 * the top plus a per-tab screen below it. Tabs switch on mouse click
 * (routed through ``ra_display_pal_host_macos_poll_click``). Runs in a
 * live macOS window, or headless to a PNG via ``--png`` (use ``--tab N``
 * to pick which screen to render). The same ``ra_gfx`` code runs on the
 * EK-RA8D2 LCD by swapping ``cfg.iface`` to ``k_display_backend_lcd_ra_glcdc``.
 *
 * Example / development tool, not firmware: links the host build
 * (``RA_SIMULATOR_MODE``) plus the AppKit window shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "floorplan_png.h"
#include "ra_display_pal.h"
#include "ra_display_pal_host_macos.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"

/**
 * @enum demo_geometry_t
 * @brief Canvas + tab-bar layout geometry in pixels.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_canvas_w      = 1024U, /**< Window / framebuffer width.            */
  k_canvas_h      = 600U,  /**< Window / framebuffer height.           */
  k_tabbar_h      = 48U,   /**< Tab-bar height.                        */
  k_tab_count     = 3U,    /**< Number of tabs / screens.              */
  k_label_h       = 16U,   /**< 8x16 font height (vertical centering). */
  k_tab_pad_x     = 12U,   /**< Tab label left inset.                  */
  k_content_pad   = 24U,   /**< Screen content left inset.             */
  k_heading_y_off = 24U,   /**< Heading y offset below the tab bar.    */
  k_body_y_off    = 52U,   /**< Body-text y offset below the tab bar.  */
} demo_geometry_t;

/**
 * @enum demo_color_t
 * @brief Palette as 0x00RRGGBB values (ra_gfx down-converts to RGB565).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_color_tabbar_bg  = 0x222831U, /**< Tab-bar background.        */
  k_color_tab_active = 0xFFFFFFU, /**< Active tab fill.           */
  k_color_tab_idle   = 0x394150U, /**< Inactive tab fill.        */
  k_color_tab_border = 0x10151CU, /**< Tab outline.              */
  k_color_text_dark  = 0x1A2230U, /**< Dark text (on light bg).  */
  k_color_text_light = 0xCDD3DDU, /**< Light text (on dark bg).  */
  k_color_heading    = 0x1A2230U, /**< Screen heading text.      */
  k_color_screen0_bg = 0xF5F1E8U, /**< Screen 0 background.      */
  k_color_screen1_bg = 0xE8F0F5U, /**< Screen 1 background.      */
  k_color_screen2_bg = 0xECF5E8U, /**< Screen 2 background.      */
} demo_color_t;

/**
 * @enum demo_loop_t
 * @brief Run-loop pacing constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_frame_sleep_us = 16000U, /**< ~60 Hz idle pacing. */
} demo_loop_t;

/** @brief RGB565 framebuffer; the caller owns it. */
static uint16_t s_framebuffer[(size_t)k_canvas_w * (size_t)k_canvas_h];

/** @brief Currently selected tab (0 .. k_tab_count-1). */
static uint16_t s_active_tab = 0U;

/** @brief Tab / screen captions. */
static const char* const s_tab_names[k_tab_count] = {
  "Primary Bedroom",
  "Secondary Bedroom 1",
  "Secondary Bedroom 2",
};

/** @brief Per-screen background tints (indexed by tab). */
static const uint32_t s_screen_bg[k_tab_count] = {
  (uint32_t)k_color_screen0_bg,
  (uint32_t)k_color_screen1_bg,
  (uint32_t)k_color_screen2_bg,
};

/**
 * @brief Draw the tab bar with the active tab highlighted.
 *
 * @param[in] active Index of the active tab.
 *
 * @return ra_err_t Error code from the first failing ra_gfx call.
 * @retval k_ra_ok       Tab bar drawn.
 * @retval (from ra_gfx) Forwarded from the primitives.
 *
 * @pre ra_gfx_init() has succeeded.
 * @pre active < k_tab_count.
 * @post The tab bar occupies rows 0 .. k_tabbar_h-1.
 * @post Iteration is bounded by k_tab_count.
 *
 * @since 0.1.0
 */
static ra_err_t draw_tab_bar(uint16_t active)
{
  ra_err_t e = ra_gfx_rect(0, 0, (int32_t)k_canvas_w, (int32_t)k_tabbar_h, k_color_tabbar_bg, true);
  if (e != k_ra_ok) {
    return e;
  }
  const int32_t tabw = (int32_t)k_canvas_w / (int32_t)k_tab_count;
  for (uint16_t i = 0U; i < (uint16_t)k_tab_count; ++i) {
    const int32_t  x0   = (int32_t)i * tabw;
    const int32_t  x1   = (i == (uint16_t)(k_tab_count - 1U)) ? (int32_t)k_canvas_w : (x0 + tabw);
    const bool     on   = (i == active);
    const uint32_t fill = on ? (uint32_t)k_color_tab_active : (uint32_t)k_color_tab_idle;
    const uint32_t fg   = on ? (uint32_t)k_color_text_dark : (uint32_t)k_color_text_light;
    e                   = ra_gfx_rect(x0, 0, x1 - x0, (int32_t)k_tabbar_h, fill, true);
    if (e != k_ra_ok) {
      return e;
    }
    e = ra_gfx_rect(x0, 0, x1 - x0, (int32_t)k_tabbar_h, (uint32_t)k_color_tab_border, false);
    if (e != k_ra_ok) {
      return e;
    }
    e = ra_gfx_text_out(x0 + (int32_t)k_tab_pad_x,
                        (int32_t)((k_tabbar_h - k_label_h) / 2U),
                        s_tab_names[i],
                        &ra_gfx_font_8x16,
                        fg,
                        fill);
    if (e != k_ra_ok) {
      return e;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Draw the content area for one tab's screen.
 *
 * @param[in] tab Index of the screen to draw.
 *
 * @return ra_err_t Error code from the first failing ra_gfx call.
 * @retval k_ra_ok       Screen drawn.
 * @retval (from ra_gfx) Forwarded from the primitives.
 *
 * @pre ra_gfx_init() has succeeded.
 * @pre tab < k_tab_count.
 * @post The area below the tab bar holds the screen.
 * @post No window present happens here.
 *
 * @since 0.1.0
 */
static ra_err_t draw_screen(uint16_t tab)
{
  const int32_t  y0 = (int32_t)k_tabbar_h;
  const uint32_t bg = s_screen_bg[tab];
  ra_err_t       e  = ra_gfx_rect(0, y0, (int32_t)k_canvas_w, (int32_t)k_canvas_h - y0, bg, true);
  if (e != k_ra_ok) {
    return e;
  }
  e = ra_gfx_text_out((int32_t)k_content_pad,
                      y0 + (int32_t)k_heading_y_off,
                      s_tab_names[tab],
                      &ra_gfx_font_8x16,
                      (uint32_t)k_color_heading,
                      bg);
  if (e != k_ra_ok) {
    return e;
  }
  return ra_gfx_text_out((int32_t)k_content_pad,
                         y0 + (int32_t)k_body_y_off,
                         "(blank screen -- add content here)",
                         &ra_gfx_font_8x16,
                         (uint32_t)k_color_text_dark,
                         bg);
}

/**
 * @brief Paint the whole scene: active screen + tab bar on top.
 *
 * @return ra_err_t Error code from the first failing ra_gfx call.
 * @retval k_ra_ok       Scene painted.
 * @retval (from ra_gfx) Forwarded from the primitives.
 *
 * @pre ra_gfx_init() has succeeded with the demo framebuffer.
 * @post The framebuffer holds the tab bar + active screen.
 * @post No window present happens here (the caller flushes).
 *
 * @since 0.1.0
 */
static ra_err_t draw_scene(void)
{
  ra_err_t e = draw_screen(s_active_tab);
  if (e != k_ra_ok) {
    return e;
  }
  return draw_tab_bar(s_active_tab);
}

/**
 * @brief Map a framebuffer click to a tab index.
 *
 * @param[in] cx Click column.
 * @param[in] cy Click row.
 *
 * @return int32_t Tab index 0 .. k_tab_count-1, or -1 if the click was
 *         not inside the tab bar.
 *
 * @pre None.
 * @post No state mutated.
 *
 * @since 0.1.0
 */
static int32_t tab_hit_test(uint16_t cx, uint16_t cy)
{
  if (cy >= (uint16_t)k_tabbar_h) {
    return -1;
  }
  const uint16_t tabw = (uint16_t)((uint16_t)k_canvas_w / (uint16_t)k_tab_count);
  uint16_t       t    = (uint16_t)(cx / tabw);
  if (t >= (uint16_t)k_tab_count) {
    t = (uint16_t)(k_tab_count - 1U);
  }
  return (int32_t)t;
}

/**
 * @brief Pump OS events, route clicks to tab switches, present each frame.
 *
 * @param[in] disp Bound PAL handle.
 * @return int 0 on clean (user-closed) exit, 1 on any runtime error.
 *
 * @since 0.1.0
 */
static int run_event_loop(display_handle_t* disp)
{
  bool should_close = false;
  while (!should_close) {
    ra_err_t e = ra_display_pal_host_macos_pump(&should_close);
    if (e != k_ra_ok) {
      (void)fprintf(stderr, "pump failed: %s\n", ra_err_to_str(e));
      return 1;
    }
    bool     has_click = false;
    uint16_t cx        = 0U;
    uint16_t cy        = 0U;
    e                  = ra_display_pal_host_macos_poll_click(&has_click, &cx, &cy);
    if (e != k_ra_ok) {
      (void)fprintf(stderr, "poll_click failed: %s\n", ra_err_to_str(e));
      return 1;
    }
    if (has_click) {
      const int32_t t = tab_hit_test(cx, cy);
      if (t >= 0 && (uint16_t)t != s_active_tab) {
        s_active_tab = (uint16_t)t;
        e            = draw_scene();
        if (e != k_ra_ok) {
          (void)fprintf(stderr, "draw_scene failed: %s\n", ra_err_to_str(e));
          return 1;
        }
      }
    }
    e = display_flush(disp, display_full_rect(disp), k_display_refresh_fast);
    if (e != k_ra_ok) {
      (void)fprintf(stderr, "display_flush failed: %s\n", ra_err_to_str(e));
      return 1;
    }
    (void)usleep((useconds_t)k_frame_sleep_us);
  }
  return 0;
}

/**
 * @brief Windowed run mode: bind the PAL, draw, present, run the loop.
 *
 * @return int 0 on clean exit, 1 on any setup failure.
 *
 * @since 0.1.0
 */
static int run_window(void)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_host_macos,
    .framebuffer       = s_framebuffer,
    .framebuffer_bytes = (uint32_t)sizeof(s_framebuffer),
    .width_px          = (uint16_t)k_canvas_w,
    .height_px         = (uint16_t)k_canvas_h,
    .pixfmt            = k_display_pixfmt_rgb565,
  };

  display_handle_t* disp = nullptr;
  ra_err_t          e    = display_init(&cfg, &disp);
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "display_init failed: %s\n", ra_err_to_str(e));
    return 1;
  }

  e =
    ra_gfx_init(s_framebuffer, (uint16_t)k_canvas_w, (uint16_t)k_canvas_h, k_ra_gfx_format_rgb565);
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "ra_gfx_init failed: %s\n", ra_err_to_str(e));
    (void)display_deinit(disp);
    return 1;
  }

  e = draw_scene();
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "draw_scene failed: %s\n", ra_err_to_str(e));
    (void)display_deinit(disp);
    return 1;
  }

  e = display_flush(disp, display_full_rect(disp), k_display_refresh_quality);
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "display_flush failed: %s\n", ra_err_to_str(e));
    (void)display_deinit(disp);
    return 1;
  }

  (void)fprintf(stderr, "demo: window open -- click a tab; close it to exit\n");
  const int rc = run_event_loop(disp);

  (void)display_deinit(disp);
  return rc;
}

/**
 * @brief Headless render mode: draw the active tab and write it to a PNG.
 *
 * @param[in] path Destination PNG path.
 *
 * @return int 0 on success, 1 on any failure.
 *
 * @since 0.1.0
 */
static int run_headless(const char* path)
{
  ra_err_t e =
    ra_gfx_init(s_framebuffer, (uint16_t)k_canvas_w, (uint16_t)k_canvas_h, k_ra_gfx_format_rgb565);
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "ra_gfx_init failed: %s\n", ra_err_to_str(e));
    return 1;
  }
  e = draw_scene();
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "draw_scene failed: %s\n", ra_err_to_str(e));
    return 1;
  }
  e = floorplan_write_png(path,
                          s_framebuffer,
                          (uint16_t)k_canvas_w,
                          (uint16_t)k_canvas_h,
                          (uint32_t)k_canvas_w * 2U);
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "floorplan_write_png failed: %s\n", ra_err_to_str(e));
    return 1;
  }
  (void)fprintf(stderr, "demo: wrote %s (tab %u)\n", path, (unsigned)s_active_tab);
  return 0;
}

/**
 * @brief Demo entry point.
 *
 * @details
 * ``demo [--tab N] [--png <path>]``. ``--png`` renders once headless and
 * exits; otherwise the live window opens. ``--tab N`` (0..2) selects the
 * initial / rendered screen.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 *
 * @return int 0 on clean exit, 1 on failure.
 *
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  const char* png = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--png") == 0 && (i + 1) < argc) {
      png = argv[i + 1];
      ++i;
    } else if (strcmp(argv[i], "--tab") == 0 && (i + 1) < argc) {
      const char c = argv[i + 1][0];
      if (c >= '0' && c <= '2') {
        s_active_tab = (uint16_t)(c - '0');
      }
      ++i;
    }
  }
  if (png != nullptr) {
    return run_headless(png);
  }
  return run_window();
}
