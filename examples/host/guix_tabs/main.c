/**
 * @file main.c
 * @brief Host GUIX 3-tab bedroom UI -- macOS preview of the panel app
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Drives the shared GUIX UI (``bedroom_ui``) on the macOS host: GUIX renders
 * into an RGB565 framebuffer via the host 565rgb driver behind the display
 * PAL's ``display_bind_guix`` seam (same call the panel uses), and the macOS
 * window backend presents it. Tabs switch on mouse click, fed into GUIX as
 * pen events. ``--png <path> [--tab N]`` renders one screen headless.
 *
 * The GUIX UI code is identical to what flashes on the EK-RA8D2; only the
 * backend (macOS vs GLCDC) and RTOS bind (generic vs ThreadX) differ.
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
#include "ra_panel.h"

typedef enum : uint16_t {
  k_w = k_panel_width_px,
  k_h = k_panel_height_px,
} guix_tabs_geom_t;

typedef enum : uint32_t {
  k_frame_sleep_us = 16000U,
} guix_tabs_loop_t;

/** @brief RGB565 framebuffer shared by GUIX (canvas) and the PAL (present). */
static uint16_t       s_fb[(size_t)k_w * (size_t)k_h];
static GX_DISPLAY     s_gxdisp;
static GX_CANVAS      s_canvas;
static GX_WINDOW_ROOT s_gxroot;

static VOID* host_alloc(ULONG size)
{
  return malloc((size_t)size); /* alloc-allow: host preview; target uses static pool */
}

static VOID host_free(VOID* mem)
{
  free(mem); /* alloc-allow: host preview; target uses static pool */
}

/**
 * @brief Stand up GUIX against the given display-driver setup-fn + dims.
 *
 * @param[in] setup_fn GUIX display_driver_setup callback.
 * @return ra_err_t k_ra_ok on success, k_ra_fail on any GUIX failure.
 */
static ra_err_t guix_build(UINT (*setup_fn)(GX_DISPLAY*), uint16_t width, uint16_t height)
{
  if (gx_system_memory_allocator_set(host_alloc, host_free) != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_system_initialize() != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_display_create(&s_gxdisp, "host", setup_fn, (GX_VALUE)width, (GX_VALUE)height) !=
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
                       (ULONG)width * (ULONG)height * 2UL) != GX_SUCCESS) {
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

static void guix_send_pen(uint16_t x, uint16_t y)
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

static int write_ppm(const char* path, const uint16_t* fb, int w, int h)
{
  FILE* f = fopen(path, "wb");
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
 *
 * @param[in] path    Output PPM path.
 * @param[in] tab     Tab to make active before any click.
 * @param[in] click_x Click x (px), or negative for no click.
 * @param[in] click_y Click y (px), or negative for no click.
 *
 * @details When @p click_x / @p click_y are non-negative, a real pen
 *          down/up is injected at that pixel before rendering -- exercising
 *          the same GUIX hit-test -> button -> CLICKED -> tab-switch path the
 *          live window uses, so click handling is verifiable without a mouse.
 */
static int run_headless(const char* path, uint16_t tab, int click_x, int click_y)
{
  (void)memset(s_fb, 0, sizeof(s_fb));
  if (host_guix_display_driver_bind(s_fb, (uint16_t)k_w, (uint16_t)k_h) != k_ra_ok) {
    (void)fprintf(stderr, "host driver bind failed\n");
    return 1;
  }
  if (guix_build(host_guix_display_driver_setup, (uint16_t)k_w, (uint16_t)k_h) != k_ra_ok) {
    (void)fprintf(stderr, "guix_build failed\n");
    return 1;
  }
  bedroom_ui_set_active_tab(tab);
  if ((click_x >= 0) && (click_y >= 0)) {
    guix_send_pen((uint16_t)click_x, (uint16_t)click_y);
  }
  gx_host_pump();
  (void)gx_system_canvas_refresh();
  if (write_ppm(path, s_fb, (int)k_w, (int)k_h) != 0) {
    (void)fprintf(stderr, "write_ppm failed\n");
    return 1;
  }
  (void)fprintf(stderr, "guix_tabs: wrote %s (tab %u)\n", path, (unsigned)tab);
  return 0;
}

/**
 * @brief Live macOS window: GUIX through the PAL, clicks -> pen events.
 */
static int run_window(uint16_t tab)
{
  const display_cfg_t cfg = {
    .iface             = &k_display_backend_host_macos,
    .framebuffer       = s_fb,
    .framebuffer_bytes = (uint32_t)sizeof(s_fb),
    .width_px          = (uint16_t)k_w,
    .height_px         = (uint16_t)k_h,
    .pixfmt            = k_display_pixfmt_rgb565,
  };
  display_handle_t* disp = nullptr;
  if (display_init(&cfg, &disp) != k_ra_ok) {
    (void)fprintf(stderr, "display_init failed\n");
    return 1;
  }
  display_guix_attach_t attach = {nullptr, 0U, 0U};
  if (display_bind_guix(disp, &attach) != k_ra_ok) {
    (void)fprintf(stderr, "display_bind_guix failed\n");
    (void)display_deinit(disp);
    return 1;
  }
  if (guix_build((UINT (*)(GX_DISPLAY*))attach.setup_fn, attach.width_px, attach.height_px) !=
      k_ra_ok) {
    (void)fprintf(stderr, "guix_build failed\n");
    (void)display_deinit(disp);
    return 1;
  }
  bedroom_ui_set_active_tab(tab);
  gx_host_pump();
  (void)gx_system_canvas_refresh();
  (void)display_flush(disp, display_full_rect(disp), k_display_refresh_quality);

  (void)fprintf(stderr, "guix_tabs: window open -- click a tab; close to exit\n");
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
      guix_send_pen(cx, cy);
    }
    gx_host_pump();
    (void)gx_system_canvas_refresh();
    if (display_flush(disp, display_full_rect(disp), k_display_refresh_fast) != k_ra_ok) {
      break;
    }
    (void)usleep((useconds_t)k_frame_sleep_us);
  }
  (void)display_deinit(disp);
  return 0;
}

int main(int argc, char** argv)
{
  const char* png     = nullptr;
  uint16_t    tab     = 0U;
  int         click_x = -1;
  int         click_y = -1;
  for (int i = 1; i < argc; ++i) {
    /* sizeof(literal) bounds the compare and includes the NUL, so this is an
     * exact match (no unbounded strcmp, no reads past either string). */
    if ((strncmp(argv[i], "--png", sizeof("--png")) == 0) && ((i + 1) < argc)) {
      png = argv[i + 1];
      ++i;
    } else if ((strncmp(argv[i], "--tab", sizeof("--tab")) == 0) && ((i + 1) < argc)) {
      const char c = argv[i + 1][0];
      if ((c >= '0') && (c <= '2')) {
        tab = (uint16_t)(c - '0');
      }
      ++i;
    } else if ((strncmp(argv[i], "--click", sizeof("--click")) == 0) && ((i + 2) < argc)) {
      click_x = (int)strtol(argv[i + 1], nullptr, 10);
      click_y = (int)strtol(argv[i + 2], nullptr, 10);
      i += 2;
    }
  }
  if (png != nullptr) {
    return run_headless(png, tab, click_x, click_y);
  }
  return run_window(tab);
}
