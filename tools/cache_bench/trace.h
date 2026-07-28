/**
 * @file trace.h
 * @brief Reader access-trace generation + loading for the #147 cache benchmark.
 *
 * @details
 * Produces the workload corpus #147 calls for -- sequential page-turn flooding,
 * random TOC/bookmark jumps, back-and-forth re-reading (locality), image-tile
 * scroll, a hot-set-plus-scan scan-resistance case, a GB-class "huge book" whose
 * ~7 GiB footprint dwarfs every swept cache, and a mixed realistic session -- and
 * loads real traces captured from the reader (ra8_emulator or the EK-RA8D2 over
 * UART) as `<object> <page>` lines.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "cache_bench.h"

/** @brief A named access trace (heap-owned key array). */
typedef struct {
  const char* name;      /**< Workload name for the report.              */
  cb_key_t*   keys;      /**< Access stream.                             */
  uint64_t    n;         /**< Number of accesses.                        */
  uint32_t    footprint; /**< Distinct pages touched (working-set size). */
} cb_trace_t;

/**
 * @brief Build the built-in synthetic workload corpus.
 * @param[out] out_count Receives the number of traces returned.
 * @return Heap array of @p out_count traces (free with cb_traces_free).
 */
cb_trace_t* cb_traces_synthetic(uint32_t* out_count);

/**
 * @brief Load a captured `<object> <page>` trace file.
 * @param[in]  path Trace file path.
 * @param[in]  name Display name for the trace.
 * @return One heap-owned trace, or a trace with n==0 on error.
 */
cb_trace_t cb_trace_load(const char* path, const char* name);

/** @brief Free a trace corpus from cb_traces_synthetic. */
void cb_traces_free(cb_trace_t* traces, uint32_t count);

/** @brief Free a single loaded trace's keys. */
void cb_trace_free(cb_trace_t* t);
