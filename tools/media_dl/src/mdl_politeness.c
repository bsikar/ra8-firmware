/**
 * @file mdl_politeness.c
 * @brief Seeded xorshift64 jitter, injectable clock, and the per-host governor.
 *
 * @details Implements deterministic jitter, fixed-capacity per-host pacing,
 * throttle backoff, and `Retry-After` parsing behind injectable clock seams.
 * Production uses the host clocks; tests can advance virtual time without
 * sleeping. All governor state remains in caller-owned storage.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mdl_politeness.h"
#include "ra8_attributes.h"

/**
 * @enum mdl_seed_t
 * @brief Fallback seed so the xorshift64 state is never 0.
 * @details A zero state is the one fixed point of xorshift64 -- it would emit
 *          zeros forever. The constant is the golden-ratio odd multiplier used
 *          by SplitMix64, chosen for good avalanche from a small seed.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_seed_fallback = 0x9E3779B97F4A7C15ULL, /**< Substituted when the seed is 0. */
} mdl_seed_t;

/** @brief xorshift64 shift triple (Marsaglia's 13/7/17). */
typedef enum : uint8_t {
  k_xs_shift_a = 13U, /**< First left shift.  */
  k_xs_shift_b = 7U,  /**< Right shift.       */
  k_xs_shift_c = 17U, /**< Second left shift. */
} mdl_xorshift_t;

/** @brief Time-unit conversions shared by the sleep and governor clocks. */
typedef enum : uint32_t {
  k_ms_per_s  = 1000U,    /**< Milliseconds per second.      */
  k_ns_per_ms = 1000000U, /**< Nanoseconds per millisecond.  */
  k_dec_base  = 10U,      /**< Base-10 radix for `strtoull`. */
} mdl_time_unit_t;

void mdl_politeness_init(mdl_politeness_t* p, uint64_t seed)
{
  mdl_politeness_init_clock(p, seed, nullptr, nullptr);
}

void mdl_politeness_init_clock(mdl_politeness_t* p,
                               uint64_t          seed,
                               mdl_sleep_fn      sleep_fn,
                               void*             sleep_ctx)
{
  if (p == nullptr) {
    return;
  }
  p->state     = (seed == 0U) ? (uint64_t)k_seed_fallback : seed;
  p->sleep_fn  = sleep_fn;
  p->sleep_ctx = sleep_ctx;
}

/**
 * @brief Advance an xorshift64 state in place and return the new value.
 * @details Applies the fixed three-shift recurrence used by every jitter draw.
 * @param[in,out] state Non-zero PRNG state.
 * @return The next PRNG value.
 * @retval other Updated non-zero xorshift64 state.
 * @pre @p state points to writable storage.
 * @pre `*state` is non-zero.
 * @post `*state` equals the returned value.
 * @post Exactly one PRNG step has been consumed.
 * @note Not thread-safe when callers share @p state.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_next_rand(uint64_t* state)
{
  uint64_t x = *state;
  x ^= x << (uint64_t)k_xs_shift_a;
  x ^= x >> (uint64_t)k_xs_shift_b;
  x ^= x << (uint64_t)k_xs_shift_c;
  *state = x;
  return x;
}

/**
 * @brief Draw a jittered value in [min_ms, max(min_ms, max_ms)] from `state`.
 * @details Advances the PRNG once and maps the result across the inclusive range.
 * @param[in,out] state Non-zero PRNG state.
 * @param[in] min_ms Inclusive lower bound.
 * @param[in] max_ms Inclusive upper bound, clamped up to @p min_ms.
 * @return The selected bounded value.
 * @retval other A value in the documented inclusive range.
 * @pre @p state points to writable non-zero state.
 * @pre Both bounds are expressed in milliseconds.
 * @post `*state` has advanced exactly once.
 * @post The return is never below @p min_ms.
 * @note Not thread-safe when callers share @p state.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_draw_range(uint64_t* state, uint32_t min_ms, uint32_t max_ms)
{
  if (max_ms < min_ms) {
    max_ms = min_ms;
  }
  /* 64-bit span keeps the full-range (min=0, max=UINT32_MAX) case from wrapping
   * while preserving the exact modulo of the original jitter for smaller spans. */
  const uint64_t span = (uint64_t)(max_ms - min_ms) + 1U;
  return min_ms + (uint32_t)(internal_next_rand(state) % span);
}

/**
 * @brief Block for `ms` milliseconds on the host clock.
 * @details Converts milliseconds to `timespec` and delegates to `nanosleep`.
 * @param[in] ms Requested duration in milliseconds.
 * @return Nothing.
 * @pre @p ms is a finite `uint32_t` duration.
 * @pre The caller permits the current thread to block.
 * @post One host sleep has been requested.
 * @post No caller-owned state is modified.
 * @note An interrupted sleep is not resumed.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_sleep_ms(uint32_t ms)
{
  struct timespec ts = {.tv_sec  = (time_t)(ms / k_ms_per_s),
                        .tv_nsec = (long)((ms % k_ms_per_s) * k_ns_per_ms)};
  (void)nanosleep(&ts, nullptr);
}

uint32_t mdl_politeness_wait(mdl_politeness_t* p, uint32_t min_ms, uint32_t max_ms)
{
  if (p == nullptr) {
    return 0U;
  }
  const uint32_t delayms = internal_draw_range(&p->state, min_ms, max_ms);

  if (p->sleep_fn != nullptr) {
    p->sleep_fn(p->sleep_ctx, delayms);
  } else {
    internal_host_sleep_ms(delayms);
  }
  return delayms;
}

/* ======================================================================== *
 *  Retry-After parsing (#301)                                              *
 * ======================================================================== */

/** @brief HTTP status codes the governor treats as a throttle. */
typedef enum : uint16_t {
  k_http_too_many_req = 429U, /**< Too Many Requests.   */
  k_http_unavailable  = 503U, /**< Service Unavailable. */
} mdl_http_throttle_t;

/** @brief Governor default tunables (conservative; see mdl_gov_cfg_default). */
typedef enum : uint32_t {
  k_def_rate_per_min    = 60U,    /**< ~1 request/second sustained. */
  k_def_burst           = 4U,     /**< Small burst allowance.       */
  k_def_backoff_base_ms = 1000U,  /**< 1 s first backoff window.    */
  k_def_backoff_max_ms  = 60000U, /**< 60 s backoff ceiling.        */
} mdl_gov_defaults_t;

/** @brief Governor default counts that fit uint16 fields. */
typedef enum : uint16_t {
  k_def_decay_after  = 4U, /**< Successes that drop one backoff level. */
  k_def_max_inflight = 1U, /**< Strictly serial per host by default.   */
} mdl_gov_defaults16_t;

/**
 * @brief Saturating conversion of a signed-seconds delay into a ms delay.
 * @details Clamps past dates to zero and values above the millisecond range to `UINT32_MAX`.
 * @param[in] secs Signed duration in seconds.
 * @return Saturated duration in milliseconds.
 * @retval 0 @p secs is non-positive.
 * @retval UINT32_MAX The converted value would overflow.
 * @pre @p secs uses the same second scale as the parsed date.
 * @pre Negative values represent elapsed deadlines.
 * @post The return is within the `uint32_t` range.
 * @post No state is modified.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_ms_from_secs(int64_t secs)
{
  if (secs <= 0) {
    return 0U;
  }
  if (secs > (int64_t)(UINT32_MAX / k_ms_per_s)) {
    return UINT32_MAX;
  }
  return (uint32_t)(secs * (int64_t)k_ms_per_s);
}

/**
 * @brief True when `s` is a non-empty run of ASCII digits.
 * @details Rejects an empty string and any byte outside `0` through `9`.
 * @param[in] s NUL-terminated candidate string.
 * @return Whether every byte is an ASCII digit and at least one exists.
 * @retval true A non-empty digit run was found.
 * @retval false The string is empty or contains another byte.
 * @pre @p s is non-NULL and NUL-terminated.
 * @pre The caller requires locale-independent ASCII classification.
 * @post @p s is unchanged.
 * @post No global state is modified.
 * @note Thread-safe: reads only its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_all_digits(const char* s)
{
  if (*s == '\0') {
    return false;
  }
  for (const char* c = s; *c != '\0'; ++c) {
    if ((*c < '0') || (*c > '9')) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Parse an HTTP-date `Retry-After` (IMF-fixdate or RFC 850) into a delay.
 * @details Tries both supported UTC formats and saturates the deadline delta to milliseconds.
 * @param[in] value NUL-terminated HTTP-date text.
 * @param[in] now_wall_s Current Unix wall-clock time in seconds.
 * @param[out] out_ms Parsed non-negative delay.
 * @return Whether a supported date parsed successfully.
 * @retval true @p out_ms was written.
 * @retval false Neither date format was valid.
 * @pre @p value and @p out_ms are non-NULL.
 * @pre @p now_wall_s and the parsed date share the Unix epoch.
 * @post On true, @p out_ms contains a saturated delay.
 * @post On false, @p out_ms is unchanged.
 * @note Thread-safe on platforms providing thread-safe `timegm`/`strptime`.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_parse_http_date(const char* value, int64_t now_wall_s, uint32_t* out_ms)
{
  static const char* const k_fmts[] = {
    "%a, %d %b %Y %H:%M:%S GMT", /* IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT  */
    "%A, %d-%b-%y %H:%M:%S GMT", /* RFC 850:     Sunday, 06-Nov-94 08:49:37 GMT */
  };
  for (size_t i = 0U; i < (sizeof(k_fmts) / sizeof(k_fmts[0])); ++i) {
    struct tm tmv = {};
    if (strptime(value, k_fmts[i], &tmv) != nullptr) {
      const time_t t = timegm(&tmv);
      if (t == (time_t)-1) {
        continue;
      }
      *out_ms = internal_ms_from_secs((int64_t)t - now_wall_s);
      return true;
    }
  }
  return false;
}

bool mdl_retry_after_parse(const char* value, int64_t now_wall_s, uint32_t* out_ms)
{
  if ((value == nullptr) || (out_ms == nullptr)) {
    return false;
  }
  while ((*value == ' ') || (*value == '\t')) {
    ++value;
  }
  if (*value == '\0') {
    return false;
  }
  if (internal_all_digits(value)) {
    *out_ms = internal_ms_from_secs((int64_t)strtoull(value, nullptr, k_dec_base));
    return true;
  }
  return internal_parse_http_date(value, now_wall_s, out_ms);
}

/* ======================================================================== *
 *  Per-host politeness governor (#301)                                     *
 * ======================================================================== */

/**
 * @brief Smaller of two signed millisecond values.
 * @details Performs a direct signed comparison.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The smaller value.
 * @retval other Either @p a or @p b.
 * @pre Both arguments use the same unit and timeline.
 * @pre Signed comparison is the intended ordering.
 * @post No state is modified.
 * @post The result is no greater than either argument.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_min_i64(int64_t a, int64_t b)
{
  return (a < b) ? a : b;
}

/**
 * @brief Larger of two signed millisecond values.
 * @details Performs a direct signed comparison.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The larger value.
 * @retval other Either @p a or @p b.
 * @pre Both arguments use the same unit and timeline.
 * @pre Signed comparison is the intended ordering.
 * @post No state is modified.
 * @post The result is no less than either argument.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_max_i64(int64_t a, int64_t b)
{
  return (a > b) ? a : b;
}

/**
 * @brief Read the governor's clock: injected `now_fn`, else `CLOCK_MONOTONIC`.
 * @details Preserves the arbitrary monotonic epoch used for rate and backoff differences.
 * @param[in] g Initialised governor containing the optional clock seam.
 * @return Current monotonic time in milliseconds.
 * @retval other A reading on the governor timeline.
 * @pre @p g is non-NULL and initialised.
 * @pre An injected clock, when present, does not run backward.
 * @post Governor state is unchanged.
 * @post At most one clock source is read.
 * @note Thread safety follows the injected clock implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_gov_now(const mdl_governor_t* g)
{
  if (g->now_fn != nullptr) {
    return g->now_fn(g->now_ctx);
  }
  /* CLOCK_MONOTONIC, not CLOCK_REALTIME (#509). The governor only ever
   * subtracts two readings -- token refill, request spacing, the backoff
   * gate -- and CLOCK_REALTIME is steppable: an NTP correction forward makes
   * the governor believe the spacing has elapsed and hammer the remote host,
   * and one backward makes it wait out the step. A ~4 minute backward step
   * was measured happening repeatedly on a fleet host, so this is an observed
   * hazard rather than a theoretical one. */
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * (int64_t)k_ms_per_s) + ((int64_t)ts.tv_nsec / (int64_t)k_ns_per_ms);
}

/**
 * @brief Sleep `ms` through the injected sleeper, else the host clock.
 * @details Ignores non-positive delays and saturates positive delays to `uint32_t`.
 * @param[in,out] g Initialised governor containing the optional sleeper seam.
 * @param[in] ms Requested signed delay in milliseconds.
 * @return Nothing.
 * @pre @p g is non-NULL and initialised.
 * @pre The caller permits a positive delay to block.
 * @post Non-positive input performs no sleep.
 * @post Positive input requests exactly one bounded sleep.
 * @note Thread safety follows the injected sleeper implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gov_sleep(mdl_governor_t* g, int64_t ms)
{
  if (ms <= 0) {
    return;
  }
  const uint32_t d = (ms > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
  if (g->sleep_fn != nullptr) {
    g->sleep_fn(g->sleep_ctx, d);
  } else {
    internal_host_sleep_ms(d);
  }
}

/**
 * @brief Token interval (ms per request); 0 when rate limiting is disabled.
 * @details Converts the configured requests-per-minute ceiling by integer division.
 * @param[in] cfg Governor rate configuration.
 * @return Milliseconds charged per request.
 * @retval 0 Rate limiting is disabled.
 * @retval other Positive request interval.
 * @pre @p cfg is non-NULL.
 * @pre `rate_per_min == 0` denotes disabled pacing.
 * @post @p cfg is unchanged.
 * @post No state is modified.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_gov_interval_ms(const mdl_gov_cfg_t* cfg)
{
  return (cfg->rate_per_min > 0U) ? ((int64_t)k_mdl_gov_ms_per_req / (int64_t)cfg->rate_per_min)
                                  : 0;
}

/**
 * @brief Token-bucket capacity in ms (`interval * burst`).
 * @details Expresses the configured burst capacity on the rate-credit timeline.
 * @param[in] cfg Governor rate and burst configuration.
 * @return Maximum token credit in milliseconds.
 * @retval 0 Rate limiting is disabled or burst is zero.
 * @retval other Product of interval and burst.
 * @pre @p cfg is non-NULL.
 * @pre The configuration was clamped by governor initialisation.
 * @post @p cfg is unchanged.
 * @post No state is modified.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_gov_cap_ms(const mdl_gov_cfg_t* cfg)
{
  return internal_gov_interval_ms(cfg) * (int64_t)cfg->burst;
}

/** @brief Find an existing per-host record, or NULL. */
RA8_INTERNAL static mdl_host_rec_t* internal_gov_find(mdl_governor_t* g, const char* host)
{
  if (host == nullptr) {
    return nullptr;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_mdl_gov_max_hosts; ++i) {
    if (g->hosts[i].used && (strcmp(g->hosts[i].host, host) == 0)) {
      return &g->hosts[i];
    }
  }
  return nullptr;
}

/** @brief Find-or-create a per-host record; NULL if the table is full or host NULL. */
RA8_INTERNAL static mdl_host_rec_t*
internal_gov_get(mdl_governor_t* g, const char* host, int64_t now)
{
  mdl_host_rec_t* rec = internal_gov_find(g, host);
  if ((rec != nullptr) || (host == nullptr)) {
    return rec;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_mdl_gov_max_hosts; ++i) {
    if (!g->hosts[i].used) {
      g->hosts[i]                  = (mdl_host_rec_t){};
      g->hosts[i].used             = true;
      g->hosts[i].credit_ms        = internal_gov_cap_ms(&g->cfg); /* start full: allow a burst */
      g->hosts[i].last_ms          = now;
      g->hosts[i].earliest_next_ms = now;
      (void)snprintf(g->hosts[i].host, sizeof(g->hosts[i].host), "%s", host);
      return &g->hosts[i];
    }
  }
  return nullptr;
}

/**
 * @brief Refill credit to `now`, gate on rate + backoff, consume one token.
 * @details Caps accrued credit, selects the later rate/backoff gate, and charges one request.
 * @param[in] g Initialised governor configuration.
 * @param[in,out] rec Host record to schedule.
 * @param[in] now Current monotonic time in milliseconds.
 * @return Required wait before the request may start.
 * @retval 0 The request may start immediately.
 * @retval other Positive wait in milliseconds.
 * @pre @p g and @p rec are non-NULL and belong to the same governor.
 * @pre @p now is on the governor's monotonic timeline.
 * @post Credit remains within its configured capacity.
 * @post @p rec records the scheduled start and consumed token.
 * @note Not thread-safe: mutates @p rec.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t
internal_gov_schedule(mdl_governor_t* g, mdl_host_rec_t* rec, int64_t now)
{
  const int64_t interval  = internal_gov_interval_ms(&g->cfg);
  const int64_t cap       = internal_gov_cap_ms(&g->cfg);
  const int64_t elapsed   = internal_max_i64(now - rec->last_ms, 0);
  rec->credit_ms          = internal_min_i64(rec->credit_ms + elapsed, cap);
  const int64_t rate_wait = (rec->credit_ms < interval) ? (interval - rec->credit_ms) : 0;
  const int64_t target    = internal_max_i64(now + rate_wait, rec->earliest_next_ms);
  const int64_t wait      = target - now;
  rec->credit_ms = internal_max_i64(internal_min_i64(rec->credit_ms + wait, cap) - interval, 0);
  rec->last_ms   = target;
  return wait;
}

mdl_gov_cfg_t mdl_gov_cfg_default(void)
{
  return (mdl_gov_cfg_t){
    .rate_per_min    = (uint32_t)k_def_rate_per_min,
    .burst           = (uint32_t)k_def_burst,
    .backoff_base_ms = (uint32_t)k_def_backoff_base_ms,
    .backoff_max_ms  = (uint32_t)k_def_backoff_max_ms,
    .decay_after     = (uint16_t)k_def_decay_after,
    .max_inflight    = (uint16_t)k_def_max_inflight,
  };
}

void mdl_governor_init(mdl_governor_t* g, const mdl_gov_cfg_t* cfg, uint64_t seed)
{
  mdl_governor_init_clock(g, cfg, seed, nullptr, nullptr, nullptr, nullptr);
}

void mdl_governor_init_clock(mdl_governor_t*      g,
                             const mdl_gov_cfg_t* cfg,
                             uint64_t             seed,
                             mdl_now_fn           now_fn,
                             void*                now_ctx,
                             mdl_sleep_fn         sleep_fn,
                             void*                sleep_ctx)
{
  if (g == nullptr) {
    return;
  }
  memset(g, 0, sizeof(*g));
  g->cfg = (cfg != nullptr) ? *cfg : mdl_gov_cfg_default();
  if (g->cfg.burst == 0U) {
    g->cfg.burst = 1U;
  }
  if (g->cfg.max_inflight == 0U) {
    g->cfg.max_inflight = 1U;
  }
  g->rng       = (seed == 0U) ? (uint64_t)k_seed_fallback : seed;
  g->now_fn    = now_fn;
  g->now_ctx   = now_ctx;
  g->sleep_fn  = sleep_fn;
  g->sleep_ctx = sleep_ctx;
}

ra8_err_t mdl_governor_acquire(mdl_governor_t* g,
                               const char*     host,
                               uint32_t        jitter_min_ms,
                               uint32_t        jitter_max_ms)
{
  if (g == nullptr) {
    return k_ra8_ok; /* pacing disabled -- matches a NULL jitter source */
  }
  const int64_t   now = internal_gov_now(g);
  mdl_host_rec_t* rec = internal_gov_get(g, host, now);
  if (rec == nullptr) {
    /* NULL host or table full: still space requests with jitter, no tracking. */
    internal_gov_sleep(g, (int64_t)internal_draw_range(&g->rng, jitter_min_ms, jitter_max_ms));
    return k_ra8_ok;
  }
  if (rec->inflight >= g->cfg.max_inflight) {
    return k_ra8_err_would_block;
  }
  const int64_t wait   = internal_gov_schedule(g, rec, now);
  const int64_t jitter = (int64_t)internal_draw_range(&g->rng, jitter_min_ms, jitter_max_ms);
  internal_gov_sleep(g, wait + jitter);
  rec->inflight += 1U;
  return k_ra8_ok;
}

void mdl_governor_release(mdl_governor_t* g, const char* host)
{
  if (g == nullptr) {
    return;
  }
  mdl_host_rec_t* rec = internal_gov_find(g, host);
  if ((rec != nullptr) && (rec->inflight > 0U)) {
    rec->inflight -= 1U;
  }
}

/**
 * @brief Exponential backoff window for a level: `min(base << (level-1), ceil)`.
 * @details Doubles from the configured base with bounded shifts and ceiling saturation.
 * @param[in] cfg Governor backoff configuration.
 * @param[in] level One-based throttle level; zero uses the base window.
 * @return Bounded backoff window in milliseconds.
 * @retval other A value no greater than `backoff_max_ms`.
 * @pre @p cfg is non-NULL.
 * @pre @p level is bounded by ::k_mdl_gov_level_max in stored state.
 * @post @p cfg is unchanged.
 * @post No state is modified.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t internal_gov_backoff_window(const mdl_gov_cfg_t* cfg, uint16_t level)
{
  int64_t        window = (int64_t)cfg->backoff_base_ms;
  const uint16_t shifts = (level > 1U) ? (uint16_t)(level - 1U) : 0U;
  for (uint16_t i = 0U; (i < shifts) && (i < (uint16_t)k_mdl_gov_level_max); ++i) {
    window <<= 1;
  }
  if ((window < 0) || (window > (int64_t)cfg->backoff_max_ms)) {
    window = (int64_t)cfg->backoff_max_ms;
  }
  return window;
}

/**
 * @brief Apply a throttle: raise the backoff level and push the gate forward.
 * @details Applies capped exponential full jitter while allowing a longer Retry-After to win.
 * @param[in,out] g Governor whose jitter state advances.
 * @param[in,out] rec Matching host record.
 * @param[in] now Current monotonic time in milliseconds.
 * @param[in] retry_ms Parsed Retry-After delay, or zero.
 * @return Nothing.
 * @pre @p g and @p rec are non-NULL and associated.
 * @pre @p now is on the governor timeline.
 * @post The backoff level is raised at most to its ceiling.
 * @post The earliest-next gate never moves backward.
 * @note Not thread-safe: mutates governor and host state.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_gov_on_throttle(mdl_governor_t* g, mdl_host_rec_t* rec, int64_t now, uint32_t retry_ms)
{
  if (rec->backoff_level < (uint16_t)k_mdl_gov_level_max) {
    rec->backoff_level += 1U;
  }
  rec->success_streak   = 0U;
  const int64_t window  = internal_gov_backoff_window(&g->cfg, rec->backoff_level);
  const int64_t backoff = (int64_t)internal_draw_range(&g->rng, 0U, (uint32_t)window);
  const int64_t gate    = now + internal_max_i64(backoff, (int64_t)retry_ms);
  rec->earliest_next_ms = internal_max_i64(rec->earliest_next_ms, gate);
}

/**
 * @brief Apply a non-throttle outcome: count success, decay, honour Retry-After.
 * @details Advances the success streak, decays one level at threshold, and applies an optional gate.
 * @param[in] g Governor providing the decay threshold.
 * @param[in,out] rec Matching host record.
 * @param[in] now Current monotonic time in milliseconds.
 * @param[in] has_retry Whether @p retry_ms came from a valid header.
 * @param[in] retry_ms Parsed Retry-After delay.
 * @return Nothing.
 * @pre @p g and @p rec are non-NULL and associated.
 * @pre @p now is on the governor timeline.
 * @post At most one backoff level is removed.
 * @post A valid Retry-After never moves the gate backward.
 * @note Not thread-safe: mutates the host record.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gov_on_success(mdl_governor_t* g,
                                                 mdl_host_rec_t* rec,
                                                 int64_t         now,
                                                 bool            has_retry,
                                                 uint32_t        retry_ms)
{
  rec->success_streak += 1U;
  if ((g->cfg.decay_after > 0U) && (rec->success_streak >= g->cfg.decay_after)) {
    if (rec->backoff_level > 0U) {
      rec->backoff_level -= 1U;
    }
    rec->success_streak = 0U;
  }
  if (has_retry) {
    rec->earliest_next_ms = internal_max_i64(rec->earliest_next_ms, now + (int64_t)retry_ms);
  }
}

void mdl_governor_observe_at_wall(mdl_governor_t* g,
                                  const char*     host,
                                  long            status,
                                  const char*     retry_after,
                                  int64_t         now_wall_s)
{
  if (g == nullptr) {
    return;
  }
  const int64_t   now = internal_gov_now(g);
  mdl_host_rec_t* rec = internal_gov_get(g, host, now);
  if (rec == nullptr) {
    return; /* NULL host or table full */
  }
  uint32_t   retry_ms = 0U;
  const bool has_retry =
    (retry_after != nullptr) && mdl_retry_after_parse(retry_after, now_wall_s, &retry_ms);
  const bool throttled =
    (status == (long)k_http_too_many_req) || (status == (long)k_http_unavailable);
  if (throttled) {
    internal_gov_on_throttle(g, rec, now, retry_ms);
  } else {
    internal_gov_on_success(g, rec, now, has_retry, retry_ms);
  }
}

void mdl_governor_observe(mdl_governor_t* g, const char* host, long status, const char* retry_after)
{
  mdl_governor_observe_at_wall(g, host, status, retry_after, (int64_t)time(nullptr));
}

bool mdl_governor_peek(const mdl_governor_t* g,
                       const char*           host,
                       uint16_t*             backoff_level,
                       int64_t*              earliest_next_ms)
{
  if ((g == nullptr) || (host == nullptr)) {
    return false;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_mdl_gov_max_hosts; ++i) {
    if (g->hosts[i].used && (strcmp(g->hosts[i].host, host) == 0)) {
      if (backoff_level != nullptr) {
        *backoff_level = g->hosts[i].backoff_level;
      }
      if (earliest_next_ms != nullptr) {
        *earliest_next_ms = g->hosts[i].earliest_next_ms;
      }
      return true;
    }
  }
  return false;
}
