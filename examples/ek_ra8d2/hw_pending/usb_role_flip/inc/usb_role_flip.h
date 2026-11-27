/**
 * @file usb_role_flip.h
 * @brief USB runtime role-flip self-loop definitions.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the state machine and step functions for dynamically tearing down
 * and swapping the HS and FS controller roles (Device vs Host) at runtime.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "ra8_err.h"
#include "ra8_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum usb_role_flip_phase_t
 * @brief States in the role-flip execution ladder.
 */
typedef enum : uint32_t {
  k_usb_role_flip_phase_boot      = 0U, /**< Boot / hardware bringup.       */
  k_usb_role_flip_phase_cfg_a     = 1U, /**< Config A: FS Device + HS Host. */
  k_usb_role_flip_phase_teardown1 = 2U, /**< Teardown Config A.             */
  k_usb_role_flip_phase_cfg_b     = 3U, /**< Config B: FS Host + HS Device. */
  k_usb_role_flip_phase_teardown2 = 4U, /**< Teardown Config B.             */
  k_usb_role_flip_phase_pass      = 5U, /**< Both configurations verified.  */
} usb_role_flip_phase_t;

/**
 * @brief Bring up Phase 1 (Config A): FS in Device role, HS in Host role.
 *
 * @return ra8_err_t outcome.
 */
[[nodiscard]] ra8_err_t usb_role_flip_setup_cfg_a(void);

/**
 * @brief Tear down Phase 1: uninitialize FS device (via DCD & PAL) and HS host.
 *
 * @return ra8_err_t outcome.
 */
[[nodiscard]] ra8_err_t usb_role_flip_teardown_cfg_a(void);

/**
 * @brief Bring up Phase 2 (Config B): FS in Host role, HS in Device role.
 *
 * @return ra8_err_t outcome.
 */
[[nodiscard]] ra8_err_t usb_role_flip_setup_cfg_b(void);

/**
 * @brief Tear down Phase 2: uninitialize FS host and HS device (via DCD & PAL).
 *
 * @return ra8_err_t outcome.
 */
[[nodiscard]] ra8_err_t usb_role_flip_teardown_cfg_b(void);

/**
 * @brief Execute one complete role-flip cycle (Config A -> Teardown -> Config B -> Teardown).
 *
 * @return ra8_err_t outcome.
 */
[[nodiscard]] ra8_err_t usb_role_flip_run_cycle(void);

#ifdef __cplusplus
}
#endif
