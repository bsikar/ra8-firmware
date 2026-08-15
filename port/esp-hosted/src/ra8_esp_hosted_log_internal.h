/**
 * @file port/esp-hosted/src/ra8_esp_hosted_log_internal.h
 * @brief Library-private surface of the esp-hosted logging bridge.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The bridge's public face is ``port/esp-hosted/inc/idf_compat/esp_log.h``,
 * which the vendored core includes by name. This header carries the two
 * pieces that are not part of that face: the runtime level decision, which
 * is promoted out of `static` so its conditions can be driven directly by
 * a host test, and the threshold reader that lets a test observe what
 * ``esp_log_level_set`` did without reaching into the module.
 *
 * Nothing outside ``port/esp-hosted/`` and ``tests/`` may include this.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @brief Emit one formatted line from an already-started argument list.
 *
 * @details
 * The body of the logging bridge. ``ra8_esp_hosted_log_write`` is a thin
 * varargs face over it, and the vtable's log row is another, so a line
 * emitted through either path is formatted and filtered identically.
 *
 * @param[in] level ESP-IDF level of the line.
 * @param[in] tag Tag to attribute the line to; null is replaced with the
 *                port's own tag rather than dereferenced.
 * @param[in] fmt Format string; null suppresses the line.
 * @param[in] ap Argument list positioned at the first conversion argument.
 *               The caller owns starting and ending it.
 *
 * @return Nothing.
 *
 * @pre The caller has started ``ap`` and will end it.
 * @pre The variable arguments match the conversions in ``fmt``.
 * @post At most one line reaches the sink.
 * @post No module state is modified.
 *
 * @note Reentrant; formats onto a bounded stack line and holds no lock,
 *       so it is safe from interrupt context as far as this module is
 *       concerned.
 *
 * @par MC/DC:
 * Shares the level and null-format decision with
 * ``priv_ra8_esp_hosted_log_accepts``, which the tests drive directly.
 *
 * @par Example:
 * @code
 * va_start(ap, format);
 * priv_ra8_esp_hosted_log_vwrite(level, tag, format, ap);
 * va_end(ap);
 * @endcode
 *
 * @see priv_ra8_esp_hosted_log_accepts
 * @since 0.1.0
 */
RA8_PRIV
void priv_ra8_esp_hosted_log_vwrite(int level, const char* tag, const char* fmt, va_list ap);

/**
 * @enum ra8_esp_hosted_log_dump_t
 * @brief Constants the hexdump renderer works from.
 *
 * @details
 * Named here rather than left as literals so the dump width and the
 * zero-padding rule are stated once and are visible to the test that
 * checks them.
 *
 * @invariant ::k_ra8_esp_hosted_log_dump_nibbles is the number of
 *            hexadecimal digits one byte occupies, so a shorter render is
 *            left-padded with a zero.
 *
 * @par Example:
 * @code
 * const uint8_t base = (uint8_t)k_ra8_esp_hosted_log_dump_radix;
 * @endcode
 *
 * @see ra8_esp_hosted_log_hexdump
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_log_dump_radix   = 16U, /**< Hexadecimal.                */
  k_ra8_esp_hosted_log_dump_nibbles = 2U,  /**< Digits one byte renders as. */
} ra8_esp_hosted_log_dump_t;

/**
 * @brief Decide whether a line at this level reaches the sink.
 *
 * @details
 * Applies the runtime threshold that ``esp_log_level_set`` maintains. The
 * compile-time ``LOG_LOCAL_LEVEL`` filter has already run at the call
 * site; this is the second, adjustable gate.
 *
 * @param[in] level ESP-IDF level of the candidate line. Any integer is
 *                  accepted, including values outside the enumeration.
 *
 * @return Whether the line should be emitted.
 * @retval true The level is a real level and is at or above the threshold.
 * @retval false The level is the suppress-everything value, is below the
 *         threshold, or is outside the enumeration.
 *
 * @pre None; the function is total over the integers.
 * @pre The threshold has been left at its default or set by the setter.
 * @post No state is modified.
 * @post The answer depends only on ``level`` and the current threshold.
 *
 * @note Reentrant; a single read of one integer.
 *
 * @par MC/DC:
 * Promoted from `static` so both conditions of
 * ``(level > ESP_LOG_NONE) && (level <= threshold)`` can be varied
 * independently. Production callers are the write and hexdump entry
 * points; nothing outside `tests/` may call it.
 *
 * @par Example:
 * @code
 * if (priv_ra8_esp_hosted_log_accepts((int)ESP_LOG_WARN)) { emit(); }
 * @endcode
 *
 * @see priv_ra8_esp_hosted_log_level_get
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] bool priv_ra8_esp_hosted_log_accepts(int level);

/**
 * @brief Report the current runtime verbosity threshold.
 *
 * @details
 * Exists so a test can assert what ``esp_log_level_set`` stored, including
 * the clamping it applies, without declaring the module's static.
 *
 * @return The threshold, as an ``esp_log_level_t`` value widened to `int`.
 * @retval 0 Every line is suppressed.
 *
 * @pre None; safe before any logging call.
 * @pre The caller tolerates a value a concurrent setter may stale.
 * @post No state is modified.
 * @post The result is within the ``esp_log_level_t`` range.
 *
 * @note Reentrant; a single read of one integer.
 *
 * @par MC/DC:
 * Branch-free observer; carries no decision of its own. Tests only.
 *
 * @par Example:
 * @code
 * esp_log_level_set(nullptr, ESP_LOG_DEBUG);
 * TEST_ASSERT_EQ((int)ESP_LOG_DEBUG, priv_ra8_esp_hosted_log_level_get());
 * @endcode
 *
 * @see priv_ra8_esp_hosted_log_accepts
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] int priv_ra8_esp_hosted_log_level_get(void);
