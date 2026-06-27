/**
 * @file test_ra_exception.c
 * @brief Unit tests for ra_exception.c fault diagnostic helpers
 *
 * @details
 * `ra_exception_report()` is declared `noreturn` because on target it
 * tail-calls `internal_ra_fatal_error()`, which halts the CPU. The
 * host implementation of `internal_ra_fatal_error()` normally calls
 * `__builtin_trap()`. To keep the test runnable we override it here
 * with a weak-symbol-clobbering definition that simply records the
 * call and returns via `longjmp()` so the test process survives.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_exception.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static jmp_buf s_fatal_jmp;
static uint8_t s_fatal_hit = 0U;

/* Override the weak default in ra_error_handler.c so the test can
 * invoke ra_exception_report() without aborting the process. */
[[noreturn]] void internal_ra_fatal_error(const char* tag, const char* message, uint32_t err);

void internal_ra_fatal_error(const char* tag, const char* message, uint32_t err)
{
  (void)tag;
  (void)message;
  (void)err;
  s_fatal_hit = 1U;
  longjmp(s_fatal_jmp, 1);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_diagnostics_happy(void)
{
  TEST_BEGIN("ra_exception_capture_diagnostics fills buffer");
  ra_sim_mmap_reset();

  /* Pre-load the SCB fault status registers via the mapped core
   * window so the capture has something to read. */
  *(volatile uint32_t*)0xE000ED28UL = 0xC0FFEE00UL;
  *(volatile uint32_t*)0xE000ED2CUL = 0xC0FFEE04UL;
  *(volatile uint32_t*)0xE000ED30UL = 0xC0FFEE08UL;
  *(volatile uint32_t*)0xE000ED34UL = 0xC0FFEE0CUL;
  *(volatile uint32_t*)0xE000ED38UL = 0xC0FFEE10UL;
  *(volatile uint32_t*)0xE000ED3CUL = 0xC0FFEE14UL;

  ra_exception_diagnostics_t diag = {};
  ra_exception_capture_diagnostics(&diag);

  TEST_ASSERT_EQ(0xC0FFEE00L, diag.cfsr);
  TEST_ASSERT_EQ(0xC0FFEE04L, diag.hfsr);
  TEST_ASSERT_EQ(0xC0FFEE08L, diag.dfsr);
  TEST_ASSERT_EQ(0xC0FFEE0CL, diag.mmfar);
  TEST_ASSERT_EQ(0xC0FFEE10L, diag.bfar);
  TEST_ASSERT_EQ(0xC0FFEE14L, diag.afsr);

  TEST_END("ra_exception_capture_diagnostics fills buffer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_diagnostics_null(void)
{
  TEST_BEGIN("ra_exception_capture_diagnostics tolerates NULL");
  ra_sim_mmap_reset();
  /* Must early-return without touching anything. */
  ra_exception_capture_diagnostics(nullptr);
  TEST_END("ra_exception_capture_diagnostics tolerates NULL");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_exception_report_with_frame(void)
{
  TEST_BEGIN("ra_exception_report logs frame and halts via fatal");
  ra_sim_mmap_reset();
  s_fatal_hit = 0U;

  const ra_exception_frame_t frame = {
    .r0   = 0x01U,
    .r1   = 0x02U,
    .r2   = 0x03U,
    .r3   = 0x04U,
    .r12  = 0x05U,
    .lr   = 0x06U,
    .pc   = 0x07U,
    .xpsr = 0x08U,
  };
  if (setjmp(s_fatal_jmp) == 0) {
    ra_exception_report(&frame, 3U);
    /* Unreachable once internal_ra_fatal_error is invoked. */
    TEST_FAIL_FMT("%s", "ra_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);

  TEST_END("ra_exception_report logs frame and halts via fatal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_exception_report_null_frame(void)
{
  TEST_BEGIN("ra_exception_report accepts NULL frame");
  ra_sim_mmap_reset();
  s_fatal_hit = 0U;

  if (setjmp(s_fatal_jmp) == 0) {
    ra_exception_report(nullptr, 2U);
    TEST_FAIL_FMT("%s", "ra_exception_report returned");
  }
  TEST_ASSERT_EQ(1, s_fatal_hit);

  TEST_END("ra_exception_report accepts NULL frame");
}

int32_t main(void)
{
  test_capture_diagnostics_happy();
  test_capture_diagnostics_null();
  test_exception_report_with_frame();
  test_exception_report_null_frame();
  (void)fprintf(stderr, "[OK  ] test_ra_exception.c\n");
  return 0;
}
