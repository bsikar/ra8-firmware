/**
 * @file ra8_eth_mfwd.h
 * @brief Ethernet Message Forwarding Engine (MFWD) driver
 * @ingroup grp_hal_net
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

#include "ra8_err.h"

/**
 * @typedef ra8_eth_mfwd_event_fn_t
 * @brief MFWD event callback.
 */
typedef void (*ra8_eth_mfwd_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise the MFWD block (MSTP enable + reset regs). @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_init(void);

/** @brief Tear down the MFWD block. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_deinit(void);

/** @brief Read the MFWD_STS status register. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_get_status(uint32_t* out_mask);

/** @brief Clear bits in MFWD_STS via MFWD_ICLR. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_attach_handler(ra8_eth_mfwd_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an MFWD event. @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_eth_mfwd_dispatch(void);

/** @brief Put MFWD into MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_exit_stop(void);

/**
 * @brief Program the per-port forwarding-destination masks (FWPBFC0).
 *
 * @details
 * Sets ``MFWD.FWPBFC0[i].PBDV = mask`` for each port i in 0..2 (the
 * two GMAC ports + the host/GWCA port). PBDV is a 7-bit bitmask of
 * destination ports a frame received on this port is allowed to reach.
 * Per FSP r_layer3_switch_open this must be programmed BEFORE the
 * GWCA mode transitions (alongside FWPC10/11/12.DDE).
 *
 * The simplest permissive setting is ``0x7F`` (all destinations
 * allowed) on every port -- that lets wire-to-host RX flow through
 * the switch without any L3 filtering.
 *
 * @param[in] port_masks Array of three masks: index 0 = port 0,
 *                       index 1 = port 1, index 2 = host port.
 *                       Caller-owned; only the values are read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Masks programmed.
 * @retval k_ra8_err_null_ptr port_masks is null.
 *
 * @pre ra8_eth_mfwd_init or board MSTP setup has run.
 * @pre Each mask is 7 bits or fewer (extra bits are ignored).
 * @post FWPBFC0/1/2.PBDV reflect the supplied masks.
 * @post No other bits of FWPBFC0/1/2 are disturbed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_set_forwarding_masks(const uint8_t port_masks[3]);

/**
 * @brief Route inbound frames from a GMAC port into a GWCA RX queue.
 *
 * @details
 * Programs ``MFWD.FWPBFCSDC0[port].PBCSD = queue_index`` so the
 * forwarding engine knows where to deliver port-to-host frames.
 * Without this call the GWCA RX queue stays empty even when frames
 * arrive on the wire. Per FSP ``r_layer3_switch_StartDescriptorQueue``
 * this MUST be programmed after the GWCA queue is in OPERATION and
 * before the MAC is allowed to receive.
 *
 * @param[in] port         GMAC port index (0..1 on RA8D2).
 * @param[in] queue_index  GWCA queue index that receives the frames (<= 31).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Routing programmed.
 * @retval k_ra8_err_invalid_arg port > 1 or queue_index > 31.
 *
 * @pre ra8_eth_mfwd_init succeeded earlier (or ra8_eth_init brought
 *      up the shared ESWM gate).
 * @pre GWCA queue indicated by queue_index has been configured.
 * @post FWPBFCSDC0[port].PBCSD == queue_index on success.
 * @post Other bits of FWPBFCSDC0[port] preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_mfwd_route_queue(uint8_t port, uint8_t queue_index);

#ifdef __cplusplus
}
#endif
