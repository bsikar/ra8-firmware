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
 * The vtable covers the whole life of a pin, not only its levels.
 * An earlier version carried `output_init`, `write`, `read` and
 * `toggle` alone, which left every injected driver calling
 * `ra8_gpio_input_init` and `ra8_gpio_release` straight through to
 * the HAL. A driver injectable for three of its five pin operations
 * is not injectable: a mock sees the writes and misses the
 * configuration that decided what those writes meant, and the same
 * driver cannot be hosted on a part whose pins are claimed some
 * other way. `input_init` and `release` are rows for that reason.
 * Interrupt attachment deliberately is not: an ICU channel is a
 * chip resource with its own numbering and lifetime, and bridging
 * it needs a port of its own rather than a row here.
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
   * @brief Configure a pin as an input with an internal pull selection.
   */
  ra8_err_t (*input_init)(void* ctx, ra8_port_pin_t pin, ra8_pin_pull_t pull);
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
   * @brief Return a pin to its reset state and drop this driver's claim.
   */
  ra8_err_t (*release)(void* ctx, ra8_port_pin_t pin);
  /**
   * @brief Opaque context handed to every call.
   */
  void* ctx;
} ra8_pin_interface_t;

/**
 * @brief Return the interface that drives this part's own pins.
 *
 * @details
 * The production vtable is defined in `libs/ra8_hal/src/gpio.c` as
 * `g_ra8_gpio_pin_interface`. Naming that object at a call site puts
 * the chip's GPIO block back into code that was supposed to be free
 * of it, so consumers ask for it by this chip-agnostic name instead
 * and a port for another part supplies its own definition. Callers
 * that accept an injected interface should keep doing so; this is
 * for the composition root that has to name a default.
 *
 * @return Pointer to the part's pin interface. Never NULL.
 *
 * @post The returned vtable has every row populated.
 *
 * @note Thread-safe: the returned object is immutable.
 * @since 0.1.0
 */
const ra8_pin_interface_t* ra8_pin_interface_default(void);

#ifdef __cplusplus
}
#endif
