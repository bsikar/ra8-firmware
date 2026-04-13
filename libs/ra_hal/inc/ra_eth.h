/**
 * @file ra_eth.h
 * @brief Ethernet Switch Module (ESWM) unified scaffold driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 6.3 introduces a unified minimal scaffold for the RA8D2
 * ethernet subsystem. This driver owns the ESWM (Ethernet Switch
 * Module) MSTP reference and exposes the lifecycle + status +
 * IRQ + power-transition surface shared by all ethernet blocks.
 *
 * The full Wave 6 plan calls for five separate drivers:
 *
 *  - ra_eth_swm   -- Layer 3 Ethernet Switch Module (THIS FILE)
 *  - ra_eth_mfwd  -- Message Forwarding Engine
 *  - ra_eth_coma  -- Common Agent
 *  - ra_eth_gwca  -- CPU Agent
 *  - ra_eth_gptp  -- Generic PTP Timer
 *
 * mfwd / coma / gwca / gptp are deferred to Wave 7 (network stack
 * integration) since they only become useful once a consumer like
 * lwIP or the Ethernet PAL is in place. Only the ESWM block is
 * scaffolded in Wave 6.3 to keep the gate MSTP reference counted
 * and allow early "is the ethernet module even alive" sanity
 * checks in board bring-up.
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
 * @typedef ra_eth_event_fn_t
 * @brief Ethernet switch event callback.
 */
typedef void (*ra_eth_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Initialise the ESWM block (MSTP enable + reset regs).
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_init(void);

/**
 * @brief Tear down the ESWM block.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_deinit(void);

/**
 * @brief Read the ESWM_STS status register.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_get_status(uint32_t* out_mask);

/**
 * @brief Clear bits in ESWM_STS via ESWM_ICLR.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_clear_status(uint32_t mask);

/**
 * @brief Attach a shared event callback for ethernet events.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_attach_handler(ra_eth_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an ESWM event -- snapshot + fire callback.
 * @since 0.2.0
 */
void ra_eth_dispatch(void);

/**
 * @brief Put the ethernet switch into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_eth_exit_stop(void);

#ifdef __cplusplus
}
#endif
