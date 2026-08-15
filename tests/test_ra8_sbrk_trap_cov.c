/**
 * @file test_ra8_sbrk_trap_cov.c
 * @brief Line-coverage + death test for the newlib `_sbrk()` trap
 *        (`ra8_sbrk_trap.c`).
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * `ra8_sbrk_trap.c` provides a strong `_sbrk` that never returns: any accidental
 * newlib heap dependency traps into `ra8_fatal_error` and the firmware
 * halts, enforcing NASA Power of 10 Rule 3 (zero dynamic allocation). On the
 * host the production `_sbrk` is linked from `ra8_core_hal` but is never called
 * (glibc's `malloc` resolves its own program break), so the trap body shows 0%
 * and cannot be reached through any public path.
 *
 * To cover the three trap lines this TU drives the PRODUCTION `_sbrk` directly
 * (declared `extern` and linked from `ra8_core_hal`), so the coverage lands in
 * the one object gcovr reports for `ra8_sbrk_trap.c`; a renamed white-box copy
 * (`#define _sbrk _sbrk_cov` + include) would not merge into the aggregate. Its
 * one dependency -- the weak `ra8_fatal_error` sink in
 * `ra8_error_handler.c` -- is redirected with a strong definition here (the same
 * technique as `test_ra8_exception.c`). The mocked sink either `longjmp()`s
 * straight back so the in-process leg records the three lines and inspects the
 * exact policy call, or, inside a forked child, halts via `abort()` so a classic
 * death test proves the trap never returns to its caller. No hardware line is
 * bypassed by an exclusion marker.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_error_handler.h"
#include "ra8_sbrk_trap.h"
#include "unity_minimal.h"

/**
 * @enum trap_mode_t
 * @brief Selects how the mocked fatal-error sink leaves the trap.
 */
typedef enum : uint8_t {
  k_trap_mode_longjmp = 0U, /**< Escape via longjmp so the process survives.  */
  k_trap_mode_die     = 1U, /**< Halt via abort() inside a forked death-test. */
} trap_mode_t;

/** @brief Representative heap-grow request handed to the trap. */
typedef enum : int32_t {
  k_sbrk_incr = 32, /**< Bytes newlib would ask the break to advance by. */
} sbrk_arg_t;

/** @brief setjmp() return value stamped when the sink longjmps out. */
typedef enum : int32_t {
  k_trap_escaped = 1, /**< Non-zero: control came back through the sink. */
} longjmp_token_t;

/** @brief Death-test exit codes and watchdog budget. */
typedef enum : uint8_t {
  k_child_returned   = 43U, /**< Child exit code if `_sbrk` wrongly returned. */
  k_death_watchdog_s = 10U, /**< Parent waitpid watchdog, in seconds.         */
} death_test_t;

/** @brief Sentinel stored in ::s_sink_err before the sink runs. */
typedef enum : uint32_t {
  k_sink_err_unset = 0xFFFFFFFFU, /**< Distinguishes "not captured yet". */
} sink_err_sentinel_t;

/** @brief longjmp landing pad the mocked sink returns through. */
static jmp_buf s_sink_jmp;
/** @brief Active sink behaviour (::k_trap_mode_longjmp / ::k_trap_mode_die). */
static trap_mode_t s_trap_mode = k_trap_mode_longjmp;
/** @brief Number of times the mocked fatal sink was entered. */
static uint32_t s_sink_hits = 0U;
/** @brief `tag` argument captured from the last sink call. */
static const char* s_sink_tag = nullptr;
/** @brief `message` argument captured from the last sink call. */
static const char* s_sink_msg = nullptr;
/** @brief `err` argument captured from the last sink call. */
static uint32_t s_sink_err = (uint32_t)k_sink_err_unset;
/** @brief Set from the SIGALRM handler if the death-test child ever hangs. */
static volatile sig_atomic_t s_watchdog_fired = 0;

/**
 * @brief Strong override of the weak `ra8_fatal_error` sink.
 *
 * @details The trap under test tail-calls this function; controlling it here
 * lets the coverage leg survive (via longjmp) while the death-test leg halts
 * (via abort) exactly where the firmware would spin. Matches the header's
 * `[[noreturn]]` contract on both paths.
 */
void ra8_fatal_error(const char* tag, const char* message, uint32_t err)
{
  s_sink_tag = tag;
  s_sink_msg = message;
  s_sink_err = err;
  ++s_sink_hits;
  if (s_trap_mode == k_trap_mode_longjmp) {
    longjmp(s_sink_jmp, (int)k_trap_escaped);
  }
  /* k_trap_mode_die: emulate the firmware halt -- never return to `_sbrk`. */
  abort();
}

/**
 * @brief Flag expiry of the forked-child death-test watchdog.
 * @details Records signal delivery without performing non-signal-safe cleanup.
 * @param[in] sig Delivered signal number; accepted for the handler ABI only.
 * @pre The parent installed this handler only for SIGALRM.
 * @pre ::s_watchdog_fired is writable signal-safe scalar storage.
 * @post ::s_watchdog_fired is set to one.
 * @post The handler returns without changing child process state.
 * @note Parent-side code performs kill and wait cleanup after observing the flag.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_death_watchdog(int sig)
{
  (void)sig;
  s_watchdog_fired = 1;
}

/* Drive the PRODUCTION `_sbrk` (the strong newlib trap linked from ra8_core_hal)
 * directly. A renamed white-box copy (`#define _sbrk _sbrk_cov` + include) does
 * NOT merge into the aggregate -- gcovr reports the production ra8_sbrk_trap.o,
 * whose `_sbrk` a renamed copy never touches -- so cover the real symbol here.
 * glibc's malloc does not grow the program break during this short test (it
 * mmaps its arena), so the strong `_sbrk` stays dormant except for the explicit
 * calls below, where the mocked sink returns control via longjmp/abort. */

/**
 * @test test_sbrk_trap_reaches_fatal_sink
 *
 * @brief Verify the production `_sbrk` trap reaches the fatal policy sink.
 * @details Uses longjmp mode to observe the exact tag, message, and error code
 * without allowing the production non-returning path to halt the test process.
 * @pre The strong test sink overrides the weak production fatal backend.
 * @pre ::s_sink_jmp is established before `_sbrk` is invoked.
 * @post The sink is entered exactly once with the SBRK heap-free policy.
 * @post `_sbrk` does not return through its ordinary call site.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `_sbrk` is straight-line: an
 * ignored-parameter cast followed by an unconditional `[[noreturn]]` call to
 * `ra8_fatal_error`; no `&&` or `||`.)
 *
 * @note In-process coverage leg. The mocked sink longjmp()s straight back, so
 * the three trap lines execute and are recorded without halting the process.
 * The captured arguments prove the exact heap-free policy call fired.
 */
RA8_INTERNAL static void internal_test_sbrk_trap_reaches_fatal_sink(void)
{
  TEST_BEGIN("sbrk trap: enters the fatal sink with the heap-free policy tag");

  s_trap_mode = k_trap_mode_longjmp;
  s_sink_hits = 0U;
  s_sink_tag  = nullptr;
  s_sink_msg  = nullptr;
  s_sink_err  = (uint32_t)k_sink_err_unset;

  if (setjmp(s_sink_jmp) == 0) {
    /* `_sbrk` must not return: it tail-calls the fatal sink, which longjmps
     * back to the fall-through below. Reaching the next line would be a defect. */
    (void)_sbrk((int32_t)k_sbrk_incr);
    TEST_ASSERT(false); /* unreachable: the trap never returns. */
  }

  TEST_ASSERT_EQ(1U, s_sink_hits);
  TEST_ASSERT(s_sink_tag != nullptr);
  TEST_ASSERT_EQ(0, strcmp(s_sink_tag, "SBRK"));
  TEST_ASSERT(s_sink_msg != nullptr);
  TEST_ASSERT(strstr(s_sink_msg, "heap-free") != nullptr);
  TEST_ASSERT_EQ(0U, s_sink_err);

  TEST_END("sbrk trap: enters the fatal sink with the heap-free policy tag");
}

/**
 * @test test_sbrk_trap_never_returns
 *
 * @brief Verify the production `_sbrk` trap terminates a forked child.
 * @details Runs the fatal sink in abort mode under a bounded parent watchdog
 * and distinguishes signal termination from an erroneous normal return.
 * @pre fork, waitpid, signal, and alarm are available on the host.
 * @pre The child disables core dumps before invoking `_sbrk`.
 * @post The child terminates by signal rather than the return sentinel.
 * @post The parent cancels the watchdog and reaps the child.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `_sbrk` is straight-line;
 * this leg only proves the `[[noreturn]]` contract, it has no branch to cover.)
 *
 * @note Classic fork death test. The child drives the real trap with the sink
 * in die-mode, so it halts via abort() exactly where the firmware would spin.
 * If `_sbrk` ever handed control back, the child would fall through to `_exit()`
 * with a sentinel code; the parent asserts the child was instead killed by a
 * signal, proving the trap never returns to its caller. The parent alarm is the
 * one permitted death-test watchdog and cannot spuriously fire because the child
 * aborts immediately.
 */
RA8_INTERNAL static void internal_test_sbrk_trap_never_returns(void)
{
  TEST_BEGIN("sbrk trap: forked child halts and never returns from _sbrk");

  const pid_t pid = fork();
  TEST_ASSERT(pid >= 0);

  if (pid == 0) {
    /* Child: suppress the core file, halt loudly inside the trap, never return. */
    const struct rlimit no_core = {.rlim_cur = 0, .rlim_max = 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    s_trap_mode = k_trap_mode_die;
    (void)_sbrk((int32_t)k_sbrk_incr);
    _exit((int)k_child_returned); /* only reached if `_sbrk` wrongly returned. */
  }

  /* Parent: watchdog the wait so a hung child can never wedge the suite. */
  struct sigaction sa = {};
  sa.sa_handler       = internal_death_watchdog;
  (void)sigaction(SIGALRM, &sa, nullptr);
  (void)alarm((unsigned int)k_death_watchdog_s);

  int         status = 0;
  const pid_t waited = waitpid(pid, &status, 0);
  (void)alarm(0U);

  if ((waited < 0) && (s_watchdog_fired != 0)) {
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, &status, 0);
    TEST_ASSERT(false); /* watchdog: child never terminated -> trap hung. */
  }

  TEST_ASSERT_EQ(pid, waited);
  TEST_ASSERT(WIFSIGNALED(status));          /* did NOT exit normally.  */
  TEST_ASSERT_EQ(SIGABRT, WTERMSIG(status)); /* halted inside the trap. */

  TEST_END("sbrk trap: forked child halts and never returns from _sbrk");
}

int32_t main(void)
{
  internal_test_sbrk_trap_reaches_fatal_sink();
  internal_test_sbrk_trap_never_returns();
  return 0;
}
