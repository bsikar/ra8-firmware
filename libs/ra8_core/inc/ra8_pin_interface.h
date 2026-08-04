/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_pin_interface.h
 * @brief Abstract pin-driver interface for dependency injection
 * @ingroup grp_core
 *
 * @details
 * Drivers that need to toggle a GPIO pin (LED blinkers, motor
 * enables, nFAULT inputs, chip-select lines) should not call
 * `ra8_gpio_*` directly in production code -- that couples them to
 * the real hardware and prevents unit testing. Instead they take an
 * `ra8_pin_interface_t` pointer:
 *
 * @code{.c}
 * typedef struct {
 *     const ra8_pin_interface_t* pin_if;
 *     ra8_port_pin_t             led_pin;
 *     bool                      initialized;
 * } led_driver_t;
 *
 * ra8_err_t led_driver_blink(led_driver_t* drv) {
 *     return drv->pin_if->write(drv->pin_if->ctx, drv->led_pin,
 *                               k_ra8_level_high);
 * }
 * @endcode
 *
 * In production `pin_if` points at `g_ra8_gpio_pin_interface`
 * (defined in `libs/ra8_hal/src/gpio.c`). In tests it points at a
 * mock that records every call.
 *
 * This is the "Dependency Inversion" D of SOLID. NASA Power of 10
 * Rule 9 nominally bans function pointers, but this project makes
 * the DI exception called out in CLAUDE.md.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"
#include "ra8_port_constants.h"

/**
 * @struct ra8_pin_interface_t
 * @brief Vtable for a pin driver.
 */
typedef struct {
  /**
   * @brief Configure a pin as an output at an initial level.
   */
  ra8_err_t (*output_init)(void* ctx, ra8_port_pin_t pin, ra8_level_t init_level);
  /**
   * @brief Drive a pin.
   */
  ra8_err_t (*write)(void* ctx, ra8_port_pin_t pin, ra8_level_t level);
  /**
   * @brief Read a pin.
   */
  ra8_err_t (*read)(void* ctx, ra8_port_pin_t pin, ra8_level_t* out_level);
  /**
   * @brief Toggle a pin.
   */
  ra8_err_t (*toggle)(void* ctx, ra8_port_pin_t pin);
  /**
   * @brief Opaque context handed to every call.
   */
  void* ctx;
} ra8_pin_interface_t;

#ifdef __cplusplus
}
#endif
