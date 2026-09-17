/**
 * @file ra8_board_ek_ra8d2_camera.c
 * @brief EK-RA8D2 J35 camera clock, routing, reset, and SCCB adapter.
 * @ingroup grp_board
 * @details Owns only board-specific GPT, U15, pin-mux, reset, and RIIC wiring;
 *          sensor protocol and capture policy remain in reusable libraries.
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_gpt.h"
#include "ra8_i2c.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

typedef enum : uint8_t {
  k_camera_reg_address_bytes = 2U,    /**< SCCB register-address bytes. */
  k_camera_write_bytes       = 3U,    /**< Address plus value bytes.    */
  k_camera_high_byte_shift   = 8U,    /**< Register high-byte shift.    */
  k_camera_xclk_gpt_channel  = 12U,   /**< GPT channel driving XCLK.    */
  k_camera_sw46_output       = 0xDFU, /**< U15 latch with SW4-6 ON.     */
  k_camera_sw46_mask         = 0x20U, /**< U15 SW4-6 override bit.      */
} ra8_board_camera_protocol_t;

typedef enum : uint32_t {
  k_camera_reset_low_ms  = 20U, /**< Reset assertion interval. */
  k_camera_reset_high_ms = 20U, /**< Reset release interval.   */
} ra8_board_camera_delay_t;

typedef enum : uint32_t {
  k_camera_gpt_period_max = 0xFFFFU, /**< Maximum GPT period register value. */
} ra8_board_camera_limit_t;

static const ra8_port_pin_t s_camera_parallel_pins[] = {
  (ra8_port_pin_t)k_ra8_board_cam_d0,
  (ra8_port_pin_t)k_ra8_board_cam_d1,
  (ra8_port_pin_t)k_ra8_board_cam_d2,
  (ra8_port_pin_t)k_ra8_board_cam_d3,
  (ra8_port_pin_t)k_ra8_board_cam_d4,
  (ra8_port_pin_t)k_ra8_board_cam_d5,
  (ra8_port_pin_t)k_ra8_board_cam_d6,
  (ra8_port_pin_t)k_ra8_board_cam_d7,
  (ra8_port_pin_t)k_ra8_board_cam_vsync,
  (ra8_port_pin_t)k_ra8_board_cam_hsync,
  (ra8_port_pin_t)k_ra8_board_cam_pclk,
};

/* See the public header for the documented contract. */
ra8_err_t ra8_board_camera_xclk_start(uint32_t frequency_hz)
{
  if (frequency_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t  pclkd_hz = 0U;
  ra8_err_t err      = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclkd, &pclkd_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t period = pclkd_hz / frequency_hz;
  if ((period < 2U) || (period > (uint32_t)k_camera_gpt_period_max)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_gpt_cfg_t timer_cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = period - 1U,
    .duty_a     = period / 2U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  err = ra8_gpt_init((uint8_t)k_camera_xclk_gpt_channel, &timer_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_gpt_pwm_pin_cfg_t pin_cfg = {
    .output_enable    = true,
    .polarity         = k_ra8_gpt_pol_active_high,
    .stop_level       = k_ra8_gpt_stop_low,
    .disable_on_fault = k_ra8_gpt_disable_none,
  };
  err = ra8_gpt_pwm_pin_configure((uint8_t)k_camera_xclk_gpt_channel, k_ra8_gpt_pin_a, &pin_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_cam_xclk,
                                 k_ra8_psel_gpt0,
                                 "board.camera.xclk");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_set_drive_strength((ra8_port_pin_t)k_ra8_board_cam_xclk,
                                    k_ra8_pfs_dscr_high_speed_high);
}

/* See the public header for the documented contract. */
ra8_err_t ra8_board_camera_select_parallel(void)
{
  return ra8_board_io_expander_apply_sw4_mask((uint8_t)k_camera_sw46_output,
                                              (uint8_t)k_camera_sw46_mask);
}

/* See the public header for the documented contract. */
ra8_err_t ra8_board_camera_route_parallel_pins(void)
{
  const uint32_t count =
    (uint32_t)(sizeof(s_camera_parallel_pins) / sizeof(s_camera_parallel_pins[0]));
  for (uint32_t i = 0U; i < count; i += 1U) {
    const ra8_err_t err =
      ra8_pfs_route_peripheral(s_camera_parallel_pins[i], k_ra8_psel_ceu, "board.camera.ceu");
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* See the public header for the documented contract. */
ra8_err_t ra8_board_camera_reset(void)
{
  const ra8_port_pin_t reset_pin = (ra8_port_pin_t)k_ra8_board_cam_rst;
  ra8_err_t            err       = ra8_gpio_output_init(reset_pin, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_camera_reset_low_ms);
  err = ra8_gpio_write(reset_pin, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_camera_reset_high_ms);
  return k_ra8_ok;
}

/* See the public header for the documented contract. */
ra8_err_t
ra8_board_camera_sccb_read_reg(void* ctx, uint8_t address, uint16_t reg, uint8_t* out_value)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(out_value, "board.camera", "read");
  const uint8_t register_address[k_camera_reg_address_bytes] = {
    (uint8_t)(reg >> (uint16_t)k_camera_high_byte_shift),
    (uint8_t)reg,
  };
  return ra8_i2c_transfer((uint8_t)k_ra8_board_camera_i2c_channel,
                          address,
                          register_address,
                          (uint32_t)sizeof(register_address),
                          out_value,
                          1U);
}

/* See the public header for the documented contract. */
ra8_err_t ra8_board_camera_sccb_write_reg(void* ctx, uint8_t address, uint16_t reg, uint8_t value)
{
  (void)ctx;
  const uint8_t payload[k_camera_write_bytes] = {
    (uint8_t)(reg >> (uint16_t)k_camera_high_byte_shift),
    (uint8_t)reg,
    value,
  };
  return ra8_i2c_write((uint8_t)k_ra8_board_camera_i2c_channel,
                       address,
                       payload,
                       (uint32_t)sizeof(payload),
                       true);
}

/* See the public header for the documented contract. */
void ra8_board_camera_delay_ms(void* ctx, uint32_t milliseconds)
{
  (void)ctx;
  ra8_delay_ms(milliseconds);
}
