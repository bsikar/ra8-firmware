/**
 * @file ra8_log.h
 * @brief Lightweight Logging Interface for ra8-firmware
 * @ingroup grp_core
 *
 * @details
 * Zero-allocation, tag + message + optional value logging API. The
 * interface is designed so the vast majority of firmware code can call
 * `ra8_log_info("TAG", "some message")` and the backend decides where
 * the bytes go (SEGGER RTT, UART, null sink).
 *
 * ## Design
 *
 * - **Four levels**: error, warn, info, debug. Anything below the
 *   compile-time `RA8_LOG_LEVEL` is a no-op (the call disappears at -O2).
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
 * | `k_ra8_log_level_none` | 0       | Compile logging out entirely         |
 * | `k_ra8_log_level_error`| 1       | Unrecoverable or escalated errors    |
 * | `k_ra8_log_level_warn` | 2       | Recoverable issues, degraded mode    |
 * | `k_ra8_log_level_info` | 3       | Significant state transitions        |
 * | `k_ra8_log_level_debug`| 4       | Per-iteration / per-packet tracing   |
 *
 * `RA8_LOG_LEVEL` defaults to `k_ra8_log_level_info` in Debug builds and
 * `k_ra8_log_level_warn` in Release builds. Override with
 * `-DRA8_LOG_LEVEL=k_ra8_log_level_debug` etc.
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
 * @enum ra8_log_level_t
 * @brief Compile-time and runtime log level.
 */
#define RA8_LOG_LEVEL_NONE (0)
/** @brief RA8 LOG LEVEL ERROR. */
#define RA8_LOG_LEVEL_ERROR (1)
/** @brief RA8 LOG LEVEL WARN. */
#define RA8_LOG_LEVEL_WARN (2)
/** @brief RA8 LOG LEVEL INFO. */
#define RA8_LOG_LEVEL_INFO (3)
/** @brief RA8 LOG LEVEL DEBUG. */
#define RA8_LOG_LEVEL_DEBUG (4)

typedef enum : uint8_t {
  k_ra8_log_level_none  = RA8_LOG_LEVEL_NONE,  /**< Disable all logging.                          */
  k_ra8_log_level_error = RA8_LOG_LEVEL_ERROR, /**< Errors only.                                  */
  k_ra8_log_level_warn  = RA8_LOG_LEVEL_WARN,  /**< Errors + warnings.                            */
  k_ra8_log_level_info  = RA8_LOG_LEVEL_INFO,  /**< Errors + warnings + info (default for Debug). */
  k_ra8_log_level_debug = RA8_LOG_LEVEL_DEBUG, /**< Everything, including per-iteration traces.   */
} ra8_log_level_t;

#ifndef RA8_LOG_LEVEL
#ifdef DEBUG
/** @brief RA8 LOG LEVEL. */
#define RA8_LOG_LEVEL RA8_LOG_LEVEL_INFO
#else
/** @brief RA8 LOG LEVEL. */
#define RA8_LOG_LEVEL RA8_LOG_LEVEL_WARN
#endif
#endif

/* =============================================================================
 * Backend entry points (implemented in ra8_log.c)
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
 * @pre `SystemInit()` has run.
 * @pre Called exactly once.
 * @post Log calls become routable.
 * @post No global flag is set -- backend readiness is detected per emit.
 *
 * @note Log calls made before `ra8_log_init()` runs are dropped
 *       silently -- no buffering. Not thread-safe.
 *
 * @since 0.1.0
 */
void ra8_log_init(void);

/**
 * @typedef ra8_log_byte_sink_fn_t
 * @brief Optional per-byte sink for redirecting log output off ITM.
 *
 * @details
 * When a sink is installed with ::ra8_log_set_byte_sink, every byte the logger
 * would have written to the ITM stimulus port is handed to this callback
 * instead. The `ra8_io` fabric installs an adapter that forwards the bytes to an
 * `ra8_io_stream_t`, making the log destination run-time selectable without
 * `ra8_core` depending on `ra8_io`.
 *
 * @param[in] ctx  Opaque cookie supplied to ::ra8_log_set_byte_sink.
 * @param[in] byte The log byte to consume.
 *
 * @since 0.1.0
 */
typedef void (*ra8_log_byte_sink_fn_t)(void* ctx, uint8_t byte);

/**
 * @brief Install (or clear) an optional byte sink for log output.
 *
 * @details
 * Pass a non-NULL `fn` to redirect all subsequent log bytes to `fn`; pass NULL
 * to restore the default ITM backend. Off by default, so existing behaviour is
 * unchanged until a sink is installed.
 *
 * @param[in] fn  Byte sink callback, or NULL to restore the ITM default.
 * @param[in] ctx Opaque cookie passed back to `fn` (may be NULL).
 *
 * @return Nothing.
 *
 * @pre None.
 * @pre `fn`, when non-NULL, out-lives the redirect.
 * @post Subsequent log bytes route to `fn` (or ITM when `fn` is NULL).
 * @post No log line already in flight is rerouted mid-byte.
 *
 * @note Not thread-safe with respect to concurrent logging.
 *
 * @since 0.1.0
 */
void ra8_log_set_byte_sink(ra8_log_byte_sink_fn_t fn, void* ctx);

/**
 * @brief Emit an ERROR-level log line with only a tag and a message.
 *
 * @details Backend entry point for the `ra8_log_error()` macro.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run (otherwise dropped).
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_error(const char* tag, const char* message);

/**
 * @brief Emit a WARN-level log line with only a tag and a message.
 *
 * @details Backend entry point for the `ra8_log_warn()` macro.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_warn(const char* tag, const char* message);

/**
 * @brief Emit an INFO-level log line with only a tag and a message.
 *
 * @details Backend entry point for the `ra8_log_info()` macro.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_info(const char* tag, const char* message);

/**
 * @brief Emit a DEBUG-level log line with only a tag and a message.
 *
 * @details Backend entry point for the `ra8_log_debug()` macro.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_debug(const char* tag, const char* message);

/**
 * @brief Emit an ERROR log line with a `uint32_t` companion value.
 *
 * @details Adds `=<decimal>` to the line emitted by ::ra8_log_emit_error.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 * @param[in] value   Unsigned numeric companion appended to the line.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_error_val(const char* tag, const char* message, uint32_t value);

/**
 * @brief Emit a WARN log line with a `uint32_t` companion value.
 *
 * @details Adds `=<decimal>` to the line emitted by ::ra8_log_emit_warn.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 * @param[in] value   Unsigned numeric companion appended to the line.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_warn_val(const char* tag, const char* message, uint32_t value);

/**
 * @brief Emit an INFO log line with a `uint32_t` companion value.
 *
 * @details Adds `=<decimal>` to the line emitted by ::ra8_log_emit_info.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 * @param[in] value   Unsigned numeric companion appended to the line.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_info_val(const char* tag, const char* message, uint32_t value);

/**
 * @brief Emit a DEBUG log line with an `int32_t` companion value.
 *
 * @details Adds `=<signed-decimal>` to the line emitted by
 *          ::ra8_log_emit_debug. Signed companion is intentional --
 *          debug traces frequently emit deltas.
 *
 * @param[in] tag     Short component tag; must not be NULL.
 * @param[in] message Free-form ASCII message; must not be NULL.
 * @param[in] value   Signed numeric companion appended to the line.
 *
 * @pre `tag` and `message` are NUL-terminated strings.
 * @pre `ra8_log_init()` has run.
 * @post One log line emitted or dropped.
 * @post No caller-visible state modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
void ra8_log_emit_debug_val(const char* tag, const char* message, int32_t value);

/* =============================================================================
 * Public macros (level-gated)
 * =============================================================================
 */

#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_ERROR
/** @brief RA8 log error. */
#define ra8_log_error(tag, message) ra8_log_emit_error((tag), (message))
/** @brief RA8 log error val. */
#define ra8_log_error_val(tag, message, value) ra8_log_emit_error_val((tag), (message), (value))
#else
/** @brief RA8 log error. */
#define ra8_log_error(tag, message)            ((void)0)
/** @brief RA8 log error val. */
#define ra8_log_error_val(tag, message, value) ((void)0)
#endif

#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_WARN
/** @brief RA8 log warn. */
#define ra8_log_warn(tag, message) ra8_log_emit_warn((tag), (message))
/** @brief RA8 log warn val. */
#define ra8_log_warn_val(tag, message, value) ra8_log_emit_warn_val((tag), (message), (value))
#else
/** @brief RA8 log warn. */
#define ra8_log_warn(tag, message)            ((void)0)
/** @brief RA8 log warn val. */
#define ra8_log_warn_val(tag, message, value) ((void)0)
#endif

#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_INFO
/** @brief RA8 log info. */
#define ra8_log_info(tag, message) ra8_log_emit_info((tag), (message))
/** @brief RA8 log info val. */
#define ra8_log_info_val(tag, message, value) ra8_log_emit_info_val((tag), (message), (value))
#else
/** @brief RA8 log info. */
#define ra8_log_info(tag, message)            ((void)0)
/** @brief RA8 log info val. */
#define ra8_log_info_val(tag, message, value) ((void)0)
#endif

#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_DEBUG
/** @brief RA8 log debug. */
#define ra8_log_debug(tag, message) ra8_log_emit_debug((tag), (message))
/** @brief RA8 log debug val. */
#define ra8_log_debug_val(tag, message, value) ra8_log_emit_debug_val((tag), (message), (value))
#else
/** @brief RA8 log debug. */
#define ra8_log_debug(tag, message)            ((void)0)
/** @brief RA8 log debug val. */
#define ra8_log_debug_val(tag, message, value) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
