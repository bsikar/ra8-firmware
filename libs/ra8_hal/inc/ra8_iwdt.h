/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_iwdt.h
 * @brief Independent Watchdog driver header
 * @ingroup grp_hal_timers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Extends the refresh stub with: status reporting, live counter read,
 * and NMI underflow attach / dispatch. Period + window + reset-vs-NMI
 * choice still come from the OFS0 option-setting register at flash-
 * write time; the IWDT cannot be reconfigured at runtime on the RA8D2
 * (auto-start only -- the FSP ``IWDT_PRV_REGISTER_START_MODE`` path
 * is not supported on this part).
 *
 * The driver mirrors the runtime surface of the FSP ``r_iwdt`` driver:
 *  - ``ra8_iwdt_refresh_deferred`` <-> FSP ``R_IWDT_Refresh``
 *  - ``ra8_iwdt_get_status``       <-> FSP ``R_IWDT_StatusGet``
 *  - ``ra8_iwdt_clear_status``     <-> FSP ``R_IWDT_StatusClear``
 *  - ``ra8_iwdt_get_counter``      <-> FSP ``R_IWDT_CounterGet``
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_iwdt_status_mask_t
 * @brief IWDTSR status bits exposed to callers.
 *
 * @details
 * Mirrors HUM Ch 28.2.2 "IWDTSR" bit layout (UNDFF bit 14, REFEF bit
 * 15) and FSP ``IWDT_PRV_STATUS_START_BIT`` semantics. The CNTVAL
 * counter occupies the lower 14 bits and is exposed through
 * ``ra8_iwdt_get_counter`` instead.
 */
typedef enum : uint16_t {
  k_ra8_iwdt_status_none      = 0x0000U, /**< RA8 iwdt status none. */
  k_ra8_iwdt_status_underflow = 0x4000U, /**< IWDTSR.UNDFF bit 14.  */
  k_ra8_iwdt_status_refresh   = 0x8000U, /**< IWDTSR.REFEF bit 15.  */
} ra8_iwdt_status_mask_t;

/**
 * @typedef ra8_iwdt_event_fn_t
 * @brief IWDT NMI underflow event callback.
 * @param[in] ctx Caller context.
 * @param[in] status_mask Latched IWDTSR bits (UNDFF / REFEF).
 */
typedef void (*ra8_iwdt_event_fn_t)(void* ctx, uint16_t status_mask);

/**
 * @brief Initialise the IWDT driver layer.
 * @return ``k_ra8_ok`` -- stub; OFS0 controls the actual period.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_iwdt_init(void);

/**
 * @brief Refresh the IWDT counter via a wrapper function.
 *
 * @details
 * Equivalent to FSP ``R_IWDT_Refresh``: writes the mandatory
 * ``0x00`` then ``0xFF`` sequence to IWDTRR. A single write does NOT
 * refresh -- both bytes are required.
 *
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
void ra8_iwdt_refresh_deferred(void);

/**
 * @brief Read the IWDTSR status bits (UNDFF / REFEF only).
 * @param[out] out_mask Receives OR of ``k_ra8_iwdt_status_*``.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_iwdt_get_status(uint16_t* out_mask);

/**
 * @brief Clear the underflow / refresh-error flags via IWDTSR write.
 *
 * @details
 * Mirrors FSP ``R_IWDT_StatusClear`` -- writes a 0 to the relevant
 * bit and re-reads until the bit clears (the silicon takes one
 * IWDTCLK cycle to drop the flag).
 *
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_iwdt_clear_status(void);

/**
 * @brief Read the live 14-bit IWDT down-counter (CNTVAL).
 *
 * @details
 * Mirrors FSP ``R_IWDT_CounterGet`` -- reads ``IWDTSR & 0x3FFF``.
 *
 * @param[out] out_counter Receives the 14-bit counter value (0..0x3FFF).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok ``*out_counter`` populated.
 * @retval k_ra8_err_null_ptr ``out_counter == NULL``.
 *
 * @pre ``out_counter != NULL``.
 * @post ``*out_counter <= 0x3FFF``.
 *
 * @note Thread-safe (single 16-bit read).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_iwdt_get_counter(uint16_t* out_counter);

/**
 * @brief Attach a handler for the IWDT NMI event.
 * @param[in] fn Callback fired on dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_iwdt_attach_handler(ra8_iwdt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an IWDT NMI event -- snapshot status + fire callback.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_iwdt_dispatch(void);

#ifdef __cplusplus
}
#endif
