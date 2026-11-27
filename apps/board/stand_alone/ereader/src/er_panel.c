/**
 * @file er_panel.c
 * @brief E-reader Non-Secure panel bring-up, gfx binding, and refresh mode selection.
 * @ingroup grp_ereader
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "er_panel.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_display_pal_gfx.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"

/**
 * @enum er_panel_palette_t
 * @brief High-contrast e-paper inspired 24-bit RGB colour constants for ra8_gfx.
 */
typedef enum : uint32_t {
  k_er_color_paper  = 0x00F6F3EAU, /**< Warm off-white reading background. */
  k_er_color_ink    = 0x001A1D21U, /**< Primary dark charcoal text/rules.  */
  k_er_color_header = 0x002B3038U, /**< Slate top header strip.            */
  k_er_color_badge  = 0x00EAE4D5U, /**< Light pill surface inside header.  */
  k_er_color_muted  = 0x005A626DU, /**< Secondary caption text.            */
  k_er_color_rule   = 0x00C8C2B4U, /**< Thin editorial divider hairline.   */
} er_panel_palette_t;

/**
 * @enum er_panel_layout_t
 * @brief Layout geometry constants (derived cleanly for any panel size >= 120 px).
 */
typedef enum : int32_t {
  k_er_header_h     = 52,  /**< Top header bar height, px.        */
  k_er_margin_x     = 28,  /**< Outer horizontal page margin, px. */
  k_er_title_y      = 18,  /**< Header title vertical offset, px. */
  k_er_badge_pad_x  = 14,  /**< Mode pill horizontal inset, px.   */
  k_er_badge_w      = 240, /**< Mode pill width, px.              */
  k_er_badge_h      = 32,  /**< Mode pill height, px.             */
  k_er_badge_y      = 10,  /**< Mode pill vertical offset, px.    */
  k_er_body_y       = 84,  /**< Primary body block top, px.       */
  k_er_line_step_y  = 28,  /**< Vertical baseline pitch, px.      */
  k_er_footer_inset = 36,  /**< Bottom rule offset from bottom.   */
  k_er_min_dim_px   = 120, /**< Minimum supported panel edge, px. */
  k_er_footer_gap   = 8,   /**< Footer text spacing below rule.   */
} er_panel_layout_t;

/** @brief Map app refresh mode to display PAL cadence policy kind. */
static bool internal_mode_to_policy(er_refresh_mode_t mode, display_policy_kind_t* out_kind)
{
  if (out_kind == nullptr) {
    return false;
  }
  static const display_policy_kind_t k_policy_map[k_er_refresh_mode_count] = {
    [k_er_refresh_fast]       = k_display_policy_fast_only,
    [k_er_refresh_full]       = k_display_policy_quality,
    [k_er_refresh_fast_clean] = k_display_policy_fast_clean,
  };
  if ((uint8_t)mode >= (uint8_t)k_er_refresh_mode_count) {
    return false;
  }
  *out_kind = k_policy_map[mode];
  return true;
}

/** @brief Validate clean cadence or substitute the display PAL default. */
static uint16_t internal_effective_clean_every(uint16_t clean_every)
{
  if (clean_every == 0U) {
    return (uint16_t)k_display_policy_clean_every_default;
  }
  if (clean_every > (uint16_t)k_display_policy_clean_every_max) {
    return (uint16_t)k_display_policy_clean_every_max;
  }
  return clean_every;
}

/** @brief Complete framebuffer lookup, ra8_gfx bind, and policy init after display handle exists. */
[[nodiscard]] static ra8_err_t internal_finish_panel_init(er_panel_state_t* state,
                                                          er_refresh_mode_t initial_mode,
                                                          uint16_t          clean_every)
{
  if ((state == nullptr) || (state->display == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  display_policy_kind_t kind = k_display_policy_fast_only;
  if (!internal_mode_to_policy(initial_mode, &kind)) {
    (void)display_deinit(state->display);
    state->display = nullptr;
    return k_ra8_err_invalid_arg;
  }

  ra8_err_t err = display_get_framebuffer(state->display, &state->fb);
  if (err != k_ra8_ok) {
    (void)display_deinit(state->display);
    state->display = nullptr;
    return err;
  }
  err = display_bind_gfx(&state->fb);
  if (err != k_ra8_ok) {
    (void)display_deinit(state->display);
    state->display = nullptr;
    return err;
  }

  const uint16_t effective_clean = internal_effective_clean_every(clean_every);
  err                            = display_policy_init(&state->policy, kind, effective_clean);
  if (err != k_ra8_ok) {
    (void)ra8_gfx_deinit();
    (void)display_deinit(state->display);
    state->display = nullptr;
    return err;
  }

  state->mode        = initial_mode;
  state->clean_every = effective_clean;
  state->last_hint   = k_display_refresh_init;
  state->last_rect   = display_full_rect(state->display);
  state->frame_count = 0U;
  state->initialized = true;
  return k_ra8_ok;
}

ra8_err_t er_panel_init(er_panel_state_t*    state,
                        const display_cfg_t* cfg,
                        er_refresh_mode_t    initial_mode,
                        uint16_t             clean_every)
{
  if ((state == nullptr) || (cfg == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  display_policy_kind_t kind = k_display_policy_fast_only;
  if (!internal_mode_to_policy(initial_mode, &kind)) {
    return k_ra8_err_invalid_arg;
  }
  if (state->initialized) {
    return k_ra8_err_busy;
  }

  *state              = (er_panel_state_t){};
  const ra8_err_t err = display_init(cfg, &state->display);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_finish_panel_init(state, initial_mode, clean_every);
}

ra8_err_t er_panel_init_glcdc(er_panel_state_t*         state,
                              const display_fb_cfg_t*   fb_cfg,
                              const ra8_glcdc_timing_t* timing,
                              er_refresh_mode_t         initial_mode,
                              uint16_t                  clean_every)
{
  if ((state == nullptr) || (fb_cfg == nullptr) || (timing == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  display_policy_kind_t kind = k_display_policy_fast_only;
  if (!internal_mode_to_policy(initial_mode, &kind)) {
    return k_ra8_err_invalid_arg;
  }
  if (state->initialized) {
    return k_ra8_err_busy;
  }

  *state              = (er_panel_state_t){};
  const ra8_err_t err = display_pal_bind_glcdc(&state->display, fb_cfg, timing);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_finish_panel_init(state, initial_mode, clean_every);
}

ra8_err_t er_panel_deinit(er_panel_state_t* state)
{
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  (void)ra8_gfx_deinit();
  const ra8_err_t err = display_deinit(state->display);
  *state              = (er_panel_state_t){};
  return err;
}

ra8_err_t
er_panel_set_refresh_mode(er_panel_state_t* state, er_refresh_mode_t mode, uint16_t clean_every)
{
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  display_policy_kind_t kind = k_display_policy_fast_only;
  if (!internal_mode_to_policy(mode, &kind)) {
    return k_ra8_err_invalid_arg;
  }

  const uint16_t next_clean =
    (clean_every == 0U) ? state->clean_every : internal_effective_clean_every(clean_every);
  const ra8_err_t err = display_policy_init(&state->policy, kind, next_clean);
  if (err != k_ra8_ok) {
    return err;
  }
  state->mode        = mode;
  state->clean_every = next_clean;
  return k_ra8_ok;
}

er_refresh_mode_t er_panel_next_refresh_mode(er_refresh_mode_t current)
{
  static const er_refresh_mode_t k_cycle[k_er_refresh_mode_count] = {
    [k_er_refresh_fast]       = k_er_refresh_full,
    [k_er_refresh_full]       = k_er_refresh_fast_clean,
    [k_er_refresh_fast_clean] = k_er_refresh_fast,
  };
  if ((uint8_t)current >= (uint8_t)k_er_refresh_mode_count) {
    return k_er_refresh_full;
  }
  return k_cycle[current];
}

ra8_err_t er_panel_cycle_refresh_mode(er_panel_state_t* state)
{
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  return er_panel_set_refresh_mode(state, er_panel_next_refresh_mode(state->mode), 0U);
}

const char* er_panel_refresh_mode_label(er_refresh_mode_t mode)
{
  static const char* const k_labels[k_er_refresh_mode_count] = {
    [k_er_refresh_fast]       = "MODE: FAST",
    [k_er_refresh_full]       = "MODE: FULL",
    [k_er_refresh_fast_clean] = "MODE: FAST+CLEAN",
  };
  if ((uint8_t)mode >= (uint8_t)k_er_refresh_mode_count) {
    return "MODE: UNKNOWN";
  }
  return k_labels[mode];
}

ra8_err_t er_panel_render_frame(er_panel_state_t*    state,
                                display_turn_event_t event,
                                const char*          title,
                                const char*          body)
{
  if ((state == nullptr) || (title == nullptr) || (body == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!state->initialized || (state->display == nullptr)) {
    return k_ra8_err_not_initialized;
  }
  if (((int32_t)state->fb.width_px < (int32_t)k_er_min_dim_px) ||
      ((int32_t)state->fb.height_px < (int32_t)k_er_min_dim_px)) {
    return k_ra8_err_invalid_arg;
  }

  display_policy_decision_t dec = {};
  ra8_err_t                 err = display_policy_decide(&state->policy, event, &dec);
  if (err != k_ra8_ok) {
    return err;
  }

  const int32_t w = (int32_t)state->fb.width_px;
  const int32_t h = (int32_t)state->fb.height_px;

  err = ra8_gfx_clear((uint32_t)k_er_color_paper);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gfx_rect(0, 0, w, (int32_t)k_er_header_h, (uint32_t)k_er_color_header, true);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gfx_text_out((int32_t)k_er_margin_x,
                         (int32_t)k_er_title_y,
                         title,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_color_paper,
                         (uint32_t)k_er_color_header);
  if (err != k_ra8_ok) {
    return err;
  }

  const int32_t badge_x = w - (int32_t)k_er_margin_x - (int32_t)k_er_badge_w;
  err                   = ra8_gfx_rect(badge_x,
                                       (int32_t)k_er_badge_y,
                                       (int32_t)k_er_badge_w,
                                       (int32_t)k_er_badge_h,
                                       (uint32_t)k_er_color_badge,
                                       true);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gfx_text_out(badge_x + (int32_t)k_er_badge_pad_x,
                         (int32_t)k_er_title_y,
                         er_panel_refresh_mode_label(state->mode),
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_color_ink,
                         (uint32_t)k_er_color_badge);
  if (err != k_ra8_ok) {
    return err;
  }

  const char* const hint_label =
    (dec.hint == k_display_refresh_init)
      ? "HINT: INIT (FULL WAVEFORM)"
      : ((dec.hint == k_display_refresh_quality) ? "HINT: FULL QUALITY" : "HINT: FAST PARTIAL");
  err = ra8_gfx_text_out((int32_t)k_er_margin_x,
                         (int32_t)k_er_body_y,
                         body,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_color_ink,
                         (uint32_t)k_er_color_paper);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gfx_text_out((int32_t)k_er_margin_x,
                         (int32_t)k_er_body_y + (int32_t)k_er_line_step_y,
                         hint_label,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_color_muted,
                         (uint32_t)k_er_color_paper);
  if (err != k_ra8_ok) {
    return err;
  }

  const int32_t rule_y = h - (int32_t)k_er_footer_inset;
  err                  = ra8_gfx_line((int32_t)k_er_margin_x,
                                      rule_y,
                                      w - (int32_t)k_er_margin_x,
                                      rule_y,
                                      (uint32_t)k_er_color_rule);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gfx_text_out((int32_t)k_er_margin_x,
                         rule_y + (int32_t)k_er_footer_gap,
                         "SW1: page turn   SW2: switch FULL/FAST mode",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_color_muted,
                         (uint32_t)k_er_color_paper);
  if (err != k_ra8_ok) {
    return err;
  }

  const display_rect_t flush_rect = display_full_rect(state->display);
  err                             = display_flush(state->display, flush_rect, dec.hint);
  if (err != k_ra8_ok) {
    return err;
  }
  state->last_hint = dec.hint;
  state->last_rect = flush_rect;
  state->frame_count++;
  return k_ra8_ok;
}
