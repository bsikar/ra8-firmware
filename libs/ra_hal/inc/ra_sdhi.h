/**
 * @file ra_sdhi.h
 * @brief SD/MMC Host Interface (SDHI) driver scaffold
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 5.4 introduces a minimal SDHI driver scaffold covering the
 * lifecycle + status + IRQ + power-transition surface. Block-level
 * SD card command engine, DMA transfers, and 4-bit / 8-bit wide-bus
 * switching land with the first consumer.
 *
 * API surface:
 *
 *  - ``ra_sdhi_init(instance)``        -- MSTP enable + clear IRQ masks
 *  - ``ra_sdhi_deinit(instance)``      -- disable + MSTP release
 *  - ``ra_sdhi_get_status``             -- SD_INFO1/INFO2 mask
 *  - ``ra_sdhi_clear_status``           -- clear SD_INFO1/INFO2 bits
 *  - ``ra_sdhi_attach_handler``         -- install IRQ callback
 *  - ``ra_sdhi_enter_stop / exit_stop`` -- power transition
 *  - ``ra_sdhi_dispatch``               -- ISR entry point
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
 * @typedef ra_sdhi_event_fn_t
 * @brief SDHI event callback.
 */
typedef void (*ra_sdhi_event_fn_t)(void* ctx, uint8_t instance, uint64_t status_mask);

/**
 * @brief Initialise an SDHI instance.
 * @param[in] instance SDHI instance (0 or 1).
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_init(uint8_t instance);

/**
 * @brief Tear down an SDHI instance.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_deinit(uint8_t instance);

/**
 * @brief Read the SD_INFO1 status register.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_get_status(uint8_t instance, uint64_t* out_mask);

/**
 * @brief Clear SD_INFO1 status bits via write-0-to-clear.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_clear_status(uint8_t instance, uint64_t mask);

/**
 * @brief Attach an SDHI event callback (shared across instances).
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_attach_handler(ra_sdhi_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an SDHI event -- snapshot status + fire callback.
 * @since 0.2.0
 */
void ra_sdhi_dispatch(uint8_t instance);

/**
 * @brief Put an SDHI instance into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_enter_stop(uint8_t instance);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_sdhi_exit_stop(uint8_t instance);

#ifdef __cplusplus
}
#endif
