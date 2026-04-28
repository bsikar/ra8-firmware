/**
 * @file ra_eth_coma.h
 * @brief Ethernet Common Agent (COMA) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 COMA block. COMA is the management
 * agent that owns bus arbitration counters + shared per-port
 * descriptor fences. This driver covers lifecycle + status + IRQ
 * + power transition.
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
 * @typedef ra_eth_coma_event_fn_t
 * @brief COMA event callback.
 */
typedef void (*ra_eth_coma_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise COMA. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_init(void);

/** @brief Tear down COMA. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_deinit(void);

/** @brief Read COMA_STS. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_get_status(uint32_t* out_mask);

/** @brief Clear COMA_STS bits via COMA_ICLR. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_attach_handler(ra_eth_coma_event_fn_t fn, void* ctx);

/** @brief Dispatch a COMA event. @since 0.1.0 */
void ra_eth_coma_dispatch(void);

/** @brief Put COMA into MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_coma_exit_stop(void);

#ifdef __cplusplus
}
#endif
