/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/idf_compat/esp_log.h
 * @brief ESP-IDF-compatible logging surface, implemented by this port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core is written against ESP-IDF and includes
 * ``esp_log.h`` by name from eleven translation units. This project does not
 * vendor ESP-IDF, so the name is satisfied here by a first-party header that
 * offers exactly the surface the core uses -- and nothing more, because an
 * unused compatibility macro is a promise no one has checked.
 *
 * @par One implementation behind every spelling
 * ``ESP_LOGx``, ``ESP_EARLY_LOGx``, ``ESP_LOG_LEVEL_LOCAL`` and
 * ``ESP_LOG_BUFFER_HEXDUMP`` are all macros over two functions,
 * ::ra8_esp_hosted_log_write and ::ra8_esp_hosted_log_hexdump, defined in
 * ``port/esp-hosted/src/``. There is therefore one place where an
 * esp-hosted log line becomes RA8 console output, one place that decides how
 * a line is framed, and no second formatter to drift.
 *
 * @par The early variants are not a separate path
 * ESP-IDF splits ``ESP_EARLY_LOGx`` out because its normal logger is not
 * usable before the scheduler starts or from an interrupt. The vendored code
 * relies on that: ``spi_drv.c`` calls ``ESP_EARLY_LOGV`` from both side-band
 * edge handlers. This port meets the requirement by making the single writer
 * interrupt-safe rather than by keeping two loggers, so the early spellings
 * are aliases -- see the writer's contract below, which is where that
 * obligation is recorded.
 *
 * @par Compiling out
 * A call whose level is more verbose than ::LOG_LOCAL_LEVEL is guarded by a
 * comparison of two compile-time constants, which the optimiser folds away
 * along with the call and its argument evaluation. It is deliberately not a
 * preprocessor ``#if``: the arguments stay inside a real call expression, so
 * ``-Wformat=2`` keeps checking every vendored call site even at levels this
 * build never emits. A discarded ``#if`` would take that checking with it.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/* The enumerator spellings below are fixed by ESP-IDF: the vendored core
   passes them to esp_log_level_set() and compares against them, so they
   cannot take this project's k_ prefix. clang-tidy's naming rule is
   suppressed across the block, following the ThreadX shim precedent in
   libs/ra8_wdt_supervisor. */
/* NOLINTBEGIN(readability-identifier-naming) -- ESP-IDF-fixed spellings. */

/**
 * @enum esp_log_level_t
 * @brief Severity of one log line, ordered from silent to most verbose.
 *
 * @details
 * Numerically ordered so that "emit this line" is the single comparison
 * ``level <= LOG_LOCAL_LEVEL``. The values match ESP-IDF's, which matters
 * because ``esp_hosted_log.h`` passes them around as plain integers and
 * because a co-processor log capture read next to a host capture should use
 * one scale.
 *
 * @invariant ``ESP_LOG_NONE`` is zero and is never passed to the writer; it
 *            only ever appears as a threshold meaning "emit nothing".
 * @invariant The values are strictly increasing with verbosity, which is
 *            what makes the threshold test a single comparison.
 *
 * @par Example:
 * @code
 * esp_log_level_set("rpc_core", ESP_LOG_WARN);
 * @endcode
 *
 * @see LOG_LOCAL_LEVEL
 * @see ra8_esp_hosted_log_write
 * @since 0.1.0
 */
typedef enum : uint8_t {
  ESP_LOG_NONE    = 0, /**< Threshold only: suppress every line.           */
  ESP_LOG_ERROR   = 1, /**< A fault the caller cannot recover from itself. */
  ESP_LOG_WARN    = 2, /**< Something unexpected that was handled anyway.  */
  ESP_LOG_INFO    = 3, /**< Normal life-cycle milestones.                  */
  ESP_LOG_DEBUG   = 4, /**< Detail useful while bringing a link up.        */
  ESP_LOG_VERBOSE = 5, /**< Per-frame and per-edge tracing.                */
} esp_log_level_t;

/* NOLINTEND(readability-identifier-naming) */

#ifndef LOG_LOCAL_LEVEL
/**
 * @def LOG_LOCAL_LEVEL
 * @brief Most verbose level this translation unit is allowed to emit.
 * @details ::ESP_LOG_INFO by default: life-cycle milestones and everything
 * more severe reach the console, while the per-frame and per-edge tracing
 * that would swamp a 115200-baud console -- and change the timing of the
 * link it is describing -- is folded out. A translation unit that is being
 * debugged may define this to a more verbose level before including this
 * header, which is the ESP-IDF contract and the reason for the guard.
 * @note Read-only build configuration. It is an enumerator, not a
 * preprocessor integer, so it must not be used in an ``#if``.
 * @warning Raising this to ::ESP_LOG_VERBOSE turns on logging inside the two
 *          side-band interrupt handlers; that is legal here but it will
 *          dominate the interrupt latency the link is being measured for.
 * @par Example:
 * @code
 * #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
 * #include "esp_log.h"
 * @endcode
 * @since 0.1.0
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#endif /* LOG_LOCAL_LEVEL */

/**
 * @brief Emit one formatted esp-hosted log line.
 *
 * @details
 * The single entry point behind every ``ESP_LOGx`` spelling in this header.
 * The implementation in ``port/esp-hosted/src/`` prefixes the severity and
 * the tag, formats the body and hands the result to the firmware's console
 * sink.
 *
 * @param[in] level Severity as an ``int``, carrying an ::esp_log_level_t
 *                  value in the range ``ESP_LOG_ERROR`` .. ``ESP_LOG_VERBOSE``.
 *                  Taken as ``int`` because the vendored hex-dump helper
 *                  passes the level through an untyped macro parameter.
 * @param[in] tag   Module name, already carrying the ``H_`` prefix that
 *                  ``DEFINE_LOG_TAG`` adds. Never null; the macros always
 *                  supply the file-scope ``TAG``.
 * @param[in] fmt   ``printf``-style format string. Never null.
 * @param[in]  ...  Arguments matching @p fmt.
 *
 * @pre @p tag and @p fmt are non-null, null-terminated strings.
 * @pre The console sink has been initialised, or the call is discarded
 *      rather than writing to an unconfigured peripheral.
 * @post At most one line is emitted; nothing else observable changes.
 * @post The caller's arguments are unmodified.
 *
 * @note Must be safe to call from interrupt context: the vendored SPI driver
 *       logs from both side-band edge handlers through ``ESP_EARLY_LOGV``,
 *       and this port maps those onto this function rather than onto a
 *       second logger.
 * @warning Formatting is not free. At ::ESP_LOG_VERBOSE this sits on the
 *          per-frame path, so a slow console sink becomes the link's
 *          bottleneck.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_log_write(ESP_LOG_INFO, "H_spi", "clk %u MHz", clk);
 * @endcode
 *
 * @see ra8_esp_hosted_log_hexdump
 * @since 0.1.0
 */
[[gnu::format(printf, 3, 4)]] void
ra8_esp_hosted_log_write(int level, const char* tag, const char* fmt, ...);

/**
 * @brief Emit a hex dump of a buffer at a given severity.
 *
 * @details
 * Backs ``ESP_LOG_BUFFER_HEXDUMP``, which the vendored
 * ``esp_hosted_log.h`` builds its ``ESP_HEXLOGx`` helpers on. The
 * implementation decides the row width and whether an ASCII gutter is
 * printed; the core only supplies the bytes.
 *
 * @param[in] level Severity as an ``int``, carrying an ::esp_log_level_t
 *                  value.
 * @param[in] tag   Module name. Never null.
 * @param[in] buf   Start of the region to dump. May be null only when
 *                  @p len is zero.
 * @param[in] len   Bytes to dump, starting at @p buf.
 *
 * @pre @p tag is a non-null, null-terminated string.
 * @pre @p buf points to at least @p len readable bytes.
 * @post The buffer is not modified.
 * @post Output is bounded by @p len; the implementation truncates rather
 *       than growing an unbounded line.
 *
 * @note Callers already clamp the length: the vendored helper takes the
 *       lesser of the buffer length and a display length before calling.
 * @warning Dumping a whole 1600-byte frame is a hundred console lines.
 *          Clamp at the call site, as the vendored helpers do.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_log_hexdump(ESP_LOG_DEBUG, "H_spi", rx_buf, 32U);
 * @endcode
 *
 * @see ra8_esp_hosted_log_write
 * @since 0.1.0
 */
void ra8_esp_hosted_log_hexdump(int level, const char* tag, const void* buf, uint32_t len);

/**
 * @brief Raise the console threshold for one module at run time.
 *
 * @details
 * Backs ``esp_log_level_set``, which ``esp_hosted_api.c`` calls during
 * initialisation to quieten the three RPC modules. The implementation keeps
 * a small fixed table of overrides; a tag it has no room for keeps the
 * compile-time ::LOG_LOCAL_LEVEL rather than displacing another entry.
 *
 * @param[in] tag   Module name to adjust. Never null. Note that the vendored
 *                  caller passes unprefixed names, which do not match the
 *                  ``H_``-prefixed tags ``DEFINE_LOG_TAG`` produces.
 * @param[in] level New threshold as an ``int`` carrying an
 *                  ::esp_log_level_t value; ``ESP_LOG_NONE`` silences the
 *                  module completely.
 *
 * @pre @p tag is a non-null, null-terminated string.
 * @pre @p level is within ``ESP_LOG_NONE`` .. ``ESP_LOG_VERBOSE``.
 * @post Subsequent lines from a matching tag are filtered at @p level.
 * @post No line already emitted is affected.
 *
 * @note Safe to call before the console sink exists; the override table is
 *       plain data.
 * @warning The override can only make a module quieter than the compile-time
 *          threshold, never louder: the macros still fold out anything above
 *          ::LOG_LOCAL_LEVEL before the call is reached.
 *
 * @par Example:
 * @code
 * esp_log_level_set("rpc_core", ESP_LOG_WARN);
 * @endcode
 *
 * @see LOG_LOCAL_LEVEL
 * @since 0.1.0
 */
void ra8_esp_hosted_log_level_set(const char* tag, int level);

/**
 * @brief Report a failed ``ESP_ERROR_CHECK`` and stop.
 *
 * @details
 * The abort half of ``ESP_ERROR_CHECK``. ESP-IDF's macro aborts on a
 * non-zero result and the vendored core depends on that: ``transport_drv.c``
 * writes ``ESP_ERROR_CHECK(if_type >= ESP_MAX_IF)`` as a bounds assertion,
 * so a version that merely logged would let an out-of-range interface index
 * through. The implementation logs at ::ESP_LOG_ERROR and hands over to the
 * firmware's fault path.
 *
 * @param[in] tag  Module name of the failing call site. Never null.
 * @param[in] expr Stringised expression that failed. Never null.
 * @param[in] code The non-zero result the expression produced.
 *
 * @return This function does not return.
 *
 * @pre @p tag and @p expr are non-null, null-terminated strings.
 * @pre @p code is non-zero; a zero result is not a failure and must not
 *      reach here.
 * @post Control never returns to the caller.
 * @post The failure has been reported before control is surrendered.
 *
 * @note Callable from any context, including an interrupt handler.
 * @warning This is a hard stop, not an exception. Use
 *          ``ESP_ERROR_CHECK_WITHOUT_ABORT`` where the caller can carry on.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_log_fatal("H_transport", "rpc_init()", rc);
 * @endcode
 *
 * @see ra8_esp_hosted_log_write
 * @since 0.1.0
 */
[[noreturn]] void ra8_esp_hosted_log_fatal(const char* tag, const char* expr, int code);

/**
 * @def ESP_LOG_LEVEL_LOCAL
 * @brief Emit a line only if this translation unit allows that severity.
 * @details The one macro every other spelling in this header funnels
 * through. The threshold test compares two compile-time constants, so a
 * suppressed call costs nothing at run time while its arguments stay under
 * ``-Wformat`` scrutiny.
 * @param[in] level Severity, an ::esp_log_level_t value.
 * @param[in] tag   Module name string.
 * @param[in] ...   Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning Wrapped in ``do { } while (0)``, so it is a statement and needs
 *          its own semicolon; it is not usable as an expression.
 * @par Example:
 * @code
 * ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, TAG, "retry %u", n);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOG_LEVEL_LOCAL(level, tag, ...)                                                       \
  do {                                                                                             \
    if ((int)(level) <= (int)LOG_LOCAL_LEVEL) {                                                    \
      ra8_esp_hosted_log_write((int)(level), (tag), __VA_ARGS__);                                  \
    }                                                                                              \
  } while (0)

/**
 * @def ESP_LOGE
 * @brief Log a fault the caller cannot recover from itself.
 * @details Severity ::ESP_LOG_ERROR. The most-used spelling in the vendored
 * core by a wide margin, and the one level this build never folds out.
 * @param[in] tag Module name string, normally the file-scope ``TAG``.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning A statement, not an expression; it needs a semicolon.
 * @par Example:
 * @code
 * ESP_LOGE(TAG, "failed to open transport: %d", rc);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOGE(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, (tag), __VA_ARGS__)

/**
 * @def ESP_LOGW
 * @brief Log something unexpected that was handled anyway.
 * @details Severity ::ESP_LOG_WARN.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning A statement, not an expression; it needs a semicolon.
 * @par Example:
 * @code
 * ESP_LOGW(TAG, "dropping unknown event %u", evt);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOGW(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, (tag), __VA_ARGS__)

/**
 * @def ESP_LOGI
 * @brief Log a normal life-cycle milestone.
 * @details Severity ::ESP_LOG_INFO, the default ::LOG_LOCAL_LEVEL, so these
 * are the most verbose lines a stock build emits.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning A statement, not an expression; it needs a semicolon.
 * @par Example:
 * @code
 * ESP_LOGI(TAG, "co-processor up, fw %u.%u", major, minor);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOGI(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, (tag), __VA_ARGS__)

/**
 * @def ESP_LOGD
 * @brief Log detail useful while bringing the link up.
 * @details Severity ::ESP_LOG_DEBUG; folded out at the default threshold.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning A statement, not an expression; it needs a semicolon.
 * @par Example:
 * @code
 * ESP_LOGD(TAG, "init event length: %u", len);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOGD(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, (tag), __VA_ARGS__)

/**
 * @def ESP_LOGV
 * @brief Log per-frame or per-edge tracing.
 * @details Severity ::ESP_LOG_VERBOSE; folded out at the default threshold.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning Sits on the hot path. Enabling it changes the timing of the very
 *          transactions it reports.
 * @par Example:
 * @code
 * ESP_LOGV(TAG, "%s", __func__);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOGV(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, (tag), __VA_ARGS__)

/**
 * @def ESP_EARLY_LOGE
 * @brief Error-level log usable before the scheduler and inside interrupts.
 * @details The same path as ::ESP_LOGE. ESP-IDF needs a separate early
 * logger because its normal one takes a lock; this port's writer is
 * interrupt-safe by contract, so one implementation serves both.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning If the writer ever stops being interrupt-safe, this alias becomes
 *          a latent fault in the side-band edge handlers -- change the
 *          alias, not the handlers.
 * @par Example:
 * @code
 * ESP_EARLY_LOGE(TAG, "resetting host");
 * @endcode
 * @since 0.1.0
 */
#define ESP_EARLY_LOGE(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, (tag), __VA_ARGS__)

/**
 * @def ESP_EARLY_LOGW
 * @brief Warning-level log usable from early or interrupt context.
 * @details The same path as ::ESP_LOGW; see ::ESP_EARLY_LOGE.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning See ::ESP_EARLY_LOGE for the interrupt-safety contract.
 * @par Example:
 * @code
 * ESP_EARLY_LOGW(TAG, "power save on or reset in progress");
 * @endcode
 * @since 0.1.0
 */
#define ESP_EARLY_LOGW(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, (tag), __VA_ARGS__)

/**
 * @def ESP_EARLY_LOGI
 * @brief Info-level log usable from early or interrupt context.
 * @details The same path as ::ESP_LOGI; see ::ESP_EARLY_LOGE.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning See ::ESP_EARLY_LOGE for the interrupt-safety contract.
 * @par Example:
 * @code
 * ESP_EARLY_LOGI(TAG, "boot event seen");
 * @endcode
 * @since 0.1.0
 */
#define ESP_EARLY_LOGI(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, (tag), __VA_ARGS__)

/**
 * @def ESP_EARLY_LOGD
 * @brief Debug-level log usable from early or interrupt context.
 * @details The same path as ::ESP_LOGD; see ::ESP_EARLY_LOGE.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning See ::ESP_EARLY_LOGE for the interrupt-safety contract.
 * @par Example:
 * @code
 * ESP_EARLY_LOGD(TAG, "gpio_dr_isr_handler");
 * @endcode
 * @since 0.1.0
 */
#define ESP_EARLY_LOGD(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, (tag), __VA_ARGS__)

/**
 * @def ESP_EARLY_LOGV
 * @brief Verbose-level log usable from early or interrupt context.
 * @details The same path as ::ESP_LOGV; see ::ESP_EARLY_LOGE. This is the
 * spelling the vendored SPI driver uses inside both side-band edge
 * handlers, which is what makes the interrupt-safety contract binding.
 * @param[in] tag Module name string.
 * @param[in] ... Format string followed by its arguments.
 * @note Read-only build configuration.
 * @warning Folded out at the default threshold; raising ::LOG_LOCAL_LEVEL
 *          puts formatting inside an interrupt handler.
 * @par Example:
 * @code
 * ESP_EARLY_LOGV(TAG, "%s", __func__);
 * @endcode
 * @since 0.1.0
 */
#define ESP_EARLY_LOGV(tag, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, (tag), __VA_ARGS__)

/**
 * @def ESP_LOG_BUFFER_HEXDUMP
 * @brief Dump a buffer as hex at a given severity.
 * @details Guarded by the same compile-time threshold as the line macros, so
 * a dump that would be suppressed costs neither the call nor the formatting.
 * The vendored ``ESP_HEXLOGx`` helpers expand to this.
 * @param[in] tag   Module name string.
 * @param[in] buf   Start of the region to dump.
 * @param[in] len   Bytes to dump.
 * @param[in] level Severity, an ::esp_log_level_t value.
 * @note Read-only build configuration; the argument order is ESP-IDF's, with
 * the level last.
 * @warning A statement, not an expression; it needs a semicolon.
 * @par Example:
 * @code
 * ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, 32, ESP_LOG_DEBUG);
 * @endcode
 * @since 0.1.0
 */
#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, level)                                               \
  do {                                                                                             \
    if ((int)(level) <= (int)LOG_LOCAL_LEVEL) {                                                    \
      ra8_esp_hosted_log_hexdump((int)(level), (tag), (buf), (uint32_t)(len));                     \
    }                                                                                              \
  } while (0)

/**
 * @def esp_log_level_set
 * @brief Adjust one module's console threshold at run time.
 * @details Routed to ::ra8_esp_hosted_log_level_set so every ESP-IDF
 * spelling in this header resolves to the port's own logger rather than to a
 * second implementation.
 * @param[in] tag   Module name string to adjust.
 * @param[in] level New threshold, an ::esp_log_level_t value.
 * @note Read-only build configuration.
 * @warning Can only quieten a module, never make it louder than
 *          ::LOG_LOCAL_LEVEL.
 * @par Example:
 * @code
 * esp_log_level_set("rpc_evt", ESP_LOG_WARN);
 * @endcode
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(readability-identifier-naming) -- ESP-IDF-fixed lower-case spelling. */
#define esp_log_level_set(tag, level) ra8_esp_hosted_log_level_set((tag), (int)(level))

/**
 * @def ESP_ERROR_CHECK
 * @brief Evaluate an expression and stop the firmware if it is non-zero.
 * @details ESP-IDF's abort-on-error assertion, kept with its abort
 * semantics: the vendored ``transport_drv.c`` uses it as a bounds check on
 * an interface index, so downgrading it to a log would let an out-of-range
 * index through. The expression is evaluated exactly once.
 * @param[in] x Expression yielding zero on success and a non-zero error
 *              code, or a boolean that is true when something is wrong.
 * @note Read-only build configuration; ESP-IDF places this macro in
 * ``esp_err.h``, but every vendored user of it also reaches this header, and
 * keeping it beside the logger is what lets it report before it stops.
 * @warning Never put a side effect that must happen in a release build
 *          inside a construct whose name suggests it is only a check --
 *          although, unlike ``assert``, this one always evaluates @p x.
 * @par Example:
 * @code
 * ESP_ERROR_CHECK(esp_hosted_spi_set_config(nullptr));
 * @endcode
 * @since 0.1.0
 */
#define ESP_ERROR_CHECK(x)                                                                         \
  do {                                                                                             \
    const int ra8_esp_hosted_rc_ = (int)(x);                                                       \
    if (ra8_esp_hosted_rc_ != 0) {                                                                 \
      ra8_esp_hosted_log_fatal(TAG, #x, ra8_esp_hosted_rc_);                                       \
    }                                                                                              \
  } while (0)

/**
 * @def ESP_ERROR_CHECK_WITHOUT_ABORT
 * @brief Evaluate an expression, report a non-zero result, and carry on.
 * @details The recoverable half of ::ESP_ERROR_CHECK, and the reason both
 * exist: ``esp_hosted_api.c`` reconfigures the transport with this one
 * because a failed reconfiguration leaves the previous configuration
 * standing rather than corrupting anything. The expression is evaluated
 * exactly once.
 *
 * ESP-IDF's version also yields the error code. This one is a statement
 * instead, because yielding a value from a macro needs a GNU statement
 * expression and both vendored call sites discard the result -- buying a
 * non-standard construct for a value nothing reads would be the wrong
 * trade. A caller that needs the code should call the function and test it.
 * @param[in] x Expression yielding zero on success and a non-zero error code.
 * @note Read-only build configuration.
 * @warning A statement, not an expression; it needs a semicolon and cannot
 *          be used in a condition.
 * @par Example:
 * @code
 * ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hosted_reconfigure());
 * @endcode
 * @since 0.1.0
 */
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x)                                                           \
  do {                                                                                             \
    const int ra8_esp_hosted_rc_ = (int)(x);                                                       \
    if (ra8_esp_hosted_rc_ != 0) {                                                                 \
      ESP_LOGE(TAG, "%s returned 0x%x", #x, ra8_esp_hosted_rc_);                                   \
    }                                                                                              \
  } while (0)
