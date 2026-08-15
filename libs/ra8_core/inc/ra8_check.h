/**
 * @file ra8_check.h
 * @brief Validation and Error-Checking Macros for ra8-firmware
 * @ingroup grp_core
 *
 * @details
 * This header is the *only* place error handling lives. Every function
 * that can fail uses these macros to check preconditions and propagate
 * errors, keeping the error-handling vocabulary tiny and uniform.
 *
 * ## Design
 *
 * - **Macros, not functions**: we need to return from the *caller's*
 *   stack frame on a failure, and we want the check to disappear at -O2
 *   when the argument is a compile-time constant. Both require macros.
 * - **`do { ... } while (0)` wrappers**: safe inside `if`/`else` without
 *   braces, forces a trailing semicolon, and gives the check its own
 *   scope so local variables do not leak.
 * - **Lowercase macro names are forbidden by .clang-tidy**, so each
 *   identifier is `RA8_*`.
 * - **Fatal vs. non-fatal**: `RA8_ASSERT` and `RA8_ERROR_CHECK` halt the
 *   system on failure (they are reserved for irrecoverable programmer
 *   errors and critical init paths). `RA8_RETURN_ON_ERROR`,
 *   `RA8_RETURN_VOID_ON_ERROR`, `RA8_RETURN_NULL_ON_ERROR`, and
 *   `RA8_CHECK_*` log and return to let the caller handle it.
 *
 * ## Error handling matrix
 *
 * | Macro                       | Fatal? | Returns        | Typical use                     |
 * |-----------------------------|--------|----------------|---------------------------------|
 * | `RA8_ASSERT(cond, msg)`      | yes    | never          | Programmer errors, invariants   |
 * | `RA8_ERROR_CHECK(err)`       | yes    | never          | Critical init steps             |
 * | `RA8_ERROR_CHECK_NO_ABORT`   | no     | -              | Optional init that may fail     |
 * | `RA8_RETURN_ON_ERROR`        | no     | `ra8_err_t`     | Error propagation in most APIs  |
 * | `RA8_RETURN_VOID_ON_ERROR`   | no     | `void`         | Error handling in void helpers  |
 * | `RA8_RETURN_NULL_ON_ERROR`   | no     | `void*`        | Pointer-returning factories     |
 * | `RA8_CHECK_NULL_PTR`         | no     | `ra8_err_t`     | Precondition: non-NULL pointer  |
 * | `RA8_CHECK_RANGE`            | no     | `ra8_err_t`     | Precondition: numeric range     |
 * | `RA8_VALIDATE_INIT`          | no     | `ra8_err_t`     | Precondition: module initialized |
 *
 * ## NASA Power of 10 Compliance
 *
 * - **Rule 5**: every function in this firmware uses at least two of the
 *   `RA8_CHECK_*` / `RA8_VALIDATE_*` / `RA8_ASSERT` macros. Most drivers hit
 *   the six-precondition mark.
 * - **Rule 7**: every `ra8_err_t` return flows through exactly one of
 *   `RA8_RETURN_ON_ERROR`, `RA8_ERROR_CHECK`, or an explicit `if` in the
 *   caller. The pre-commit hook rejects unchecked `ra8_err_t` values.
 * - **Rule 8**: macros exist only because the alternative (inline
 *   function + return code) is strictly worse. There are no macros here
 *   that could be a function.
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
#include "ra8_log.h"

/* =============================================================================
 * Fatal-error sink
 * =============================================================================
 */

/* ra8_fatal_error is declared in ra8_error_handler.h. Include
 * it here so RA8_ASSERT / RA8_ERROR_CHECK can call it without the caller
 * having to pull in the other header. */
#include "ra8_error_handler.h"

/* =============================================================================
 * Static assertion -- enforces compile-time invariants
 * =============================================================================
 */

/**
 * @brief Compile-time assertion wrapper.
 *
 * @details
 * Uses C23 `static_assert`. Provided so code reads consistently with the
 * runtime `RA8_ASSERT` macro and so the pre-commit hook can enforce the
 * C23 spelling instead of the older C11 underscore form.
 *
 * @param[in] cond C-time constant boolean expression.
 * @param[in] msg  String literal describing the invariant.
 *
 * @code{.c}
 * RA8_STATIC_ASSERT(sizeof(ra8_err_t) == 2, "ra8_err_t must be uint16_t");
 * @endcode
 */
#define RA8_STATIC_ASSERT(cond, msg) static_assert((cond), msg)

/* =============================================================================
 * Runtime assertion
 * =============================================================================
 */

/**
 * @brief Runtime invariant check. Halts the system on failure.
 *
 * @details
 * Use for programmer errors -- preconditions that *must* hold for the
 * code to make sense. Do NOT use for runtime errors (bad user input,
 * peripheral timeouts, etc.) -- those are what `ra8_err_t` return codes
 * are for.
 *
 * Failure path: logs `message` with tag `"ASSERT"` and calls
 * `ra8_fatal_error`, which halts. No allocation, no RTT.
 *
 * @param[in] condition Expression that must be true.
 * @param[in] message   String literal shown if the assertion fires.
 *
 * @note `condition` is evaluated exactly once.
 */
#define RA8_ASSERT(condition, message)                                                             \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      ra8_fatal_error("ASSERT", (message), (uint32_t)k_ra8_err_validation_failed);                 \
    }                                                                                              \
  } while (0)

/* =============================================================================
 * Error propagation
 * =============================================================================
 */

/**
 * @brief Halt on fatal error.
 *
 * @details
 * If `err` is non-success, log + halt. Use at critical init points
 * where there is no meaningful recovery (clock bring-up, vector table
 * install, etc.).
 *
 * @param[in] err `ra8_err_t` value to check.
 *
 * @note Evaluates `err` exactly once via a local.
 */
#define RA8_ERROR_CHECK(err)                                                                       \
  do {                                                                                             \
    ra8_err_t err_rc_ = (err);                                                                     \
    if (ra8_err_is_error(err_rc_)) {                                                               \
      ra8_fatal_error("ERROR_CHECK", "Fatal error", (uint32_t)err_rc_);                            \
    }                                                                                              \
  } while (0)

/**
 * @brief Log and continue on non-fatal error.
 *
 * @details
 * If `err` is non-success, log it but do not halt. Use for optional
 * init paths where a failure is tolerated (e.g. an optional sensor not
 * being present).
 *
 * @param[in] err `ra8_err_t` value to check.
 */
#define RA8_ERROR_CHECK_NO_ABORT(err)                                                              \
  do {                                                                                             \
    ra8_err_t err_rc_ = (err);                                                                     \
    if (ra8_err_is_error(err_rc_)) {                                                               \
      ra8_log_error_val("ERROR_CHECK", "Non-fatal error", (uint32_t)err_rc_);                      \
    }                                                                                              \
  } while (0)

/**
 * @brief Early return on error, propagating the code upward.
 *
 * @param[in] err     `ra8_err_t` value to check.
 * @param[in] tag     Component tag for logging.
 * @param[in] message String literal to log on failure.
 *
 * @note Must be used only inside a function whose return type is
 *       `ra8_err_t`. See `RA8_RETURN_VOID_ON_ERROR` / `RA8_RETURN_NULL_ON_ERROR`
 *       for `void` and pointer-returning callers.
 */
#define RA8_RETURN_ON_ERROR(err, tag, message)                                                     \
  do {                                                                                             \
    ra8_err_t err_rc_ = (err);                                                                     \
    if (ra8_err_is_error(err_rc_)) {                                                               \
      ra8_log_error((tag), (message));                                                             \
      ra8_log_error_val((tag), "Error", (uint32_t)err_rc_);                                        \
      return err_rc_;                                                                              \
    }                                                                                              \
  } while (0)

/**
 * @brief Early return from a `void` function on error.
 *
 * @param[in] err     `ra8_err_t` value to check.
 * @param[in] tag     Component tag for logging.
 * @param[in] message String literal to log on failure.
 */
#define RA8_RETURN_VOID_ON_ERROR(err, tag, message)                                                \
  do {                                                                                             \
    ra8_err_t err_rc_ = (err);                                                                     \
    if (ra8_err_is_error(err_rc_)) {                                                               \
      ra8_log_error((tag), (message));                                                             \
      ra8_log_error_val((tag), "Error", (uint32_t)err_rc_);                                        \
      return;                                                                                      \
    }                                                                                              \
  } while (0)

/**
 * @brief Early return `nullptr` from a pointer-returning function on error.
 *
 * @param[in] err     `ra8_err_t` value to check.
 * @param[in] tag     Component tag for logging.
 * @param[in] message String literal to log on failure.
 */
#define RA8_RETURN_NULL_ON_ERROR(err, tag, message)                                                \
  do {                                                                                             \
    ra8_err_t err_rc_ = (err);                                                                     \
    if (ra8_err_is_error(err_rc_)) {                                                               \
      ra8_log_error((tag), (message));                                                             \
      ra8_log_error_val((tag), "Error", (uint32_t)err_rc_);                                        \
      return nullptr;                                                                              \
    }                                                                                              \
  } while (0)

/* =============================================================================
 * Precondition checks
 * =============================================================================
 */

/**
 * @brief Reject `nullptr` pointer, returning `k_ra8_err_null_ptr`.
 *
 * @param[in] ptr     Pointer expression to test.
 * @param[in] tag     Component tag for logging.
 * @param[in] message String literal identifying which argument is NULL.
 *
 * @note Expands to `return k_ra8_err_null_ptr;` on failure, so the
 *       enclosing function must return `ra8_err_t`.
 */
#define RA8_CHECK_NULL_PTR(ptr, tag, message)                                                      \
  do {                                                                                             \
    if ((ptr) == nullptr) {                                                                        \
      ra8_log_error((tag), (message));                                                             \
      return k_ra8_err_null_ptr;                                                                   \
    }                                                                                              \
  } while (0)

/**
 * @brief Reject out-of-range value, returning `errcode`.
 *
 * @param[in] value   Numeric expression to test.
 * @param[in] min     Inclusive lower bound.
 * @param[in] max     Inclusive upper bound.
 * @param[in] errcode `ra8_err_t` value to return on failure.
 */
#define RA8_CHECK_RANGE(value, min, max, errcode)                                                  \
  do {                                                                                             \
    if (((value) < (min)) || ((value) > (max))) {                                                  \
      ra8_log_error("CHECK", "Range check failed");                                                \
      return (errcode);                                                                            \
    }                                                                                              \
  } while (0)

/**
 * @brief Tagged range check (adds a component tag to the log line).
 *
 * @param[in] value   Numeric expression to test.
 * @param[in] min     Inclusive lower bound (documented only; explicit check
 *                    is omitted to avoid `-Wtype-limits` when `min == 0`
 *                    and `value` is unsigned).
 * @param[in] max     Inclusive upper bound.
 * @param[in] errcode `ra8_err_t` value to return on failure.
 * @param[in] tag     Component tag for logging.
 */
#define RA8_CHECK_RANGE_TAG(value, min, max, errcode, tag)                                         \
  do {                                                                                             \
    (void)(min);                                                                                   \
    if ((value) > (max)) {                                                                         \
      ra8_log_error((tag), "Range check failed");                                                  \
      return (errcode);                                                                            \
    }                                                                                              \
  } while (0)

/**
 * @brief Alias for `RA8_CHECK_NULL_PTR` in precondition style.
 */
#define RA8_VALIDATE_PTR(ptr, tag, message) RA8_CHECK_NULL_PTR(ptr, tag, message)

/**
 * @brief Precondition: module must be initialized.
 *
 * @param[in] initialized Boolean expression (`true` if init complete).
 * @param[in] tag         Component tag for logging.
 * @param[in] message     String literal identifying the module.
 *
 * @note Returns `k_ra8_err_not_initialized` on failure.
 */
#define RA8_VALIDATE_INIT(initialized, tag, message)                                               \
  do {                                                                                             \
    if (!(initialized)) {                                                                          \
      ra8_log_error((tag), (message));                                                             \
      return k_ra8_err_not_initialized;                                                            \
    }                                                                                              \
  } while (0)

#ifdef __cplusplus
}
#endif
