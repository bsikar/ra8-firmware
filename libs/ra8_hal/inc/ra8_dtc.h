/**
 * @file ra8_dtc.h
 * @brief Data Transfer Controller (DTC) driver
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Driver for the RA8D2 DTC block. The DTC is a lighter-weight
 * alternative to the DMAC for moving small amounts of data in response
 * to peripheral interrupts. It shares the MSTPA22 gate with DMAC0 via
 * ra8_mstp's reference counter. The driver owns the DTCCR / DTCVBR /
 * DTCST / DTCSTS surface and exposes a shared activation-callback
 * slot that the ICU dispatcher forwards DTC completion events into.
 *
 * FSP transfer-API mapping (see `r_dtc.c`):
 *  - `R_DTC_Open`        -> @ref ra8_dtc_init (programme DTCVBR + MSTP)
 *  - `R_DTC_Close`       -> @ref ra8_dtc_deinit
 *  - `R_DTC_Enable`      -> @ref ra8_dtc_enable
 *  - `R_DTC_Disable`     -> @ref ra8_dtc_disable
 *  - `R_DTC_Reconfigure` -> @ref ra8_dtc_reconfigure
 *  - `R_DTC_CallbackSet` -> @ref ra8_dtc_attach_handler
 *  - `R_DTC_Reset`, `_InfoGet`: call sites edit the TI table directly.
 *  - `R_DTC_Reload`, `_SoftwareStart`, `_SoftwareStop`: not supported
 *    (matches FSP `FSP_ERR_UNSUPPORTED`).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dtc_regs.h"
#include "ra8_err.h"

/**
 * @typedef ra8_dtc_event_fn_t
 * @brief DTC activation-complete callback.
 */
typedef void (*ra8_dtc_event_fn_t)(void* ctx, uint16_t status);

/**
 * @brief Initialise the DTC and install its vector table base.
 *
 * @param[in] vector_base Pointer to a caller-supplied DTC vector
 * table (16-byte-aligned SRAM region).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_init(void* vector_base);

/**
 * @brief Tear down the DTC (clears run bit + drops MSTP ref).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_deinit(void);

/**
 * @brief Start the DTC module.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_enable(void);

/**
 * @brief Stop the DTC module.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_disable(void);

/**
 * @brief Reconfigure the DTC vector base at runtime.
 *
 * @details
 * Safe only while the DTC is disabled. Rewrites DTCVBR + toggles
 * DTCCR.RRS so that a stale read-skip entry does not outlive the
 * reprogramming.
 *
 * @param[in] vector_base New vector table base (16-byte-aligned).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_reconfigure(void* vector_base);

/**
 * @brief Read the DTCSTS activation status register.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_get_status(uint16_t* out_mask);

/**
 * @brief Clear sticky bits in DTCSTS.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_clear_status(uint16_t mask);

/**
 * @brief Attach a shared activation-complete callback.
 *
 * @details
 * DTC activation events land on per-source IRQs via the ICU; the
 * ICU dispatcher calls ra8_dtc_dispatch() which fans them out to
 * the handler stored here.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_attach_handler(ra8_dtc_event_fn_t fn, void* ctx);

/**
 * @brief Fire the attached activation callback with current DTCSTS.
 *
 * @details
 * Reads ``DTC.DTCSTS`` (HUM Ch 17.2.10 "DTCSTS : DTC Status Register",
 * p 654) which captures the most recent DTC activation source and ACT
 * flag, and invokes the handler registered through
 * ``ra8_dtc_attach_handler()`` with that status word and the stored
 * context pointer. Silently returns when no handler is installed.
 *
 * @pre ``ra8_dtc_init()`` previously succeeded.
 * @pre Called from ISR context or unit-test driver.
 * @post Registered handler invoked at most once with current ``DTCSTS``.
 * @post No DTC register state mutated by the dispatch itself.
 *
 * @note Thread safety: ISR context only; not re-entrant.
 * @since 0.1.0
 */
void ra8_dtc_dispatch(void);

/**
 * @brief Put the DTC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop and re-arm the engine.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_exit_stop(void);

#ifdef __cplusplus
}
#endif
