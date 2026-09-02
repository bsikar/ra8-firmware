/**
 * @file mdl_politeness.h
 * @brief Jittered inter-request delay plus the per-host politeness governor.
 *
 * @details
 * Two layers live here. The lower is the jitter primitive kept from the Kotlin
 * original: ::mdl_politeness_wait sleeps a seeded, deterministic random delay
 * so fixed-interval requests do not fingerprint us. The upper is the closed-loop
 * governor (::mdl_governor_t) the issue calls for: a per-host token bucket that
 * bounds the sustained request rate, exponential backoff with full jitter on a
 * 429/503, `Retry-After` honouring (both delta-seconds and HTTP-date forms),
 * and a per-host in-flight cap. The governor reacts to what the server tells us;
 * open-loop jitter never did.
 *
 * Both layers reach the outside world only through injectable seams so their
 * timing is unit-testable with no real sleeps and no real clock: production
 * wires the host clock (`nanosleep` / `CLOCK_MONOTONIC`), tests wire fakes that
 * record the requested delay and advance a virtual clock. The maths is identical
 * either way, so a test asserts spacing, backoff growth/decay and `Retry-After`
 * precedence deterministically.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Injected blocking sleep: pause the caller for `ms` milliseconds.
 *
 * @details
 * The dependency-injection seam for ::mdl_politeness_wait. Production binds the
 * host clock; a unit test binds a fake that records `ms` and returns at once,
 * so a backoff/spacing policy is verified without wall-clock time passing.
 *
 * @param[in] ctx Opaque context supplied at ::mdl_politeness_init_clock.
 * @param[in] ms  Requested sleep duration in milliseconds.
 * @return Nothing.
 * @since 0.1.0
 */
typedef void (*mdl_sleep_fn)(void* ctx, uint32_t ms);

/**
 * @struct mdl_politeness_t
 * @brief Deterministic jitter source plus its (optional) injected clock.
 * @details Seeded xorshift64 state and the sleep seam it drives. A NULL
 *          `sleep_fn` means "use the real host clock".
 * @invariant `state` is never 0 after ::mdl_politeness_init /
 *            ::mdl_politeness_init_clock.
 * @see mdl_politeness_wait()
 * @since 0.1.0
 */
typedef struct {
  uint64_t     state;     /**< PRNG state; never 0 after init.             */
  mdl_sleep_fn sleep_fn;  /**< Injected sleeper; NULL uses the host clock. */
  void*        sleep_ctx; /**< Context passed to `sleep_fn`.               */
} mdl_politeness_t;

/**
 * @brief Seed the jitter source, using the real host clock for sleeps.
 *
 * @details
 * Equivalent to ::mdl_politeness_init_clock with a NULL sleeper: the jitter is
 * seeded deterministically and ::mdl_politeness_wait blocks on `nanosleep`.
 *
 * @param[in,out] p    State to initialise (must be non-NULL).
 * @param[in]     seed Any value; 0 is remapped to a non-zero constant.
 *
 * @return Nothing.
 *
 * @pre `p`, when non-NULL, points to writable ::mdl_politeness_t storage.
 * @pre A NULL `p` is a tolerated no-op.
 * @post `p->state` is non-zero and `p->sleep_fn` is NULL.
 * @post The same `seed` yields the same delay sequence.
 *
 * @note Not thread-safe: initialises caller storage.
 * @since 0.1.0
 */
void mdl_politeness_init(mdl_politeness_t* p, uint64_t seed);

/**
 * @brief Seed the jitter source and inject a clock for the blocking sleep.
 *
 * @details
 * The dependency-injection entry point. Wires @p sleep_fn / @p sleep_ctx as the
 * sleeper ::mdl_politeness_wait calls, so a test drives spacing/backoff timing
 * through a fake clock with no real delay. A NULL @p sleep_fn selects the host
 * clock, making this a strict superset of ::mdl_politeness_init.
 *
 * @param[in,out] p         State to initialise (must be non-NULL).
 * @param[in]     seed      Any value; 0 is remapped to a non-zero constant.
 * @param[in]     sleep_fn  Injected sleeper, or NULL for the host clock.
 * @param[in]     sleep_ctx Context forwarded to @p sleep_fn (may be NULL).
 *
 * @return Nothing.
 *
 * @pre `p`, when non-NULL, points to writable ::mdl_politeness_t storage.
 * @pre @p sleep_ctx outlives every ::mdl_politeness_wait call on `p`.
 * @post `p->state` is non-zero; `p->sleep_fn`/`p->sleep_ctx` equal the args.
 * @post The same `seed` yields the same delay sequence regardless of clock.
 *
 * @note Not thread-safe: initialises caller storage.
 * @since 0.1.0
 */
void mdl_politeness_init_clock(mdl_politeness_t* p,
                               uint64_t          seed,
                               mdl_sleep_fn      sleep_fn,
                               void*             sleep_ctx);

/**
 * @brief Sleep a jittered delay in [min_ms, max_ms] and return it.
 *
 * @details
 * Draws the next xorshift64 value, maps it into `[min_ms, max_ms]`, then blocks
 * for that long -- through the injected ::mdl_sleep_fn when one was wired, else
 * on the host `nanosleep`. The returned delay is exactly the one requested of
 * the sleeper, so a test with a recording fake can assert the timing without
 * any wall-clock time elapsing.
 *
 * @param[in,out] p      Jitter source (may be NULL).
 * @param[in]     min_ms Lower bound, milliseconds.
 * @param[in]     max_ms Upper bound, milliseconds (clamped up to min_ms).
 *
 * @return The delay actually requested, in milliseconds.
 * @retval 0     `p` is NULL (no sleep is performed).
 * @retval other A value in `[min_ms, max(min_ms, max_ms)]`.
 *
 * @pre `p`, when non-NULL, was seeded by an init function.
 * @pre A NULL `p` returns 0 without sleeping.
 * @post `p->state` has advanced by exactly one PRNG step (non-NULL `p`).
 * @post The sleeper (real or injected) was asked for the returned duration.
 *
 * @note Not thread-safe: advances `p->state`.
 * @since 0.1.0
 */
uint32_t mdl_politeness_wait(mdl_politeness_t* p, uint32_t min_ms, uint32_t max_ms);

/* ======================================================================== *
 *  Per-host politeness governor (#301)                                     *
 * ======================================================================== */

/**
 * @brief Injected monotonic clock: milliseconds since an arbitrary fixed epoch.
 *
 * @details
 * The governor reads time only through this seam. Production binds
 * `CLOCK_MONOTONIC` in ms; a unit test binds a fake it can advance by hand, so
 * token refill and backoff scheduling are deterministic. Only DIFFERENCES are
 * ever taken, and the clock must never run backward: a steppable wall clock
 * jumped forward makes the governor believe the request spacing has elapsed
 * and hammer the remote host (#509). The `Retry-After` HTTP-date form is
 * therefore compared against a wall-clock second count passed explicitly to
 * ::mdl_retry_after_parse, not derived from this seam.
 *
 * @param[in] ctx Opaque context supplied at ::mdl_governor_init_clock.
 * @return Current time in milliseconds.
 * @since 0.1.0
 */
typedef int64_t (*mdl_now_fn)(void* ctx);

/** @brief Fixed sizes and bounds for the governor's per-host table. */
typedef enum : uint16_t {
  k_mdl_gov_max_hosts  = 16U,    /**< Per-host record slots (origin + CDNs).      */
  k_mdl_gov_host_max   = 128U,   /**< Host-key buffer bytes (matches config).     */
  k_mdl_gov_level_max  = 16U,    /**< Backoff-exponent ceiling (overflow guard).  */
  k_mdl_gov_ms_per_req = 60000U, /**< Milliseconds per minute (rate -> interval). */
} mdl_gov_limits_t;

/**
 * @struct mdl_gov_cfg_t
 * @brief Per-site politeness tunables the governor is initialised with.
 *
 * @details
 * Every field is data, loaded from the site descriptor (::mdl_config_load) with
 * conservative defaults (::mdl_gov_cfg_default) so an unconfigured site is still
 * polite. The token bucket is described by `rate_per_min` (sustained ceiling)
 * and `burst` (how many requests may go out back-to-back before pacing kicks
 * in). Backoff grows from `backoff_base_ms`, doubling per consecutive throttle,
 * capped at `backoff_max_ms`, and decays one level per `decay_after` successes.
 *
 * @invariant `burst >= 1` and `max_inflight >= 1` after ::mdl_gov_cfg_default or
 *            ::mdl_governor_init (both clamp up from 0).
 * @invariant `rate_per_min == 0` disables rate limiting (backoff still applies).
 * @see mdl_governor_init
 * @see mdl_gov_cfg_default
 * @since 0.1.0
 */
typedef struct {
  uint32_t rate_per_min;    /**< Sustained per-host request ceiling (0 = off).      */
  uint32_t burst;           /**< Token-bucket capacity, in requests (>= 1).         */
  uint32_t backoff_base_ms; /**< First backoff window on a 429/503.                 */
  uint32_t backoff_max_ms;  /**< Backoff-window ceiling.                            */
  uint16_t decay_after;     /**< Consecutive successes that drop one backoff level. */
  uint16_t max_inflight;    /**< Per-host in-flight request cap (>= 1).             */
} mdl_gov_cfg_t;

/**
 * @struct mdl_host_rec_t
 * @brief One host's live governor state (a slot in the fixed per-host table).
 *
 * @details
 * Keyed by `host`; an empty `used` flag marks a free slot. `credit_ms` is the
 * token bucket measured in milliseconds of accrued rate credit (a request costs
 * one interval); `earliest_next_ms` is the backoff / `Retry-After` gate;
 * `backoff_level` is the consecutive-throttle exponent; `inflight` is the
 * concurrency counter. All timestamps are on the ::mdl_now_fn timeline.
 *
 * @invariant `credit_ms <= burst * interval` (capped on every refill).
 * @invariant `used == false` exactly when `host[0] == '\0'`.
 * @see mdl_governor_t
 * @since 0.1.0
 */
typedef struct {
  char     host[k_mdl_gov_host_max]; /**< Host key; "" when the slot is free.      */
  int64_t  credit_ms;                /**< Token-bucket credit, ms of rate.         */
  int64_t  last_ms;                  /**< Wall-ms of the previous scheduled start. */
  int64_t  earliest_next_ms;         /**< Backoff / Retry-After gate (mono-ms).    */
  uint16_t backoff_level;            /**< Consecutive-throttle exponent.           */
  uint16_t success_streak;           /**< Consecutive successes since last drop.   */
  uint16_t inflight;                 /**< Requests currently in flight.            */
  bool     used;                     /**< Slot occupied.                           */
} mdl_host_rec_t;

/**
 * @struct mdl_governor_t
 * @brief Closed-loop per-host politeness governor (rate + backoff + concurrency).
 *
 * @details
 * Holds the tunables, a fixed per-host table (zero dynamic allocation), the
 * seeded jitter PRNG, and the injected clock/sleep seams. Declare one per
 * download session and drive each request with
 * ::mdl_governor_acquire -> fetch -> ::mdl_governor_observe ->
 * ::mdl_governor_release. Large enough (a per-host table) to prefer file scope
 * over a deep stack.
 *
 * @invariant `rng` is never 0 after an init function.
 * @invariant Every occupied slot has a unique `host`.
 * @see mdl_governor_acquire
 * @see mdl_governor_observe
 * @since 0.1.0
 */
typedef struct {
  mdl_gov_cfg_t  cfg;                        /**< Politeness tunables.            */
  mdl_host_rec_t hosts[k_mdl_gov_max_hosts]; /**< Per-host records.               */
  uint64_t       rng;                        /**< Seeded xorshift64 jitter state. */
  mdl_now_fn     now_fn;                     /**< Injected monotonic clock (ms).  */
  void*          now_ctx;                    /**< Context for @ref now_fn.        */
  mdl_sleep_fn   sleep_fn;                   /**< Injected sleeper, NULL = host.  */
  void*          sleep_ctx;                  /**< Context for @ref sleep_fn.      */
} mdl_governor_t;

/**
 * @brief Conservative default tunables for a site that configures none.
 *
 * @details
 * A cautious ceiling that a careful human reader would not exceed: roughly one
 * request per second sustained with a small burst, one-second base backoff
 * doubling to a one-minute cap, and strictly serial per host. A site descriptor
 * may raise or lower any field.
 *
 * @return The default ::mdl_gov_cfg_t (60 req/min, burst 4, 1s..60s backoff,
 *         decay after 4 successes, 1 in-flight).
 * @retval (by value) Never fails; always returns the conservative defaults.
 *
 * @pre None.
 * @pre The caller accepts the documented conservative constants.
 * @post `burst >= 1` and `max_inflight >= 1` in the returned config.
 * @post `rate_per_min > 0`, so rate limiting is on by default.
 *
 * @note Thread-safe: returns a value computed from constants.
 * @since 0.1.0
 */
mdl_gov_cfg_t mdl_gov_cfg_default(void);

/**
 * @brief Initialise a governor on the real host clock and blocking sleep.
 *
 * @details
 * Equivalent to ::mdl_governor_init_clock with NULL clock seams: time comes from
 * the host monotonic clock and ::mdl_governor_acquire blocks on `nanosleep`. Clamps
 * `cfg->burst` and `cfg->max_inflight` up to 1 and seeds the jitter PRNG.
 *
 * @param[out] g    Governor to initialise (non-NULL).
 * @param[in]  cfg  Tunables to copy, or NULL for ::mdl_gov_cfg_default.
 * @param[in]  seed Jitter seed; 0 is remapped to a non-zero constant.
 *
 * @return Nothing.
 *
 * @pre `g`, when non-NULL, points to writable ::mdl_governor_t storage.
 * @pre A NULL `g` is a tolerated no-op.
 * @post The per-host table is empty and `g->rng` is non-zero.
 * @post `g->cfg.burst >= 1` and `g->cfg.max_inflight >= 1`.
 *
 * @note Not thread-safe: initialises caller storage.
 * @see mdl_governor_init_clock
 * @since 0.1.0
 */
void mdl_governor_init(mdl_governor_t* g, const mdl_gov_cfg_t* cfg, uint64_t seed);

/**
 * @brief Initialise a governor with injected clock and sleep seams (DI).
 *
 * @details
 * The dependency-injection entry point. Wires @p now_fn as the time source and
 * @p sleep_fn as the sleeper, so a test drives the whole governor against a
 * virtual clock with no real delay. NULL seams select the host clock/sleep,
 * making this a strict superset of ::mdl_governor_init.
 *
 * @param[out] g         Governor to initialise (non-NULL).
 * @param[in]  cfg       Tunables to copy, or NULL for ::mdl_gov_cfg_default.
 * @param[in]  seed      Jitter seed; 0 is remapped to a non-zero constant.
 * @param[in]  now_fn    Injected monotonic clock, or NULL for the host clock.
 * @param[in]  now_ctx   Context forwarded to @p now_fn (may be NULL).
 * @param[in]  sleep_fn  Injected sleeper, or NULL for the host clock.
 * @param[in]  sleep_ctx Context forwarded to @p sleep_fn (may be NULL).
 *
 * @return Nothing.
 *
 * @pre `g`, when non-NULL, points to writable ::mdl_governor_t storage.
 * @pre @p now_ctx / @p sleep_ctx outlive every governor call on `g`.
 * @post The per-host table is empty and `g->rng` is non-zero.
 * @post `g->now_fn`/`g->sleep_fn` equal the arguments (NULL = host default).
 *
 * @note Not thread-safe: initialises caller storage.
 * @since 0.1.0
 */
void mdl_governor_init_clock(mdl_governor_t*      g,
                             const mdl_gov_cfg_t* cfg,
                             uint64_t             seed,
                             mdl_now_fn           now_fn,
                             void*                now_ctx,
                             mdl_sleep_fn         sleep_fn,
                             void*                sleep_ctx);

/**
 * @brief Reserve an in-flight slot for a request to `host`, pacing as required.
 *
 * @details
 * The gate every request passes before hitting the network. In order it (1)
 * refuses -- without sleeping -- when `host` already has `max_inflight` requests
 * in flight, so a future parallel loop is bounded per host; (2) computes the
 * token-bucket wait so the sustained rate stays under `rate_per_min` with up to
 * `burst` back-to-back; (3) raises that to the host's backoff / `Retry-After`
 * gate if it is later; (4) adds a jittered spacing delay drawn from
 * `[jitter_min_ms, jitter_max_ms]`; then sleeps the total through the injected
 * clock and marks one request in flight. On success the caller MUST later call
 * ::mdl_governor_release. A NULL governor is a no-op that succeeds (politeness
 * disabled), matching a NULL jitter source.
 *
 * @param[in,out] g             Governor, or NULL to disable pacing.
 * @param[in]     host          Host key (e.g. from ::mdl_url_host); may be NULL.
 * @param[in]     jitter_min_ms Baseline spacing floor (e.g. the site img delay).
 * @param[in]     jitter_max_ms Baseline spacing ceiling (clamped up to the floor).
 *
 * @return Whether the slot was reserved.
 * @retval k_ra8_ok             Reserved; the request may proceed (release later).
 * @retval k_ra8_err_would_block `host` is at its in-flight cap; try again later.
 *
 * @pre `g`, when non-NULL, was initialised by a governor init function.
 * @pre The caller pairs a ::k_ra8_ok with exactly one ::mdl_governor_release.
 * @post On ::k_ra8_ok the host's in-flight count is one higher.
 * @post On ::k_ra8_err_would_block no time was slept and no state changed.
 *
 * @note Not thread-safe: mutates the per-host table and advances the clock.
 * @see mdl_governor_release
 * @see mdl_governor_observe
 * @since 0.1.0
 */
ra8_err_t mdl_governor_acquire(mdl_governor_t* g,
                               const char*     host,
                               uint32_t        jitter_min_ms,
                               uint32_t        jitter_max_ms);

/**
 * @brief Release the in-flight slot reserved by a matching ::mdl_governor_acquire.
 *
 * @details Decrements the matching host record's in-flight count when it is
 * non-zero. NULL arguments, unknown hosts, and unmatched releases are tolerated
 * no-ops so cleanup paths can remain unconditional.
 *
 * @param[in,out] g    Governor, or NULL (no-op).
 * @param[in]     host Host key passed to the paired acquire; may be NULL.
 *
 * @return Nothing.
 *
 * @pre Called exactly once per ::k_ra8_ok from ::mdl_governor_acquire.
 * @pre `g`, when non-NULL, was initialised by a governor init function.
 * @post The host's in-flight count is one lower (never below zero).
 * @post No time is slept.
 *
 * @note Not thread-safe: mutates the per-host table.
 * @see mdl_governor_acquire
 * @since 0.1.0
 */
void mdl_governor_release(mdl_governor_t* g, const char* host);

/**
 * @brief Feed a completed request's outcome back into the host's governor state.
 *
 * @details
 * Closes the loop. On a throttle (@p status 429 or 503) it raises
 * `backoff_level` by one (capped), computes an exponential window
 * `min(base << (level-1), ceil)`, draws a full-jitter delay in `[0, window]`,
 * and sets the host's earliest-next gate to `now + max(jittered_backoff,
 * Retry-After)` -- so `Retry-After` wins whenever it is longer, in both
 * delta-seconds and HTTP-date forms. On any non-throttle outcome it counts a
 * success and, after `decay_after` of them, drops one backoff level, easing back
 * toward the base rate; a non-throttle response that still carries `Retry-After`
 * is honoured as a floor. Call once per request, after the fetch, before the
 * next acquire to the same host observes the new gate.
 *
 * @param[in,out] g           Governor, or NULL (no-op).
 * @param[in]     host        Host key of the completed request; may be NULL.
 * @param[in]     status      HTTP status observed (0 if none / transport error).
 * @param[in]     retry_after Raw `Retry-After` header, or NULL/"" if absent.
 *
 * @return Nothing.
 *
 * @pre `g`, when non-NULL, was initialised by a governor init function.
 * @pre `status` and `retry_after` come from the just-finished request.
 * @post A 429/503 raises `backoff_level` and pushes `earliest_next_ms` forward.
 * @post A non-throttle outcome advances the success streak and may decay a level.
 *
 * @note Not thread-safe: mutates the per-host table and reads the clock.
 * @see mdl_governor_acquire
 * @see mdl_retry_after_parse
 * @since 0.1.0
 */
void mdl_governor_observe(mdl_governor_t* g,
                          const char*     host,
                          long            status,
                          const char*     retry_after);

/**
 * @brief Observe a response using an explicit wall-clock timestamp.
 *
 * @details This is the deterministic-clock form of ::mdl_governor_observe.
 * The governor still schedules gates against its monotonic clock, but parses
 * an HTTP-date `Retry-After` relative to @p now_wall_s. Production callers
 * should use ::mdl_governor_observe; tests and platforms with an injected wall
 * clock may use this entry point.
 *
 * @param[in,out] g          Governor, or NULL (no-op).
 * @param[in]     host       Host key of the completed request; may be NULL.
 * @param[in]     status     HTTP status observed.
 * @param[in]     retry_after Raw `Retry-After` header, or NULL/empty.
 * @param[in]     now_wall_s Current Unix wall-clock time in seconds.
 *
 * @return Nothing.
 *
 * @pre `g`, when non-NULL, was initialised by a governor init function.
 * @pre `status`, `retry_after`, and `now_wall_s` describe the same completed request.
 * @post A 429/503 raises the host backoff and advances its earliest-next gate.
 * @post Other outcomes advance the success streak and may decay one backoff level.
 *
 * @note Not thread-safe: mutates the per-host table and reads the monotonic clock.
 * @since 0.1.0
 */
void mdl_governor_observe_at_wall(mdl_governor_t* g,
                                  const char*     host,
                                  long            status,
                                  const char*     retry_after,
                                  int64_t         now_wall_s);

/**
 * @brief Read a host's current backoff level and earliest-next gate.
 *
 * @details
 * Read-only introspection into the per-host table, used by the unit tests to
 * assert backoff growth/decay and gate scheduling deterministically, and usable
 * by a caller that wants to log a host's throttle state. Does not create a
 * record: an unknown host reports false and leaves the outputs untouched.
 *
 * @param[in]  g                Governor, or NULL.
 * @param[in]  host             Host key to look up; may be NULL.
 * @param[out] backoff_level    Receives the consecutive-throttle level. May be NULL.
 * @param[out] earliest_next_ms Receives the earliest-next gate (mono-ms). May be NULL.
 *
 * @return Whether a record exists for `host`.
 * @retval true  A record exists; the requested outputs were written.
 * @retval false No record for `host` (or NULL argument); outputs untouched.
 *
 * @pre `g`, when non-NULL, was initialised by a governor init function.
 * @pre The caller treats a false return as "host not yet seen".
 * @post No governor state is modified.
 * @post Output pointers are written only when the function returns true.
 *
 * @note Not thread-safe: reads the per-host table.
 * @since 0.1.0
 */
bool mdl_governor_peek(const mdl_governor_t* g,
                       const char*           host,
                       uint16_t*             backoff_level,
                       int64_t*              earliest_next_ms);

/**
 * @brief Parse an HTTP `Retry-After` header value into a delay in milliseconds.
 *
 * @details
 * Accepts both forms RFC 7231 defines: a non-negative delta-seconds integer
 * ("120"), returned as `120000`; and an HTTP-date, whose delay is the date minus
 * @p now_wall_s (clamped at zero, so a past date yields 0). Both the preferred
 * IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") and the obsolete RFC 850 form
 * are recognised. A pure function with no clock of its own, so a test drives
 * both forms deterministically by supplying @p now_wall_s.
 *
 * @param[in]  value      Raw header value (leading spaces tolerated), or NULL.
 * @param[in]  now_wall_s Current wall-clock time in seconds (for the date form).
 * @param[out] out_ms     Receives the delay in milliseconds (non-NULL).
 *
 * @return Whether @p value parsed as a valid `Retry-After`.
 * @retval true  Parsed; `*out_ms` holds the delay (saturated at UINT32_MAX ms).
 * @retval false NULL/empty/unparseable input; `*out_ms` is left unchanged.
 *
 * @pre `out_ms` is non-NULL for a result to be written.
 * @pre `now_wall_s` is the seconds the date form is measured against.
 * @post On false the output is not modified.
 * @post On true `*out_ms` is a non-negative millisecond delay.
 *
 * @note Thread-safe: depends only on its arguments (uses `timegm`, not `mktime`).
 * @since 0.1.0
 */
bool mdl_retry_after_parse(const char* value, int64_t now_wall_s, uint32_t* out_ms);
