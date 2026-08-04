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
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 *
 * @details Opens @p path and reads decimal `<object> <page>` lines into a
 *          geometrically-grown key array, stopping at the first malformed line
 *          or on an allocation failure. Used to replay real reader traces
 *          captured from ra8_emulator or the EK-RA8D2 over UART.
 *
 * @param[in]  path Trace file path.
 * @param[in]  name Display name for the trace (static lifetime).
 *
 * @return cb_trace_t One heap-owned trace, or a trace with n == 0 on error.
 * @retval n==0 The file could not be opened/allocated, or held no valid lines.
 * @retval n>0  The parsed access stream (caller frees via ::cb_trace_free).
 *
 * @pre @p path and @p name are non-NULL.
 * @pre Called on the single benchmark thread.
 * @post On success `keys` holds `n` parsed accesses and is caller-owned.
 * @post The file is closed on every return path.
 *
 * @note Not thread-safe (uses `errno` and the C stdio file API).
 * @since 0.1.0
 */
cb_trace_t cb_trace_load(const char* path, const char* name);

/**
 * @brief Free a trace corpus from ::cb_traces_synthetic.
 *
 * @details Frees each trace's key array and then the array of traces itself, so
 *          one call releases everything ::cb_traces_synthetic returned. A NULL
 *          @p traces is a no-op.
 *
 * @param[in,out] traces Corpus array to release (NULL tolerated).
 * @param[in]     count  Number of traces in @p traces.
 *
 * @pre @p traces is NULL, or an array of @p count traces from
 *      ::cb_traces_synthetic.
 * @pre Called on the single benchmark thread.
 * @post Every key array and the corpus array are freed.
 * @post @p traces must not be dereferenced after the call.
 *
 * @note Not thread-safe: frees shared allocations.
 * @since 0.1.0
 */
void cb_traces_free(cb_trace_t* traces, uint32_t count);

/**
 * @brief Free a single loaded trace's keys.
 *
 * @details Releases the trace's key array and resets `keys`/`n`, leaving the
 *          struct safe to discard or reuse. A NULL @p t is a no-op.
 *
 * @param[in,out] t Trace whose key array is freed (NULL tolerated).
 *
 * @pre @p t is NULL, or a trace whose `keys` came from a loader/generator.
 * @pre Called on the single benchmark thread.
 * @post `t->keys == NULL` and `t->n == 0` when @p t is non-NULL.
 * @post The key memory is released.
 *
 * @note Not thread-safe: frees the trace's allocation.
 * @since 0.1.0
 */
void cb_trace_free(cb_trace_t* t);
