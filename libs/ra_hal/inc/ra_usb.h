/**
 * @file ra_usb.h
 * @brief USB controller driver (framework)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_usb_speed_t
 * @brief Which controller to talk to.
 */
typedef enum : uint8_t {
  k_ra_usb_speed_fs = 0U, /**< Full-Speed controller at 0x40250000. */
  k_ra_usb_speed_hs = 1U, /**< High-Speed controller at 0x40351000. */
} ra_usb_speed_t;

/**
 * @brief Initialise USB in device mode with D+ pull-up disabled.
 */
[[nodiscard]] ra_err_t ra_usb_device_init(ra_usb_speed_t speed);

/**
 * @brief Enable / disable the D+ pull-up (advertises attach to host).
 */
[[nodiscard]] ra_err_t ra_usb_device_attach(ra_usb_speed_t speed, bool attached);

#ifdef __cplusplus
}
#endif
