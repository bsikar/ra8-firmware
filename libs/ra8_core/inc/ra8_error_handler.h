/**
 * @file ra8_error_handler.h
 * @brief Centralised fatal-error sink
 * @ingroup grp_core
 *
 * @details
 * `ra8_fatal_error()` is the single entry point for "stop
 * running and alert the developer". Every `RA8_ASSERT` and every
 * `RA8_ERROR_CHECK` that fails eventually lands here.
 *
 * The default behaviour is:
 *  1. Mask all interrupts.
 *  2. Emit a log line on the default backend.
 *  3. If a debugger is attached, `__BKPT(0)`.
 *  4. Loop in `__WFI` forever.
 *
 * The loop is deliberate: we do not want to reset silently because
 * that erases the state the developer needs to diagnose the fault.
 * Field deployments can override `ra8_fatal_error()` with a
 * weak symbol to add a watchdog reset.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Report a fatal error, log it, and halt.
 *
 * @param[in] tag     Short tag describing the source (`"ASSERT"`,
 *                    `"ERROR_CHECK"`, `"HARDFAULT"`, ...).
 * @param[in] message String literal describing the failure.
 * @param[in] err     Error code classifying the failure (may be 0 for
 *                    plain assertion failures).
 *
 * @note Never returns.
 * @warning Do not call from time-critical ISR paths unless you accept
 *          that the system will not come back.
 */
[[noreturn]] void ra8_fatal_error(const char* tag, const char* message, uint32_t err);

#ifdef __cplusplus
}
#endif
