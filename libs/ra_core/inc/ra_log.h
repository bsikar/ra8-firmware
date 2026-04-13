/**
 * @file ra_log.h
 * @brief Lightweight Logging Interface for ra8d2-firmware
 *
 * @details
 * Zero-allocation, tag + message + optional value logging API. The
 * interface is designed so the vast majority of firmware code can call
 * `ra_log_info("TAG", "some message")` and the backend decides where
 * the bytes go (SEGGER RTT, UART, null sink).
 *
 * ## Design
 *
 * - **Four levels**: error, warn, info, debug. Anything below the
 *   compile-time `RA_LOG_LEVEL` is a no-op (the call disappears at -O2).
 * - **Tag + message + optional value**: every log line has a short tag
 *   identifying the component (`"SCI"`, `"CGC"`, `"PID"`, ...) and a
 *   short string message. A numeric companion (`uint32_t` or `int32_t`)
 *   can be attached via the `*_val` variants so we never have to call
 *   `printf`/`snprintf` in hot paths.
 * - **No varargs**: on purpose. Varargs pull in `vfprintf` (~4 KB),
 *   break `-Wstack-usage=2200`, and make the worst-case stack usage
 *   impossible to bound. Formatted output is limited to `tag`,
 *   `message`, and a single numeric value.
 * - **Thread safety is a backend concern**: the default RTT backend is
 *   reentrant (single writer, per-CPU queue). UART backends are
 *   protected with an IRQ-masked critical section.
 * - **`[[nodiscard]]`-free**: log calls are fire-and-forget.
 *
 * ## Levels
 *
 * | Level                 | Numeric | Usage                                |
 * |-----------------------|---------|--------------------------------------|
 * | `k_ra_log_level_none` | 0       | Compile logging out entirely         |
 * | `k_ra_log_level_error`| 1       | Unrecoverable or escalated errors    |
 * | `k_ra_log_level_warn` | 2       | Recoverable issues, degraded mode    |
 * | `k_ra_log_level_info` | 3       | Significant state transitions        |
 * | `k_ra_log_level_debug`| 4       | Per-iteration / per-packet tracing   |
 *
 * `RA_LOG_LEVEL` defaults to `k_ra_log_level_info` in Debug builds and
 * `k_ra_log_level_warn` in Release builds. Override with
 * `-DRA_LOG_LEVEL=k_ra_log_level_debug` etc.
 *
 * ## NASA Power of 10 Compliance
 *
 * - **Rule 3**: no dynamic allocation. Backends use fixed-size static
 *   buffers.
 * - **Rule 8**: macros exist only for conditional compilation and level
 *   gating. The actual logging functions are real `extern` entry points
 *   so they stay in one compilation unit.
 * - **Rule 10**: compiles clean under `-Wall -Wextra -Werror`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Log levels
 * =============================================================================
 */

/**
 * @enum ra_log_level_t
 * @brief Compile-time and runtime log level.
 */
typedef enum : uint8_t {
  k_ra_log_level_none  = 0, /**< Disable all logging.                          */
  k_ra_log_level_error = 1, /**< Errors only.                                  */
  k_ra_log_level_warn  = 2, /**< Errors + warnings.                            */
  k_ra_log_level_info  = 3, /**< Errors + warnings + info (default for Debug). */
  k_ra_log_level_debug = 4, /**< Everything, including per-iteration traces.   */
} ra_log_level_t;

#ifndef RA_LOG_LEVEL
#ifdef DEBUG
#define RA_LOG_LEVEL k_ra_log_level_info
#else
#define RA_LOG_LEVEL k_ra_log_level_warn
#endif
#endif

/* =============================================================================
 * Backend entry points (implemented in ra_log.c)
 * =============================================================================
 */

/**
 * @brief Initialise the logging backend.
 *
 * @details
 * Called exactly once during early boot, before the first log call.
 * The default backend is SEGGER RTT (J-Link on-board debugger) and
 * requires no hardware setup beyond this call. A UART backend is
 * provided as an alternative for boards without a J-Link.
 *
 * @note Log calls made before `ra_log_init()` runs are dropped
 *       silently -- no buffering.
 */
void ra_log_init(void);

/** @brief Emit an ERROR-level log line with only a tag and a message. */
void internal_ra_log_error(const char* tag, const char* message);
/** @brief Emit a WARN-level log line with only a tag and a message. */
void internal_ra_log_warn(const char* tag, const char* message);
/** @brief Emit an INFO-level log line with only a tag and a message. */
void internal_ra_log_info(const char* tag, const char* message);
/** @brief Emit a DEBUG-level log line with only a tag and a message. */
void internal_ra_log_debug(const char* tag, const char* message);

/** @brief Emit an ERROR log line with a `uint32_t` companion value. */
void internal_ra_log_error_val(const char* tag, const char* message, uint32_t value);
/** @brief Emit a WARN log line with a `uint32_t` companion value. */
void internal_ra_log_warn_val(const char* tag, const char* message, uint32_t value);
/** @brief Emit an INFO log line with a `uint32_t` companion value. */
void internal_ra_log_info_val(const char* tag, const char* message, uint32_t value);
/** @brief Emit a DEBUG log line with an `int32_t` companion value. */
void internal_ra_log_debug_val(const char* tag, const char* message, int32_t value);

/* =============================================================================
 * Public macros (level-gated)
 * =============================================================================
 */

#if RA_LOG_LEVEL >= k_ra_log_level_error
#define ra_log_error(tag, message)            internal_ra_log_error((tag), (message))
#define ra_log_error_val(tag, message, value) internal_ra_log_error_val((tag), (message), (value))
#else
#define ra_log_error(tag, message)            ((void)0)
#define ra_log_error_val(tag, message, value) ((void)0)
#endif

#if RA_LOG_LEVEL >= k_ra_log_level_warn
#define ra_log_warn(tag, message)            internal_ra_log_warn((tag), (message))
#define ra_log_warn_val(tag, message, value) internal_ra_log_warn_val((tag), (message), (value))
#else
#define ra_log_warn(tag, message)            ((void)0)
#define ra_log_warn_val(tag, message, value) ((void)0)
#endif

#if RA_LOG_LEVEL >= k_ra_log_level_info
#define ra_log_info(tag, message)            internal_ra_log_info((tag), (message))
#define ra_log_info_val(tag, message, value) internal_ra_log_info_val((tag), (message), (value))
#else
#define ra_log_info(tag, message)            ((void)0)
#define ra_log_info_val(tag, message, value) ((void)0)
#endif

#if RA_LOG_LEVEL >= k_ra_log_level_debug
#define ra_log_debug(tag, message)            internal_ra_log_debug((tag), (message))
#define ra_log_debug_val(tag, message, value) internal_ra_log_debug_val((tag), (message), (value))
#else
#define ra_log_debug(tag, message)            ((void)0)
#define ra_log_debug_val(tag, message, value) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
