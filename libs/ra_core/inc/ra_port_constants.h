/**
 * @file ra_port_constants.h
 * @brief Typed Port / Pin Constants for the RA8D2 IOPORT Module
 *
 * @details
 * Provides `ra_port_t`, `ra_pin_t`, and a packed `ra_port_pin_t` type
 * that every GPIO, pin-function-select (PFS) and peripheral-routing
 * API consumes.
 *
 * ## Encoding
 *
 * A single `ra_port_pin_t` encodes both port and pin in 16 bits:
 *
 * @code{.c}
 *   bits 15..8 : port index (0..14)
 *   bits  7..0 : pin  index (0..15)
 * @endcode
 *
 * This matches the FSP `BSP_IO_PORT_XX_PIN_YY` layout exactly, so code
 * can be read against the Hardware User's Manual without mental
 * arithmetic.
 *
 * ## Pin availability (EK-RA8D2, 289-BGA package)
 *
 * Not every port has 16 pins bonded out on the 289-pin BGA. The
 * pin-validator will reject claims against unpopulated pins at runtime.
 * See the Hardware User's Manual chapter 20 ("I/O Ports") for the full
 * availability table.
 *
 * ## Reference pins (from EK-RA8D2 board BSP)
 *
 * | LED   | ra_port_pin_t                  | Raw value |
 * |-------|--------------------------------|-----------|
 * | LED1  | `k_ra_pin_p6_00`               | 0x0600    |
 * | LED2  | `k_ra_pin_p3_03`               | 0x0303    |
 * | LED3  | `k_ra_pin_p10_07`              | 0x0A07    |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Port indices
 * =============================================================================
 */

/**
 * @enum ra_port_t
 * @brief Port indices for the RA8D2 IOPORT module.
 *
 * @details
 * The RA8D2 has ports 0..14. Each port's exact pin population depends
 * on the package. Use `k_ra_port_count` for loop bounds.
 */
typedef enum : uint8_t {
  k_ra_port_0     = 0U,
  k_ra_port_1     = 1U,
  k_ra_port_2     = 2U,
  k_ra_port_3     = 3U,
  k_ra_port_4     = 4U,
  k_ra_port_5     = 5U,
  k_ra_port_6     = 6U,
  k_ra_port_7     = 7U,
  k_ra_port_8     = 8U,
  k_ra_port_9     = 9U,
  k_ra_port_10    = 10U,
  k_ra_port_11    = 11U,
  k_ra_port_12    = 12U,
  k_ra_port_13    = 13U,
  k_ra_port_14    = 14U,
  k_ra_port_count = 15U, /**< Number of ports (0..14).     */
  k_ra_port_max   = 14U, /**< Highest valid port index.    */
} ra_port_t;

/* =============================================================================
 * Pin indices within a port
 * =============================================================================
 */

/**
 * @enum ra_pin_t
 * @brief Pin indices within an IOPORT port (0..15).
 */
typedef enum : uint8_t {
  k_ra_pin_0     = 0U,
  k_ra_pin_1     = 1U,
  k_ra_pin_2     = 2U,
  k_ra_pin_3     = 3U,
  k_ra_pin_4     = 4U,
  k_ra_pin_5     = 5U,
  k_ra_pin_6     = 6U,
  k_ra_pin_7     = 7U,
  k_ra_pin_8     = 8U,
  k_ra_pin_9     = 9U,
  k_ra_pin_10    = 10U,
  k_ra_pin_11    = 11U,
  k_ra_pin_12    = 12U,
  k_ra_pin_13    = 13U,
  k_ra_pin_14    = 14U,
  k_ra_pin_15    = 15U,
  k_ra_pin_count = 16U, /**< Number of pins per port.     */
  k_ra_pin_max   = 15U, /**< Highest valid pin index.     */
} ra_pin_t;

/* =============================================================================
 * Packed port+pin type (matches FSP BSP_IO_PORT_XX_PIN_YY layout)
 * =============================================================================
 */

/**
 * @enum ra_port_pin_t
 * @brief Packed `(port << 8) | pin` pin identifier.
 *
 * @details
 * Used by every GPIO, PFS and peripheral-routing API. The encoding
 * matches the FSP `BSP_IO_PORT_XX_PIN_YY` convention so pinout tables
 * from the Hardware User's Manual can be transcribed verbatim.
 *
 * @par Construction:
 * The helper macro `RA_PIN(port, pin)` builds a value at compile time:
 * @code{.c}
 * const ra_port_pin_t led1 = RA_PIN(k_ra_port_6, k_ra_pin_0);   // 0x0600
 * const ra_port_pin_t led2 = RA_PIN(k_ra_port_3, k_ra_pin_3);   // 0x0303
 * const ra_port_pin_t led3 = RA_PIN(k_ra_port_10, k_ra_pin_7);  // 0x0A07
 * @endcode
 *
 * For the three on-board LEDs we also provide ready-made
 * `k_ra_pin_led*` enumerators below.
 *
 * @par Decoding:
 * - Port index: `(pin >> 8) & 0xFF` -- use `RA_PIN_PORT(p)`.
 * - Pin index:  `(pin >> 0) & 0xFF` -- use `RA_PIN_PIN(p)`.
 *
 * @invariant Only values with `port <= k_ra_port_max` and
 *            `pin <= k_ra_pin_max` are legal. Other values will be
 *            rejected by `ra_pin_validator_claim()`.
 */
typedef enum : uint16_t {
  k_ra_pin_none = 0xFFFFU, /**< Sentinel: no pin selected.              */

  /* ---- On-board LEDs on the EK-RA8D2 (from FSP board_leds.c). ------- */
  k_ra_pin_led1 = 0x0600U, /**< LED1 on P6_00 -- (port 6  << 8) | pin 0. */
  k_ra_pin_led2 = 0x0303U, /**< LED2 on P3_03 -- (port 3  << 8) | pin 3. */
  k_ra_pin_led3 = 0x0A07U, /**< LED3 on P10_07 -- (port 10 << 8) | pin 7. */
} ra_port_pin_t;

/**
 * @brief Build a `ra_port_pin_t` from explicit port / pin indices.
 * @param port `ra_port_t` value (0..`k_ra_port_max`).
 * @param pin  `ra_pin_t`  value (0..`k_ra_pin_max`).
 * @return Packed `ra_port_pin_t`.
 */
#define RA_PIN(port, pin) ((ra_port_pin_t)(((uint16_t)(port) << 8) | (uint16_t)(pin)))

/**
 * @brief Extract the port index from a packed `ra_port_pin_t`.
 * @param p Packed pin id.
 * @return `ra_port_t` value.
 */
#define RA_PIN_PORT(p) ((ra_port_t)(((uint16_t)(p) >> 8) & 0xFFU))

/**
 * @brief Extract the pin index from a packed `ra_port_pin_t`.
 * @param p Packed pin id.
 * @return `ra_pin_t` value.
 */
#define RA_PIN_PIN(p) ((ra_pin_t)((uint16_t)(p) & 0xFFU))

/* =============================================================================
 * Pin direction / level / pull
 * =============================================================================
 */

/** @brief Pin input / output direction. */
typedef enum : uint8_t {
  k_ra_dir_input  = 0U, /**< Pin is an input  (PDR bit = 0). */
  k_ra_dir_output = 1U, /**< Pin is an output (PDR bit = 1). */
} ra_pin_dir_t;

/** @brief Digital output / input level. */
typedef enum : uint8_t {
  k_ra_level_low  = 0U, /**< Drive or read 0. */
  k_ra_level_high = 1U, /**< Drive or read 1. */
} ra_level_t;

/** @brief Pull-up/pull-down selection (where supported). */
typedef enum : uint8_t {
  k_ra_pull_none = 0U, /**< No internal pull.     */
  k_ra_pull_up   = 1U, /**< Internal pull-up.     */
  k_ra_pull_down = 2U, /**< Internal pull-down.   */
} ra_pin_pull_t;

/** @brief Output drive strength (per Hardware User's Manual IOPORT chapter). */
typedef enum : uint8_t {
  k_ra_drive_low     = 0U, /**< Low drive.          */
  k_ra_drive_medium  = 1U, /**< Medium drive.       */
  k_ra_drive_high    = 2U, /**< High drive.         */
  k_ra_drive_highest = 3U, /**< Highest drive.      */
} ra_pin_drive_t;

#ifdef __cplusplus
}
#endif
