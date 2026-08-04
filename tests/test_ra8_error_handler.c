/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_error_handler.c
 * @brief Unit tests for ra8_error_handler.c fatal sink
 *
 * @details
 * The only public function in `ra8_error_handler.c` is
 * `internal_ra8_fatal_error`, which halts the CPU on target and calls
 * `__builtin_trap()` on the host. To cover its body while keeping the
 * test process alive we install a SIGILL handler that uses
 * `siglongjmp()` to bail out of the trap instruction. gcov counters
 * increment synchronously as the function runs, so every line up to
 * (and including) the trap is credited to the parent process.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_error_handler.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum error_handler_fixture_t
 * @brief The protocol codes and identifiers exchanged.
 */
typedef enum : uint32_t {
  k_fatal_error_code =
    0xDEADBEEFUL, /**< Error code sent to the fatal handler and read back, proving it persists. */
} error_handler_fixture_t;

static sigjmp_buf s_trap_jmp;
static int32_t    s_trap_hit = 0;

static void test_sigill_handler(int32_t sig)
{
  (void)sig;
  s_trap_hit = 1;
  siglongjmp(s_trap_jmp, 1);
}

static void install_sigill_handler(void)
{
  struct sigaction sa = {};
  sa.sa_handler       = test_sigill_handler;
  sa.sa_flags         = SA_NODEFER | SA_RESETHAND;
  (void)sigaction(SIGILL, &sa, nullptr);
  (void)sigaction(SIGABRT, &sa, nullptr);
  (void)sigaction(SIGTRAP, &sa, nullptr);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fatal_error_path(void)
{
  TEST_BEGIN("internal_ra8_fatal_error executes body then traps");
  ra8_fake_mmap_reset();
  s_trap_hit = 0;
  install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    internal_ra8_fatal_error("TEST", "fatal message", (uint32_t)k_ra8_err_hw_error);
    TEST_FAIL_FMT("%s", "internal_ra8_fatal_error returned");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);

  TEST_END("internal_ra8_fatal_error executes body then traps");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fatal_error_zero_code(void)
{
  TEST_BEGIN("internal_ra8_fatal_error with err=0");
  ra8_fake_mmap_reset();
  s_trap_hit = 0;
  install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    internal_ra8_fatal_error("ASSERT", "zero err", 0U);
    TEST_FAIL_FMT("%s", "internal_ra8_fatal_error returned");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);

  TEST_END("internal_ra8_fatal_error with err=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fatal_error_large_code(void)
{
  TEST_BEGIN("internal_ra8_fatal_error with large err");
  ra8_fake_mmap_reset();
  s_trap_hit = 0;
  install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    internal_ra8_fatal_error("FAULT", "huge err", k_fatal_error_code);
    TEST_FAIL_FMT("%s", "internal_ra8_fatal_error returned");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);

  TEST_END("internal_ra8_fatal_error with large err");
}

int32_t main(void)
{
  test_fatal_error_path();
  test_fatal_error_zero_code();
  test_fatal_error_large_code();
  (void)fprintf(stderr, "[OK  ] test_ra8_error_handler.c\n");
  return 0;
}
