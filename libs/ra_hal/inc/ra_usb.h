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
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_init(ra_usb_speed_t speed);

/**
 * @brief Enable / disable the D+ pull-up (advertises attach to host).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_attach(ra_usb_speed_t speed, bool attached);

/**
 * @typedef ra_usb_event_fn_t
 * @brief USB event callback.
 */
typedef void (*ra_usb_event_fn_t)(void* ctx, ra_usb_speed_t speed, uint16_t status_mask);

/**
 * @brief Tear down a USB controller.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_deinit(ra_usb_speed_t speed);

/**
 * @brief Read INTSTS0 mask for a controller.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_get_status(ra_usb_speed_t speed, uint16_t* out_mask);

/**
 * @brief Clear INTSTS0 bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_clear_status(ra_usb_speed_t speed, uint16_t mask);

/**
 * @brief Attach a USB event callback (shared across FS/HS).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_attach_handler(ra_usb_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a USB event -- snapshot + fire callback.
 * @since 0.1.0
 */
void ra_usb_dispatch(ra_usb_speed_t speed);

/**
 * @brief Put the USB controller into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_enter_stop(ra_usb_speed_t speed);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_exit_stop(ra_usb_speed_t speed);

#ifdef __cplusplus
}
#endif
