/**
 * @file ra_eth_mfwd.h
 * @brief Ethernet Message Forwarding Engine (MFWD) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 MFWD block. The MFWD sits between
 * the GMAC ports and the CPU Agent (GWCA), making per-frame
 * forwarding decisions (port-to-port, port-to-host, multicast).
 * This driver covers lifecycle + status + IRQ + power transition;
 * the per-frame forwarding-table programming surface lands with
 * the first routing-aware consumer.
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
 * @typedef ra_eth_mfwd_event_fn_t
 * @brief MFWD event callback.
 */
typedef void (*ra_eth_mfwd_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise the MFWD block (MSTP enable + reset regs). @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_init(void);

/** @brief Tear down the MFWD block. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_deinit(void);

/** @brief Read the MFWD_STS status register. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_get_status(uint32_t* out_mask);

/** @brief Clear bits in MFWD_STS via MFWD_ICLR. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_attach_handler(ra_eth_mfwd_event_fn_t fn, void* ctx);

/** @brief Dispatch an MFWD event. @since 0.3.0 */
void ra_eth_mfwd_dispatch(void);

/** @brief Put MFWD into MSTP-gated stop. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_mfwd_exit_stop(void);

#ifdef __cplusplus
}
#endif
