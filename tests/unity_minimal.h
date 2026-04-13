/**
 * @file unity_minimal.h
 * @brief Minimal Unity-style test macros for the ra8d2-firmware test build
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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Fail the current test with a formatted message.
 */
#define TEST_FAIL_FMT(fmt, ...)                                                                    \
  do {                                                                                             \
    (void)fprintf(stderr, "[FAIL] %s:%d " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);              \
    (void)fflush(stderr);                                                                          \
    exit(1);                                                                                       \
  } while (0)

/** @brief Assert that `cond` is truthy. */
#define TEST_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      TEST_FAIL_FMT("%s", "assertion failed: " #cond);                                             \
    }                                                                                              \
  } while (0)

/** @brief Assert integer equality. */
#define TEST_ASSERT_EQ(expected, actual)                                                           \
  do {                                                                                             \
    const int64_t _e = (int64_t)(expected);                                                        \
    const int64_t _a = (int64_t)(actual);                                                          \
    if (_e != _a) {                                                                                \
      TEST_FAIL_FMT("expected %" PRId64 ", got %" PRId64, _e, _a);                                 \
    }                                                                                              \
  } while (0)

/** @brief Assert pointer non-null. */
#define TEST_ASSERT_NOT_NULL(ptr)                                                                  \
  do {                                                                                             \
    if ((ptr) == NULL) {                                                                           \
      TEST_FAIL_FMT("%s", "expected non-null pointer: " #ptr);                                     \
    }                                                                                              \
  } while (0)

/** @brief Assert pointer is null. */
#define TEST_ASSERT_NULL(ptr)                                                                      \
  do {                                                                                             \
    if ((ptr) != NULL) {                                                                           \
      TEST_FAIL_FMT("%s", "expected null pointer: " #ptr);                                         \
    }                                                                                              \
  } while (0)

/** @brief Declare and mark the start of a test function. */
#define TEST_BEGIN(name)                                                                           \
  (void)fprintf(stderr, "[RUN ] %s\n", name);                                                      \
  do {                                                                                             \
  } while (0)

/** @brief Mark a test function as passed. */
#define TEST_END(name)                                                                             \
  (void)fprintf(stderr, "[PASS] %s\n", name);                                                      \
  do {                                                                                             \
  } while (0)
