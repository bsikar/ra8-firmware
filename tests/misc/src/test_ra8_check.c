/**
 * @file test_ra8_check.c
 * @brief Unit tests for shared return-code-parameterized validation guards.
 *
 * @details
 * Exercises the foreign-callback null guard in both directions and proves its
 * caller-supplied failure expression is evaluated only on rejection.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "unity_minimal.h"

/**
 * @enum guard_fixture_status_t
 * @brief Non-::ra8_err_t statuses modeling a foreign callback ABI.
 */
typedef enum : int32_t {
  k_guard_fixture_success = 41,  /**< Callback-specific success value.          */
  k_guard_fixture_failure = -73, /**< Callback-specific invalid-argument value. */
} guard_fixture_status_t;

/**
 * @enum guard_fixture_capacity_t
 * @brief Capacity of the exact log-line capture used by the guard test.
 */
typedef enum : uint16_t {
  k_guard_log_capture_cap = 128U, /**< Includes the terminating NUL byte. */
} guard_fixture_capacity_t;

static int32_t  s_failure_value_calls = 0;
static int32_t  s_pointer_value_calls = 0;
static char     s_log_capture[k_guard_log_capture_cap];
static uint32_t s_log_capture_len = 0U;
static uint32_t s_log_line_count  = 0U;
static uint8_t  s_log_overflowed  = 0U;

/**
 * @brief Produce the foreign callback's null-argument status.
 * @return Callback-specific failure status.
 * @retval k_guard_fixture_failure Always.
 * @pre ::s_failure_value_calls is writable test-process state.
 * @post ::s_failure_value_calls is incremented exactly once.
 * @note File-local test seam for failure-expression evaluation.
 * @since 0.1.0
 */
RA8_INTERNAL static guard_fixture_status_t internal_failure_status(void)
{
  ++s_failure_value_calls;
  return k_guard_fixture_failure;
}

/**
 * @brief Produce the pointer expression consumed by the shared guard.
 * @param[in] input Pointer value to return unchanged.
 * @return @p input unchanged.
 * @pre ::s_pointer_value_calls is writable test-process state.
 * @post ::s_pointer_value_calls is incremented exactly once.
 * @note File-local test seam for pointer-expression evaluation.
 * @since 0.1.0
 */
RA8_INTERNAL static const void* internal_pointer_value(const void* input)
{
  ++s_pointer_value_calls;
  return input;
}

/**
 * @brief Capture every byte emitted by the validation guard's error log.
 * @param[in,out] ctx Opaque sink context; unused by the static capture.
 * @param[in] byte Emitted log byte.
 * @pre The capture state is writable test-process storage.
 * @post @p byte is appended or ::s_log_overflowed is set.
 * @post ::s_log_line_count counts every emitted newline.
 * @note The newline count advances even after overflow so duplicate lines fail.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_byte_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  if (byte == (uint8_t)'\n') {
    ++s_log_line_count;
  }
  if (s_log_capture_len >= ((uint32_t)k_guard_log_capture_cap - 1U)) {
    s_log_overflowed = 1U;
    return;
  }
  s_log_capture[s_log_capture_len] = (char)byte;
  ++s_log_capture_len;
  s_log_capture[s_log_capture_len] = '\0';
}

/**
 * @brief Reset the exact log-line capture before a guard call.
 * @pre The capture state is not being written concurrently.
 * @post The captured string is empty and all counters are zero.
 * @note File-local single-threaded test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_log_capture(void)
{
  s_log_capture[0]  = '\0';
  s_log_capture_len = 0U;
  s_log_line_count  = 0U;
  s_log_overflowed  = 0U;
}

/**
 * @brief Verify the log capture remains bounded after it fills.
 * @pre The capture state is not being written concurrently.
 * @post The capture is full, NUL-terminated, and marked overflowed.
 * @post A newline emitted after overflow is still counted.
 * @note Exercises the overflow arm independently of the validation guard.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_log_capture_overflow(void)
{
  TEST_BEGIN("log capture remains bounded after overflow");
  internal_reset_log_capture();

  for (uint32_t index = 0U; index < (uint32_t)k_guard_log_capture_cap; ++index) {
    internal_log_byte_sink(nullptr, (uint8_t)'x');
  }
  internal_log_byte_sink(nullptr, (uint8_t)'\n');

  TEST_ASSERT_EQ(1U, s_log_overflowed);
  TEST_ASSERT_EQ(k_guard_log_capture_cap - 1U, s_log_capture_len);
  TEST_ASSERT_EQ('\0', s_log_capture[k_guard_log_capture_cap - 1U]);
  TEST_ASSERT_EQ(1U, s_log_line_count);
  TEST_END("log capture remains bounded after overflow");
}

/**
 * @brief Model an externally specified callback with non-RA8 status values.
 * @param[in] input Required callback input.
 * @return Callback-specific result.
 * @retval k_guard_fixture_success @p input is non-null.
 * @retval k_guard_fixture_failure @p input is `nullptr`.
 * @pre None; the guard validates @p input.
 * @post The failure expression is evaluated only for a null input.
 * @note File-local adapter used to exercise ::RA8_CHECK_NULL_PTR_RETURN.
 * @since 0.1.0
 */
RA8_INTERNAL static guard_fixture_status_t internal_foreign_callback(const void* input)
{
  RA8_CHECK_NULL_PTR_RETURN(internal_pointer_value(input),
                            internal_failure_status(),
                            "CHECK_TEST",
                            "input must not be nullptr");
  return k_guard_fixture_success;
}

/**
 * @brief Exercise the guard with a caller identifier once used by its expansion.
 * @param[in] ra8_checked_ptr Required callback input.
 * @return Callback-specific result.
 * @retval k_guard_fixture_success @p ra8_checked_ptr is non-null.
 * @retval k_guard_fixture_failure @p ra8_checked_ptr is `nullptr`.
 * @pre None; the guard validates @p ra8_checked_ptr.
 * @post The caller identifier is evaluated without macro-local shadowing.
 * @note This collision regression keeps macro expansion hygienic.
 * @since 0.1.0
 */
RA8_INTERNAL static guard_fixture_status_t
internal_foreign_callback_collision(const void* ra8_checked_ptr)
{
  RA8_CHECK_NULL_PTR_RETURN(ra8_checked_ptr,
                            internal_failure_status(),
                            "CHECK_TEST",
                            "collision input must not be nullptr");
  return k_guard_fixture_success;
}

/**
 * @brief Verify the parameterized null guard accepts and rejects correctly.
 * @details
 * Runs the non-null arm first to prove the failure expression stays quiet,
 * then runs the null arm to prove the exact foreign status is returned once.
 * @pre Test-process static state is writable.
 * @pre ::internal_foreign_callback uses the shared validation authority.
 * @post Both guard arms have been exercised and the pointer expression has
 * executed exactly once on each call.
 * @post The failure expression and error log execute only on rejection.
 * @note One single-condition decision is exercised in both directions.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `((ptr) == nullptr)` inside
 * ::RA8_CHECK_NULL_PTR_RETURN has one
 * condition. A valid object covers false; `nullptr` covers true.
 */
RA8_INTERNAL static void internal_test_parameterized_null_guard(void)
{
  TEST_BEGIN("parameterized null guard preserves foreign status ABI");
  uint8_t valid = 0U;
  ra8_log_set_byte_sink(internal_log_byte_sink, nullptr);

  s_failure_value_calls = 0;
  s_pointer_value_calls = 0;
  internal_reset_log_capture();

  TEST_ASSERT_EQ(k_guard_fixture_success, internal_foreign_callback(&valid));
  TEST_ASSERT_EQ(1, s_pointer_value_calls);
  TEST_ASSERT_EQ(0, s_failure_value_calls);
  TEST_ASSERT_EQ(0U, s_log_capture_len);
  TEST_ASSERT_EQ(0U, s_log_line_count);

  s_pointer_value_calls = 0;
  TEST_ASSERT_EQ(k_guard_fixture_failure, internal_foreign_callback(nullptr));
  TEST_ASSERT_EQ(1, s_pointer_value_calls);
  TEST_ASSERT_EQ(1, s_failure_value_calls);
  TEST_ASSERT_EQ(0U, s_log_overflowed);
  TEST_ASSERT_EQ(1U, s_log_line_count);
  TEST_ASSERT(strcmp(s_log_capture, "[CHECK_TEST] ERROR: input must not be nullptr\r\n") == 0);

  s_failure_value_calls = 0;
  internal_reset_log_capture();
  TEST_ASSERT_EQ(k_guard_fixture_success, internal_foreign_callback_collision(&valid));
  TEST_ASSERT_EQ(0, s_failure_value_calls);
  TEST_ASSERT_EQ(0U, s_log_capture_len);
  TEST_ASSERT_EQ(0U, s_log_line_count);

  ra8_log_set_byte_sink(nullptr, nullptr);

  TEST_END("parameterized null guard preserves foreign status ABI");
}

int main(void)
{
  internal_test_log_capture_overflow();
  internal_test_parameterized_null_guard();
  return 0;
}
