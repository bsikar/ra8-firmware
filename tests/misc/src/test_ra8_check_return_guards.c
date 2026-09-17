/**
 * @file test_ra8_check_return_guards.c
 * @brief Unit tests for the void, pointer, and alias validation guards.
 *
 * @details
 * `ra8_check.h` publishes an error-handling matrix whose non-``ra8_err_t``
 * arms had no caller and no test anywhere in the tree, so nothing ever
 * expanded ::RA8_RETURN_VOID_ON_ERROR, ::RA8_RETURN_NULL_ON_ERROR,
 * ::RA8_VALIDATE_PTR, or ::RA8_STATIC_ASSERT in any configuration. These
 * cases pin the contracts the header documents: the early return happens,
 * the success path falls through, the checked expression is evaluated
 * exactly once, the two-line failure log renders byte for byte, and the
 * documented alias really is ::RA8_CHECK_NULL_PTR.
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
 * @enum guard_capture_capacity_t
 * @brief Capacity of the exact log-line capture used by these guards.
 */
typedef enum : uint16_t {
  k_guard_capture_cap = 192U, /**< Includes the terminating NUL byte. */
} guard_capture_capacity_t;

/**
 * @enum guard_sentinel_t
 * @brief Sentinels proving which arm of a guard ran.
 */
typedef enum : uint32_t {
  k_guard_tail_unreached = 0U,          /**< Guard returned before the tail.  */
  k_guard_tail_reached   = 0xA5A5A5A5U, /**< Guard fell through to the tail.  */
} guard_sentinel_t;

/* `k_ra8_err_timeout` is 0x108, which the value emitter renders as decimal
 * 264; the expected log lines below hard-code that rendering on purpose so a
 * change to either the code or the emitter format fails here. */
RA8_STATIC_ASSERT(k_ra8_err_timeout == 0x108, "timeout code pins the log fixtures");
RA8_STATIC_ASSERT(k_ra8_ok == 0, "success must be the zero value");

static uint32_t s_err_evals   = 0U;
static uint32_t s_void_tail   = k_guard_tail_unreached;
static uint8_t  s_factory_obj = 0U;
static char     s_log[k_guard_capture_cap];
static uint32_t s_log_len     = 0U;
static uint32_t s_log_lines   = 0U;
static uint8_t  s_log_overflow = 0U;

/**
 * @brief Yield a status code while counting how often it was evaluated.
 * @param[in] err Status code to hand back unchanged.
 * @return @p err unchanged.
 * @pre ::s_err_evals is writable test-process state.
 * @post ::s_err_evals is incremented exactly once.
 * @note File-local seam for the documented single-evaluation guarantee.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_error_source(ra8_err_t err)
{
  ++s_err_evals;
  return err;
}

/**
 * @brief Capture every byte emitted by the guards' error logs.
 * @param[in,out] ctx Opaque sink context; unused by the static capture.
 * @param[in] byte Emitted log byte.
 * @pre The capture state is writable test-process storage.
 * @post @p byte is appended or ::s_log_overflow is set.
 * @post ::s_log_lines counts every emitted newline.
 * @note The newline count advances even after overflow so lost lines fail.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_byte_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  if (byte == (uint8_t)'\n') {
    ++s_log_lines;
  }
  if (s_log_len >= ((uint32_t)k_guard_capture_cap - 1U)) {
    s_log_overflow = 1U;
    return;
  }
  s_log[s_log_len] = (char)byte;
  ++s_log_len;
  s_log[s_log_len] = '\0';
}

/**
 * @brief Reset every counter and the log capture before a guard call.
 * @pre The capture state is not being written concurrently.
 * @post The captured string is empty and all counters are zero.
 * @note File-local single-threaded test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset(void)
{
  s_log[0]       = '\0';
  s_log_len      = 0U;
  s_log_lines    = 0U;
  s_log_overflow = 0U;
  s_err_evals    = 0U;
  s_void_tail    = k_guard_tail_unreached;
}

/**
 * @brief Void-returning helper guarded by ::RA8_RETURN_VOID_ON_ERROR.
 * @param[in] err Status code fed through ::internal_error_source.
 * @pre ::s_void_tail is writable test-process state.
 * @post ::s_void_tail is set only when @p err is ::k_ra8_ok.
 * @note Models the "error handling in void helpers" row of the matrix.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_void_helper(ra8_err_t err)
{
  RA8_RETURN_VOID_ON_ERROR(internal_error_source(err), "VOIDG", "bind failed");
  s_void_tail = k_guard_tail_reached;
}

/**
 * @brief Pointer-returning factory guarded by ::RA8_RETURN_NULL_ON_ERROR.
 * @param[in] err Status code fed through ::internal_error_source.
 * @return Factory result.
 * @retval &s_factory_obj @p err is ::k_ra8_ok.
 * @retval nullptr @p err is any error code.
 * @pre ::s_factory_obj is writable test-process state.
 * @post No object state is mutated on either arm.
 * @note Models the "pointer-returning factories" row of the matrix.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_factory(ra8_err_t err)
{
  RA8_RETURN_NULL_ON_ERROR(internal_error_source(err), "NULLG", "no slot free");
  return &s_factory_obj;
}

/**
 * @brief Precondition helper written with ::RA8_VALIDATE_PTR.
 * @param[in] ptr Pointer expression to test.
 * @return Validation result.
 * @retval k_ra8_ok @p ptr is non-null.
 * @retval k_ra8_err_null_ptr @p ptr is `nullptr`.
 * @pre None; the guard validates @p ptr.
 * @post One error line is emitted only on rejection.
 * @note Alias arm of the documented ::RA8_CHECK_NULL_PTR parity claim.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_alias(const void* ptr)
{
  RA8_VALIDATE_PTR(ptr, "ALIASG", "handle must not be nullptr");
  return k_ra8_ok;
}

/**
 * @brief Same precondition written with ::RA8_CHECK_NULL_PTR.
 * @param[in] ptr Pointer expression to test.
 * @return Validation result.
 * @retval k_ra8_ok @p ptr is non-null.
 * @retval k_ra8_err_null_ptr @p ptr is `nullptr`.
 * @pre None; the guard validates @p ptr.
 * @post One error line is emitted only on rejection.
 * @note Reference arm the alias must match byte for byte.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_reference(const void* ptr)
{
  RA8_CHECK_NULL_PTR(ptr, "ALIASG", "handle must not be nullptr");
  return k_ra8_ok;
}

/**
 * @brief Verify the void guard returns early and logs the documented pair.
 * @details
 * Runs the success arm first to prove the tail is reached with no logging,
 * then the failure arm to prove the tail is skipped, the status expression
 * ran exactly once, and both the message line and the value line rendered.
 * @pre Test-process static state is writable.
 * @post Both arms of the guard's single decision have been exercised.
 * @note One single-condition decision, covered in both directions.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `ra8_err_is_error(err_rc_)` inside
 * ::RA8_RETURN_VOID_ON_ERROR has one condition. ::k_ra8_ok covers false;
 * ::k_ra8_err_timeout covers true.
 */
RA8_INTERNAL static void internal_test_void_guard(void)
{
  TEST_BEGIN("void guard returns early and logs message plus value");
  ra8_log_set_byte_sink(internal_log_byte_sink, nullptr);

  internal_reset();
  internal_void_helper(k_ra8_ok);
  TEST_ASSERT_EQ(k_guard_tail_reached, s_void_tail);
  TEST_ASSERT_EQ(1U, s_err_evals);
  TEST_ASSERT_EQ(0U, s_log_len);
  TEST_ASSERT_EQ(0U, s_log_lines);

  internal_reset();
  internal_void_helper(k_ra8_err_timeout);
  TEST_ASSERT_EQ(k_guard_tail_unreached, s_void_tail);
  TEST_ASSERT_EQ(1U, s_err_evals);
  TEST_ASSERT_EQ(0U, s_log_overflow);
  TEST_ASSERT_EQ(2U, s_log_lines);
  TEST_ASSERT(strcmp(s_log, "[VOIDG] ERROR: bind failed\r\n[VOIDG] ERROR: Error=264\r\n") == 0);

  ra8_log_set_byte_sink(nullptr, nullptr);
  TEST_END("void guard returns early and logs message plus value");
}

/**
 * @brief Verify the pointer guard yields `nullptr` only on the error arm.
 * @details
 * The success arm must hand back the factory object itself, not merely a
 * non-null address, so a guard that swallowed the result would fail.
 * @pre Test-process static state is writable.
 * @post Both arms of the guard's single decision have been exercised.
 * @note One single-condition decision, covered in both directions.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `ra8_err_is_error(err_rc_)` inside
 * ::RA8_RETURN_NULL_ON_ERROR has one condition. ::k_ra8_ok covers false;
 * ::k_ra8_err_timeout covers true.
 */
RA8_INTERNAL static void internal_test_null_guard(void)
{
  TEST_BEGIN("pointer guard yields nullptr only on the error arm");
  ra8_log_set_byte_sink(internal_log_byte_sink, nullptr);

  internal_reset();
  TEST_ASSERT(internal_factory(k_ra8_ok) == (void*)&s_factory_obj);
  TEST_ASSERT_EQ(1U, s_err_evals);
  TEST_ASSERT_EQ(0U, s_log_len);
  TEST_ASSERT_EQ(0U, s_log_lines);

  internal_reset();
  TEST_ASSERT_NULL(internal_factory(k_ra8_err_timeout));
  TEST_ASSERT_EQ(1U, s_err_evals);
  TEST_ASSERT_EQ(0U, s_log_overflow);
  TEST_ASSERT_EQ(2U, s_log_lines);
  TEST_ASSERT(strcmp(s_log, "[NULLG] ERROR: no slot free\r\n[NULLG] ERROR: Error=264\r\n") == 0);

  ra8_log_set_byte_sink(nullptr, nullptr);
  TEST_END("pointer guard yields nullptr only on the error arm");
}

/**
 * @brief Verify ::RA8_VALIDATE_PTR is the documented alias, not a lookalike.
 * @details
 * Captures the alias line and the ::RA8_CHECK_NULL_PTR line from identical
 * call sites and compares them, so a divergent tag, message, code, or line
 * count fails rather than passing on "both rejected nullptr".
 * @pre Test-process static state is writable.
 * @post Both arms of the guard's single decision have been exercised.
 * @note One single-condition decision, covered in both directions.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `((ptr) == nullptr)` inside ::RA8_CHECK_NULL_PTR, reached
 * through ::RA8_VALIDATE_PTR, has one condition. A valid object covers
 * false; `nullptr` covers true.
 */
RA8_INTERNAL static void internal_test_validate_ptr_alias(void)
{
  TEST_BEGIN("validate-ptr alias matches the null-ptr guard byte for byte");
  char alias_line[k_guard_capture_cap];
  uint8_t object = 0U;
  ra8_log_set_byte_sink(internal_log_byte_sink, nullptr);

  internal_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_validate_alias(&object));
  TEST_ASSERT_EQ(0U, s_log_len);
  TEST_ASSERT_EQ(0U, s_log_lines);

  internal_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, internal_validate_alias(nullptr));
  TEST_ASSERT_EQ(1U, s_log_lines);
  TEST_ASSERT_EQ(0U, s_log_overflow);
  (void)memcpy(alias_line, s_log, (size_t)s_log_len + 1U);

  internal_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, internal_validate_reference(nullptr));
  TEST_ASSERT_EQ(1U, s_log_lines);
  TEST_ASSERT(strcmp(s_log, alias_line) == 0);
  TEST_ASSERT(strcmp(s_log, "[ALIASG] ERROR: handle must not be nullptr\r\n") == 0);

  ra8_log_set_byte_sink(nullptr, nullptr);
  TEST_END("validate-ptr alias matches the null-ptr guard byte for byte");
}

/**
 * @brief Verify ::RA8_STATIC_ASSERT accepts a true compile-time invariant.
 * @details
 * The file-scope assertions above already fail the build on a false
 * condition; this case exercises the block-scope expansion and re-checks
 * the same invariants at runtime so the contract is visible in the report.
 * @pre None.
 * @post No test-process state is modified.
 * @note Compile-time coverage; the runtime checks mirror the conditions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_static_assert(void)
{
  TEST_BEGIN("static assert wrapper holds at block scope");
  RA8_STATIC_ASSERT(sizeof(ra8_err_t) >= 2U, "ra8_err_t holds the 0x5xx codes");
  RA8_STATIC_ASSERT(k_ra8_err_null_ptr != k_ra8_ok, "null-ptr code is an error");

  TEST_ASSERT(sizeof(ra8_err_t) >= 2U);
  TEST_ASSERT(ra8_err_is_error(k_ra8_err_null_ptr));
  TEST_ASSERT(!ra8_err_is_error(k_ra8_ok));
  TEST_END("static assert wrapper holds at block scope");
}

int main(void)
{
  internal_test_void_guard();
  internal_test_null_guard();
  internal_test_validate_ptr_alias();
  internal_test_static_assert();
  return 0;
}
