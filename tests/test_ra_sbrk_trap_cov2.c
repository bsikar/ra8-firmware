/**
 * @file test_ra_sbrk_trap_cov2.c
 * @brief Complementary white-box line-coverage test for the newlib `_sbrk()`
 *        trap body in `ra_sbrk_trap.c` (the never-returning lines 94/96/97).
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * `ra_sbrk_trap.c` provides a strong `_sbrk` that never returns: any accidental
 * newlib heap dependency traps into `internal_ra_fatal_error` and the firmware
 * halts, enforcing NASA Power of 10 Rule 3 (zero dynamic allocation). The three
 * trap-body lines -- the signature (94), the ignored-argument cast (96) and the
 * unconditional `[[noreturn]]` call to the fatal sink (97) -- are straight-line
 * code with no compound decision and no `for(;;)` spin (the spin lives inside
 * the sink, in a different translation unit). Every one of the three is
 * reachable, so none is excluded.
 *
 * This TU is the second half of the union that gcovr forms per source file: it
 * compiles a private, instrumented copy of the module -- `#include`-ing the
 * source with the reserved `_sbrk` renamed to `_sbrk_cov2` so the copy does not
 * collide with the production `_sbrk` linked from `ra_core_hal` -- and redirects
 * the one dependency, the weak `internal_ra_fatal_error` sink, to a strong mock
 * here (the same technique used by `test_ra_error_handler.c` and
 * `test_ra_sbrk_trap_cov.c`). It exercises the trap body two ways, both of which
 * flush the counters that gcov increments synchronously as the block executes:
 *
 *  1. In-process leg: the mock sink `longjmp()`s straight back, so the three
 *     trap lines run and are credited to the surviving process, which flushes
 *     normally at exit. This is the proven `test_ra_error_handler.c` pattern for
 *     crediting a line that ends in a non-returning call.
 *  2. Forked-child leg: the child drives the real trap, and the mock sink -- now
 *     in dump-exit mode -- explicitly `__gcov_dump()`s the child's live counters
 *     (which already include the just-executed trap lines) BEFORE `_exit(0)`, so
 *     a SIGKILL/`abort()` can never discard them. Because the trap body runs to
 *     completion before any spin, the child covers the lines without needing to
 *     be killed, and the parent proves the trap never returned to `_sbrk_cov2`.
 *
 * `__gcov_dump` is only linked when gcov instrumentation is active
 * (`RA_COVERAGE=ON`); the clang MC/DC and UBSan build trees compile the same
 * source with coverage off, so the symbol is declared `[[gnu::weak]]` and the
 * call is guarded by a null check (the repo idiom from `ra_time.c`). No hardware
 * line is bypassed by an exclusion marker.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ra_error_handler.h"
#include "unity_minimal.h"

/**
 * @enum trap_mode_t
 * @brief Selects how the mocked fatal-error sink leaves the trap.
 */
typedef enum : uint8_t {
  k_trap_mode_longjmp   = 0U, /**< Escape via longjmp so the process survives.     */
  k_trap_mode_dump_exit = 1U, /**< Flush gcov then _exit(0) inside a forked child. */
} trap_mode_t;

/** @brief Representative heap-grow request handed to the trap. */
typedef enum : int32_t {
  k_sbrk_incr = 32, /**< Bytes newlib would ask the break to advance by. */
} sbrk_arg_t;

/** @brief setjmp() return value stamped when the sink longjmps out. */
typedef enum : int32_t {
  k_trap_escaped = 1, /**< Non-zero: control came back through the sink. */
} longjmp_token_t;

/** @brief Forked-child exit codes for the flush-preserving death leg. */
typedef enum : uint8_t {
  k_child_dumped_ok = 0U,  /**< Child flushed gcov in the sink and exited cleanly. */
  k_child_returned  = 43U, /**< Child exit code if `_sbrk` wrongly returned.       */
} child_exit_t;

/** @brief Sentinel stored in ::s_sink_err before the sink runs. */
typedef enum : uint32_t {
  k_sink_err_unset = 0xFFFFFFFFU, /**< Distinguishes "not captured yet". */
} sink_err_sentinel_t;

/**
 * @brief gcov's explicit counter-dump entry point (gcc libgcov / clang gcov).
 *
 * @details Declared `[[gnu::weak]]` because it is only linked when gcov
 * instrumentation is active (`RA_COVERAGE=ON`). In the clang MC/DC and UBSan
 * build trees the symbol is absent, so `&__gcov_dump` resolves to null and the
 * call is skipped -- the child still `_exit(0)`s cleanly there.
 */
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
[[gnu::weak]] extern void __gcov_dump(void);

/** @brief longjmp landing pad the mocked sink returns through. */
static jmp_buf s_sink_jmp;
/** @brief Active sink behaviour (::k_trap_mode_longjmp / ::k_trap_mode_dump_exit). */
static trap_mode_t s_trap_mode = k_trap_mode_longjmp;
/** @brief Number of times the mocked fatal sink was entered. */
static uint32_t s_sink_hits = 0U;
/** @brief `tag` argument captured from the last sink call. */
static const char* s_sink_tag = nullptr;
/** @brief `message` argument captured from the last sink call. */
static const char* s_sink_msg = nullptr;
/** @brief `err` argument captured from the last sink call. */
static uint32_t s_sink_err = (uint32_t)k_sink_err_unset;

/**
 * @brief Strong override of the weak `internal_ra_fatal_error` sink.
 *
 * @details The trap under test tail-calls this function; controlling it here
 * lets the coverage leg survive (via longjmp) while the forked-child leg flushes
 * the live gcov counters and exits cleanly, exactly where the firmware would
 * spin. Both paths honour the header's `[[noreturn]]` contract.
 */
void internal_ra_fatal_error(const char* tag, const char* message, uint32_t err)
{
  s_sink_tag = tag;
  s_sink_msg = message;
  s_sink_err = err;
  ++s_sink_hits;
  if (s_trap_mode == k_trap_mode_longjmp) {
    longjmp(s_sink_jmp, (int)k_trap_escaped);
  }
  /* k_trap_mode_dump_exit: the trap body (94/96/97) has already executed and
   * gcov has already bumped their counters in this child's memory. Flush them
   * explicitly so termination cannot discard the .gcda, then exit cleanly --
   * never returning to `_sbrk_cov2`. The dump is guarded because __gcov_dump is
   * only linked under RA_COVERAGE=ON. */
  if (__gcov_dump != ((void*)0)) {
    __gcov_dump();
  }
  _exit((int)k_child_dumped_ok);
}

/* Compile the private, instrumented copy of the trap. `_sbrk` is a reserved
 * identifier fixed by newlib, so rename it to `_sbrk_cov2` to avoid colliding
 * with the production `_sbrk` linked from ra_core_hal (and with `_sbrk_cov` in
 * the sibling test executable). */
#define _sbrk _sbrk_cov2
#include "ra_sbrk_trap.c" // NOLINT(bugprone-suspicious-include) -- white-box copy
#undef _sbrk

/**
 * @test test_sbrk_trap_body_inprocess
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `_sbrk` is straight-line: an
 * ignored-parameter cast followed by an unconditional `[[noreturn]]` call to
 * `internal_ra_fatal_error`; no `&&` or `||`.)
 *
 * @note In-process coverage leg. The mocked sink longjmp()s straight back, so
 * the three trap lines (94/96/97) execute and are credited to the surviving
 * process, which flushes at normal exit. The captured arguments prove the exact
 * heap-free policy call fired.
 */
static void test_sbrk_trap_body_inprocess(void)
{
  TEST_BEGIN("sbrk trap: in-process run credits the trap body via the fatal sink");

  s_trap_mode = k_trap_mode_longjmp;
  s_sink_hits = 0U;
  s_sink_tag  = nullptr;
  s_sink_msg  = nullptr;
  s_sink_err  = (uint32_t)k_sink_err_unset;

  if (setjmp(s_sink_jmp) == 0) {
    /* `_sbrk_cov2` must not return: it tail-calls the fatal sink, which longjmps
     * back to the fall-through below. Reaching the next line would be a defect. */
    (void)_sbrk_cov2((int32_t)k_sbrk_incr);
    TEST_ASSERT(false); /* unreachable: the trap never returns. */
  }

  TEST_ASSERT_EQ(1U, s_sink_hits);
  TEST_ASSERT(s_sink_tag != nullptr);
  TEST_ASSERT_EQ(0, strcmp(s_sink_tag, "SBRK"));
  TEST_ASSERT(s_sink_msg != nullptr);
  TEST_ASSERT(strstr(s_sink_msg, "heap-free") != nullptr);
  TEST_ASSERT_EQ(0U, s_sink_err);

  TEST_END("sbrk trap: in-process run credits the trap body via the fatal sink");
}

/**
 * @test test_sbrk_trap_child_flushes_coverage
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `_sbrk` is straight-line;
 * this leg only proves the `[[noreturn]]` contract and preserves the child's
 * coverage, it has no branch to cover.)
 *
 * @note Deterministic fork leg (no SIGALRM). The child drives the real trap with
 * the sink in dump-exit mode, so it `__gcov_dump()`s its live counters and
 * `_exit(0)`s inside the trap -- never returning and never being killed, so the
 * child's .gcda (with lines 94/96/97 credited) survives. If `_sbrk` ever handed
 * control back, the child would instead reach the sentinel `_exit(k_child_returned)`;
 * the parent asserts the child exited cleanly with code 0, proving the trap
 * never returns to its caller. The child cannot hang -- the mocked sink runs a
 * bounded dump then exits -- so no watchdog is required.
 */
static void test_sbrk_trap_child_flushes_coverage(void)
{
  TEST_BEGIN("sbrk trap: forked child flushes gcov then exits without returning");

  const pid_t pid = fork();
  TEST_ASSERT(pid >= 0);

  if (pid == 0) {
    /* Child: run the real trap, flush coverage in the sink, exit cleanly. */
    s_trap_mode = k_trap_mode_dump_exit;
    (void)_sbrk_cov2((int32_t)k_sbrk_incr);
    _exit((int)k_child_returned); /* only reached if `_sbrk` wrongly returned. */
  }

  int         status = 0;
  const pid_t waited = waitpid(pid, &status, 0);

  TEST_ASSERT_EQ(pid, waited);
  TEST_ASSERT(WIFEXITED(status));                              /* clean exit, not a signal. */
  TEST_ASSERT_EQ((int)k_child_dumped_ok, WEXITSTATUS(status)); /* dumped inside the trap.   */

  TEST_END("sbrk trap: forked child flushes gcov then exits without returning");
}

int32_t main(void)
{
  test_sbrk_trap_body_inprocess();
  test_sbrk_trap_child_flushes_coverage();
  (void)fprintf(stderr, "[OK ] test_ra_sbrk_trap_cov2.c\n");
  return 0;
}
