/**
 * @file main.c
 * @brief RA8D2 UI simulator -- a config-driven companion dev tool (macOS)
 *
 * @details
 * A standalone development instrument (not a firmware example): it BECOMES the
 * display described by a TOML panel config (``--panel <file>``) and runs the
 * shared GUIX UI against it, so screens can be built and clicked on the Mac
 * without hardware. The same UI source flashes to the EK-RA8D2 panel.
 *
 * GUIX renders into an RGB565 framebuffer through the host 565rgb driver behind
 * the display PAL's display_bind_guix seam (the call the panel uses); the macOS
 * window backend presents it and turns clicks into GUIX pen events.
 *
 *   tools/simulator/sim --panel tools/simulator/panels/ek_ra8d2.toml
 *   tools/simulator/sim --panel <file> --png out.ppm --tab 1   # headless
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bedroom_ui.h"
#include "gx_api.h"
#include "gx_display_driver_host.h"
#include "gx_host_run.h"
#include "ra_display_pal.h"
#include "ra_display_pal_guix.h"
#include "ra_display_pal_host_macos.h"
#include "ra_err.h"
#include "sim_panel.h"

typedef enum : uint32_t {
  k_sim_frame_sleep_us = 16000U, /**< ~60 Hz present cadence. */
  k_sim_rgb565_bpp     = 2U,
} sim_loop_t;

/** @brief RGB565 framebuffer, sized for the largest panel; canvas uses w*h. */
static uint16_t       s_fb[(size_t)k_sim_panel_max_w * (size_t)k_sim_panel_max_h];
static GX_DISPLAY     s_gxdisp;
static GX_CANVAS      s_canvas;
static GX_WINDOW_ROOT s_gxroot;

static VOID* host_alloc(ULONG size)
{
  return malloc((size_t)size); /* alloc-allow: host dev tool, not firmware */
}

static VOID host_free(VOID* mem)
{
  free(mem); /* alloc-allow: host dev tool, not firmware */
}

/**
 * @brief Stand up GUIX against the given display-driver setup-fn + dims.
 */
static ra_err_t sim_guix_build(UINT (*setup_fn)(GX_DISPLAY*), uint16_t width, uint16_t height)
{
  if (gx_system_memory_allocator_set(host_alloc, host_free) != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_system_initialize() != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_display_create(&s_gxdisp, "sim", setup_fn, (GX_VALUE)width, (GX_VALUE)height) !=
      GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_canvas_create(&s_canvas,
                       "canvas",
                       &s_gxdisp,
                       GX_CANVAS_MANAGED_VISIBLE,
                       (UINT)width,
                       (UINT)height,
                       (GX_COLOR*)s_fb,
                       (ULONG)width * (ULONG)height * (ULONG)k_sim_rgb565_bpp) != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_canvas_show(&s_canvas) != GX_SUCCESS) {
    return k_ra_fail;
  }
  GX_RECTANGLE root_rect;
  gx_utility_rectangle_define(&root_rect, 0, 0, (GX_VALUE)(width - 1U), (GX_VALUE)(height - 1U));
  if (gx_window_root_create(&s_gxroot, "root", &s_canvas, GX_STYLE_NONE, 0, &root_rect) !=
      GX_SUCCESS) {
    return k_ra_fail;
  }
  if (bedroom_ui_create(&s_gxdisp, &s_gxroot) != k_ra_ok) {
    return k_ra_fail;
  }
  if (gx_widget_show((GX_WIDGET*)&s_gxroot) != GX_SUCCESS) {
    return k_ra_fail;
  }
  return k_ra_ok;
}

static void sim_send_pen(uint16_t x, uint16_t y)
{
  GX_EVENT e;
  (void)memset(&e, 0, sizeof(e));
  e.gx_event_payload.gx_event_pointdata.gx_point_x = (GX_VALUE)x;
  e.gx_event_payload.gx_event_pointdata.gx_point_y = (GX_VALUE)y;
  e.gx_event_target                                = GX_NULL;
  e.gx_event_type                                  = GX_EVENT_PEN_DOWN;
  (void)gx_system_event_send(&e);
  e.gx_event_type = GX_EVENT_PEN_UP;
  (void)gx_system_event_send(&e);
}

static int sim_write_ppm(const char* path, const uint16_t* fb, int w, int h)
{
  FILE* f = fopen(path, "wb"); /* alloc-allow: host dev tool, not firmware */
  if (f == nullptr) {
    return -1;
  }
  (void)fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    const uint16_t p      = fb[i];
    const uint32_t r5     = (p >> 11) & 0x1FU;
    const uint32_t g6     = (p >> 5) & 0x3FU;
    const uint32_t b5     = p & 0x1FU;
    const uint8_t  rgb[3] = {(uint8_t)((r5 << 3) | (r5 >> 2)),
                             (uint8_t)((g6 << 2) | (g6 >> 4)),
                             (uint8_t)((b5 << 3) | (b5 >> 2))};
    (void)fwrite(rgb, 1U, 3U, f);
  }
  (void)fclose(f);
  return 0;
}

/**
 * @brief Headless render of one tab to a PPM (no window).
 */
static int sim_run_headless(const sim_panel_t* panel, const char* path, uint16_t tab)
{
  const uint16_t w = panel->width_px;
  const uint16_t h = panel->height_px;
  (void)memset(s_fb, 0, (size_t)w * (size_t)h * (size_t)k_sim_rgb565_bpp);
  if (host_guix_display_driver_bind(s_fb, w, h) != k_ra_ok) {
    (void)fprintf(stderr, "sim: host driver bind failed\n");
    return 1;
  }
  if (sim_guix_build(host_guix_display_driver_setup, w, h) != k_ra_ok) {
    (void)fprintf(stderr, "sim: guix build failed\n");
    return 1;
  }
  bedroom_ui_set_active_tab(tab);
  gx_host_pump();
  (void)gx_system_canvas_refresh();
  if (sim_write_ppm(path, s_fb, (int)w, (int)h) != 0) {
    (void)fprintf(stderr, "sim: write_ppm failed\n");
    return 1;
  }
  (void)fprintf(stderr,
                "sim: wrote %s (%s, %ux%u, tab %u)\n",
                path,
                panel->name,
                (unsigned)w,
                (unsigned)h,
                (unsigned)tab);
  return 0;
}

/**
 * @brief Live window: GUIX through the PAL on the configured display.
 */
static int sim_run_window(const sim_panel_t* panel)
{
  const uint16_t      w   = panel->width_px;
  const uint16_t      h   = panel->height_px;
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_host_macos,
    .framebuffer       = s_fb,
    .framebuffer_bytes = (uint32_t)((size_t)w * (size_t)h * (size_t)k_sim_rgb565_bpp),
    .width_px          = w,
    .height_px         = h,
    .pixfmt            = k_display_pixfmt_rgb565,
  };
  display_handle_t* disp = nullptr;
  if (display_init(&cfg, &disp) != k_ra_ok) {
    (void)fprintf(stderr, "sim: display_init failed\n");
    return 1;
  }
  display_guix_attach_t attach = {nullptr, 0U, 0U};
  if (display_bind_guix(disp, &attach) != k_ra_ok) {
    (void)fprintf(stderr, "sim: display_bind_guix failed\n");
    (void)display_deinit(disp);
    return 1;
  }
  if (sim_guix_build((UINT (*)(GX_DISPLAY*))attach.setup_fn, attach.width_px, attach.height_px) !=
      k_ra_ok) {
    (void)fprintf(stderr, "sim: guix build failed\n");
    (void)display_deinit(disp);
    return 1;
  }
  gx_host_pump();
  (void)gx_system_canvas_refresh();
  (void)display_flush(disp, display_full_rect(disp), k_display_refresh_quality);

  (void)fprintf(stderr,
                "sim: %s %ux%u @%uppi (%s) -- click to interact; close to exit\n",
                panel->name,
                (unsigned)w,
                (unsigned)h,
                (unsigned)panel->ppi,
                sim_pixfmt_str(panel->format));
  bool should_close = false;
  while (!should_close) {
    if (ra_display_pal_host_macos_pump(&should_close) != k_ra_ok) {
      break;
    }
    bool     has_click = false;
    uint16_t cx        = 0U;
    uint16_t cy        = 0U;
    if (ra_display_pal_host_macos_poll_click(&has_click, &cx, &cy) != k_ra_ok) {
      break;
    }
    if (has_click) {
      sim_send_pen(cx, cy);
    }
    gx_host_pump();
    (void)gx_system_canvas_refresh();
    if (display_flush(disp, display_full_rect(disp), k_display_refresh_fast) != k_ra_ok) {
      break;
    }
    (void)usleep((useconds_t)k_sim_frame_sleep_us);
  }
  (void)display_deinit(disp);
  return 0;
}

static void sim_usage(void)
{
  (void)fprintf(stderr,
                "usage: sim --panel <file.toml> [--png <out.ppm> [--tab 0|1|2]]\n"
                "  --panel   required; a display config under tools/simulator/panels/\n"
                "  --png     headless render to a PPM instead of opening a window\n"
                "  --tab N   which screen to show (0..2)\n");
}

int main(int argc, char** argv)
{
  const char* panel_path = nullptr;
  const char* png        = nullptr;
  uint16_t    tab        = 0U;
  for (int i = 1; i < argc; ++i) {
    if ((strncmp(argv[i], "--panel", sizeof("--panel")) == 0) && ((i + 1) < argc)) {
      panel_path = argv[i + 1];
      ++i;
    } else if ((strncmp(argv[i], "--png", sizeof("--png")) == 0) && ((i + 1) < argc)) {
      png = argv[i + 1];
      ++i;
    } else if ((strncmp(argv[i], "--tab", sizeof("--tab")) == 0) && ((i + 1) < argc)) {
      const char c = argv[i + 1][0];
      if ((c >= '0') && (c <= '2')) {
        tab = (uint16_t)(c - '0');
      }
      ++i;
    }
  }
  if (panel_path == nullptr) {
    sim_usage();
    return 2;
  }

  sim_panel_t panel;
  if (sim_panel_load(panel_path, &panel) != k_ra_ok) {
    return 1;
  }
  if (panel.format != k_sim_pixfmt_rgb565) {
    (void)fprintf(stderr,
                  "sim: format '%s' not rendered yet (v1 is rgb565 only)\n",
                  sim_pixfmt_str(panel.format));
    return 1;
  }

  if (png != nullptr) {
    return sim_run_headless(&panel, png, tab);
  }
  return sim_run_window(&panel);
}
