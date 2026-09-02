/**
 * @file inc/trace.h
 * @brief Resettable, allocation-free access streams for cache_bench.
 * @details Defines immutable trace descriptors and caller-owned cursors for
 *          deterministic synthetic or injected captured workloads.
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
#include "cache_bench_io.h"

/** @brief Number of built-in deterministic workloads. */
typedef enum : uint16_t {
  k_cb_synthetic_trace_count = 8U,   /**< Fixed corpus size.                 */
  k_cb_trace_name_capacity   = 64U,  /**< Captured trace display-name bound. */
  k_cb_trace_line_capacity   = 128U, /**< Historical line-parser bound.      */
  k_cb_trace_read_capacity   = 256U, /**< Bounded source read-ahead.         */
} cb_trace_limit_t;

/** @brief Workload implementation selected by a trace descriptor. */
typedef enum : uint8_t {
  k_cb_trace_seq = 0U, /**< Sequential page walk.              */
  k_cb_trace_random,   /**< Uniform pseudo-random accesses.    */
  k_cb_trace_reread,   /**< Hot-set reread workload.           */
  k_cb_trace_jumps,    /**< Mostly sequential with jumps.      */
  k_cb_trace_scroll,   /**< Repeated scrolling window.         */
  k_cb_trace_scan,     /**< Hot-set plus streaming scan.       */
  k_cb_trace_huge,     /**< Oversized hot-set and scan.        */
  k_cb_trace_mixed,    /**< Alternating access phases.         */
  k_cb_trace_captured, /**< Caller-injected captured accesses. */
} cb_trace_kind_t;

/** @brief Named resettable trace; captured bytes remain owned by the caller. */
typedef struct cb_trace {
  const char*     name;        /**< Stable display name.                    */
  uint64_t        n;           /**< Valid access count.                     */
  uint32_t        footprint;   /**< Reported working-set pages.             */
  cb_trace_kind_t kind;        /**< Generator/parser selection.             */
  cb_source_t     source;      /**< Captured source binding, when selected. */
  uint64_t        fingerprint; /**< Bound captured-content fingerprint.     */
  /** @brief Captured-name copy. */
  char name_storage[k_cb_trace_name_capacity];
} cb_trace_t;

/** @brief Caller-owned state for one independent trace pass. */
typedef struct {
  const cb_trace_t* trace;                                 /**< Bound immutable descriptor. */
  uint64_t          index;                                 /**< Accesses emitted.           */
  uint64_t          rng;                                   /**< Generator state.            */
  uint64_t          fingerprint;                           /**< Captured pass fingerprint.  */
  uint64_t          source_offset;                         /**< Next source byte offset.    */
  uint32_t          page;                                  /**< Stateful generator page.    */
  uint32_t          hot;                                   /**< Stateful hot-window base.   */
  size_t            read_at;                               /**< Read-ahead cursor.          */
  size_t            read_count;                            /**< Valid read-ahead bytes.     */
  uint8_t           read_buffer[k_cb_trace_read_capacity]; /**< Bounded read-ahead.         */
  char              line[k_cb_trace_line_capacity];        /**< Bounded line assembly.      */
} cb_trace_cursor_t;

/**
 * @brief Populate the fixed built-in corpus without acquiring storage.
 * @details Initializes all names, counts, footprints, and generator kinds in
 *          stable report order.
 * @param[out] out Array of ::k_cb_synthetic_trace_count descriptors.
 * @pre @p out is non-NULL and has the declared array capacity.
 * @pre Static corpus geometry fits the published descriptor fields.
 * @post Every output descriptor is initialized and resettable.
 * @post No external storage or ownership is acquired.
 * @note Descriptor names point to read-only static strings.
 * @since 0.1.0
 */
void cb_traces_synthetic(cb_trace_t out[k_cb_synthetic_trace_count]);

/**
 * @brief Validate and bind one captured decimal `<object> <page>` source.
 * @details Copies the bounded display name, validates a complete pass through
 *          the injected source, and snapshots its count plus fingerprint.
 * @param[in] source Borrowed immutable byte source.
 * @param[in] name Display name bytes (need not be NUL-terminated).
 * @param[in] name_length Display-name byte count.
 * @param[out] out Bound trace on success; zeroed on failure.
 * @return I/O, capacity, or success status.
 * @retval k_cb_io_ok @p out contains a validated captured trace.
 * @retval k_cb_io_capacity The display name exceeds fixed storage.
 * @retval k_cb_io_fault A binding, parser, or source contract failed.
 * @retval k_cb_io_mutated The source changed during validation.
 * @pre @p source has a non-NULL read callback and stable size snapshot.
 * @pre @p name and @p out are non-NULL.
 * @post On success, @p out borrows @p source and owns its copied name only.
 * @post On failure, @p out is all-zero.
 * @note Caller must keep the source context alive through every replay.
 * @since 0.1.0
 */
cb_io_status_t
cb_trace_bind(const cb_source_t* source, const char* name, size_t name_length, cb_trace_t* out);

/**
 * @brief Reset a cursor for an independent pass over a trace.
 * @details Clears cursor state, binds @p trace, and selects the deterministic
 *          seed required by its workload kind.
 * @param[in] trace Immutable trace descriptor.
 * @param[out] cursor Cursor to initialize.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The cursor is ready for its first key.
 * @retval k_cb_io_fault An argument or trace kind is invalid.
 * @pre @p trace and @p cursor are non-NULL.
 * @pre @p trace was initialized by a published trace binder.
 * @post On success, cursor index and fingerprint are at their initial values.
 * @post @p trace is not modified and remains borrowed.
 * @note Distinct cursors may replay the same immutable trace independently.
 * @since 0.1.0
 */
cb_io_status_t cb_trace_cursor_open(const cb_trace_t* trace, cb_trace_cursor_t* cursor);

/**
 * @brief Emit the next key.
 * @details Dispatches to the captured parser or selected deterministic
 *          generator, then advances count and fingerprint for emitted keys.
 * @param[in,out] cursor Open cursor.
 * @param[out] key Next key when `done` is false.
 * @param[out] done True at a clean end.
 * @return Source/parser status.
 * @retval k_cb_io_ok A key or clean end was produced.
 * @retval k_cb_io_fault A cursor, generator, parser, or source contract failed.
 * @retval k_cb_io_mutated The injected source reported mutation.
 * @pre All pointers are non-NULL and @p cursor is open.
 * @pre @p key and @p done point to writable storage.
 * @post When @p done is false, cursor count advances and @p key is initialized.
 * @post Once the declared count is reached, @p done is true.
 * @note Call ::cb_trace_cursor_finish after termination to validate stability.
 * @since 0.1.0
 */
cb_io_status_t cb_trace_cursor_next(cb_trace_cursor_t* cursor, cb_key_t* key, bool* done);

/**
 * @brief Validate a captured pass against its bound count and fingerprint.
 * @details Compares emitted count and accumulated fingerprint with the values
 *          snapshotted by ::cb_trace_bind; synthetic passes require count only.
 * @param[in] cursor Finished trace cursor.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok Count and, for captured traces, fingerprint match.
 * @retval k_cb_io_fault A binding or count check failed.
 * @retval k_cb_io_mutated Captured content changed between passes.
 * @pre @p cursor and `cursor->trace` are non-NULL.
 * @pre Iteration reached clean completion before this call.
 * @post Cursor and trace state are not modified.
 * @post A captured mismatch is distinguishable from an ordinary fault.
 * @note This closes logical validation only; it owns no host descriptor.
 * @since 0.1.0
 */
cb_io_status_t cb_trace_cursor_finish(const cb_trace_cursor_t* cursor);
