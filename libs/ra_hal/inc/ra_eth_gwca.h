/**
 * @file ra_eth_gwca.h
 * @brief Ethernet CPU Agent (GWCA) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 6 driver for the RA8D2 GWCA block. GWCA is the bridge
 * between MFWD/COMA and CPU memory; it owns the per-channel
 * descriptor rings the host uses for TX/RX staging. This driver
 * covers lifecycle + status + IRQ + power transition; the
 * descriptor-ring programming surface lands with the first
 * NIC consumer.
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
 * @typedef ra_eth_gwca_event_fn_t
 * @brief GWCA event callback.
 */
typedef void (*ra_eth_gwca_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise GWCA. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_init(void);

/** @brief Tear down GWCA. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_deinit(void);

/** @brief Read GWCA_STS. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_get_status(uint32_t* out_mask);

/** @brief Clear GWCA_STS bits via GWCA_ICLR. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_attach_handler(ra_eth_gwca_event_fn_t fn, void* ctx);

/** @brief Dispatch a GWCA event. @since 0.3.0 */
void ra_eth_gwca_dispatch(void);

/** @brief Put GWCA into MSTP-gated stop. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.3.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_exit_stop(void);

#ifdef __cplusplus
}
#endif
