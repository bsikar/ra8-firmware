/**
 * @file ra_eth_gwca.h
 * @brief Ethernet CPU Agent (GWCA) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 GWCA block. GWCA is the bridge
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

#include "ra8d2_ether_regs.h"
#include "ra_err.h"

/**
 * @typedef ra_eth_gwca_event_fn_t
 * @brief GWCA event callback.
 */
typedef void (*ra_eth_gwca_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise GWCA. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_init(void);

/** @brief Tear down GWCA. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_deinit(void);

/** @brief Read GWCA_STS. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_get_status(uint32_t* out_mask);

/** @brief Clear GWCA_STS bits via GWCA_ICLR. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_attach_handler(ra_eth_gwca_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a GWCA event. @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra_eth_gwca_dispatch(void);

/** @brief Put GWCA into MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_eth_gwca_exit_stop(void);

/**
 * @brief Transition the GWCA / ESWM state machine to a new OPC mode.
 *
 * @details Writes GWMC.OPC[1:0] = @p mode and polls GWMS.OPS[1:0]
 * until it reflects the new mode (or the bounded poll budget elapses).
 * This is the canonical state-machine transition for GWCA, used by
 * the LINKFIX init sequence (RESET -> DISABLE -> CONFIG -> ... ->
 * OPERATION). Mirrors FSP `r_layer3_switch_update_gwca_operation_mode`.
 *
 * @param[in] mode Target operation mode from ::ra_gwmc_opc_t.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok             GWMS.OPS now reflects @p mode.
 * @retval k_ra_err_invalid_arg @p mode is out of range.
 * @retval k_ra_err_hw_timeout GWMS.OPS never converged.
 *
 * @pre ::ra_eth_gwca_init has been called (MSTP-gate cleared).
 * @pre Caller is single-threaded with respect to GWCA edits.
 * @post On success GWMC.OPC and GWMS.OPS both equal @p mode.
 * @post On timeout GWMC.OPC may have been written even if OPS
 *       never converged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_set_operation_mode(ra_gwmc_opc_t mode);

/**
 * @brief Request the GWCA AXI bridge to initialize via GWARIRM.ARIOG.
 *
 * @details Asserts GWARIRM.ARIOG (bit 0) and polls GWARIRM.ARR
 * (bit 1) until it reads 1. Called once from the LINKFIX init flow
 * after entering CONFIG mode. Mirrors the FSP
 * `r_layer3_switch_initialize_gwca` AXI-init step.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              ARR asserted within the budget.
 * @retval k_ra_err_hw_timeout ARR never asserted.
 *
 * @pre ::ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config) returned ok.
 * @post On success ARR=1 indicates the AXI manager is ready.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_eth_gwca_axi_init(void);

#ifdef __cplusplus
}
#endif
