/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_politeness.h
 * @brief Minimal jittered inter-request delay (v0 politeness).
 *
 * @details
 * v0 keeps only the piece worth keeping from the Kotlin original: a jittered
 * delay between requests so we do not hammer a host. The PRNG is seeded and
 * deterministic, so a run is reproducible in tests. The full governor (global
 * per-host token bucket, adaptive backoff on 429/503, Retry-After) is a later
 * milestone; this is intentionally the smallest useful version.
 *
 * The blocking sleep is reached through an injectable seam (::mdl_sleep_fn) so
 * the timing behaviour is unit-testable without real sleeps: production wires
 * the host clock (`nanosleep`), tests wire a fake that records the requested
 * delay and returns immediately. The jitter maths is identical either way.
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

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
RA8_DI_SLOT("politeness_sleep")
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
