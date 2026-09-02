/**
 * @file test_mdl_governor.c
 * @brief Host unit tests for the #301 per-host politeness governor.
 *
 * @details
 * Drives ::mdl_governor_t entirely through its injected clock/sleep seams, so
 * every timing behaviour is asserted deterministically with no real time
 * passing: a recording fake clock advances only when the governor sleeps, and
 * the seeded jitter PRNG is reproducible. The suite proves the four behaviours
 * the issue's acceptance criteria call out --
 *   - a per-host token bucket bounds the sustained request rate (a small burst
 *     goes out free, then requests are paced at the configured interval);
 *   - a 429/503 grows exponential backoff (capped) and sustained success decays
 *     it back toward the base rate;
 *   - `Retry-After` is honoured in both delta-seconds and HTTP-date forms and
 *     wins over the computed backoff when it is longer;
 *   - the per-host in-flight cap refuses a request past the limit and is
 *     enforced independently per host --
 * plus the pure ::mdl_retry_after_parse helper for both header forms. Uses the
 * repo's `unity_minimal.h` harness, matching the repository's category-local test sources.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "mdl_politeness.h"
#include "ra8_test_output.h"
#include "unity_minimal.h"

/** @brief 32-bit governor-test constants (no bare literals). */
typedef enum : uint32_t {
  k_gov_rate_60       = 60U,     /**< 60 req/min -> 1000 ms interval.      */
  k_gov_burst_3       = 3U,      /**< Three back-to-back before pacing.    */
  k_gov_interval_ms   = 1000U,   /**< Derived token interval.              */
  k_gov_backoff_base  = 1000U,   /**< First backoff window.                */
  k_gov_backoff_cap   = 8000U,   /**< Backoff ceiling for the growth test. */
  k_gov_retry_120s_ms = 120000U, /**< 120 s Retry-After in ms.             */
  k_gov_retry_60s_ms  = 60000U,  /**< 60 s HTTP-date Retry-After in ms.    */
  k_gov_two_secs      = 2000U,   /**< Lower bound for five paced requests. */
  k_gov_jitter_5      = 5U,      /**< Fixed jitter value (min == max).     */
} gov_const_t;

/** @brief 16-bit governor-test constants. */
typedef enum : uint16_t {
  k_gov_decay_2    = 2U, /**< Successes per one-level decay. */
  k_gov_inflight_1 = 1U, /**< Serial in-flight cap.          */
  k_gov_inflight_2 = 2U, /**< Two-in-flight cap.             */
  k_gov_seed       = 1U, /**< Fixed jitter seed.             */
  k_gov_throttles  = 5U, /**< Consecutive throttles applied. */
} gov_const16_t;

/** @brief HTTP status codes used to drive the governor. */
typedef enum : uint16_t {
  k_http_ok       = 200U, /**< Success (non-throttle).         */
  k_http_too_many = 429U, /**< Too Many Requests (throttle).   */
  k_http_unavail  = 503U, /**< Service Unavailable (throttle). */
} gov_http_t;

/**
 * @struct gov_clock_t
 * @brief Recording virtual clock: `now` advances only when the governor sleeps.
 * @details `now_ms` is what ::mdl_now_fn returns; each injected sleep both
 *          advances `now_ms` (so the token bucket refills across calls) and
 *          records the requested duration, so timing is asserted without any
 *          wall-clock time elapsing.
 * @since 0.1.0
 */
typedef struct {
  int64_t  now_ms;      /**< Current virtual time (ms).   */
  int64_t  total_slept; /**< Sum of all requested sleeps. */
  uint32_t last_slept;  /**< Last requested sleep (ms).   */
  uint32_t calls;       /**< Number of sleep calls.       */
} gov_clock_t;

/**
 * @brief Return the current time from the injected virtual clock.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @return Value produced by the bounded test helper.
 * @retval 0 The helper produced its zero-valued boundary result.
 * @retval nonzero The helper produced its documented nonzero result.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_gov_clk_now(void* ctx)
{
  return ((const gov_clock_t*)ctx)->now_ms;
}

/** @brief Injected sleeper: advance the virtual clock and record the request.
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @param[in,out] ctx Opaque caller-owned fixture context.
 * @param[in] ms Requested virtual sleep duration in milliseconds.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @pre The caller retains ownership of every supplied fixture object.
 * @post Documented outputs reflect the processed fixture on success.
 * @post Failure preserves caller-owned resources as documented.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gov_clk_sleep(void* ctx, uint32_t ms)
{
  gov_clock_t* c = (gov_clock_t*)ctx;
  c->now_ms += (int64_t)ms;
  c->total_slept += (int64_t)ms;
  c->last_slept = ms;
  c->calls += 1U;
}

/** @brief The governor under test (a per-host table -- keep off the stack). */
static mdl_governor_t s_gov;

/**
 * @test internal_test_gov_retry_after_parse
 *
 * @par MC/DC:
 * ::mdl_retry_after_parse branches on `value==NULL || out_ms==NULL` (guard),
 * then on all-digits vs HTTP-date. Vectors: a NULL value and a NULL out (guard
 * true each way), a digit string (digits branch), an IMF-fixdate and an RFC-850
 * date (date branch, both formats), a past date (clamp-to-zero relational), and
 * an unparseable string (date branch false). Each single-condition branch is
 * driven both ways.
 * @brief Exercise the gov retry after parse media-downloader scenario.
 * @details Exercises the gov retry after parse scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gov_retry_after_parse(void)
{
  TEST_BEGIN("gov retry-after parse");
  uint32_t ms = 0U;
  /* delta-seconds form */
  TEST_ASSERT(mdl_retry_after_parse("120", 0, &ms));
  TEST_ASSERT_EQ((int64_t)k_gov_retry_120s_ms, (int64_t)ms);
  TEST_ASSERT(mdl_retry_after_parse("0", 0, &ms));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)ms);
  TEST_ASSERT(mdl_retry_after_parse("  30", 0, &ms)); /* leading spaces tolerated */
  TEST_ASSERT_EQ((int64_t)30000, (int64_t)ms);
  /* IMF-fixdate: two minutes past the epoch, measured from the epoch. */
  TEST_ASSERT(mdl_retry_after_parse("Thu, 01 Jan 1970 00:02:00 GMT", 0, &ms));
  TEST_ASSERT_EQ((int64_t)k_gov_retry_120s_ms, (int64_t)ms);
  /* A past HTTP-date clamps to zero. */
  TEST_ASSERT(mdl_retry_after_parse("Thu, 01 Jan 1970 00:00:00 GMT", 100, &ms));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)ms);
  /* RFC 850 (obsolete) form is also recognised. */
  ms = 0U;
  TEST_ASSERT(mdl_retry_after_parse("Sunday, 06-Nov-94 08:49:37 GMT", 0, &ms));
  TEST_ASSERT(ms > 0U);
  /* Guard / invalid cases. */
  TEST_ASSERT(!mdl_retry_after_parse("not-a-date", 0, &ms));
  TEST_ASSERT(!mdl_retry_after_parse("", 0, &ms));
  TEST_ASSERT(!mdl_retry_after_parse(nullptr, 0, &ms));
  TEST_ASSERT(!mdl_retry_after_parse("120", 0, nullptr));
  TEST_END("gov retry-after parse");
}

/**
 * @test internal_test_gov_rate_limit
 *
 * @par MC/DC:
 * (no compound decision in this test; it asserts the token-bucket timing --
 * `burst` requests go out with no sleep, then each further request is paced at
 * the configured interval -- through the recording fake clock.)
 * @brief Exercise the gov rate limit media-downloader scenario.
 * @details Exercises the gov rate limit scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gov_rate_limit(void)
{
  TEST_BEGIN("gov rate limit token bucket");
  gov_clock_t   clk = {};
  mdl_gov_cfg_t cfg = mdl_gov_cfg_default();
  cfg.rate_per_min  = (uint32_t)k_gov_rate_60; /* 1000 ms interval */
  cfg.burst         = (uint32_t)k_gov_burst_3;
  cfg.max_inflight  = (uint16_t)k_gov_inflight_1;
  mdl_governor_init_clock(&s_gov,
                          &cfg,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk,
                          internal_gov_clk_sleep,
                          &clk);

  /* The first `burst` (3) requests are admitted immediately -- no sleeping. */
  for (uint16_t i = 0U; i < (uint16_t)k_gov_burst_3; ++i) {
    TEST_ASSERT(mdl_governor_acquire(&s_gov, "h", 0U, 0U) == k_ra8_ok);
    mdl_governor_release(&s_gov, "h");
  }
  TEST_ASSERT_EQ((int64_t)0, clk.total_slept);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)clk.calls);

  /* The 4th and 5th are paced one interval apart. */
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "h", 0U, 0U) == k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_gov_interval_ms, (int64_t)clk.last_slept);
  mdl_governor_release(&s_gov, "h");
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "h", 0U, 0U) == k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_gov_interval_ms, (int64_t)clk.last_slept);
  mdl_governor_release(&s_gov, "h");
  TEST_ASSERT(clk.total_slept >= (int64_t)k_gov_two_secs);
  TEST_END("gov rate limit token bucket");
}

/**
 * @test internal_test_gov_backoff_growth_and_decay
 *
 * @par MC/DC:
 * Covers the throttle decision `status == 429 || status == 503` in
 * ::mdl_governor_observe with the 503-true/429-false vector (this test) and the
 * both-false vector (the status-200 successes here); the 429-true/503-false
 * vector lives in internal_test_gov_retry_after_precedence, completing N+1 = 3 for the
 * two-condition OR. Backoff growth and decay are asserted on the deterministic
 * level via ::mdl_governor_peek, and the scheduled gate is bounded by the
 * configured window ceiling.
 * @brief Exercise the gov backoff growth and decay media-downloader scenario.
 * @details Exercises the gov backoff growth and decay scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gov_backoff_growth_and_decay(void)
{
  TEST_BEGIN("gov backoff growth + decay");
  gov_clock_t   clk   = {};
  mdl_gov_cfg_t cfg   = mdl_gov_cfg_default();
  cfg.rate_per_min    = 0U; /* isolate backoff from the rate limiter */
  cfg.backoff_base_ms = (uint32_t)k_gov_backoff_base;
  cfg.backoff_max_ms  = (uint32_t)k_gov_backoff_cap;
  cfg.decay_after     = (uint16_t)k_gov_decay_2;
  mdl_governor_init_clock(&s_gov,
                          &cfg,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk,
                          internal_gov_clk_sleep,
                          &clk);

  uint16_t level = 0U;
  int64_t  gate  = 0;
  for (uint16_t k = 1U; k <= (uint16_t)k_gov_throttles; ++k) {
    mdl_governor_observe(&s_gov, "h", (long)k_http_unavail, nullptr);
    TEST_ASSERT(mdl_governor_peek(&s_gov, "h", &level, &gate));
    TEST_ASSERT_EQ((int64_t)k, (int64_t)level); /* level grows 1..5 */
    TEST_ASSERT(gate >= 0);
    TEST_ASSERT(gate <= (int64_t)k_gov_backoff_cap); /* full jitter within cap */
  }

  /* Two successes drop one level; the streak resets between drops. */
  mdl_governor_observe(&s_gov, "h", (long)k_http_ok, nullptr); /* streak 1: no drop */
  TEST_ASSERT(mdl_governor_peek(&s_gov, "h", &level, nullptr));
  TEST_ASSERT_EQ((int64_t)k_gov_throttles, (int64_t)level);
  mdl_governor_observe(&s_gov, "h", (long)k_http_ok, nullptr); /* streak 2: drop to 4 */
  TEST_ASSERT(mdl_governor_peek(&s_gov, "h", &level, nullptr));
  TEST_ASSERT_EQ((int64_t)4, (int64_t)level);
  mdl_governor_observe(&s_gov, "h", (long)k_http_ok, nullptr);
  mdl_governor_observe(&s_gov, "h", (long)k_http_ok, nullptr); /* drop to 3 */
  TEST_ASSERT(mdl_governor_peek(&s_gov, "h", &level, nullptr));
  TEST_ASSERT_EQ((int64_t)3, (int64_t)level);
  TEST_END("gov backoff growth + decay");
}

/**
 * @test internal_test_gov_retry_after_precedence
 *
 * @par MC/DC:
 * Exercises the 429-true/503-false vector of the throttle OR in
 * ::mdl_governor_observe (paired with internal_test_gov_backoff_growth_and_decay). Also
 * asserts that `Retry-After` (delta-seconds and HTTP-date) sets the gate over a
 * shorter computed backoff, and that a later acquire waits exactly that long.
 * @brief Exercise the gov retry after precedence media-downloader scenario.
 * @details Exercises the gov retry after precedence scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gov_retry_after_precedence(void)
{
  TEST_BEGIN("gov retry-after precedence");
  gov_clock_t   clk   = {};
  mdl_gov_cfg_t cfg   = mdl_gov_cfg_default();
  cfg.rate_per_min    = 0U;
  cfg.backoff_base_ms = (uint32_t)k_gov_backoff_base;
  cfg.backoff_max_ms  = (uint32_t)k_gov_backoff_base; /* window <= 1 s, so Retry-After wins */
  mdl_governor_init_clock(&s_gov,
                          &cfg,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk,
                          internal_gov_clk_sleep,
                          &clk);

  /* delta-seconds Retry-After (120 s) beats the <= 1 s backoff, on a 429. */
  mdl_governor_observe(&s_gov, "a", (long)k_http_too_many, "120");
  int64_t gate = 0;
  TEST_ASSERT(mdl_governor_peek(&s_gov, "a", nullptr, &gate));
  TEST_ASSERT_EQ((int64_t)k_gov_retry_120s_ms, gate);
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "a", 0U, 0U) == k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_gov_retry_120s_ms, (int64_t)clk.last_slept);
  mdl_governor_release(&s_gov, "a");

  /* HTTP-date Retry-After (60 s past the epoch, clock at the epoch), on a 503. */
  gov_clock_t clk2 = {};
  mdl_governor_init_clock(&s_gov,
                          &cfg,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk2,
                          internal_gov_clk_sleep,
                          &clk2);
  mdl_governor_observe_at_wall(&s_gov,
                               "b",
                               (long)k_http_unavail,
                               "Thu, 01 Jan 1970 00:01:00 GMT",
                               0);
  gate = 0;
  TEST_ASSERT(mdl_governor_peek(&s_gov, "b", nullptr, &gate));
  TEST_ASSERT_EQ((int64_t)k_gov_retry_60s_ms, gate);

  /* A valid Retry-After also governs a non-throttle response. */
  mdl_governor_observe_at_wall(&s_gov, "c", (long)k_http_ok, "Thu, 01 Jan 1970 00:01:00 GMT", 0);
  gate = 0;
  TEST_ASSERT(mdl_governor_peek(&s_gov, "c", nullptr, &gate));
  TEST_ASSERT_EQ((int64_t)k_gov_retry_60s_ms, gate);
  TEST_END("gov retry-after precedence");
}

/**
 * @test internal_test_gov_concurrency_cap
 *
 * @par MC/DC:
 * Decision: `rec->inflight >= max_inflight` in ::mdl_governor_acquire (single
 * relational condition, N+1 = 2). Vector A: in-flight below the cap -> reserved
 * (k_ra8_ok). Vector B: in-flight at the cap -> refused (would_block). Also
 * shows the cap is per host (a second host is admitted while the first is full)
 * and that a release re-opens a slot.
 * @brief Exercise the gov concurrency cap media-downloader scenario.
 * @details Exercises the gov concurrency cap scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gov_concurrency_cap(void)
{
  TEST_BEGIN("gov concurrency cap");
  gov_clock_t   clk = {};
  mdl_gov_cfg_t cfg = mdl_gov_cfg_default();
  cfg.rate_per_min  = 0U; /* no pacing noise */
  cfg.max_inflight  = (uint16_t)k_gov_inflight_1;
  mdl_governor_init_clock(&s_gov,
                          &cfg,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk,
                          internal_gov_clk_sleep,
                          &clk);

  TEST_ASSERT(mdl_governor_acquire(&s_gov, "a", 0U, 0U) == k_ra8_ok);              /* below cap  */
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "a", 0U, 0U) == k_ra8_err_would_block); /* at cap     */
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "b", 0U, 0U) == k_ra8_ok);              /* other host */
  mdl_governor_release(&s_gov, "a");
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "a", 0U, 0U) == k_ra8_ok); /* slot freed */
  mdl_governor_release(&s_gov, "a");
  mdl_governor_release(&s_gov, "b");

  /* A cap of 2 admits two in flight and refuses the third. */
  gov_clock_t   clk2 = {};
  mdl_gov_cfg_t c2   = cfg;
  c2.max_inflight    = (uint16_t)k_gov_inflight_2;
  mdl_governor_init_clock(&s_gov,
                          &c2,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk2,
                          internal_gov_clk_sleep,
                          &clk2);
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "c", 0U, 0U) == k_ra8_ok);
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "c", 0U, 0U) == k_ra8_ok);
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "c", 0U, 0U) == k_ra8_err_would_block);
  mdl_governor_release(&s_gov, "c");
  TEST_ASSERT(mdl_governor_acquire(&s_gov, "c", 0U, 0U) == k_ra8_ok);
  TEST_END("gov concurrency cap");
}

/**
 * @test internal_test_gov_null_and_untracked
 *
 * @par MC/DC:
 * Decision: `g == NULL` in the governor entry points (single condition, N+1 =
 * 2 across the suite). NULL-`g` vectors here return the disabled-pacing answer;
 * the non-NULL side is exercised by every other test. Also covers the NULL-host
 * degradation path: no per-host record, only jitter spacing, and ::mdl_governor_peek
 * reports the host as unknown.
 * @brief Exercise the gov null and untracked media-downloader scenario.
 * @details Exercises the gov null and untracked scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gov_null_and_untracked(void)
{
  TEST_BEGIN("gov null + untracked host");
  /* NULL governor: acquire succeeds (pacing disabled); the rest are no-ops. */
  TEST_ASSERT(mdl_governor_acquire(nullptr, "h", 0U, 0U) == k_ra8_ok);
  mdl_governor_release(nullptr, "h");
  mdl_governor_observe(nullptr, "h", (long)k_http_too_many, "120");
  TEST_ASSERT(!mdl_governor_peek(nullptr, "h", nullptr, nullptr));

  gov_clock_t clk = {};
  mdl_governor_init_clock(&s_gov,
                          nullptr,
                          (uint64_t)k_gov_seed,
                          internal_gov_clk_now,
                          &clk,
                          internal_gov_clk_sleep,
                          &clk);
  /* NULL host: no per-host record; still spaced by the fixed jitter. */
  TEST_ASSERT(
    mdl_governor_acquire(&s_gov, nullptr, (uint32_t)k_gov_jitter_5, (uint32_t)k_gov_jitter_5) ==
    k_ra8_ok);
  TEST_ASSERT_EQ((int64_t)k_gov_jitter_5, (int64_t)clk.last_slept);
  TEST_ASSERT(!mdl_governor_peek(&s_gov, "never-seen", nullptr, nullptr));
  TEST_END("gov null + untracked host");
}

/**
 * @brief Run every governor unit test in sequence.
 * @return 0 when all tests passed, non-zero on the first failure.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_gov_retry_after_parse();
  internal_test_gov_rate_limit();
  internal_test_gov_backoff_growth_and_decay();
  internal_test_gov_retry_after_precedence();
  internal_test_gov_concurrency_cap();
  internal_test_gov_null_and_untracked();
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_mdl_governor.c\n");
  return 0;
}
