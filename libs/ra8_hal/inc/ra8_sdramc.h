/**
 * @file ra8_sdramc.h
 * @brief External SDRAM controller driver (framework)
 *
 * @details Declares the fixed-sequence external SDRAM controller initialization and readiness interface.
 * @ingroup grp_hal_memory
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
 * @brief Initialise the external SDRAM at `k_ra8_sdram_base_addr`.
 *
 * @details
 * Runs the documented SDRAM bring-up sequence (precharge all ->
 * mode register write -> refresh enable). Uses EK-RA8D2 board
 * defaults; change the refresh interval for a different panel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdramc_init(void);

/**
 * @brief Tear down the SDRAM controller (disable refresh, clear SDICR).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdramc_deinit(void);

/**
 * @brief Change the SDRAM auto-refresh interval at runtime.
 * @param[in] sdrfcr New SDRFCR value.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdramc_set_refresh_interval(uint16_t sdrfcr);

/**
 * @brief Read the refresh-enable bit as a status mask.
 * @param[out] out_enabled Non-zero when SDRFEN is set.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdramc_get_status(uint8_t* out_enabled);

/**
 * @brief Disable auto-refresh for deep-sleep entry.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdramc_enter_stop(void);

/**
 * @brief Re-enable auto-refresh after wake-up.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdramc_exit_stop(void);

/**
 * @brief Put the external SDRAM into JEDEC self-refresh mode.
 *
 * @details
 * Self-refresh is the low-power retention state in which the SDRAM
 * device drives its own internal refresh cycles from its own oscillator
 * while the external bus is idle. This is the required state before the
 * MCU enters Software Standby so the SDRAM contents (working set at
 * 0x68000000) survive sleep.
 *
 * Entry sequence (HUM Ch 15.3.14, Table 15.8):
 * 1. Confirm all SDSR status bits are clear.
 * 2. Disable auto-refresh (SDRFEN = 0).
 * 3. Assert SDSELF.SFEN = 1; the hardware issues a final auto-refresh
 *    then switches the device to self-refresh mode.
 * 4. Poll SDSR.SRFST until it clears, confirming the SDRAM has
 *    accepted the self-refresh command. The poll is bounded by
 *    `k_ra8_sdramc_spin_max` iterations (NASA P10 Rule 2).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               SDRAM is now in self-refresh.
 * @retval k_ra8_err_invalid_state SDSR status bits were non-zero on
 *                                entry, or SDSELF.SFEN was already 1.
 * @retval k_ra8_err_hw_timeout   SDSR.SRFST did not clear within
 *                                `k_ra8_sdramc_spin_max` polls.
 *
 * @pre SDSR.SRFST, INIST, and MRSST are all 0 (no operation in progress).
 * @pre SDSELF.SFEN is 0 (not already in self-refresh).
 * @post On k_ra8_ok, SDSELF.SFEN is 1 (self-refresh active).
 * @post On k_ra8_ok, SDSR.SRFST is 0 (transition complete).
 *
 * @note Not thread-safe. Must be called from a context that guarantees
 *       exclusive access to the SDRAMC register block.
 * @note SDRAM contents remain valid throughout self-refresh. No DMA or
 *       CPU access to the SDRAM window (0x68000000) may occur while in
 *       self-refresh.
 *
 * @see ra8_sdramc_exit_self_refresh() Paired exit function.
 * @see ra8_sdramc_enter_stop() Deeper stop (loses refresh entirely).
 *
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control Register" p 606.
 * Ch 15.3.22 "SDSR : SDRAM Status Register" p 613.
 * Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608.
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: SDSR poll loop bounded by `k_ra8_sdramc_spin_max`.
 * - Rule 5: 2 precondition checks (SDSR idle, SFEN clear).
 */
[[nodiscard]] ra8_err_t ra8_sdramc_enter_self_refresh(void);

/**
 * @brief Exit SDRAM self-refresh and resume normal auto-refresh operation.
 *
 * @details
 * Clears SDSELF.SFEN to end self-refresh. The hardware performs
 * `SDRFCR.REFW` self-refresh clearing cycles before returning the
 * device to normal auto-refresh mode. Auto-refresh is then explicitly
 * re-enabled via SDRFEN.
 *
 * Exit sequence (HUM Ch 15.3.14, Table 15.8):
 * 1. Confirm SDSELF.SFEN is 1 (currently in self-refresh).
 * 2. Confirm SDSR.SRFST is 0 (no transition in progress).
 * 3. Clear SDSELF.SFEN = 0; the hardware runs the clearing cycles.
 * 4. Poll SDSR.SRFST until it clears (recovery complete).
 * 5. Re-enable auto-refresh (SDRFEN = 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               SDRAM has returned to auto-refresh.
 * @retval k_ra8_err_invalid_state SDSELF.SFEN was 0 (not in self-refresh)
 *                                or SDSR.SRFST was set on entry.
 * @retval k_ra8_err_hw_timeout   SDSR.SRFST did not clear within
 *                                `k_ra8_sdramc_spin_max` polls.
 *
 * @pre SDSELF.SFEN is 1 (currently in self-refresh).
 * @pre SDSR.SRFST is 0 (self-refresh entry transition is complete).
 * @post On k_ra8_ok, SDSELF.SFEN is 0 (self-refresh inactive).
 * @post On k_ra8_ok, SDRFEN.RFEN is 1 (auto-refresh re-enabled).
 *
 * @note Not thread-safe. Must be called before resuming any DMA or
 *       CPU access to the SDRAM window (0x68000000).
 * @note Must be called after the MCU returns from Software Standby
 *       before any access to 0x68000000 is safe.
 *
 * @see ra8_sdramc_enter_self_refresh() Paired entry function.
 *
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 15.3.14 "SDSELF : SDRAM Self-Refresh Control Register" p 606.
 * Ch 15.3.22 "SDSR : SDRAM Status Register" p 613.
 * Ch 15.3.16 "SDRFEN : SDRAM Auto-Refresh Control Register" p 608.
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: SDSR poll loop bounded by `k_ra8_sdramc_spin_max`.
 * - Rule 5: 2 precondition checks (SFEN set, SRFST clear).
 */
[[nodiscard]] ra8_err_t ra8_sdramc_exit_self_refresh(void);

#ifdef __cplusplus
}
#endif
