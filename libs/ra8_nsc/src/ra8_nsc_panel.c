/**
 * @file ra8_nsc_panel.c
 * @brief NSC veneers for EK-RA8D2 panel backlight and user push-buttons.
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Exposes Secure-owned board GPIO panel backlight control (BLEN P514) and
 * user switch polling (SW1 P009, SW2 P008) to Non-Secure applications such as
 * the ThreadX e-reader. Pointer outputs are validated via
 * ``RA8_NSC_CHECK_NS_RANGE_RW`` before delegating to ``ra8_board_*``.
 * Targets without EK-RA8D2 board headers fail closed with
 * ``k_ra8_err_not_supported``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"

#if defined(RA8_OFF_TARGET) || __has_include("ra8_board_ek_ra8d2.h")
#include "ra8_board_ek_ra8d2.h"
#include "ra8_port_utils.h"
#define RA8_NSC_HAVE_PANEL_BOARD 1
#else
#define RA8_NSC_HAVE_PANEL_BOARD 0
#endif

#if defined(RA8_OFF_TARGET) || __has_include("ra8_board_ek_ra8d2_touch.h")
#include "ra8_board_ek_ra8d2_touch.h"
#include "ra8_touch.h"
#define RA8_NSC_HAVE_PANEL_TOUCH 1
#else
#define RA8_NSC_HAVE_PANEL_TOUCH 0
#endif

static const char s_tag[] = "nsc.panel";

RA8_NSC_VENEER ra8_err_t ra8_nsc_backlight_init(bool initial_on)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  return ra8_board_backlight_init(initial_on);
#else
  (void)initial_on;
  (void)s_tag;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_backlight_set(bool on)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  return ra8_board_backlight_set(on);
#else
  (void)on;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_backlight_set_brightness(uint8_t percent)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  return ra8_board_backlight_set_brightness(percent);
#else
  (void)percent;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_backlight_get_caps(ra8_nsc_backlight_caps_t* out_caps)
{
  RA8_CHECK_NULL_PTR(out_caps, s_tag, "backlight_get_caps: out_caps");
  RA8_NSC_CHECK_NS_RANGE_RW(out_caps, sizeof(*out_caps));
#if RA8_NSC_HAVE_PANEL_BOARD
  ra8_board_backlight_caps_t board_caps = {};
  const ra8_err_t            err        = ra8_board_backlight_caps(&board_caps);
  if (err != k_ra8_ok) {
    return err;
  }
  out_caps->min_pct      = board_caps.min_pct;
  out_caps->max_pct      = board_caps.max_pct;
  out_caps->supports_pwm = board_caps.supports_pwm;
  return k_ra8_ok;
#else
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_sw_init(uint8_t sw_id)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  if (sw_id >= (uint8_t)k_ra8_board_sw_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_board_sw_init((ra8_board_sw_id_t)sw_id);
#else
  (void)sw_id;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_sw_read(uint8_t sw_id, bool* out_pressed)
{
  RA8_CHECK_NULL_PTR(out_pressed, s_tag, "sw_read: out_pressed");
  RA8_NSC_CHECK_NS_RANGE_RW(out_pressed, sizeof(*out_pressed));
#if RA8_NSC_HAVE_PANEL_BOARD
  if (sw_id >= (uint8_t)k_ra8_board_sw_count) {
    return k_ra8_err_invalid_arg;
  }
  ra8_board_sw_state_t state = k_ra8_board_sw_released;
  const ra8_err_t      err   = ra8_board_sw_read((ra8_board_sw_id_t)sw_id, &state);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_pressed = (state == k_ra8_board_sw_pressed);
  return k_ra8_ok;
#else
  (void)sw_id;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_lcd_panel_power_on(void)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  return ra8_board_lcd_panel_power_on();
#else
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_lcd_panel_reset_init(bool high)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  return ra8_gpio_output_init((ra8_port_pin_t)k_ra8_board_lcd_reset_l,
                              high ? k_ra8_level_high : k_ra8_level_low);
#else
  (void)high;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_lcd_panel_reset_write(bool high)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  return ra8_gpio_write((ra8_port_pin_t)k_ra8_board_lcd_reset_l,
                        high ? k_ra8_level_high : k_ra8_level_low);
#else
  (void)high;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_board_glcdc_init(uint8_t fmt)
{
#if RA8_NSC_HAVE_PANEL_BOARD
  if (fmt > (uint8_t)k_ra8_board_glcdc_fmt_rgb565) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_board_glcdc_init((ra8_board_glcdc_fmt_t)fmt);
#else
  (void)fmt;
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_board_touch_open(const ra8_nsc_touch_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "board_touch_open: cfg");
  RA8_NSC_CHECK_NS_RANGE_R(cfg, sizeof(*cfg));
#if RA8_NSC_HAVE_PANEL_TOUCH
  const ra8_board_touch_cfg_t board_cfg = {
    .max_points = cfg->max_points,
    .irq_pin    = cfg->irq_pin,
  };
  return ra8_board_touch_open(&board_cfg);
#else
  return k_ra8_err_not_supported;
#endif
}

RA8_NSC_VENEER ra8_err_t ra8_nsc_touch_read(ra8_nsc_touch_point_t* out_points,
                                           uint8_t                max_count,
                                           uint8_t*               out_got_count)
{
  RA8_CHECK_NULL_PTR(out_got_count, s_tag, "touch_read: out_got_count");
  RA8_NSC_CHECK_NS_RANGE_RW(out_got_count, sizeof(*out_got_count));
  if (max_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  RA8_CHECK_NULL_PTR(out_points, s_tag, "touch_read: out_points");
  RA8_NSC_CHECK_NS_RANGE_RW(out_points, (size_t)max_count * sizeof(ra8_nsc_touch_point_t));
#if RA8_NSC_HAVE_PANEL_TOUCH
  ra8_touch_point_t sec_pts[k_ra8_touch_max_points];
  const uint8_t     cap =
    (max_count < (uint8_t)k_ra8_touch_max_points) ? max_count : (uint8_t)k_ra8_touch_max_points;
  uint8_t         got = 0U;
  const ra8_err_t err = ra8_touch_read(sec_pts, cap, &got);
  if (err != k_ra8_ok) {
    *out_got_count = 0U;
    return err;
  }
  for (uint8_t i = 0U; i < got; ++i) {
    out_points[i].x        = sec_pts[i].x;
    out_points[i].y        = sec_pts[i].y;
    out_points[i].track_id = sec_pts[i].track_id;
    out_points[i].pressure = sec_pts[i].pressure;
  }
  *out_got_count = got;
  return k_ra8_ok;
#else
  (void)out_points;
  (void)max_count;
  *out_got_count = 0U;
  return k_ra8_err_not_supported;
#endif
}
