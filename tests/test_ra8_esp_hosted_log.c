/**
 * @file test_ra8_esp_hosted_log.c
 * @brief Unit tests for the esp-hosted logging bridge onto `ra8_log`.
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Drives every entry point of ``port/esp-hosted/src/ra8_esp_hosted_log.c``
 * except ``ra8_esp_hosted_log_fatal``, which parks in an unbounded loop by
 * design and is marked ``GCOVR_EXCL`` for exactly that reason -- entering it
 * from a unit test would hang the suite rather than report anything.
 *
 * @par How the output is observed
 * The bridge hands finished lines to the project logger, whose default
 * back-end writes to the ITM stimulus port -- which does not exist on the
 * host. ``ra8_log_set_byte_sink`` redirects those bytes to a callback, so the
 * tests install one that accumulates into a static buffer and then assert the
 * exact text. That makes the level mapping (which ``ra8_log_*`` sink each
 * ESP-IDF level lands on) observable rather than inferred, which matters:
 * the two ladders have different lengths and verbose folds into debug.
 *
 * @par Why the info and debug expectations are compile-time selected
 * ``ra8_log`` gates its levels with the preprocessor, so whether an info or
 * debug line reaches the sink at all depends on ``RA8_LOG_LEVEL`` in the
 * build under test (the host build leaves it at its default). Asserting one
 * answer unconditionally would make this file pass or fail for a reason that
 * has nothing to do with the bridge. Instead ::t_assert_info_line and
 * ::t_assert_debug_line assert the line when the level is compiled in and
 * assert silence when it is not -- both are real assertions, and one of them
 * is always the correct one.
 *
 * No hardware registers are touched; no ``ra8_fake_mmap`` window is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ra8_esp_hosted_log_internal.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum t_log_const_t
 * @brief Fixture sizes and fixed expectations this translation unit uses.
 *
 * @details
 * Named so a capacity is distinguishable from a byte count at the point of
 * use, and so the hexdump expectations state which property they pin.
 *
 * @invariant ::k_t_log_sink_cap exceeds the bridge's own line budget, so a
 *            full line plus its framing always fits in the capture buffer.
 *
 * @par Example:
 * @code
 * char want[k_t_log_sink_cap] = {};
 * @endcode
 *
 * @see ra8_esp_hosted_log_hexdump
 */
typedef enum : uint32_t {
  k_t_log_sink_cap      = 512U, /**< Capture-buffer capacity.              */
  k_t_log_dump_over     = 40U,  /**< Byte count exceeding the dump width.  */
  k_t_log_dump_shown    = 32U,  /**< Bytes one dump line actually renders. */
  k_t_log_dump_per_byte = 3U,   /**< Characters one rendered byte costs.   */
} t_log_const_t;

/**
 * @enum t_log_signed_vector_t
 * @brief Signed vectors driving the level clamp and the `%d` expansion.
 *
 * @details
 * The two level vectors sit outside the `ESP_LOG_NONE`..`ESP_LOG_VERBOSE`
 * range on either side, which is the only way to reach both clamp arms; a
 * value inside the range would leave the clamps untaken. ::k_t_log_vwrite_int
 * is an ordinary in-range argument, present only so the expanded line has a
 * digit in it that the assertion can pin.
 *
 * @invariant ::k_t_log_level_below_min is below `ESP_LOG_NONE` and
 *            ::k_t_log_level_above_max is above `ESP_LOG_VERBOSE`, so neither
 *            can silently become an in-range level if the enum is extended.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_log_level_set(nullptr, k_t_log_level_above_max);
 * @endcode
 *
 * @see t_log_arg_t
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_t_log_level_below_min = -5, /**< Level below `ESP_LOG_NONE`; clamps up.      */
  k_t_log_level_above_max = 99, /**< Level above `ESP_LOG_VERBOSE`; clamps down. */
  k_t_log_vwrite_int      = 5,  /**< `%d` argument the vwrite line renders.      */
} t_log_signed_vector_t;

/**
 * @enum t_log_arg_t
 * @brief Unsigned argument fixtures the formatted-line tests pass through.
 *
 * @details
 * Each is picked so the rendered text could not have come from any other
 * argument: the retry count renders as a single decimal digit, the edge byte
 * renders as two lowercase hex nibbles, and the refused allocation renders as
 * a four-digit byte count no other value in the file produces.
 *
 * @invariant ::k_t_log_edge_arg has both nibbles set to different values, so a
 *            formatter that emitted only one of them would fail the assertion.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_log_write((int)ESP_LOG_WARN, "H_t", "retry %u", k_t_log_retry_arg);
 * @endcode
 *
 * @see t_log_signed_vector_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_log_retry_arg  = 7U,    /**< `%u` argument rendering as "7".        */
  k_t_log_edge_arg   = 0xABU, /**< `%x` argument rendering as "ab".       */
  k_t_log_alloc_size = 1600U, /**< Byte count the exhausted pool refuses. */
} t_log_arg_t;

/**
 * @var s_sink_buf
 * @brief Bytes the project logger emitted since the last reset.
 * @details Accumulated by ::t_sink and asserted against by the tests.
 * @note Reset by ::t_sink_reset before every observation.
 * @warning Not thread-safe; the host test driver is single-threaded.
 */
static char s_sink_buf[(size_t)k_t_log_sink_cap];

/**
 * @var s_sink_len
 * @brief Number of bytes currently held in ::s_sink_buf.
 * @details Doubles as the "nothing was emitted" observation when zero.
 * @note Reset by ::t_sink_reset before every observation.
 * @warning Not thread-safe; the host test driver is single-threaded.
 */
static uint32_t s_sink_len;

/**
 * @brief Byte sink that accumulates the logger's output for inspection.
 *
 * @details
 * Keeps the buffer NUL-terminated after every byte so a test can compare it
 * as a string without a separate finalising step. Silently drops anything
 * past the capacity, which cannot happen for the line lengths under test but
 * keeps a runaway line from overrunning the fixture.
 *
 * @param[in] ctx Unused cookie; the buffer is file-scope.
 * @param[in] byte Byte the logger emitted.
 *
 * @pre ::t_sink_reset has run since the last observation.
 * @pre The capture buffer has room, or the byte is dropped.
 * @post At most one byte is appended.
 * @post The buffer stays NUL-terminated.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  if (s_sink_len < ((uint32_t)k_t_log_sink_cap - 1U)) {
    s_sink_buf[s_sink_len] = (char)byte;
    s_sink_len++;
    s_sink_buf[s_sink_len] = '\0';
  }
}

/**
 * @brief Discard whatever the sink captured so far.
 *
 * @details
 * Called before each observation so an assertion sees exactly the line the
 * call under test produced, and never a residue of the previous one.
 *
 * @pre The sink is installed, or the reset is harmless.
 * @pre No log call is in flight.
 * @post The capture buffer is empty and terminated.
 * @post The captured length is zero.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_sink_reset(void)
{
  s_sink_len    = 0U;
  s_sink_buf[0] = '\0';
}

/**
 * @brief Assert the sink captured exactly one line with the given framing.
 *
 * @details
 * Rebuilds the project logger's line format -- ``[tag] LEVEL: message`` with
 * a carriage-return / line-feed terminator -- and compares the whole thing,
 * so a change to the framing or to the level word is caught rather than
 * silently accepted.
 *
 * @param[in] tag Tag the line should be attributed to.
 * @param[in] level Level word the project logger should have used.
 * @param[in] msg Message text the bridge should have formatted.
 *
 * @pre All three arguments are non-null NUL-terminated strings.
 * @pre The sink was reset before the call under test.
 * @post Returns only when the captured text matches exactly.
 * @post The process has exited with status 1 on mismatch.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_assert_line(const char* tag, const char* level, const char* msg)
{
  char want[(size_t)k_t_log_sink_cap];
  (void)snprintf(want, sizeof want, "[%s] %s: %s\r\n", tag, level, msg);
  TEST_ASSERT(strcmp(want, s_sink_buf) == 0);
}

/**
 * @brief Assert nothing at all reached the sink.
 *
 * @details
 * The observation a dropped line produces. Distinct from "an empty message
 * was logged", which would still carry the tag and level framing.
 *
 * @pre The sink was reset before the call under test.
 * @pre The sink is installed.
 * @post Returns only when the capture buffer is empty.
 * @post The process has exited with status 1 otherwise.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_assert_silent(void)
{
  TEST_ASSERT_EQ(0, s_sink_len);
}

/**
 * @brief Assert what an info-level line does in this build.
 *
 * @details
 * The project logger folds every level below ``RA8_LOG_LEVEL`` out at compile
 * time, so an info line either reaches the sink with the ``INFO`` level word
 * or is not emitted at all. Both are correct bridge behaviour; which one
 * applies is a property of the build, and the selection is made here once
 * rather than at every call site.
 *
 * @param[in] tag Tag the line should be attributed to.
 * @param[in] msg Message text the bridge should have formatted.
 *
 * @pre @p tag and @p msg are non-null NUL-terminated strings.
 * @pre The sink was reset before the call under test.
 * @post Returns only when the build-appropriate expectation holds.
 * @post The process has exited with status 1 otherwise.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_assert_info_line(const char* tag, const char* msg)
{
#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_INFO
  t_assert_line(tag, "INFO", msg);
#else
  (void)tag;
  (void)msg;
  t_assert_silent();
#endif
}

/**
 * @brief Assert what a debug-level line does in this build.
 *
 * @details
 * The debug counterpart of ::t_assert_info_line, and the sink both the
 * ESP-IDF debug and verbose levels map onto -- the bridge has four project
 * levels to serve five ESP-IDF ones, and verbose folding into debug is the
 * single lossy step.
 *
 * @param[in] tag Tag the line should be attributed to.
 * @param[in] msg Message text the bridge should have formatted.
 *
 * @pre @p tag and @p msg are non-null NUL-terminated strings.
 * @pre The sink was reset before the call under test.
 * @post Returns only when the build-appropriate expectation holds.
 * @post The process has exited with status 1 otherwise.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_assert_debug_line(const char* tag, const char* msg)
{
#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_DEBUG
  t_assert_line(tag, "DEBUG", msg);
#else
  (void)tag;
  (void)msg;
  t_assert_silent();
#endif
}

/**
 * @brief Reach `ra8_esp_hosted_log_vwrite` through a variable-argument bridge.
 *
 * @details
 * The internal entry point takes an already-started ``va_list``, which is how
 * the vtable's log row and the ``ESP_LOGx`` macros both reach it. A test
 * needs one bridge of its own to drive that path directly rather than only
 * through the varargs face.
 *
 * @param[in] level ESP-IDF level of the line.
 * @param[in] tag Tag to attribute the line to; may be null.
 * @param[in] fmt Format string; may be null to drive that guard.
 * @param[in] ... Arguments consumed by @p fmt.
 *
 * @pre The variable arguments match the conversions in @p fmt.
 * @pre The sink was reset if the caller intends to observe the line.
 * @post The argument list is ended exactly once.
 * @post At most one line reaches the sink.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_vwrite(int level, const char* tag, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  ra8_esp_hosted_log_vwrite(level, tag, fmt, ap);
  va_end(ap);
}

/**
 * @test test_log_level_default_and_clamp
 *
 * @brief The threshold starts at info and is clamped at both ends when set.
 *
 * @details
 * Runs first so it observes the load-time default before any other test has
 * moved the threshold. The setter's tag argument is ignored by design -- this
 * bridge keeps one global threshold rather than the per-tag table ESP-IDF
 * maintains, because a per-tag table needs dynamic registration and this
 * image has no allocator -- so both a real tag and a null one are passed to
 * show the answer does not depend on it.
 *
 * @par MC/DC:
 * Decision A: `if (clamped < ESP_LOG_NONE) { clamped = ESP_LOG_NONE; }`
 * (1 condition, 2 vectors)
 * - Vector A1: level=::k_t_log_level_below_min (-5) -> true;  clamped up to none
 * - Vector A2: level=ESP_LOG_WARN                   -> false; stored unchanged
 *
 * Decision B: `if (clamped > ESP_LOG_VERBOSE) { clamped = ESP_LOG_VERBOSE; }`
 * (1 condition, 2 vectors)
 * - Vector B1: level=::k_t_log_level_above_max (99) -> true;  clamped down to
 *   verbose
 * - Vector B2: level=ESP_LOG_WARN                   -> false; stored unchanged
 * The two clamps are independent single-condition decisions rather than one
 * compound one, so two vectors each is the complete set.
 *
 * @pre No earlier test has moved the threshold.
 * @post The threshold is left at verbose for the tests that follow.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_level_default_and_clamp(void)
{
  TEST_BEGIN("log level default and clamping");

  TEST_ASSERT_EQ(ESP_LOG_INFO, ra8_esp_hosted_log_level_get());

  ra8_esp_hosted_log_level_set("rpc_core", (int)ESP_LOG_WARN);
  TEST_ASSERT_EQ(ESP_LOG_WARN, ra8_esp_hosted_log_level_get());

  ra8_esp_hosted_log_level_set(nullptr, k_t_log_level_below_min);
  TEST_ASSERT_EQ(ESP_LOG_NONE, ra8_esp_hosted_log_level_get());

  ra8_esp_hosted_log_level_set(nullptr, k_t_log_level_above_max);
  TEST_ASSERT_EQ(ESP_LOG_VERBOSE, ra8_esp_hosted_log_level_get());

  /* Both bounds are inclusive: setting exactly the extremes is not clamping. */
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_NONE);
  TEST_ASSERT_EQ(ESP_LOG_NONE, ra8_esp_hosted_log_level_get());
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);
  TEST_ASSERT_EQ(ESP_LOG_VERBOSE, ra8_esp_hosted_log_level_get());
  TEST_END("log level default and clamping");
}

/**
 * @test test_log_accepts_mcdc
 *
 * @brief `ra8_esp_hosted_log_accepts` applies both halves of its threshold.
 *
 * @details
 * The gate has to reject two quite different things: the suppress-everything
 * value (and anything below it, since the parameter is a plain `int` and the
 * vendored core is free to pass one), and anything above the current
 * threshold. Only varying both independently proves neither half was dropped.
 *
 * @par MC/DC:
 * Decision: `(level > ESP_LOG_NONE) && (level <= threshold)` in
 * `port/esp-hosted/src/ra8_esp_hosted_log.c@ra8_esp_hosted_log_accepts`, with
 * the threshold held at info (2 conditions, 3 vectors)
 * - Vector 1: level=ESP_LOG_INFO(3)  -> true,  true  -> true  (control)
 * - Vector 2: level=ESP_LOG_NONE(0)  -> false        -> false (varies the
 *   lower half only; the upper half would have been true)
 * - Vector 3: level=ESP_LOG_DEBUG(4) -> true,  false -> false (varies the
 *   upper half only; the lower half stays true)
 * Vectors 1+2 prove the lower comparison independently affects the outcome;
 * vectors 1+3 prove the same for the threshold comparison. N+1 = 3 vectors
 * for N=2: minimal MC/DC.
 *
 * @pre The threshold can be moved freely by this test.
 * @post The threshold is left at verbose for the tests that follow.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_accepts_mcdc(void)
{
  TEST_BEGIN("log accepts threshold");

  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_INFO);
  TEST_ASSERT(ra8_esp_hosted_log_accepts((int)ESP_LOG_INFO));
  TEST_ASSERT(!ra8_esp_hosted_log_accepts((int)ESP_LOG_NONE));
  TEST_ASSERT(!ra8_esp_hosted_log_accepts((int)ESP_LOG_DEBUG));

  /* A level below the enumeration entirely is rejected by the lower half,
     which is why that half exists at all. */
  TEST_ASSERT(!ra8_esp_hosted_log_accepts(-1));

  /* Everything at or below the threshold passes. */
  TEST_ASSERT(ra8_esp_hosted_log_accepts((int)ESP_LOG_ERROR));
  TEST_ASSERT(ra8_esp_hosted_log_accepts((int)ESP_LOG_WARN));

  /* Raising the threshold admits what it previously rejected -- proving the
     comparison reads the live threshold rather than a compile-time one. */
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);
  TEST_ASSERT(ra8_esp_hosted_log_accepts((int)ESP_LOG_DEBUG));
  TEST_ASSERT(ra8_esp_hosted_log_accepts((int)ESP_LOG_VERBOSE));

  /* Suppressing everything rejects even an error. */
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_NONE);
  TEST_ASSERT(!ra8_esp_hosted_log_accepts((int)ESP_LOG_ERROR));

  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);
  TEST_END("log accepts threshold");
}

/**
 * @test test_log_write_level_mapping
 *
 * @brief Each ESP-IDF level lands on the matching project-logger sink.
 *
 * @details
 * Five ESP-IDF levels map onto four project levels, so the mapping is not the
 * identity and cannot be assumed. Each level is emitted with the threshold
 * wide open and the resulting text is asserted, which pins both the level
 * word and the fact that the format string was expanded rather than passed
 * through.
 *
 * @par MC/DC:
 * Decision: the `internal_emit` level ladder -- `level <= ESP_LOG_ERROR`,
 * then `level == ESP_LOG_WARN`, then `level == ESP_LOG_INFO`, else debug
 * (3 single-condition decisions, 4 vectors)
 * - Vector 1: ESP_LOG_ERROR   -> first test true                  -> error sink
 * - Vector 2: ESP_LOG_WARN    -> first false, second true         -> warn sink
 * - Vector 3: ESP_LOG_INFO    -> first two false, third true      -> info sink
 * - Vector 4: ESP_LOG_DEBUG   -> all three false                  -> debug sink
 * - Vector 5: ESP_LOG_VERBOSE -> all three false                  -> debug sink
 * Vector 5 is not needed for MC/DC of the ladder, but it is what proves the
 * documented verbose-folds-into-debug behaviour rather than leaving it to
 * inspection.
 *
 * @pre The threshold admits every level.
 * @post The threshold is left at verbose.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_write_level_mapping(void)
{
  TEST_BEGIN("log level mapping");
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);

  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_ERROR, "H_t", "code %d", -3);
  t_assert_line("H_t", "ERROR", "code -3");

  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_WARN, "H_t", "retry %u", k_t_log_retry_arg);
  t_assert_line("H_t", "WARN", "retry 7");

  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_INFO, "H_t", "link %s", "up");
  t_assert_info_line("H_t", "link up");

  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_DEBUG, "H_t", "edge %x", k_t_log_edge_arg);
  t_assert_debug_line("H_t", "edge ab");

  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_VERBOSE, "H_t", "frame %u", 1U);
  t_assert_debug_line("H_t", "frame 1");
  TEST_END("log level mapping");
}

/**
 * @test test_log_write_guards
 *
 * @brief The writer drops a filtered level and a null format, and names a
 *        null tag.
 *
 * @details
 * A null tag would be dereferenced by the project logger, so the bridge
 * substitutes its own -- which is a substitution, not a drop, and the two are
 * distinguished here. A null format is a drop, because there is nothing to
 * format and emitting an empty line would be noise.
 *
 * @par MC/DC:
 * Decision A: `if (!ra8_esp_hosted_log_accepts(level) || (fmt == nullptr))` in
 * `port/esp-hosted/src/ra8_esp_hosted_log.c@ra8_esp_hosted_log_vwrite`
 * (2 conditions, 3 vectors)
 * - Vector A1: level admitted, fmt="x"  -> false, false -> false (control:
 *   the line is emitted)
 * - Vector A2: level filtered, fmt="x"  -> true          -> true  (varies the
 *   level only; the line is dropped)
 * - Vector A3: level admitted, fmt=null -> false, true   -> true  (varies the
 *   format only; the line is dropped)
 * A1+A2 prove the level check independently affects the outcome; A1+A3 prove
 * the same for the format check. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision B: `(tag == nullptr) ? k_ra8_esp_hosted_log_tag : tag`
 * (1 condition, 2 vectors)
 * - Vector B1: tag="H_t"  -> false; the caller's tag is used
 * - Vector B2: tag=null   -> true;  the bridge's own tag is used
 *
 * @pre The threshold can be moved freely by this test.
 * @post The threshold is left at verbose.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_write_guards(void)
{
  TEST_BEGIN("log write guards");
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_ERROR);

  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_ERROR, "H_t", "kept");
  t_assert_line("H_t", "ERROR", "kept");

  /* Above the threshold: dropped before any formatting happens. */
  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_WARN, "H_t", "dropped");
  t_assert_silent();

  /* Admitted level, but nothing to format. */
  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_ERROR, "H_t", nullptr);
  t_assert_silent();

  /* A null tag is replaced with the bridge's own rather than dereferenced. */
  t_sink_reset();
  ra8_esp_hosted_log_write((int)ESP_LOG_ERROR, nullptr, "anon");
  t_assert_line("C6LINK", "ERROR", "anon");

  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);
  TEST_END("log write guards");
}

/**
 * @test test_log_vwrite_direct
 *
 * @brief The list-taking entry point behaves identically to the varargs face.
 *
 * @details
 * Both the vtable's log row and the ``ESP_LOGx`` macros reach the bridge
 * through the list-taking form, so it is driven here directly rather than
 * only via ``ra8_esp_hosted_log_write``. If the two paths ever diverged, a
 * line emitted through the vtable would be filtered or framed differently
 * from one emitted directly, which is precisely the bug this asserts against.
 *
 * @par MC/DC:
 * Shares its only decision with ::test_log_write_guards -- the combined level
 * and null-format gate -- and drives the same three vectors through the
 * list-taking entry point:
 * - Vector 1: level admitted, fmt non-null -> the line is emitted
 * - Vector 2: level filtered, fmt non-null -> dropped
 * - Vector 3: level admitted, fmt null     -> dropped
 *
 * @pre The threshold can be moved freely by this test.
 * @post The threshold is left at verbose.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_vwrite_direct(void)
{
  TEST_BEGIN("log vwrite direct");
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_WARN);

  t_sink_reset();
  t_vwrite((int)ESP_LOG_WARN, "H_v", "n=%d s=%s", k_t_log_vwrite_int, "ok");
  t_assert_line("H_v", "WARN", "n=5 s=ok");

  t_sink_reset();
  t_vwrite((int)ESP_LOG_INFO, "H_v", "filtered");
  t_assert_silent();

  t_sink_reset();
  t_vwrite((int)ESP_LOG_WARN, "H_v", nullptr);
  t_assert_silent();

  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);
  TEST_END("log vwrite direct");
}

/**
 * @test test_log_hexdump
 *
 * @brief The hexdump renders two nibbles per byte and stops at its width.
 *
 * @details
 * Byte values below sixteen render as one digit, so the renderer prepends a
 * zero -- without which a dump would be ambiguous (``0f 05`` and ``f 5``
 * carry different byte counts at a glance). The dump width is a bound rather
 * than a preference: a caller handing over a whole 1600-byte frame gets the
 * first thirty-two bytes, not a stack overflow.
 *
 * @par MC/DC:
 * Decision A: `if (!ra8_esp_hosted_log_accepts(level) || (buf == nullptr) ||
 * (len == 0U))` in
 * `port/esp-hosted/src/ra8_esp_hosted_log.c@ra8_esp_hosted_log_hexdump`
 * (3 conditions, 4 vectors)
 * - Vector A1: admitted, buf=valid, len=3 -> false, false, false -> false
 *   (control: the dump is emitted)
 * - Vector A2: filtered, buf=valid, len=3 -> true                -> true
 *   (varies the level only)
 * - Vector A3: admitted, buf=null,  len=3 -> false, true         -> true
 *   (varies the buffer only)
 * - Vector A4: admitted, buf=valid, len=0 -> false, false, true  -> true
 *   (varies the length only)
 * A1 pairs with each of A2, A3 and A4 to prove that condition independently
 * affects the outcome. N+1 = 4 vectors for N=3: minimal MC/DC.
 *
 * Decision B: `if (shown > k_dump_bytes) { shown = k_dump_bytes; }`
 * (1 condition, 2 vectors)
 * - Vector B1: len=3  -> false; all three bytes render
 * - Vector B2: len=40 -> true;  exactly thirty-two render
 *
 * Decision C: `if (digits < k_dump_nibbles) { emit a leading zero; }`
 * (1 condition, 2 vectors)
 * - Vector C1: byte=0xA5 -> false; two digits already
 * - Vector C2: byte=0x00 -> true;  one digit, so a zero is prepended
 *
 * @pre The threshold can be moved freely by this test.
 * @post The threshold is left at verbose.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_hexdump(void)
{
  TEST_BEGIN("log hexdump");
  const uint8_t bytes[]                         = {0x00U, 0x0FU, 0xA5U};
  uint8_t       wide[(size_t)k_t_log_dump_over] = {};

  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);

  t_sink_reset();
  ra8_esp_hosted_log_hexdump((int)ESP_LOG_ERROR, "H_d", bytes, (uint32_t)sizeof(bytes));
  t_assert_line("H_d", "ERROR", "00 0f a5 ");

  /* Filtered level: nothing is rendered, not even an empty line. */
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_ERROR);
  t_sink_reset();
  ra8_esp_hosted_log_hexdump((int)ESP_LOG_WARN, "H_d", bytes, (uint32_t)sizeof(bytes));
  t_assert_silent();
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);

  t_sink_reset();
  ra8_esp_hosted_log_hexdump((int)ESP_LOG_ERROR, "H_d", nullptr, (uint32_t)sizeof(bytes));
  t_assert_silent();

  t_sink_reset();
  ra8_esp_hosted_log_hexdump((int)ESP_LOG_ERROR, "H_d", bytes, 0U);
  t_assert_silent();

  /* More bytes than one line renders: the excess is dropped, not wrapped. */
  for (uint32_t i = 0U; i < (uint32_t)k_t_log_dump_over; i++) {
    wide[i] = (uint8_t)i;
  }
  t_sink_reset();
  ra8_esp_hosted_log_hexdump((int)ESP_LOG_ERROR, "H_d", wide, (uint32_t)k_t_log_dump_over);
  {
    const size_t framing = strlen("[H_d] ERROR: ") + strlen("\r\n");
    const size_t payload = (size_t)k_t_log_dump_shown * (size_t)k_t_log_dump_per_byte;
    TEST_ASSERT_EQ((framing + payload), s_sink_len);
  }

  /* A null tag is replaced here too, on the same rule as the writer. */
  t_sink_reset();
  ra8_esp_hosted_log_hexdump((int)ESP_LOG_ERROR, nullptr, bytes, (uint32_t)sizeof(bytes));
  t_assert_line("C6LINK", "ERROR", "00 0f a5 ");
  TEST_END("log hexdump");
}

/**
 * @test test_log_mem_dump
 *
 * @brief The pool-state report names its caller and reads live statistics.
 *
 * @details
 * ``ra8_esp_hosted_mem_dump`` exists so a budgeting mistake is visible at the
 * moment it is cheapest to see, which means it has to read the real byte-pool
 * statistics rather than print a constant. The numbers themselves depend on
 * whether the pool has been created, so the assertion pins the framing and
 * the label -- including the substitution made for a null label -- rather
 * than a byte count that would make the test a fixture of pool sizing.
 *
 * @par MC/DC:
 * Decision: `(label == nullptr) ? "(unnamed)" : label` (1 condition, 2 vectors)
 * - Vector 1: label="probe"  -> false; the caller's label appears
 * - Vector 2: label=null     -> true;  the placeholder appears
 *
 * @pre The threshold admits informational lines.
 * @post The threshold is left at verbose.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_mem_dump(void)
{
  TEST_BEGIN("log mem dump");
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);

#if RA8_LOG_LEVEL >= RA8_LOG_LEVEL_INFO
  t_sink_reset();
  ra8_esp_hosted_mem_dump("probe");
  TEST_ASSERT(strstr(s_sink_buf, "[C6LINK] INFO: probe: pool free ") != nullptr);
  TEST_ASSERT(strstr(s_sink_buf, " fragments\r\n") != nullptr);

  t_sink_reset();
  ra8_esp_hosted_mem_dump(nullptr);
  TEST_ASSERT(strstr(s_sink_buf, "(unnamed): pool free ") != nullptr);
#else
  /* Informational lines are folded out of this build, so the observable
     contract is that the call is harmless and emits nothing. */
  t_sink_reset();
  ra8_esp_hosted_mem_dump("probe");
  t_assert_silent();
  ra8_esp_hosted_mem_dump(nullptr);
  t_assert_silent();
#endif
  TEST_END("log mem dump");
}

/**
 * @test test_log_alloc_failed
 *
 * @brief An exhausted pool reports both the requester and the size refused.
 *
 * @details
 * The two together are what size a pool without guesswork: the size alone
 * says how much was short, and the requester alone says who to look at, but
 * only the pair says how much to add and where. This runs at error level, so
 * it is present in every build regardless of ``RA8_LOG_LEVEL``.
 *
 * @par MC/DC:
 * Decision: `(func == nullptr) ? "(unnamed)" : func` (1 condition, 2 vectors)
 * - Vector 1: func="internal_h_malloc" -> false; the caller's name appears
 * - Vector 2: func=null                -> true;  the placeholder appears
 *
 * @pre The threshold admits error lines.
 * @post The threshold is left at verbose.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_log_alloc_failed(void)
{
  TEST_BEGIN("log alloc failed");
  ra8_esp_hosted_log_level_set(nullptr, (int)ESP_LOG_VERBOSE);

  t_sink_reset();
  ra8_esp_hosted_alloc_failed("internal_h_malloc", (size_t)k_t_log_alloc_size);
  t_assert_line("C6LINK", "ERROR", "internal_h_malloc: pool cannot serve 1600 bytes");

  t_sink_reset();
  ra8_esp_hosted_alloc_failed(nullptr, (size_t)0U);
  t_assert_line("C6LINK", "ERROR", "(unnamed): pool cannot serve 0 bytes");
  TEST_END("log alloc failed");
}

int32_t main(void)
{
  ra8_log_init();
  ra8_log_set_byte_sink(t_sink, nullptr);

  test_log_level_default_and_clamp();
  test_log_accepts_mcdc();
  test_log_write_level_mapping();
  test_log_write_guards();
  test_log_vwrite_direct();
  test_log_hexdump();
  test_log_mem_dump();
  test_log_alloc_failed();

  ra8_log_set_byte_sink(nullptr, nullptr);
  (void)fprintf(stderr, "[OK  ] test_ra8_esp_hosted_log.c\n");
  return 0;
}
