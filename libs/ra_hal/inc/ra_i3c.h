/**
 * @file ra_i3c.h
 * @brief I3C Bus Interface driver scaffold
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * introduces a minimal I3C driver scaffold covering the
 * lifecycle + status + IRQ + power-transition surface. Full CCC
 * (Common Command Code) handling, IBI support, and HDR / DDR
 * modes land with the first consumer.
 *
 * API surface:
 *
 * - ``ra_i3c_init()`` -- MSTP enable + reset IE/BIE
 * - ``ra_i3c_deinit()`` -- disable + MSTP release
 * - ``ra_i3c_get_status`` -- read INST status mask
 * - ``ra_i3c_clear_status`` -- clear INST bits
 * - ``ra_i3c_attach_handler`` -- install IRQ callback
 * - ``ra_i3c_enter_stop / exit_stop`` -- power transition
 * - ``ra_i3c_dispatch`` -- ISR entry point
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
 * @typedef ra_i3c_event_fn_t
 * @brief I3C event callback.
 */
typedef void (*ra_i3c_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Initialise the I3C controller.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_init(void);

/**
 * @brief Tear down the I3C controller.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_deinit(void);

/**
 * @brief Programme the active 7-bit master/slave device address.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_set_address(uint32_t addr);

/**
 * @brief Enable / disable bus-master operation via BCTL.BUSE.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_bus_enable(bool enable);

/**
 * @brief Read the INST status register.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_get_status(uint32_t* out_mask);

/**
 * @brief Clear INST status bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_clear_status(uint32_t mask);

/**
 * @brief Attach an I3C event callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_attach_handler(ra_i3c_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an I3C event -- snapshot status + fire callback.
 * @since 0.1.0
 */
void ra_i3c_dispatch(void);

/**
 * @brief Put I3C into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_exit_stop(void);

#ifdef __cplusplus
}
#endif
