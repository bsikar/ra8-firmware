/**
 * @file test_ra8_error_handler.c
 * @brief Unit tests for ra8_error_handler.c fatal sink
 *
 * @details
 * The only public function in `ra8_error_handler.c` is
 * `ra8_fatal_error`, which halts the CPU on target and calls
 * `__builtin_trap()` on the host. To cover its body while keeping the
 * test process alive we install a SIGILL handler that uses
 * `siglongjmp()` to bail out of the trap instruction. gcov counters
 * increment synchronously as the function runs, so every line up to
 * (and including) the trap is credited to the parent process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
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

/**
 * @brief Escape from the host trap raised by the fatal backend.
 * @details Records signal delivery and jumps back to the active test vector.
 * @param[in] sig Delivered host signal number; accepted for handler ABI only.
 * @pre ::s_trap_jmp contains an active sigsetjmp checkpoint.
 * @pre The handler was installed for the expected trap signal.
 * @post ::s_trap_hit is set to one.
 * @post Control resumes at the active sigsetjmp checkpoint.
 * @note This handler is test-process state and is not async-signal-safe generally.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sigill_handler(int32_t sig)
{
  (void)sig;
  s_trap_hit = 1;
  siglongjmp(s_trap_jmp, 1);
}

/**
 * @brief Install the host signal handlers used by fatal-error vectors.
 * @details Routes SIGILL, SIGABRT, and SIGTRAP to the one-shot jump handler.
 * @pre No production thread depends on the process signal dispositions.
 * @pre ::s_trap_jmp will be initialized before any handled signal is raised.
 * @post All three trap-class signals use ::internal_sigill_handler.
 * @post Each installed disposition resets after delivery.
 * @note The test process owns and intentionally does not restore these handlers.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_install_sigill_handler(void)
{
  struct sigaction sa = {};
  sa.sa_handler       = internal_sigill_handler;
  sa.sa_flags         = SA_NODEFER | SA_RESETHAND;
  (void)sigaction(SIGILL, &sa, nullptr);
  (void)sigaction(SIGABRT, &sa, nullptr);
  (void)sigaction(SIGTRAP, &sa, nullptr);
}

/**
 * @brief Verify a representative fatal error logs and traps.
 * @details Invokes the weak default backend with a hardware error and catches
 * its host trap through the signal checkpoint.
 * @pre Fake MMIO and the trap signal handlers are available.
 * @pre ::ra8_fatal_error uses the host trap path in this build.
 * @post The fatal call cannot return through its ordinary call site.
 * @post The signal handler records exactly one trap observation.
 * @note The diagnostic bytes may be dropped by the unarmed fake ITM backend.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_fatal_error_path(void)
{
  TEST_BEGIN("ra8_fatal_error executes body then traps");
  ra8_fake_mmap_reset();
  s_trap_hit = 0;
  internal_install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    ra8_fatal_error("TEST", "fatal message", (uint32_t)k_ra8_err_hw_error);
    TEST_FAIL_FMT("%s", "ra8_fatal_error returned");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);

  TEST_END("ra8_fatal_error executes body then traps");
}

/**
 * @brief Verify zero remains a valid fatal diagnostic companion value.
 * @details Calls the fatal backend with an ASSERT tag and zero error code,
 * then proves that the backend still reaches its non-returning trap.
 * @pre Fake MMIO and the trap signal handlers are available.
 * @pre The host fatal backend raises a handled trap signal.
 * @post The call site after ::ra8_fatal_error remains unreachable.
 * @post One trap delivery is recorded.
 * @note Zero is data for this API, not a request to suppress the halt.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_fatal_error_zero_code(void)
{
  TEST_BEGIN("ra8_fatal_error with err=0");
  ra8_fake_mmap_reset();
  s_trap_hit = 0;
  internal_install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    ra8_fatal_error("ASSERT", "zero err", 0U);
    TEST_FAIL_FMT("%s", "ra8_fatal_error returned");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);

  TEST_END("ra8_fatal_error with err=0");
}

/**
 * @brief Verify a full-width fatal code reaches the same halt policy.
 * @details Sends the all-nibble fixture value and catches the host trap,
 * covering the widest unsigned decimal formatting input.
 * @pre Fake MMIO and the trap signal handlers are available.
 * @pre k_fatal_error_code is representable as uint32_t.
 * @post The fatal backend traps rather than returning.
 * @post One trap delivery is recorded.
 * @note Numeric rendering is bounded independently by the logger implementation.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
RA8_INTERNAL static void internal_test_fatal_error_large_code(void)
{
  TEST_BEGIN("ra8_fatal_error with large err");
  ra8_fake_mmap_reset();
  s_trap_hit = 0;
  internal_install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    ra8_fatal_error("FAULT", "huge err", k_fatal_error_code);
    TEST_FAIL_FMT("%s", "ra8_fatal_error returned");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);

  TEST_END("ra8_fatal_error with large err");
}

/**
 * @brief Verify a succeeding boot step passes through ::RA8_BOOT_REQUIRE.
 * @details Feeds ::k_ra8_ok through the macro and proves control continues
 * past it without any trap delivery, so a healthy boot pays only the test.
 * @pre Fake MMIO and the trap signal handlers are available.
 * @pre The step expression evaluates to a success status.
 * @post Execution reaches the statement after the macro.
 * @post No trap delivery is recorded.
 * @note The step expression is evaluated exactly once; the counter proves it.
 * @since 0.1.0
 *
 * @par MC/DC:
 * `ra8_err_is_error(boot_rc_)` false alone keeps the halt unreached; the
 * paired failure vector below drives the same decision true.
 */
RA8_INTERNAL static void internal_test_boot_require_success_passes(void)
{
  TEST_BEGIN("RA8_BOOT_REQUIRE continues on success");
  ra8_fake_mmap_reset();
  s_trap_hit          = 0;
  int32_t eval_count  = 0;
  int32_t reached     = 0;
  internal_install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    RA8_BOOT_REQUIRE((eval_count += 1, k_ra8_ok), "healthy_step");
    reached = 1;
  }
  TEST_ASSERT_EQ(0, s_trap_hit);
  TEST_ASSERT_EQ(1, eval_count);
  TEST_ASSERT_EQ(1, reached);

  TEST_END("RA8_BOOT_REQUIRE continues on success");
}

/**
 * @brief Verify a failing boot step halts through the fatal sink.
 * @details Drives one non-success status into the macro and proves the
 * statement after it is unreachable, which is the diagnostic a private
 * `while (1) { wfi; }` helper discards.
 * @pre Fake MMIO and the trap signal handlers are available.
 * @pre The host fatal backend raises a handled trap signal.
 * @post The statement after the macro is never executed.
 * @post Exactly one trap delivery is recorded.
 * @note The failing status is evaluated exactly once before the halt.
 * @since 0.1.0
 *
 * @par MC/DC:
 * `ra8_err_is_error(boot_rc_)` true alone reaches the halt; the success
 * vector above drives the same decision false.
 */
RA8_INTERNAL static void internal_test_boot_require_failure_halts(void)
{
  TEST_BEGIN("RA8_BOOT_REQUIRE halts on error");
  ra8_fake_mmap_reset();
  s_trap_hit         = 0;
  int32_t eval_count = 0;
  internal_install_sigill_handler();

  if (sigsetjmp(s_trap_jmp, 1) == 0) {
    RA8_BOOT_REQUIRE((eval_count += 1, k_ra8_err_hw_error), "failing_step");
    TEST_FAIL_FMT("%s", "RA8_BOOT_REQUIRE returned on error");
  }
  TEST_ASSERT_EQ(1, s_trap_hit);
  TEST_ASSERT_EQ(1, eval_count);

  TEST_END("RA8_BOOT_REQUIRE halts on error");
}

int main(void)
{
  internal_test_fatal_error_path();
  internal_test_fatal_error_zero_code();
  internal_test_fatal_error_large_code();
  internal_test_boot_require_success_passes();
  internal_test_boot_require_failure_halts();
  return 0;
}
