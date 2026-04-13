/**
 * @file ra_eth_gptp.h
 * @brief Ethernet Generic PTP Timer (GPTP) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 6 driver for the RA8D2 GPTP block. GPTP is the IEEE-1588
 * hardware timestamp counter shared by the GMAC ports for
 * boundary-clock and ordinary-clock applications. This driver
 * covers lifecycle + status + IRQ + power transition; the
 * timestamp counter / sync-frame encode-decode logic lands with
 * a real PTP stack.
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
 * @typedef ra_eth_gptp_event_fn_t
 * @brief GPTP event callback.
 */
typedef void (*ra_eth_gptp_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise GPTP. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_init(void);

/** @brief Tear down GPTP. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_deinit(void);

/** @brief Read GPTP_STS. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_get_status(uint32_t* out_mask);

/** @brief Clear GPTP_STS bits. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_attach_handler(ra_eth_gptp_event_fn_t fn, void* ctx);

/** @brief Dispatch a GPTP event. @since 0.3.0 */
void ra_eth_gptp_dispatch(void);

/** @brief Put GPTP into MSTP-gated stop. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gptp_exit_stop(void);

#ifdef __cplusplus
}
#endif
