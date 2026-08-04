/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
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
 * failure with the caller's file/line, flush, `exit(1)`) is unchanged.
 */

#pragma once

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @enum unity_minimal_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_unity_minimal_detail_cap = 64, /**< Per-assertion detail string capacity. */
} unity_minimal_uint8_const_t;

/**
 * @brief Fail the current test with a formatted message.
 *
 * @details
 * Prints `[FAIL] <file>:<line> <formatted message>` to stderr, flushes,
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
    (void)fprintf(stderr, "[FAIL] %s:%d " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);              \
    (void)fflush(stderr);                                                                          \
    exit(1);                                                                                       \
  } while (0)

/**
 * @brief Report an assertion failure raised by one of the `ra8_test_*` helpers.
 *
 * @details
 * Shared failure path for every assertion helper below: prints the
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
 *
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void ra8_test_fail(const char* file, int line, const char* detail)
{
  (void)fprintf(stderr, "[FAIL] %s:%d %s\n", file, line, detail);
  (void)fflush(stderr);
  exit(1);
}

/**
 * @brief Assert that a condition holds.
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
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void ra8_test_assert_true(bool cond, const char* detail, const char* file, int line)
{
  if (!cond) {
    ra8_test_fail(file, line, detail);
  }
}

/**
 * @brief Assert that two integers compare equal.
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
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void ra8_test_assert_eq(int64_t expected, int64_t actual, const char* file, int line)
{
  if (expected != actual) {
    char detail[k_unity_minimal_detail_cap];
    (void)snprintf(detail, sizeof detail, "expected %" PRId64 ", got %" PRId64, expected, actual);
    ra8_test_fail(file, line, detail);
  }
}

/**
 * @brief Assert that a pointer is non-null.
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
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void
ra8_test_assert_not_null(const volatile void* ptr, const char* detail, const char* file, int line)
{
  if (ptr == nullptr) {
    ra8_test_fail(file, line, detail);
  }
}

/**
 * @brief Assert that a pointer is null.
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
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void
ra8_test_assert_null(const volatile void* ptr, const char* detail, const char* file, int line)
{
  if (ptr != nullptr) {
    ra8_test_fail(file, line, detail);
  }
}

/**
 * @brief Announce that a named test case is starting.
 *
 * @param[in] name Test-case name printed after the `[RUN ]` tag.
 *
 * @pre @p name is a non-null NUL-terminated string.
 * @pre stderr is open.
 * @post One `[RUN ]` line has been written to stderr.
 * @post The stream position of stderr has advanced.
 *
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void ra8_test_begin(const char* name)
{
  (void)fprintf(stderr, "[RUN ] %s\n", name);
}

/**
 * @brief Announce that a named test case passed.
 *
 * @param[in] name Test-case name printed after the `[PASS]` tag.
 *
 * @pre @p name is a non-null NUL-terminated string.
 * @pre stderr is open.
 * @post One `[PASS]` line has been written to stderr.
 * @post The stream position of stderr has advanced.
 *
 * @note Not thread-safe (writes to the shared stderr stream).
 */
static inline void ra8_test_end(const char* name)
{
  (void)fprintf(stderr, "[PASS] %s\n", name);
}

/** @brief Assert that `cond` is truthy. */
#define TEST_ASSERT(cond)                                                                          \
  ra8_test_assert_true((cond), "assertion failed: " #cond, __FILE__, __LINE__)

/** @brief Assert integer equality. */
#define TEST_ASSERT_EQ(expected, actual)                                                           \
  ra8_test_assert_eq((int64_t)(expected), (int64_t)(actual), __FILE__, __LINE__)

/** @brief Assert pointer non-null. */
#define TEST_ASSERT_NOT_NULL(ptr)                                                                  \
  ra8_test_assert_not_null((ptr), "expected non-null pointer: " #ptr, __FILE__, __LINE__)

/** @brief Assert pointer is null. */
#define TEST_ASSERT_NULL(ptr)                                                                      \
  ra8_test_assert_null((ptr), "expected null pointer: " #ptr, __FILE__, __LINE__)

/** @brief Declare and mark the start of a test function. */
#define TEST_BEGIN(name) ra8_test_begin(name)

/** @brief Mark a test function as passed. */
#define TEST_END(name) ra8_test_end(name)
