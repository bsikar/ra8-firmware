/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_eth_coma.h
 * @brief Ethernet Common Agent (COMA) driver
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 COMA block. COMA is the management
 * agent that owns bus arbitration counters + shared per-port
 * descriptor fences. This driver covers lifecycle + status + IRQ
 * + power transition.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @typedef ra8_eth_coma_event_fn_t
 * @brief COMA event callback.
 */
typedef void (*ra8_eth_coma_event_fn_t)(void* ctx, uint32_t status_mask);

/** @brief Initialise COMA. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_init(void);

/** @brief Tear down COMA. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_deinit(void);

/**
 * @brief Bring the COMA switch out of reset, init the buffer pool, and fan
 *        every per-agent clock out.
 *
 * @details
 * The chip-generic COMA bring-up sequence every RA8 Ethernet board runs
 * once, after the ESWM module-stop gate is released and the peripheral
 * domain is powered, before any per-port RMAC / ETHA register window can
 * be read or written. Until COMA does this those windows read back 0 and
 * writes are silently dropped, so this is a hard prerequisite for
 * ::ra8_eth_open and for ::ra8_eth_rgmii_select. It is a faithful in-tree
 * re-implementation of the FSP ``r_layer3_switch_reset_coma`` flow, and it
 * previously lived open-coded in the EK-RA8D2 board Ethernet bring-up --
 * it is chip-generic, so it belongs here in the HAL, not in board glue.
 *
 * Sequence (HUM Ch 31.4 software flows):
 *   1. Pulse COMA.RRC.RR (1 then 0) to reset the ESWM IP, then settle.
 *   2. Set COMA.RCEC.RCE alone to enable the switch clock, then settle.
 *   3. Write COMA.CABPIRM.BPIOG = 1 and poll CABPIRM.BPR until the shared
 *      buffer pool reports ready (bounded wait).
 *   4. Set COMA.RCEC = RCE | ACE[6:0] to fan every per-agent clock out so
 *      RMAC0/1 + ETHA0/1 + GWCA + MFWD + GPTP become accessible.
 *
 * Without step 3 the MFAB pointer pool stays non-operational: the RMAC
 * cannot obtain a buffer for an inbound frame, so every RX frame overflows.
 *
 * @return ::ra8_err_t Result code.
 * @retval k_ra8_ok             COMA out of reset, buffer pool ready, clocks fanned out.
 * @retval k_ra8_err_hw_timeout CABPIRM.BPR never asserted within budget.
 *
 * @pre The ESWM module-stop gate (::k_ra8_mstp_eswm) has been released.
 * @pre The Ethernet peripheral power domain is on (PDCTRESWM.PDDE = 0).
 * @post On success COMA.RCEC.RCE = 1, ACE[6:0] = all-ones, CABPIRM.BPR = 1.
 * @post On success the per-port RMAC / ETHA register windows are accessible.
 *
 * @note Not thread-safe; call from a single-threaded init context.
 * @note eth is HW-blocked on silicon (issue #21); this path is host-tested
 *       and sim-modeled, not hardware-validated.
 *
 * @see ra8_eth_rgmii_select
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_coma_bringup(void);

/** @brief Read COMA_STS. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_get_status(uint32_t* out_mask);

/** @brief Clear COMA_STS bits via COMA_ICLR. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_clear_status(uint32_t mask);

/** @brief Attach the shared event handler. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_attach_handler(ra8_eth_coma_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a COMA event. @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_eth_coma_dispatch(void);

/** @brief Put COMA into MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_enter_stop(void);

/** @brief Exit MSTP-gated stop. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_eth_coma_exit_stop(void);

#ifdef __cplusplus
}
#endif
