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
 */
#pragma once

#include <stdint.h>

/** @brief Deterministic jitter source (seeded xorshift64 state). */
typedef struct {
  uint64_t state; /**< PRNG state; never 0 after init. */
} mdl_politeness_t;

/**
 * @brief Seed the jitter source.
 * @param[in,out] p    State to initialise (must be non-NULL).
 * @param[in]     seed Any value; 0 is remapped to a non-zero constant.
 */
void mdl_politeness_init(mdl_politeness_t* p, uint64_t seed);

/**
 * @brief Sleep a jittered delay in [min_ms, max_ms] and return it.
 * @param[in,out] p      Jitter source.
 * @param[in]     min_ms Lower bound, milliseconds.
 * @param[in]     max_ms Upper bound, milliseconds (clamped up to min_ms).
 * @return The delay actually slept, in milliseconds (0 if `p` is NULL).
 */
uint32_t mdl_politeness_wait(mdl_politeness_t* p, uint32_t min_ms,
                             uint32_t max_ms);
