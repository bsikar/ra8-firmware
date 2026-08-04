/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_log.c
 * @brief Bridge from the esp-hosted logging surface onto ``ra8_log``.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements the four entry points declared in
 * ``port/esp-hosted/inc/idf_compat/esp_log.h`` plus the two allocator
 * reporting hooks declared in ``ra8_esp_hosted_port.h``.
 *
 * The vendored core logs with printf-style calls; this project's logger
 * takes a plain message and, optionally, one integer. The bridge closes
 * that gap by formatting into a stack line through
 * ``ra8_esp_hosted_fmt_vformat`` -- a bounded, allocation-free formatter --
 * and handing the finished string to the matching ``ra8_log_*`` macro.
 * Nothing here allocates, and the only module state is the runtime level
 * threshold, which is read but never written on a logging path.
 *
 * @par Level filtering happens twice, deliberately
 * ``LOG_LOCAL_LEVEL`` folds out call sites at compile time, and the
 * runtime threshold below rejects what survives. The compile-time filter
 * keeps unreachable formatting out of the image; the runtime one lets an
 * application raise verbosity during bring-up without a rebuild.
 * ``esp_log_level_set`` is per-tag in ESP-IDF; here it sets one global
 * threshold, which is stated in its contract rather than pretended
 * otherwise -- a per-tag table would need dynamic registration this image
 * has no allocator for.
 *
 * @since 0.1.0
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "ra8_attributes.h"
#include "ra8_esp_hosted_fmt_internal.h"
#include "ra8_esp_hosted_log_internal.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_log.h"

/**
 * @enum ra8_esp_hosted_log_bound_t
 * @brief Fixed sizes the log bridge works within.
 *
 * @details
 * One stack line and one hexdump line, both bounded so a format string or
 * a co-processor-supplied buffer cannot decide how much stack the logger
 * uses. The line budget is the dominant term in this module's stack frame,
 * which the project's ``-Wstack-usage`` build measures.
 *
 * @invariant ::k_ra8_esp_hosted_log_line_max leaves room for the longest
 *            format the vendored core uses at its widest arguments.
 * @invariant ::k_ra8_esp_hosted_log_dump_bytes multiplied by three, plus a
 *            terminator, fits inside the line budget.
 *
 * @par Example:
 * @code
 * char line[k_ra8_esp_hosted_log_line_max] = {};
 * @endcode
 *
 * @see ra8_esp_hosted_log_write
 * @since 0.1.0
 */
typedef enum : uint16_t {
  /** Bytes of stack the formatted line may occupy, terminator included. */
  k_ra8_esp_hosted_log_line_max = 160U,
  /** Bytes of a buffer a single hexdump line renders. */
  k_ra8_esp_hosted_log_dump_bytes = 32U,
  /** Characters one dumped byte costs: two nibbles and a separator. */
  k_ra8_esp_hosted_log_dump_cost = 3U,
} ra8_esp_hosted_log_bound_t;

/**
 * @var s_ra8_esp_hosted_log_level
 * @brief Runtime verbosity threshold for the esp-hosted bridge.
 * @details Lines at a level numerically greater than this are dropped.
 * Starts at the informational level so a default build reports link
 * life-cycle events without per-frame noise.
 * @note Read on every logging call; written only by
 * ::ra8_esp_hosted_log_level_set.
 * @warning Do not modify directly; the setter clamps the value.
 * @since 0.1.0
 */
static int s_ra8_esp_hosted_log_level = (int)ESP_LOG_INFO;

/**
 * @var k_ra8_esp_hosted_log_tag
 * @brief Fallback tag used when a caller passes none.
 * @details Keeps a null tag from reaching ``ra8_log_*``, which would
 * dereference it.
 * @note Read-only.
 * @warning Never freed; it is a string literal.
 * @since 0.1.0
 */
static const char* const k_ra8_esp_hosted_log_tag = "C6LINK";

/**
 * @brief Hand a finished line to the project logger at the mapped level.
 *
 * @details
 * ESP-IDF has five levels and ``ra8_log`` has four, so verbose folds into
 * debug. That is the only lossy step and it is stated here rather than
 * spread across the call sites.
 *
 * @param[in] level ESP-IDF level of the line.
 * @param[in] tag Tag to attribute the line to. Must be non-null.
 * @param[in] line Formatted text. Must be non-null.
 *
 * @return Nothing.
 *
 * @pre ``tag`` and ``line`` are non-null.
 * @pre The runtime threshold has already accepted this level.
 * @post Exactly one line is handed to the project logger.
 * @post No module state is modified.
 *
 * @note Reentrant with respect to this module; the sink beneath decides
 *       its own concurrency rules.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_emit(int level, const char* tag, const char* line)
{
  if (level <= (int)ESP_LOG_ERROR) {
    ra8_log_error(tag, line);
  } else if (level == (int)ESP_LOG_WARN) {
    ra8_log_warn(tag, line);
  } else if (level == (int)ESP_LOG_INFO) {
    ra8_log_info(tag, line);
  } else {
    ra8_log_debug(tag, line);
  }
}

/** @brief Implementation of `ra8_esp_hosted_log_accepts()` -- the single
 *  runtime level decision, promoted so its two conditions are testable. */
RA8_PRIV
bool ra8_esp_hosted_log_accepts(int level)
{
  return (level > (int)ESP_LOG_NONE) && (level <= s_ra8_esp_hosted_log_level);
}

/** @brief Implementation of `ra8_esp_hosted_log_vwrite()` -- formats onto a
 *  bounded stack line, so it allocates nothing and is reentrant. */
RA8_PRIV
void ra8_esp_hosted_log_vwrite(int level, const char* tag, const char* fmt, va_list ap)
{
  if (!ra8_esp_hosted_log_accepts(level) || (fmt == nullptr)) {
    return;
  }

  char line[(uint32_t)k_ra8_esp_hosted_log_line_max] = {};
  (void)ra8_esp_hosted_fmt_vformat(line, (uint32_t)sizeof(line), fmt, ap);
  internal_emit(level, (tag == nullptr) ? k_ra8_esp_hosted_log_tag : tag, line);
}

/** @brief Implementation of `ra8_esp_hosted_log_write()` -- the varargs face
 *  of `ra8_esp_hosted_log_vwrite()`. */
void ra8_esp_hosted_log_write(int level, const char* tag, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  ra8_esp_hosted_log_vwrite(level, tag, fmt, ap);
  va_end(ap);
}

/** @brief Implementation of `ra8_esp_hosted_log_hexdump()` -- renders at
 *  most ::k_ra8_esp_hosted_log_dump_bytes bytes onto one bounded line. */
void ra8_esp_hosted_log_hexdump(int level, const char* tag, const void* buf, uint32_t len)
{
  if (!ra8_esp_hosted_log_accepts(level) || (buf == nullptr) || (len == 0U)) {
    return;
  }

  const uint8_t* bytes = (const uint8_t*)buf;
  uint32_t       shown = len;
  if (shown > (uint32_t)k_ra8_esp_hosted_log_dump_bytes) {
    shown = (uint32_t)k_ra8_esp_hosted_log_dump_bytes;
  }

  char     line[(uint32_t)k_ra8_esp_hosted_log_line_max] = {};
  uint32_t pos                                           = 0U;
  for (uint32_t i = 0U; i < shown; i++) {
    char          nibbles[(uint32_t)k_ra8_esp_hosted_fmt_digits_max + 2U] = {};
    const uint8_t digits = ra8_esp_hosted_fmt_utoa(nibbles,
                                                   (uint64_t)bytes[i],
                                                   (uint8_t)k_ra8_esp_hosted_log_dump_radix,
                                                   false);
    if ((pos + (uint32_t)k_ra8_esp_hosted_log_dump_cost) >= sizeof(line)) {
      break;
    }
    if (digits < (uint8_t)k_ra8_esp_hosted_log_dump_nibbles) {
      line[pos] = '0';
      pos++;
    }
    for (uint8_t d = 0U; d < digits; d++) {
      line[pos] = nibbles[d];
      pos++;
    }
    line[pos] = ' ';
    pos++;
  }
  line[pos] = '\0';

  internal_emit(level, (tag == nullptr) ? k_ra8_esp_hosted_log_tag : tag, line);
}

/** @brief Implementation of `ra8_esp_hosted_log_level_set()` -- one global
 *  threshold rather than the per-tag table ESP-IDF keeps; see the file
 *  header for why. */
void ra8_esp_hosted_log_level_set(const char* tag, int level)
{
  (void)tag;
  int clamped = level;
  if (clamped < (int)ESP_LOG_NONE) {
    clamped = (int)ESP_LOG_NONE;
  }
  if (clamped > (int)ESP_LOG_VERBOSE) {
    clamped = (int)ESP_LOG_VERBOSE;
  }
  s_ra8_esp_hosted_log_level = clamped;
}

/** @brief Implementation of `ra8_esp_hosted_log_level_get()` -- reads the
 *  threshold the setter last clamped. */
RA8_PRIV
int ra8_esp_hosted_log_level_get(void)
{
  return s_ra8_esp_hosted_log_level;
}

/** @brief Implementation of `ra8_esp_hosted_log_fatal()` -- reports and
 *  parks; the caller's check already proved the state is unrecoverable. */
void ra8_esp_hosted_log_fatal(const char* tag, const char* expr, int code)
{
  const char* safe_tag = (tag == nullptr) ? k_ra8_esp_hosted_log_tag : tag;
  ra8_esp_hosted_log_write((int)ESP_LOG_ERROR,
                           safe_tag,
                           "fatal: %s -> %d",
                           (expr == nullptr) ? "(expr)" : expr,
                           code);
  /* GCOVR_EXCL_START -- a park loop no host input can leave; entering it in
     a unit test would hang the suite rather than report anything. */
  while (true) {
#ifndef RA8_OFF_TARGET
    __asm__ volatile("wfi");
#endif
  }
  /* GCOVR_EXCL_STOP */
}

/** @brief Implementation of `ra8_esp_hosted_mem_dump()` -- reads the live
 *  ThreadX byte-pool statistics, so the numbers are real. */
void ra8_esp_hosted_mem_dump(const char* label)
{
  uint32_t available = 0U;
  uint32_t fragments = 0U;
  ra8_esp_hosted_rtos_pool_stats(&available, &fragments);
  ra8_esp_hosted_log_write((int)ESP_LOG_INFO,
                           k_ra8_esp_hosted_log_tag,
                           "%s: pool free %u bytes in %u fragments",
                           (label == nullptr) ? "(unnamed)" : label,
                           (unsigned int)available,
                           (unsigned int)fragments);
}

/** @brief Implementation of `ra8_esp_hosted_alloc_failed()` -- names the
 *  requester and the size, which together size the pool without guesswork. */
void ra8_esp_hosted_alloc_failed(const char* func, size_t bytes)
{
  ra8_esp_hosted_log_write((int)ESP_LOG_ERROR,
                           k_ra8_esp_hosted_log_tag,
                           "%s: pool cannot serve %zu bytes",
                           (func == nullptr) ? "(unnamed)" : func,
                           bytes);
}
