/**
 * @file ra_pdm.h
 * @brief Pulse Density Modulation Interface (PDM-IF) driver scaffold
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * introduces a minimal PDM-IF driver scaffold covering
 * the lifecycle + status + IRQ + power-transition surface. Full
 * PCM decimation / FIR filter / stereo capture land with the
 * first audio consumer.
 *
 * API surface:
 *
 * - ``ra_pdm_init`` -- MSTP enable + clear regs
 * - ``ra_pdm_deinit`` -- disable + MSTP release
 * - ``ra_pdm_get_status`` -- PDM_STAT mask
 * - ``ra_pdm_clear_status`` -- clear PDM_STAT bits
 * - ``ra_pdm_attach_handler`` -- install IRQ callback
 * - ``ra_pdm_enter_stop / exit_stop`` -- power transition
 * - ``ra_pdm_dispatch`` -- ISR entry point
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
 * @typedef ra_pdm_event_fn_t
 * @brief PDM event callback.
 */
typedef void (*ra_pdm_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Initialise the PDM-IF peripheral.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_init(void);

/**
 * @brief Tear down the PDM-IF peripheral.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_deinit(void);

/**
 * @brief Read PDM_STAT status mask.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_get_status(uint32_t* out_mask);

/**
 * @brief Clear PDM_STAT status bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_clear_status(uint32_t mask);

/**
 * @brief Attach PDM event callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_attach_handler(ra_pdm_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a PDM event -- snapshot + fire callback.
 * @since 0.1.0
 */
void ra_pdm_dispatch(void);

/**
 * @brief Put PDM-IF into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_pdm_exit_stop(void);

#ifdef __cplusplus
}
#endif
