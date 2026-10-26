/**
 * @file fw_if_gpio.h
 * @brief Architecture-neutral GPIO port: pin direction, output and level read
 * behind one narrow vtable.
 * @ingroup grp_fw_gpio
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable code needs to blink a status line, drive a enable pin and sense a
 * button, and every chip spells GPIO differently. This port carries the three
 * intents such a driver actually has -- configure a pin, drive it, read it --
 * and leaves the mechanism to a binding supplied by the chip and board.
 *
 * Pins are named in *board* numbering, dense and zero-based, never in chip
 * channel numbers: a board that wires two status LEDs presents pins 0 and 1
 * however the silicon allocates them, and its binding maps them. That
 * indirection is what lets an application move between boards without
 * renumbering its wiring, and it is the same rule the clock port set for
 * module instances. How many pins exist is a board fact, so the membrane does
 * not range-check the number; the binding owns it and answers
 * ::k_ra8_err_not_found for a pin it does not carry.
 *
 * Configuration here is direction and pull only. Drive strength, slew, open
 * drain, alternate functions and debouncing are chip and board policy: a
 * binding that needs them exposes them in its own binding header, which the
 * composition root may name because the composition root already knows which
 * chip it picked. This port deliberately has no edge-interrupt surface; level
 * polling composes, callbacks do not, and a caller that needs edges belongs
 * below this port.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. Handles are
 * caller-owned. This interface performs no allocation and contains no chip,
 * operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum fw_gpio_direction_t
 * @brief What a pin is configured to do.
 */
typedef enum : uint8_t {
  k_fw_gpio_direction_none   = 0U, /**< Not a direction; rejected.     */
  k_fw_gpio_direction_input  = 1U, /**< Sense the level.               */
  k_fw_gpio_direction_output = 2U, /**< Drive the level.               */
} fw_gpio_direction_t;

/** @brief One past the last valid ::fw_gpio_direction_t enumerator. */
#define K_FW_GPIO_DIRECTION_COUNT 3U

/**
 * @enum fw_gpio_pull_t
 * @brief What holds the pin when nothing drives it.
 *
 * @details
 * `none` is a real answer, not a rejected one: an externally pulled line is
 * the common case, and a binding on silicon with no internal pulls maps every
 * request onto it.
 */
typedef enum : uint8_t {
  k_fw_gpio_pull_none = 0U, /**< Floating; the board decides. */
  k_fw_gpio_pull_up   = 1U, /**< Weak to the high level.      */
  k_fw_gpio_pull_down = 2U, /**< Weak to the low level.       */
} fw_gpio_pull_t;

/** @brief One past the last valid ::fw_gpio_pull_t enumerator. */
#define K_FW_GPIO_PULL_COUNT 3U

/**
 * @struct fw_gpio_pin_cfg_s
 * @brief Direction and pull for one pin.
 */
typedef struct fw_gpio_pin_cfg_s {
  fw_gpio_direction_t direction; /**< What the pin does.                 */
  fw_gpio_pull_t      pull;      /**< What holds it when nothing drives. */
} fw_gpio_pin_cfg_t;

/**
 * @struct fw_gpio_iface_s
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Three entries, because the port has three intents. Each takes the binding's
 * own context as its first argument, so one binding can serve more than one
 * port without a file-scope variable. A NULL op is a malformed binding, not a
 * declined capability, and ::fw_gpio_bind rejects it.
 */
typedef struct fw_gpio_iface_s {
  /** @brief Put @p pin into @p cfg's direction and pull. */
  ra8_err_t (*configure)(void *ctx, uint16_t pin, fw_gpio_pin_cfg_t cfg);
  /** @brief Drive @p pin to @p value. An input pin rejects this. */
  ra8_err_t (*write)(void *ctx, uint16_t pin, bool value);
  /** @brief Sample @p pin's level. */
  ra8_err_t (*read)(void *ctx, uint16_t pin, bool *out_value);
} fw_gpio_iface_t;

/**
 * @struct fw_gpio_s
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_gpio_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_gpio_s {
  const fw_gpio_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;  /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_gpio_bind only.        */
} fw_gpio_t;

/**
 * @brief Bind a handle to a chip-and-board GPIO binding.
 *
 * @param[out] gpio  Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p gpio.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p gpio or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_bind(fw_gpio_t *gpio, const fw_gpio_iface_t *iface, void *ctx);

/**
 * @brief Put one pin into a direction and pull.
 *
 * @param[in] gpio Bound handle.
 * @param[in] pin  Pin in board numbering.
 * @param[in] cfg  Direction and pull; a zeroed cfg is rejected.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle or a
 *         malformed cfg, ::k_ra8_err_not_found when the board has no such
 *         pin, or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_configure(const fw_gpio_t *gpio, uint16_t pin, fw_gpio_pin_cfg_t cfg);

/**
 * @brief Drive one output pin.
 *
 * @param[in] gpio  Bound handle.
 * @param[in] pin   Pin in board numbering.
 * @param[in] value Level to drive.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle,
 *         ::k_ra8_err_not_found when the board has no such pin,
 *         ::k_ra8_err_invalid_state when the pin is not an output, or
 *         whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_write(const fw_gpio_t *gpio, uint16_t pin, bool value);

/**
 * @brief Sample one pin's level.
 *
 * @param[in]  gpio Bound handle.
 * @param[in]  pin  Pin in board numbering.
 * @param[out] out_value On success, the sampled level; false on any failure.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output or an unbound
 *         handle, ::k_ra8_err_not_found when the board has no such pin, or
 *         whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_gpio_read(const fw_gpio_t *gpio, uint16_t pin, bool *out_value);

#ifdef __cplusplus
}
#endif
