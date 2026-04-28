/**
 * @file ra_iwdt.h
 * @brief Independent Watchdog driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * extends the refresh stub with: status reporting,
 * NMI underflow attach / dispatch. Period + window + reset-vs-NMI
 * choice still come from the OFS0 option-setting register at flash-
 * write time; the IWDT cannot be reconfigured at runtime.
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
 * @enum ra_iwdt_status_mask_t
 * @brief IWDTSR status bits.
 */
typedef enum : uint16_t {
  k_ra_iwdt_status_none      = 0x0000U,
  k_ra_iwdt_status_underflow = 0x4000U, /**< IWDTSR.UNDFF bit14. */
  k_ra_iwdt_status_refresh   = 0x8000U, /**< IWDTSR.REFEF bit15. */
} ra_iwdt_status_mask_t;

/**
 * @typedef ra_iwdt_event_fn_t
 * @brief IWDT NMI underflow event callback.
 * @param[in] ctx Caller context.
 * @param[in] status_mask Latched IWDTSR bits.
 */
typedef void (*ra_iwdt_event_fn_t)(void* ctx, uint16_t status_mask);

/**
 * @brief Initialise the IWDT driver layer.
 * @return `k_ra_ok` -- stub; OFS0 controls the actual period.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iwdt_init(void);

/**
 * @brief Refresh the IWDT counter via a wrapper function.
 */
void ra_iwdt_refresh_deferred(void);

/**
 * @brief Read the IWDTSR status bits.
 * @param[out] out_mask Receives OR of ``k_ra_iwdt_status_*``.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iwdt_get_status(uint16_t* out_mask);

/**
 * @brief Clear the underflow / refresh-error flags via IWDTSR write.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iwdt_clear_status(void);

/**
 * @brief Attach a handler for the IWDT NMI event.
 * @param[in] fn Callback fired on dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_iwdt_attach_handler(ra_iwdt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an IWDT NMI event -- snapshot status + fire callback.
 * @since 0.2.0
 */
void ra_iwdt_dispatch(void);

#ifdef __cplusplus
}
#endif
