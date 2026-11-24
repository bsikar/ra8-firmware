/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_ui/src/ereader_ui_figure_zoom.c
 * @brief Tap-to-zoom overlay for inline raster figures in the Reading view.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Connects `reflow_hit_test_image` + `ra8_img_decode_gray8` + `zoom_mem` +
 * `zoom_popup` to the `ereader_ui` Reading screen. All scratch and decoded
 * gray8 pixel storage live in external `.sdram_data` so internal SRAM remains
 * undisturbed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ereader_ui_steps.h"
#include "ra8_display_pal.h"
#include "ra8_err.h"
#include "ra8_time.h"
#include "reflow.h"
#include "reflow_image.h"
#include "zoom.h"
#include "zoom_mem.h"
#include "zoom_popup.h"

/** @brief Telemetry globals for emulator/SWD inspection. */
volatile uint32_t g_er_fig_active;
volatile uint32_t g_er_fig_scale;

/** @brief Decoded gray8 raster plane for the currently open figure popup. */
[[gnu::section(".sdram_data")]] static uint8_t s_fig_gray8[k_er_fig_gray8_cap];

/** @brief Strip-compositing scratch buffers in external SDRAM. */
[[gnu::section(".sdram_data")]] static uint8_t s_zoom_row[k_er_fb_w];
[[gnu::section(
  ".sdram_data")]] static uint8_t s_zoom_strip[(size_t)k_er_fb_w * (size_t)k_er_zoom_strip_rows];
[[gnu::section(".sdram_data")]] static uint8_t
  s_zoom_packed[((size_t)k_er_fb_w * (size_t)k_er_zoom_strip_rows) / 2U];

/** @brief Backing source descriptor + popup state. */
static zoom_mem_src_t s_fig_mem;
static zoom_popup_t   s_fig_popup;

/** @brief Update public figure-zoom telemetry globals from ::s_fig_popup. */
static void er_fig_sync_telemetry(void)
{
  const bool open = zoom_popup_active(&s_fig_popup);
  g_er_fig_active = open ? 1U : 0U;
  g_er_fig_scale  = open ? (uint32_t)s_fig_popup.view.scale : 0U;
}

/** @brief Build the SDRAM-backed strip scratch descriptor for zoom_popup_open. */
static zoom_scratch_t er_fig_scratch(void)
{
  return (zoom_scratch_t){
    .row        = s_zoom_row,
    .row_cap    = (uint32_t)sizeof(s_zoom_row),
    .strip      = s_zoom_strip,
    .strip_cap  = (uint32_t)sizeof(s_zoom_strip),
    .packed     = s_zoom_packed,
    .packed_cap = (uint32_t)sizeof(s_zoom_packed),
  };
}

/** @brief Render ::s_fig_popup and flush the resulting dirty rect on the panel. */
static void er_fig_render_and_present(void)
{
  if (zoom_popup_render(&s_fig_popup) != k_ra8_ok) {
    return;
  }
  zoom_present_t plan = {};
  if ((zoom_popup_present(&s_fig_popup, &plan) != k_ra8_ok) || !plan.present) {
    return;
  }
  const display_refresh_hint_t hint =
    (plan.refresh == k_zoom_refresh_fast) ? k_display_refresh_fast : k_display_refresh_quality;
  er_flush_rect(plan.rect.x, plan.rect.y, plan.rect.w, plan.rect.h, hint);
}

/** @brief Map a page-local tap coordinate inside @p box onto source coordinates. */
static int32_t er_fig_focus_axis(int32_t tap_rel, int32_t box_len, uint32_t src_len)
{
  if (src_len == 0U) {
    return 0;
  }
  if ((box_len <= 0) || (tap_rel < 0) || (tap_rel >= box_len)) {
    return (int32_t)(src_len / (uint32_t)k_er_fig_dim_half);
  }
  const int64_t scaled = ((int64_t)tap_rel * (int64_t)src_len) / (int64_t)box_len;
  const int32_t max_c  = (int32_t)(src_len - 1U);
  return (scaled > (int64_t)max_c) ? max_c : (int32_t)scaled;
}

bool er_figure_zoom_active(void)
{
  return zoom_popup_active(&s_fig_popup);
}

bool er_reading_figure_tap(int32_t x, int32_t y)
{
  if (!s_reflow_open) {
    return false;
  }
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  const int32_t tx       = x - (int32_t)k_er_margin_x;
  const int32_t ty       = y - body_top;
  uint32_t      img_idx  = 0U;
  if (reflow_hit_test_image(&s_reflow_engine, s_reading_page, tx, ty, &img_idx) != k_ra8_ok) {
    return false;
  }
  const reflow_image_box_t* box   = &s_reflow_engine.image_boxes[img_idx];
  const uint8_t*            bytes = nullptr;
  size_t                    len   = 0U;
  const char*               href  = (const char*)&s_reflow_engine.text_pool[box->src_off];
  if (er_image_loader(nullptr, href, box->src_len, &bytes, &len) != k_ra8_ok) {
    return false;
  }
  ra8_img_arena_t arena = {
    .base   = s_img_arena_buf,
    .cap    = (size_t)k_er_img_arena,
    .offset = 0U,
    .live   = 0U,
  };
  uint32_t img_w = 0U;
  uint32_t img_h = 0U;
  if (ra8_img_decode_gray8(&arena, bytes, len, s_fig_gray8, sizeof(s_fig_gray8), &img_w, &img_h) !=
      k_ra8_ok) {
    return false;
  }
  zoom_source_t zs = {};
  if ((zoom_mem_src_init(&s_fig_mem, s_fig_gray8, img_w, img_h, 0U) != k_ra8_ok) ||
      (zoom_mem_src_bind(&s_fig_mem, &zs) != k_ra8_ok)) {
    return false;
  }
  const zoom_popup_cfg_t cfg = {
    .src       = zs,
    .scratch   = er_fig_scratch(),
    .fb_w      = s_fb.width_px,
    .fb_h      = s_fb.height_px,
    .scale     = (uint8_t)k_zoom_scale_min,
    .scale_max = (uint8_t)k_er_zoom_scale_max,
    .policy    = k_zoom_policy_responsive,
    .focus_x   = er_fig_focus_axis(tx - box->x, box->w, img_w),
    .focus_y   = er_fig_focus_axis(ty - box->y, box->h, img_h),
  };
  if (zoom_popup_open(&s_fig_popup, &cfg) != k_ra8_ok) {
    return false;
  }
  er_fig_sync_telemetry();
  return true;
}

void er_figure_zoom_render(void)
{
  if (zoom_popup_active(&s_fig_popup)) {
    (void)zoom_popup_render(&s_fig_popup);
  }
}

void er_figure_zoom_present_open(void)
{
  if (zoom_popup_active(&s_fig_popup)) {
    er_fig_render_and_present();
  }
}

void er_figure_zoom_tap(int32_t x, int32_t y)
{
  zoom_popup_action_t act = k_zoom_popup_act_none;
  if (zoom_popup_tap(&s_fig_popup, x, y, ra8_time_ms(), &act) != k_ra8_ok) {
    return;
  }
  er_fig_sync_telemetry();
  if (act == k_zoom_popup_act_redrawn) {
    er_fig_render_and_present();
  } else if (act == k_zoom_popup_act_closed) {
    er_render_current();
    er_flush_event(k_display_event_chapter);
  }
}

void er_figure_zoom_tick(void)
{
  if (!zoom_popup_active(&s_fig_popup)) {
    return;
  }
  if (zoom_popup_tick(&s_fig_popup, ra8_time_ms())) {
    er_fig_render_and_present();
  }
}
