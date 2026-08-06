/**
 * @file mdl_politeness.c
 * @brief Seeded xorshift64 jitter, injectable clock, and the per-host governor.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */
#include "mdl_politeness.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

RA8_DI_SLOT("politeness_sleep")
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

/** @brief Advance an xorshift64 state in place and return the new value. */
RA8_INTERNAL static uint64_t next_rand(uint64_t* state)
{
  uint64_t x = *state;
  x ^= x << (uint64_t)k_xs_shift_a;
  x ^= x >> (uint64_t)k_xs_shift_b;
  x ^= x << (uint64_t)k_xs_shift_c;
  *state = x;
  return x;
}

/** @brief Draw a jittered value in [min_ms, max(min_ms, max_ms)] from `state`. */
RA8_INTERNAL static uint32_t draw_range(uint64_t* state, uint32_t min_ms, uint32_t max_ms)
{
  if (max_ms < min_ms) {
    max_ms = min_ms;
  }
  /* 64-bit span keeps the full-range (min=0, max=UINT32_MAX) case from wrapping
   * while preserving the exact modulo of the original jitter for smaller spans. */
  const uint64_t span = (uint64_t)(max_ms - min_ms) + 1U;
  return min_ms + (uint32_t)(next_rand(state) % span);
}

/** @brief Block for `ms` milliseconds on the host clock. */
RA8_INTERNAL static void host_sleep_ms(uint32_t ms)
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
  const uint32_t delayms = draw_range(&p->state, min_ms, max_ms);

  if (p->sleep_fn != nullptr) {
    p->sleep_fn(p->sleep_ctx, delayms);
  } else {
    host_sleep_ms(delayms);
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

/** @brief Saturating conversion of a signed-seconds delay into a ms delay. */
RA8_INTERNAL static uint32_t ms_from_secs(int64_t secs)
{
  if (secs <= 0) {
    return 0U;
  }
  if (secs > (int64_t)(UINT32_MAX / k_ms_per_s)) {
    return UINT32_MAX;
  }
  return (uint32_t)(secs * (int64_t)k_ms_per_s);
}

/** @brief True when `s` is a non-empty run of ASCII digits. */
RA8_INTERNAL static bool all_digits(const char* s)
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

/** @brief Parse an HTTP-date `Retry-After` (IMF-fixdate or RFC 850) into a delay. */
RA8_INTERNAL static bool parse_http_date(const char* value, int64_t now_wall_s, uint32_t* out_ms)
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
      *out_ms = ms_from_secs((int64_t)t - now_wall_s);
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
  if (all_digits(value)) {
    *out_ms = ms_from_secs((int64_t)strtoull(value, nullptr, k_dec_base));
    return true;
  }
  return parse_http_date(value, now_wall_s, out_ms);
}

/* ======================================================================== *
 *  Per-host politeness governor (#301)                                     *
 * ======================================================================== */

/** @brief Smaller of two signed millisecond values. */
RA8_INTERNAL static int64_t min_i64(int64_t a, int64_t b)
{
  return (a < b) ? a : b;
}

/** @brief Larger of two signed millisecond values. */
RA8_INTERNAL static int64_t max_i64(int64_t a, int64_t b)
{
  return (a > b) ? a : b;
}

/** @brief Read the governor's clock: injected `now_fn`, else `CLOCK_MONOTONIC`. */
RA8_INTERNAL static int64_t gov_now(const mdl_governor_t* g)
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

/** @brief Sleep `ms` through the injected sleeper, else the host clock. */
RA8_INTERNAL static void gov_sleep(mdl_governor_t* g, int64_t ms)
{
  if (ms <= 0) {
    return;
  }
  const uint32_t d = (ms > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
  if (g->sleep_fn != nullptr) {
    g->sleep_fn(g->sleep_ctx, d);
  } else {
    host_sleep_ms(d);
  }
}

/** @brief Token interval (ms per request); 0 when rate limiting is disabled. */
RA8_INTERNAL static int64_t gov_interval_ms(const mdl_gov_cfg_t* cfg)
{
  return (cfg->rate_per_min > 0U) ? ((int64_t)k_mdl_gov_ms_per_req / (int64_t)cfg->rate_per_min)
                                  : 0;
}

/** @brief Token-bucket capacity in ms (`interval * burst`). */
RA8_INTERNAL static int64_t gov_cap_ms(const mdl_gov_cfg_t* cfg)
{
  return gov_interval_ms(cfg) * (int64_t)cfg->burst;
}

/** @brief Find an existing per-host record, or NULL. */
RA8_INTERNAL static mdl_host_rec_t* gov_find(mdl_governor_t* g, const char* host)
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
RA8_INTERNAL static mdl_host_rec_t* gov_get(mdl_governor_t* g, const char* host, int64_t now)
{
  mdl_host_rec_t* rec = gov_find(g, host);
  if ((rec != nullptr) || (host == nullptr)) {
    return rec;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_mdl_gov_max_hosts; ++i) {
    if (!g->hosts[i].used) {
      g->hosts[i]                  = (mdl_host_rec_t){};
      g->hosts[i].used             = true;
      g->hosts[i].credit_ms        = gov_cap_ms(&g->cfg); /* start full: allow a burst */
      g->hosts[i].last_ms          = now;
      g->hosts[i].earliest_next_ms = now;
      (void)snprintf(g->hosts[i].host, sizeof(g->hosts[i].host), "%s", host);
      return &g->hosts[i];
    }
  }
  return nullptr;
}

/** @brief Refill credit to `now`, gate on rate + backoff, consume one token. */
RA8_INTERNAL static int64_t gov_schedule(mdl_governor_t* g, mdl_host_rec_t* rec, int64_t now)
{
  const int64_t interval  = gov_interval_ms(&g->cfg);
  const int64_t cap       = gov_cap_ms(&g->cfg);
  const int64_t elapsed   = max_i64(now - rec->last_ms, 0);
  rec->credit_ms          = min_i64(rec->credit_ms + elapsed, cap);
  const int64_t rate_wait = (rec->credit_ms < interval) ? (interval - rec->credit_ms) : 0;
  const int64_t target    = max_i64(now + rate_wait, rec->earliest_next_ms);
  const int64_t wait      = target - now;
  rec->credit_ms          = max_i64(min_i64(rec->credit_ms + wait, cap) - interval, 0);
  rec->last_ms            = target;
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

RA8_DI_SLOT("governor_clock")
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
  const int64_t   now = gov_now(g);
  mdl_host_rec_t* rec = gov_get(g, host, now);
  if (rec == nullptr) {
    /* NULL host or table full: still space requests with jitter, no tracking. */
    gov_sleep(g, (int64_t)draw_range(&g->rng, jitter_min_ms, jitter_max_ms));
    return k_ra8_ok;
  }
  if (rec->inflight >= g->cfg.max_inflight) {
    return k_ra8_err_would_block;
  }
  const int64_t wait   = gov_schedule(g, rec, now);
  const int64_t jitter = (int64_t)draw_range(&g->rng, jitter_min_ms, jitter_max_ms);
  gov_sleep(g, wait + jitter);
  rec->inflight += 1U;
  return k_ra8_ok;
}

void mdl_governor_release(mdl_governor_t* g, const char* host)
{
  if (g == nullptr) {
    return;
  }
  mdl_host_rec_t* rec = gov_find(g, host);
  if ((rec != nullptr) && (rec->inflight > 0U)) {
    rec->inflight -= 1U;
  }
}

/** @brief Exponential backoff window for a level: `min(base << (level-1), ceil)`. */
RA8_INTERNAL static int64_t gov_backoff_window(const mdl_gov_cfg_t* cfg, uint16_t level)
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

/** @brief Apply a throttle: raise the backoff level and push the gate forward. */
RA8_INTERNAL static void
gov_on_throttle(mdl_governor_t* g, mdl_host_rec_t* rec, int64_t now, uint32_t retry_ms)
{
  if (rec->backoff_level < (uint16_t)k_mdl_gov_level_max) {
    rec->backoff_level += 1U;
  }
  rec->success_streak   = 0U;
  const int64_t window  = gov_backoff_window(&g->cfg, rec->backoff_level);
  const int64_t backoff = (int64_t)draw_range(&g->rng, 0U, (uint32_t)window);
  const int64_t gate    = now + max_i64(backoff, (int64_t)retry_ms);
  rec->earliest_next_ms = max_i64(rec->earliest_next_ms, gate);
}

/** @brief Apply a non-throttle outcome: count success, decay, honour Retry-After. */
RA8_INTERNAL static void gov_on_success(const mdl_governor_t* g,
                                        mdl_host_rec_t*       rec,
                                        int64_t               now,
                                        bool                  has_retry,
                                        uint32_t              retry_ms)
{
  rec->success_streak += 1U;
  if ((g->cfg.decay_after > 0U) && (rec->success_streak >= g->cfg.decay_after)) {
    if (rec->backoff_level > 0U) {
      rec->backoff_level -= 1U;
    }
    rec->success_streak = 0U;
  }
  if (has_retry) {
    rec->earliest_next_ms = max_i64(rec->earliest_next_ms, now + (int64_t)retry_ms);
  }
}

void mdl_governor_observe(mdl_governor_t* g, const char* host, long status, const char* retry_after)
{
  if (g == nullptr) {
    return;
  }
  const int64_t   now = gov_now(g);
  mdl_host_rec_t* rec = gov_get(g, host, now);
  if (rec == nullptr) {
    return; /* NULL host or table full */
  }
  uint32_t   retry_ms  = 0U;
  const bool has_retry = (retry_after != nullptr) &&
                         mdl_retry_after_parse(retry_after, now / (int64_t)k_ms_per_s, &retry_ms);
  const bool throttled =
    (status == (long)k_http_too_many_req) || (status == (long)k_http_unavailable);
  if (throttled) {
    gov_on_throttle(g, rec, now, retry_ms);
  } else {
    gov_on_success(g, rec, now, has_retry, retry_ms);
  }
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
