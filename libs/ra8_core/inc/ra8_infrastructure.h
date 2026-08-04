/**
 * @file ra8_infrastructure.h
 * @brief Bring-up Helpers Called Before Any Driver Init
 * @ingroup grp_core
 *
 * @details
 * `ra8_infrastructure_init()` is the one place every piece of
 * pre-driver state is brought up: the logging backend, the pin
 * validator, and any CPU-core configuration that the rest of the HAL
 * assumes is already in place (I-cache, D-cache, FPU enable, etc.).
 *
 * Call order for a typical main:
 * @code{.c}
 * int main(void)
 * {
 *     // 1. Arch / infra bring-up (no peripheral access).
 *     ra8_infrastructure_init();
 *
 *     // 2. Clock tree.
 *     ra8_err_t err = ra8_cgc_init();
 *     RA8_ERROR_CHECK(err);
 *
 *     // 3. Application peripherals.
 *     //    ...
 * }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"

/**
 * @brief Run every piece of infrastructure init that a driver may assume.
 *
 * @details
 * - Initialises the log backend (`ra8_log_init()`).
 * - Resets the pin validator (`ra8_pin_validator_reset()`).
 * - Enables the FPU (CPACR bits 20..23) if the build is configured
 *   for hard-float.
 * - Enables the I-cache and D-cache for performance.
 * - Enables the branch prediction unit.
 *
 * @pre `SystemInit()` has already executed.
 * @pre Function is called from a single-threaded context.
 * @post Log backend, pin validator, and stack canary are initialized.
 * @post Subsequent `ra8_*_init()` driver entries may be called.
 *
 * @note Must be called before any `ra8_*` driver init function. Safe
 *       to call from `main()` before ThreadX / FreeRTOS / etc. starts
 *       (no scheduling dependency). Not thread-safe.
 *
 * @since 0.1.0
 */
void ra8_infrastructure_init(void);

/**
 * @brief Verify the SRAM stack canary sentinel pattern.
 *
 * @details
 * Walks the linker-defined ``.stack_canary`` region (32 bytes
 * just below the stack top) and confirms every word still holds
 * the seed value written by ``ra8_infrastructure_init``. A mismatch
 * means the main stack has overflowed into the sentinel; callers
 * typically log a fatal error and reboot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Sentinel still intact.
 * @retval k_ra8_err_validation_failed One or more words corrupted.
 *
 * @pre ``ra8_infrastructure_init`` has been called.
 * @post No state is modified.
 *
 * @note Thread safety: safe to call concurrently with everything
 *       except the linker symbol redefinition path used by tests.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_stack_canary_check(void);

#ifdef __cplusplus
}
#endif
