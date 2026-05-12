/**
 * @file ra_pin_interface.h
 * @brief Abstract pin-driver interface for dependency injection
 *
 * @details
 * Drivers that need to toggle a GPIO pin (LED blinkers, motor
 * enables, nFAULT inputs, chip-select lines) should not call
 * `ra_gpio_*` directly in production code -- that couples them to
 * the real hardware and prevents unit testing. Instead they take an
 * `ra_pin_interface_t` pointer:
 *
 * @code{.c}
 * typedef struct {
 *     const ra_pin_interface_t* pin_if;
 *     ra_port_pin_t             led_pin;
 *     bool                      initialized;
 * } led_driver_t;
 *
 * ra_err_t led_driver_blink(led_driver_t* drv) {
 *     return drv->pin_if->write(drv->pin_if->ctx, drv->led_pin,
 *                               k_ra_level_high);
 * }
 * @endcode
 *
 * In production `pin_if` points at `g_ra_gpio_pin_interface`
 * (defined in `libs/ra_hal/src/gpio.c`). In tests it points at a
 * mock that records every call.
 *
 * This is the "Dependency Inversion" D of SOLID. NASA Power of 10
 * Rule 9 nominally bans function pointers, but this project makes
 * the DI exception called out in CLAUDE.md.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_err.h"
#include "ra_port_constants.h"

/**
 * @struct ra_pin_interface_t
 * @brief Vtable for a pin driver.
 */
typedef struct {
  /**
   * @brief Configure a pin as an output at an initial level.
   */
  // cppcheck-suppress unusedStructMember
  ra_err_t (*output_init)(void* ctx, ra_port_pin_t pin, ra_level_t init_level);
  /**
   * @brief Drive a pin.
   */
  // cppcheck-suppress unusedStructMember
  ra_err_t (*write)(void* ctx, ra_port_pin_t pin, ra_level_t level);
  /**
   * @brief Read a pin.
   */
  // cppcheck-suppress unusedStructMember
  ra_err_t (*read)(void* ctx, ra_port_pin_t pin, ra_level_t* out_level);
  /**
   * @brief Toggle a pin.
   */
  // cppcheck-suppress unusedStructMember
  ra_err_t (*toggle)(void* ctx, ra_port_pin_t pin);
  /**
   * @brief Opaque context handed to every call.
   */
  // cppcheck-suppress unusedStructMember
  void* ctx;
} ra_pin_interface_t;

#ifdef __cplusplus
}
#endif
