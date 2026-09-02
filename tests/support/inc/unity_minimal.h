/**
 * @file unity_minimal.h
 * @brief Minimal Unity-style test macros for the ra8-firmware test build
 *
 * @details
 * A tiny, dependency-free subset of ThrowTheSwitch/Unity's assertion
 * API. Provides just enough to write test_*.c files that exercise
 * the host-compiled library. No test runner, no group fixtures --
 * each TU has its own `main()` that calls its individual test
 * functions and returns 0 on success, non-zero on first failure.
 *
 * Vendoring the real Unity is overkill for a handful of unit tests
 * and would pull in a hundred files of boilerplate. If the test
 * suite ever grows enough to justify it, the real Unity can drop in
 * next to this file and the macros will still work.
 *
 * @par Why the assertions are function calls, not `do { if ... } while (0)`:
 * Every `TEST_ASSERT*` expands at the call site, so a macro carrying an
 * `if` injects a branch into the *caller's* body. Static analysis reads
 * the post-expansion AST, so a flat 20-assertion test measured as 20
 * branches and 60+ statements -- `readability-function-size` and
 * `readability-function-cognitive-complexity` fired on essentially every
 * test in the suite while the code a human reads is a straight line with
 * no control flow at all. Keeping the branch inside a `static inline`
 * helper makes each assertion exactly one statement and zero branches in
 * the caller, so the metrics measure the test that was written rather
 * than the macro that was expanded. The runtime behaviour (print the
 * failure with the caller's file/line and `exit(1)`) is unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_test_output.h"

/**
 * @enum unity_minimal_limit_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : size_t {
  k_unity_minimal_detail_cap = 256U, /**< Per-assertion detail string capacity. */
} unity_minimal_limit_t;

/**
 * @brief Mark a formatted failure detail whose complete text exceeded its
 * buffer.
 * @details Replaces the tail of an incomplete bounded format result with a
 * fixed marker so a diagnostic can never present clipped text as complete.
 * @param[in,out] detail Bounded failure-detail buffer.
 * @param[in] formatted_length Length required by the preceding `snprintf` call.
 * @pre @p detail has ::k_unity_minimal_detail_cap writable bytes.
 * @pre @p detail contains the bounded result of the preceding format operation.
 * @post A fitting result is unchanged.
 * @post A truncated result ends in `...[truncated]` and remains NUL-terminated.
 * @note A negative formatting result is handled by TEST_FAIL_FMT before this call.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void
internal_test_mark_truncated(char detail[k_unity_minimal_detail_cap], int formatted_length)
{
  if (formatted_length < (int)k_unity_minimal_detail_cap) {
    return;
  }
  const char   marker[] = "...[truncated]";
  const size_t offset   = (size_t)k_unity_minimal_detail_cap - sizeof(marker);
  (void)memcpy(&detail[offset], marker, sizeof(marker));
}

/**
 * @brief Fail the current test with a formatted message.
 *
 * @details
 * Writes `[FAIL] <file>:<line> <formatted message>` to raw descriptor 2
 * and terminates the process with exit status 1. Stays a macro (rather
 * than a variadic function) so the caller's `__FILE__` / `__LINE__` are
 * captured and so the format string keeps its compile-time `-Wformat`
 * checking without a `[[gnu::format]]` attribute.
 *
 * @param[in] fmt `printf`-style format string for the failure detail.
 * @param[in] ... Arguments consumed by @p fmt.
 *
 * @note Never returns.
 */
#define TEST_FAIL_FMT(fmt, ...)                                                                    \
  do {                                                                                             \
    char local_detail[k_unity_minimal_detail_cap];                                                 \
    int  local_length = snprintf(local_detail, sizeof(local_detail), (fmt), __VA_ARGS__);          \
    if (local_length < 0) {                                                                        \
      internal_test_fail(__FILE__, __LINE__, "failure detail formatting failed");                  \
    }                                                                                              \
    internal_test_mark_truncated(local_detail, local_length);                                      \
    internal_test_fail(__FILE__, __LINE__, local_detail);                                          \
  } while (0)

/**
 * @brief Report an assertion failure raised by one of the `ra8_test_*` helpers.
 *
 * @details
 * Shared failure path for every assertion helper below: writes the
 * caller's source location plus @p detail and terminates the process.
 * Kept separate so each helper carries exactly one branch.
 *
 * @param[in] file Source file of the failing assertion (`__FILE__`).
 * @param[in] line Source line of the failing assertion (`__LINE__`).
 * @param[in] detail Human-readable description of what was expected.
 *
 * @pre @p file is a non-null NUL-terminated string.
 * @pre @p detail is a non-null NUL-terminated string.
 * @post Never returns; the process exits with status 1.
 * @post Any descriptor failure remains confined to the diagnostic path.
 *
 * @note Uses only caller-local adapter state and borrowed descriptor 2.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_test_fail(const char* file, int line, const char* detail)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (internal_test_output_fd_init(&output, &state, STDERR_FILENO)) {
    (void)internal_test_output_text(&output, "[FAIL] ");
    (void)internal_test_output_text(&output, file);
    (void)internal_test_output_text(&output, ":");
    (void)internal_test_output_i64(&output, (int64_t)line);
    (void)internal_test_output_text(&output, " ");
    (void)internal_test_output_text(&output, detail);
    (void)internal_test_output_text(&output, "\n");
  }
  exit(1);
}

/**
 * @brief Report one integer-equality failure without a formatting stream.
 * @details Writes the source location and both signed values through one
 * caller-local descriptor sink before terminating the test process.
 * @param[in] expected Expected signed value.
 * @param[in] actual Observed signed value.
 * @param[in] file Source file of the failing assertion.
 * @param[in] line Source line of the failing assertion.
 * @pre @p file is a non-null NUL-terminated string.
 * @pre Both values are represented exactly in `int64_t`.
 * @post Never returns; the process exits with status 1.
 * @post Any descriptor failure is contained to the diagnostic path.
 * @note Uses caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void
internal_test_fail_eq(int64_t expected, int64_t actual, const char* file, int line)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (internal_test_output_fd_init(&output, &state, STDERR_FILENO)) {
    (void)internal_test_output_text(&output, "[FAIL] ");
    (void)internal_test_output_text(&output, file);
    (void)internal_test_output_text(&output, ":");
    (void)internal_test_output_i64(&output, (int64_t)line);
    (void)internal_test_output_text(&output, " expected ");
    (void)internal_test_output_i64(&output, expected);
    (void)internal_test_output_text(&output, ", got ");
    (void)internal_test_output_i64(&output, actual);
    (void)internal_test_output_text(&output, "\n");
  }
  exit(1);
}

/**
 * @brief Assert that a condition holds.
 * @details Returns directly on success and delegates the complete failure
 * diagnostic and process exit to ::internal_test_fail otherwise.
 *
 * @param[in] cond Condition result; false aborts the test.
 * @param[in] detail Stringified condition, used in the failure message.
 * @param[in] file Source file of the assertion (`__FILE__`).
 * @param[in] line Source line of the assertion (`__LINE__`).
 *
 * @pre @p detail is a non-null NUL-terminated string.
 * @pre @p file is a non-null NUL-terminated string.
 * @post Returns only when @p cond is true.
 * @post The process has exited with status 1 when @p cond is false.
 *
 * @note Failure uses only caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void
internal_test_assert_true(bool cond, const char* detail, const char* file, int line)
{
  if (!cond) {
    internal_test_fail(file, line, detail);
  }
}

/**
 * @brief Assert that two integers compare equal.
 * @details Performs one exact signed comparison and delegates a mismatch to
 * the typed equality-failure renderer.
 *
 * @param[in] expected Expected value, widened to `int64_t` by the caller.
 * @param[in] actual Observed value, widened to `int64_t` by the caller.
 * @param[in] file Source file of the assertion (`__FILE__`).
 * @param[in] line Source line of the assertion (`__LINE__`).
 *
 * @pre @p file is a non-null NUL-terminated string.
 * @pre Both values fit `int64_t` (the caller's cast is lossless).
 * @post Returns only when @p expected equals @p actual.
 * @post The process has exited with status 1 on mismatch.
 *
 * @note Failure uses only caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void
internal_test_assert_eq(int64_t expected, int64_t actual, const char* file, int line)
{
  if (expected != actual) {
    internal_test_fail_eq(expected, actual, file, line);
  }
}

/**
 * @brief Assert that a pointer is non-null.
 * @details Compares only the address, preserving pointer qualifiers, and uses
 * the caller's stringified expression in the failure diagnostic.
 *
 * @param[in] ptr Pointer under test (any qualifier combination; the helper
 *                only compares the address, so `volatile` register pointers
 *                are accepted without discarding qualifiers).
 * @param[in] detail Stringified expression, used in the failure message.
 * @param[in] file Source file of the assertion (`__FILE__`).
 * @param[in] line Source line of the assertion (`__LINE__`).
 *
 * @pre @p detail is a non-null NUL-terminated string.
 * @pre @p file is a non-null NUL-terminated string.
 * @post Returns only when @p ptr is non-null.
 * @post The process has exited with status 1 when @p ptr is null.
 *
 * @note Failure uses only caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_test_assert_not_null(const volatile void* ptr,
                                                              const char*          detail,
                                                              const char*          file,
                                                              int                  line)
{
  if (ptr == nullptr) {
    internal_test_fail(file, line, detail);
  }
}

/**
 * @brief Assert that a pointer is null.
 * @details Compares only the address, preserving pointer qualifiers, and uses
 * the caller's stringified expression in the failure diagnostic.
 *
 * @param[in] ptr Pointer under test (any qualifier combination; the helper
 *                only compares the address, so `volatile` register pointers
 *                are accepted without discarding qualifiers).
 * @param[in] detail Stringified expression, used in the failure message.
 * @param[in] file Source file of the assertion (`__FILE__`).
 * @param[in] line Source line of the assertion (`__LINE__`).
 *
 * @pre @p detail is a non-null NUL-terminated string.
 * @pre @p file is a non-null NUL-terminated string.
 * @post Returns only when @p ptr is null.
 * @post The process has exited with status 1 when @p ptr is non-null.
 *
 * @note Failure uses only caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void
internal_test_assert_null(const volatile void* ptr, const char* detail, const char* file, int line)
{
  if (ptr != nullptr) {
    internal_test_fail(file, line, detail);
  }
}

/**
 * @brief Announce that a named test case is starting.
 * @details Composes the exact `[RUN ]` line through stack-local descriptor
 * adapter state; an unavailable diagnostic descriptor does not fail the test.
 *
 * @param[in] name Test-case name printed after the `[RUN ]` tag.
 *
 * @pre @p name is a non-null NUL-terminated string.
 * @pre Raw descriptor 2 is available when the diagnostic must be observed.
 * @post One `[RUN ]` line has been attempted on raw descriptor 2.
 * @post No process-global output state is changed.
 *
 * @note Uses only caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_test_begin(const char* name)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (internal_test_output_fd_init(&output, &state, STDERR_FILENO)) {
    (void)internal_test_output_text(&output, "[RUN ] ");
    (void)internal_test_output_text(&output, name);
    (void)internal_test_output_text(&output, "\n");
  }
}

/**
 * @brief Announce that a named test case passed.
 * @details Composes the exact `[PASS]` line through stack-local descriptor
 * adapter state; an unavailable diagnostic descriptor does not fail the test.
 *
 * @param[in] name Test-case name printed after the `[PASS]` tag.
 *
 * @pre @p name is a non-null NUL-terminated string.
 * @pre Raw descriptor 2 is available when the diagnostic must be observed.
 * @post One `[PASS]` line has been attempted on raw descriptor 2.
 * @post No process-global output state is changed.
 *
 * @note Uses only caller-local raw-descriptor adapter state.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_test_end(const char* name)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (internal_test_output_fd_init(&output, &state, STDERR_FILENO)) {
    (void)internal_test_output_text(&output, "[PASS] ");
    (void)internal_test_output_text(&output, name);
    (void)internal_test_output_text(&output, "\n");
  }
}

/** @brief Assert that `cond` is truthy. */
#define TEST_ASSERT(cond)                                                                          \
  internal_test_assert_true((cond), "assertion failed: " #cond, __FILE__, __LINE__)

/** @brief Assert integer equality. */
#define TEST_ASSERT_EQ(expected, actual)                                                           \
  internal_test_assert_eq((int64_t)(expected), (int64_t)(actual), __FILE__, __LINE__)

/** @brief Assert pointer non-null. */
#define TEST_ASSERT_NOT_NULL(ptr)                                                                  \
  internal_test_assert_not_null((ptr), "expected non-null pointer: " #ptr, __FILE__, __LINE__)

/** @brief Assert pointer is null. */
#define TEST_ASSERT_NULL(ptr)                                                                      \
  internal_test_assert_null((ptr), "expected null pointer: " #ptr, __FILE__, __LINE__)

/** @brief Declare and mark the start of a test function. */
#define TEST_BEGIN(name) internal_test_begin(name)

/** @brief Mark a test function as passed. */
#define TEST_END(name) internal_test_end(name)
