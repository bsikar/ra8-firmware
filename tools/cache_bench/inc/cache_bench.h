/**
 * @file inc/cache_bench.h
 * @brief Eviction-policy comparison harness for the #147 memory-hierarchy
 *        decision record: the DIP seam every replacement policy implements, the
 *        fixed-frame cache it drives, and the per-run metrics it reports.
 *
 * @details
 * This is a HOST tool (not firmware): it replays reader access traces through
 * each candidate Layer-2 page-cache eviction policy at swept cache sizes and
 * reports hit rate, per-eviction worst-case frames scanned (a WCET proxy), and
 * per-frame metadata cost, so #147 can pick the knee of the hit-rate-vs-RAM
 * curve that also clears the WCET / MC/DC bars. Policies plug in behind
 * ::cache_policy_t exactly as the eventual firmware Layer 2 will (NASA Rule 9
 * allows the function-pointer vtable for this DIP seam).
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct cb_trace cb_trace_t;

/** @brief A cache key: one (object, page) the reader's vm_get touches. */
typedef struct {
  uint32_t object_id; /**< Opaque object handle (book / archive / font).      */
  uint32_t page;      /**< Page index within the object (offset / page-size). */
} cb_key_t;

/** @brief One frame slot in the fixed page cache. */
typedef struct {
  cb_key_t key;      /**< Resident key, valid only when @ref live is true. */
  bool     live;     /**< true: slot holds a resident page.                */
  uint8_t  meta[16]; /**< Per-policy scratch (ref bits, RRPV, list links). */
} cb_frame_t;

/**
 * @struct cb_cache_t
 * @brief The fixed-capacity frame cache a policy manages.
 * @details The harness owns the frame array and the resident-set lookup; a
 *          policy only decides ordering: which frame to evict, and how to react
 *          to hits / inserts. Frame indices are stable for the run.
 */
typedef struct {
  cb_frame_t* frames;                 /**< @ref capacity frame slots.                    */
  uint32_t    capacity;               /**< Number of frame slots (the RAM budget knob).  */
  void*       policy_data;            /**< Policy-private state (rings, stacks, sketch). */
  void*       policy_workspace;       /**< Caller-provided policy-state storage.         */
  size_t      policy_workspace_bytes; /**< Bytes available at the storage.               */
} cb_cache_t;

/** @brief Caller-owned replay workspace and exact capacity diagnostics. */
typedef struct {
  uint8_t* data;       /**< Aligned writable storage.                   */
  size_t   capacity;   /**< Supplied bytes.                             */
  size_t   required;   /**< Exact bytes required by the latest request. */
  size_t   high_water; /**< Largest successfully provisioned request.   */
} cb_workspace_t;

/**
 * @struct cache_policy_t
 * @brief The replacement-policy DIP seam (the eventual firmware Layer-2 seam).
 *
 * @details Every callback is bounded and side-effects only @ref cb_frame_t
 *          ::meta and @ref cb_cache_t ::policy_data. `pick_victim` must return a
 *          live frame index; the harness evicts it, inserts the new key there,
 *          then calls `on_insert`.
 */
typedef struct {
  const char* name;              /**< Policy name for the report table.            */
  size_t      meta_bytes;        /**< Per-frame metadata actually used (RAM cost). */
  size_t      state_base_bytes;  /**< Fixed policy workspace bytes.                */
  size_t      state_frame_bytes; /**< Additional bytes per frame.                  */
  /** @brief Allocate + init policy state for a @ref cb_cache_t. Returns 0 ok. */
  int (*init)(cb_cache_t* c);
  /** @brief Release policy state. */
  void (*deinit)(cb_cache_t* c);
  /** @brief A resident key was just hit at @p frame (update recency/freq). */
  void (*on_access)(cb_cache_t* c, uint32_t frame);
  /** @brief @p frame was just (re)populated with a freshly-loaded key. */
  void (*on_insert)(cb_cache_t* c, uint32_t frame);
  /**
   * @brief Choose a live frame to evict.
   * @param[out] scanned Receives frames examined this call (WCET proxy).
   * @return Index of the victim frame (always < capacity).
   */
  uint32_t (*pick_victim)(cb_cache_t* c, uint32_t* scanned);
} cache_policy_t;

/** @brief Per-(policy, trace, size) result row. */
typedef struct {
  uint64_t accesses;   /**< Total accesses replayed.            */
  uint64_t hits;       /**< Resident-set hits.                  */
  uint64_t evictions;  /**< pick_victim calls.                  */
  uint32_t worst_scan; /**< Max frames scanned in any eviction. */
  uint64_t total_scan; /**< Sum of frames scanned (avg proxy).  */
} cb_result_t;

/**
 * @brief Replay an access trace through one policy at a fixed capacity.
 *
 * @details Drives @p trace through an exact resident-set hash while delegating
 *          only eviction ordering to @p pol: hits update recency/frequency via
 *          the policy callbacks, misses evict the policy's victim and load the
 *          new key into that frame. Fills @p out with hit/miss and worst-case
 *          scan accounting for one (policy, trace, capacity) sweep point.
 *
 * @param[in]  pol      Policy to exercise.
 * @param[in]  trace    Resettable access stream.
 * @param[in]  capacity Frame count (the swept RAM budget).
 * @param[in,out] workspace Caller-provided exact replay storage.
 * @param[out] out      Receives the metrics for this run.
 *
 * @return int 0 on success, non-zero on capacity, source, or argument failure.
 * @retval 0 The replay completed and @p out holds the metrics.
 * @retval 1 A NULL/zero argument, policy bind, source, or workspace check failed.
 *
 * @pre @p pol has `pick_victim` bound and @p trace is resettable.
 * @pre @p out is non-NULL and writable.
 * @post On success, `out->accesses == trace->n` and hits do not exceed accesses.
 * @post No storage ownership changes; exact demand is recorded in @p workspace.
 *
 * @note Independent calls are safe when their traces and workspaces are distinct.
 * @since 0.1.0
 */
int cb_replay(const cache_policy_t* pol,
              const cb_trace_t*     trace,
              uint32_t              capacity,
              cb_workspace_t*       workspace,
              cb_result_t*          out);

/**
 * @brief Return the exact caller workspace required by one replay.
 * @details Sums aligned frame, resident-index, link, and policy-state regions
 *          with overflow guards, without touching caller storage.
 * @param[in] pol Policy whose private state is included.
 * @param[in] capacity Frame count.
 * @return Exact bytes, or zero for invalid/overflowing input.
 * @retval 0 A policy/capacity check or size calculation failed.
 * @retval other Exact aligned workspace bytes required by ::cb_replay.
 * @pre @p pol is NULL or points to readable policy geometry.
 * @pre @p capacity is an intended fixed cache-frame count.
 * @post A non-zero result covers every replay workspace region.
 * @post No policy or caller storage is modified.
 * @note Thread-safe: this function reads immutable policy geometry only.
 * @since 0.1.0
 */
size_t cb_replay_workspace_required(const cache_policy_t* pol, uint32_t capacity);

/**
 * @var g_cb_policy_slru
 * @brief Segmented-LRU: the scan-resistant candidate (src/policy_scanresist.c).
 * @details A probationary + protected LRU pair; one-time scans churn only
 *          the probationary segment, so the re-referenced hot set survives.
 * @note Read-only after load; registered in ::g_cb_policies.
 * @since 0.1.0
 */
extern const cache_policy_t g_cb_policy_slru;

/**
 * @var g_cb_policy_srrip
 * @brief SRRIP: the 2-bit re-reference-interval candidate (src/policy_scanresist.c).
 * @details Inserts predict a distant re-reference so scanned-once pages are
 *          evicted before re-referenced ones; a hit predicts immediate reuse.
 * @note Read-only after load; registered in ::g_cb_policies.
 * @since 0.1.0
 */
extern const cache_policy_t g_cb_policy_srrip;

/** @brief The registered policy table (defined in src/policies.c). */
extern const cache_policy_t* const g_cb_policies[];
/** @brief Number of entries in ::g_cb_policies. */
extern const uint32_t g_cb_policy_count;
