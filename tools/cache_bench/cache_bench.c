/**
 * @file cache_bench.c
 * @brief #147 eviction-policy benchmark harness: replay + sweep + report.
 *
 * @details
 * Replays each access trace through each registered ::cache_policy_t at a swept
 * set of cache capacities and prints a markdown report (hit-rate matrix +
 * WCET/metadata summary + a recommendation) suitable for pasting into the #147
 * decision record. The resident-set lookup is an exact key->frame chained hash
 * (one node per frame, no tombstones) so hit accounting is precise; only
 * ordering is delegated to the policy under test.
 *
 * `--sweep-block` selects the orthogonal #208 mode instead: sweep the block /
 * frame / chunk SIZE in bytes through the real ::ra8_vmem stack (see
 * sweep_block.h) rather than the capacity in frames.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include "cache_bench.h"

#include <string.h>

#include "cache_bench_host.h"
#include "cache_bench_io.h"
#include "ra8_attributes.h"
#include "sweep_block.h"
#include "trace.h"

/**
 * @brief Round @p v up to a power of two (>= 1).
 * @details Starts at 1 and left-shifts until the running value reaches or
 *          exceeds @p v, yielding the smallest power of two not less than
 *          @p v. Used to size the resident-set hash table at four buckets per
 *          frame so bucket masking is a single AND.
 * @param[in] v Target to round up; both 0 and 1 map to 1.
 * @return uint32_t The smallest power of two that is >= @p v (never 0).
 * @retval 1     @p v was 0 or 1.
 * @retval other The next power of two at or above @p v.
 * @pre @p v <= 2^31 so the next power of two is representable in 32 bits.
 * @pre Called on the single benchmark thread.
 * @post The result is an exact power of two.
 * @post The result is >= @p v and >= 1; no shared state is touched.
 * @note Thread-safe: a pure function of @p v with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_pow2_ceil(uint32_t v)
{
  uint32_t p = 1U;
  while (p < v) {
    p <<= 1U;
  }
  return p;
}

/**
 * @enum cb_hash_mul_t
 * @brief Murmur3 finalizer constants used in ::internal_hash.
 * @details These are the canonical Murmur3 64-bit finalization mix constants
 *          (Austin Appleby, 2011). They are algorithm-specified bit patterns
 *          chosen for avalanche quality and must not be renamed to hide their
 *          origin; the MAGIC-OK markers document the rationale.
 * @see https://github.com/aappleby/smhasher
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_hash_mix_mul = 0xFF51AFD7ED558CCDULL, /**< MAGIC-OK: Murmur3 64-bit finalization multiplier */
} cb_hash_mul_t;

/**
 * @enum cb_hash_shift_t
 * @brief Murmur3 finalizer bit-shift amounts used in ::internal_hash.
 * @details The value 33 is mandated by the Murmur3 64-bit finalization
 *          algorithm. It must equal exactly 33 for the required avalanche
 *          properties; the MAGIC-OK marker documents this constraint.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_hash_shift = 33U, /**< MAGIC-OK: Murmur3 64-bit finalizer shift amount */
} cb_hash_shift_t;

/**
 * @brief Mix an (object,page) key into a 32-bit hash for bucket selection.
 * @details Packs @p k as `(object_id << 32) | page` and runs the canonical
 *          Murmur3 64-bit finalizer (::k_hash_mix_mul, ::k_hash_shift) for
 *          avalanche, returning the low 32 bits. The caller masks the result
 *          with the (power-of-two) bucket count.
 * @param[in] k The (object_id, page) key to hash (taken by value).
 * @return uint32_t The low 32 bits of the finalized hash.
 * @retval 0     Possible for some keys (0 is a valid bucket seed).
 * @retval other The mixed hash for @p k.
 * @pre ::k_hash_shift equals 33 (the Murmur3 finalizer shift).
 * @pre Called on the single benchmark thread.
 * @post The same @p k always maps to the same value within a run.
 * @post No global or heap state is modified.
 * @note Thread-safe: a pure function of @p k.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_hash(cb_key_t k)
{
  uint64_t h = ((uint64_t)k.object_id << 32U) | (uint64_t)k.page;
  h ^= h >> (uint8_t)k_hash_shift;
  h *= (uint64_t)k_hash_mix_mul;
  h ^= h >> (uint8_t)k_hash_shift;
  return (uint32_t)h;
}

/**
 * @brief Report whether two cache keys name the same (object, page).
 * @details Compares both fields; equal hashes are necessary but not sufficient,
 *          so the chained-hash lookup calls this to confirm a bucket match is a
 *          true key match (no tombstones, so hit accounting stays exact).
 * @param[in] a First key (by value).
 * @param[in] b Second key (by value).
 * @return bool true when both fields are equal, false otherwise.
 * @retval true  @p a and @p b have equal `object_id` and `page`.
 * @retval false The keys differ in at least one field.
 * @pre @p a and @p b are fully initialized keys.
 * @pre Called on the single benchmark thread.
 * @post Neither argument is modified.
 * @post The comparison is symmetric: eq(a,b) == eq(b,a).
 * @note Thread-safe: a pure comparison of its arguments.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_key_eq(cb_key_t a, cb_key_t b)
{
  return (a.object_id == b.object_id) && (a.page == b.page);
}

/**
 * @struct cb_index_t
 * @brief Exact key->frame lookup over the resident set (chained hash).
 * @details One chain node per frame, no tombstones, so hit accounting is
 *          precise; only eviction ordering is delegated to the policy.
 * @invariant `mask + 1` is the (power-of-two) bucket count.
 * @since 0.1.0
 */
typedef struct {
  int32_t* bucket; /**< Head frame index per bucket, or -1. */
  int32_t* next;   /**< Per-frame chain link, or -1.        */
  uint32_t mask;   /**< Bucket-count-minus-one bit mask.    */
} cb_index_t;

/**
 * @brief Find the resident frame currently holding @p key, or -1.
 * @details Walks the bucket chain at `hash(key) & mask`, confirming each
 *          candidate with ::internal_key_eq, and returns the matching frame index.
 *          A -1 result is a cache miss; the harness then loads @p key.
 * @param[in] idx    Resident-set index (buckets + per-frame chain links).
 * @param[in] frames Frame array the chain indexes into.
 * @param[in] key    The (object, page) key to look up.
 * @return int32_t The resident frame index, or -1 when @p key is absent.
 * @retval -1    @p key is not resident (a miss).
 * @retval other The index of the frame holding @p key (a hit).
 * @pre @p idx was populated by ::internal_replay_open and the insert path.
 * @pre @p frames has at least `capacity` initialized entries.
 * @post Neither @p idx nor @p frames is modified (pure read).
 * @post A non-negative result indexes a live frame whose key equals @p key.
 * @note Not thread-safe: concurrent inserts would race the chain walk.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_index_find(const cb_index_t* idx, const cb_frame_t* frames, cb_key_t key)
{
  for (int32_t j = idx->bucket[internal_hash(key) & idx->mask]; j != -1; j = idx->next[j]) {
    if (internal_key_eq(frames[j].key, key)) {
      return j;
    }
  }
  return -1;
}

/**
 * @brief Unlink @p frame's current key from its bucket chain before eviction.
 * @details Recomputes the bucket from the frame's resident key and splices the
 *          frame out of that singly-linked chain (updating the bucket head or
 *          the predecessor's link). Must run before the frame is repopulated so
 *          the stale key stops being findable.
 * @param[in,out] idx    Resident-set index whose chain is edited.
 * @param[in]     frames Frame array (read for the victim's current key).
 * @param[in]     frame  Index of the frame being evicted.
 * @pre @p frame is currently linked under `hash(frames[frame].key)`.
 * @pre @p frame is a valid index < capacity.
 * @post @p frame no longer appears in any bucket chain.
 * @post Only the affected bucket chain is altered; @p frames is unchanged.
 * @note Not thread-safe: mutates the shared chain. Call on the benchmark thread.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_index_remove(cb_index_t* idx, const cb_frame_t* frames, uint32_t frame)
{
  const uint32_t ob   = internal_hash(frames[frame].key) & idx->mask;
  int32_t        prev = -1;
  for (int32_t j = idx->bucket[ob]; j != -1; prev = j, j = idx->next[j]) {
    if (j == (int32_t)frame) {
      if (prev == -1) {
        idx->bucket[ob] = idx->next[j];
      } else {
        idx->next[prev] = idx->next[j];
      }
      break;
    }
  }
}

/**
 * @brief Link @p frame into the bucket chain for @p key after a fresh insert.
 * @details Prepends the frame to the chain at `hash(key) & mask` (an O(1) head
 *          insert), making @p key findable by ::internal_index_find. Pairs with
 *          ::internal_index_remove on the eviction path.
 * @param[in,out] idx   Resident-set index whose chain is extended.
 * @param[in]     frame Index of the frame now holding @p key.
 * @param[in]     key   The key just written into that frame.
 * @pre @p frame is not currently linked in any chain.
 * @pre @p frame is a valid index < capacity.
 * @post @p key is findable and resolves to @p frame.
 * @post Only the target bucket chain grows by one node.
 * @note Not thread-safe: mutates the shared chain. Call on the benchmark thread.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_index_push(cb_index_t* idx, uint32_t frame, cb_key_t key)
{
  const uint32_t b = internal_hash(key) & idx->mask;
  idx->next[frame] = idx->bucket[b];
  idx->bucket[b]   = (int32_t)frame;
}

/**
 * @brief Pick the frame that receives @p key: a free frame while the cache
 *        fills, else the policy's victim (accounted + unlinked).
 * @details Charges eviction stats (count, total/worst scan) to @p out and
 *          unlinks an evicted frame's old key from the index, so the caller
 *          only relinks the new key.
 * @param[in]     pol    Policy under test (its `pick_victim` may run).
 * @param[in,out] cache  The frame cache (frames + policy state).
 * @param[in,out] idx    Resident-set index (victim unlinked in place).
 * @param[in,out] filled Frames used so far; grows while cold.
 * @param[in,out] out    Metrics row receiving eviction accounting.
 * @return uint32_t Frame index to (re)populate (always < capacity).
 * @retval <capacity Always: a still-free frame while cold, else the victim.
 * @pre The cache is missing the current key (lookup already failed).
 * @pre `pol->pick_victim` is bound (every registered policy binds it).
 * @post On the eviction path, the victim's old key is no longer findable.
 * @post `*filled <= cache->capacity`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_replay_take_frame(const cache_policy_t* pol,
                                           cb_cache_t*           cache,
                                           cb_index_t*           idx,
                                           uint32_t*             filled,
                                           cb_result_t*          out)
{
  if (*filled < cache->capacity) {
    const uint32_t frame = *filled;
    (*filled)++;
    return frame;
  }
  uint32_t       scanned = 0U;
  const uint32_t frame   = pol->pick_victim(cache, &scanned);
  out->evictions++;
  out->total_scan += scanned;
  if (scanned > out->worst_scan) {
    out->worst_scan = scanned;
  }
  internal_index_remove(idx, cache->frames, frame);
  return frame;
}

/**
 * @brief Round a byte count to the next maximum fundamental alignment.
 * @details Adds the alignment-minus-one bias with an overflow guard, then
 *          clears the low bits required by ::max_align_t.
 * @param[in] value Unaligned byte count.
 * @return The aligned byte count, or zero when rounding would overflow.
 * @retval 0 @p value cannot be represented after alignment.
 * @retval other Smallest aligned byte count not less than @p value.
 * @pre `alignof(max_align_t)` is a non-zero power of two.
 * @pre @p value is an ordinary object-size request.
 * @post A non-zero result is a multiple of `alignof(max_align_t)`.
 * @post No storage is read or modified.
 * @note Thread-safe: this is a pure arithmetic helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t internal_align_size(size_t value)
{
  const size_t alignment = alignof(max_align_t);
  if (value > (SIZE_MAX - (alignment - 1U))) {
    return 0U;
  }
  return (value + alignment - 1U) & ~(alignment - 1U);
}

/**
 * @brief Multiply byte factors without overflowing `size_t`.
 * @details Checks the division bound before evaluating the product so caller
 *          sizing cannot wrap to a smaller workspace request.
 * @param[in] left First factor.
 * @param[in] right Second factor.
 * @param[out] result Receives the product on success.
 * @return Whether the product was written.
 * @retval true @p result contains `left * right`.
 * @retval false @p result is NULL or the product would overflow.
 * @pre @p left and @p right describe byte-count factors.
 * @pre @p result is NULL or points to writable `size_t` storage.
 * @post On success, @p result contains the exact mathematical product.
 * @post On failure, @p result is not modified.
 * @note Thread-safe for distinct @p result bindings.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_size_multiply(size_t left, size_t right, size_t* result)
{
  if ((result == nullptr) || ((left != 0U) && (right > (SIZE_MAX / left)))) {
    return false;
  }
  *result = left * right;
  return true;
}

typedef enum : uint32_t {
  k_cb_max_hash_capacity = UINT32_C(1) << 29U, /**< Largest safe fourfold hash input. */
} cb_hash_limit_t;

size_t cb_replay_workspace_required(const cache_policy_t* pol, uint32_t capacity)
{
  if ((pol == nullptr) || (capacity == 0U) || (capacity > (uint32_t)k_cb_max_hash_capacity)) {
    return 0U;
  }
  const uint32_t hsize = internal_pow2_ceil(capacity * 4U);
  size_t         frame_bytes;
  size_t         hash_bytes;
  size_t         link_bytes;
  size_t         policy_frame_bytes;
  if (!internal_size_multiply(capacity, sizeof(cb_frame_t), &frame_bytes) ||
      !internal_size_multiply(hsize, sizeof(int32_t), &hash_bytes) ||
      !internal_size_multiply(capacity, sizeof(int32_t), &link_bytes) ||
      !internal_size_multiply(capacity, pol->state_frame_bytes, &policy_frame_bytes) ||
      (pol->state_base_bytes > (SIZE_MAX - policy_frame_bytes))) {
    return 0U;
  }
  const size_t parts[] = {
    frame_bytes,
    hash_bytes,
    link_bytes,
    pol->state_base_bytes + policy_frame_bytes,
  };
  size_t total = 0U;
  for (size_t i = 0U; i < (sizeof(parts) / sizeof(parts[0])); ++i) {
    const size_t aligned = internal_align_size(parts[i]);
    if ((aligned == 0U) || (total > (SIZE_MAX - aligned))) {
      return 0U;
    }
    total += aligned;
  }
  return total;
}

RA8_INTERNAL
static void* internal_workspace_take(cb_workspace_t* workspace, size_t* used, size_t bytes)
{
  const size_t span = internal_align_size(bytes);
  if ((span == 0U) || (*used > workspace->capacity) || (span > (workspace->capacity - *used))) {
    return nullptr;
  }
  void* result = &workspace->data[*used];
  *used += span;
  return result;
}

/**
 * @brief Carve and initialize one replay's exact caller-owned storage.
 * @details Validates the aligned workspace capacity, partitions it into frame,
 *          hash, link, and policy regions, then initializes every borrowed
 *          binding without acquiring ownership.
 * @param[out] idx Receives the resident-index binding.
 * @param[out] frames Receives the frame-array binding.
 * @param[out] policy_data Receives the policy-state binding.
 * @param[in] pol Policy whose storage geometry is applied.
 * @param[in] capacity Number of cache frames.
 * @param[in,out] workspace Caller-owned aligned workspace and diagnostics.
 * @return Whether every required region was bound and initialized.
 * @retval true All output bindings are valid for one replay.
 * @retval false An argument, alignment, overflow, or capacity check failed.
 * @pre @p idx, @p frames, @p policy_data, and @p workspace are writable.
 * @pre @p pol and @p capacity satisfy ::cb_replay_workspace_required.
 * @post `workspace->required` records the exact request on every path.
 * @post On success, frames and policy bytes are zeroed and index buckets are empty.
 * @note Thread-safe for distinct caller-owned workspaces.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_replay_open(cb_index_t*           idx,
                                 cb_frame_t**          frames,
                                 void**                policy_data,
                                 const cache_policy_t* pol,
                                 uint32_t              capacity,
                                 cb_workspace_t*       workspace)
{
  const size_t required = cb_replay_workspace_required(pol, capacity);
  workspace->required   = required;
  if ((required == 0U) || (workspace->data == nullptr) ||
      (((uintptr_t)workspace->data % alignof(max_align_t)) != 0U) ||
      (workspace->capacity < required)) {
    return false;
  }
  size_t         used  = 0U;
  const uint32_t hsize = internal_pow2_ceil(capacity * 4U);
  *frames =
    (cb_frame_t*)internal_workspace_take(workspace, &used, (size_t)capacity * sizeof(cb_frame_t));
  idx->bucket =
    (int32_t*)internal_workspace_take(workspace, &used, (size_t)hsize * sizeof(int32_t));
  idx->next =
    (int32_t*)internal_workspace_take(workspace, &used, (size_t)capacity * sizeof(int32_t));
  *policy_data =
    internal_workspace_take(workspace,
                            &used,
                            pol->state_base_bytes + ((size_t)capacity * pol->state_frame_bytes));
  idx->mask = hsize - 1U;
  if ((*frames == nullptr) || (idx->bucket == nullptr) || (idx->next == nullptr) ||
      (*policy_data == nullptr)) {
    return false;
  }
  memset(*frames, 0, (size_t)capacity * sizeof(cb_frame_t));
  memset(*policy_data, 0, pol->state_base_bytes + ((size_t)capacity * pol->state_frame_bytes));
  for (uint32_t i = 0U; i < hsize; ++i) {
    idx->bucket[i] = -1;
  }
  if (used > workspace->high_water) {
    workspace->high_water = used;
  }
  return true;
}

/**
 * @brief End the borrowed workspace bindings made by ::internal_replay_open.
 * @details Zeroes the borrowed index binding. The workspace itself remains
 *          caller-owned and immediately reusable on every replay exit path.
 * @param[in,out] idx    Index whose borrowed pointers are zeroed.
 * @param[in]     frames Borrowed frame array (NULL tolerated).
 * @pre @p idx is non-NULL (its buffers may individually be NULL).
 * @pre Any non-NULL pointers came from ::internal_replay_open.
 * @post @p idx is all-zero; caller-owned bytes remain available for reuse.
 * @post @p frames is not accessed or modified.
 * @note Thread-safe for distinct caller-owned bindings.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_replay_close(cb_index_t* idx, cb_frame_t* frames)
{
  (void)frames;
  *idx = (cb_index_t){};
}

/**
 * @brief Drive one resettable trace through an already-bound cache and index.
 * @details Opens an independent cursor, accounts hits directly, and delegates
 *          miss-victim ordering to @p pol while maintaining the exact index.
 * @param[in] pol Bound replacement policy.
 * @param[in] trace Immutable resettable trace.
 * @param[in,out] cache Initialized cache binding.
 * @param[in,out] index Initialized resident index.
 * @param[out] out Receives replay counters.
 * @return Zero on a complete stable replay, otherwise one.
 * @retval 0 Every key was replayed and the captured source remained stable.
 * @retval 1 Cursor I/O, parser, or stability validation failed.
 * @pre All pointers are non-NULL and @p cache is initialized for @p pol.
 * @pre @p index indexes the same frame array held by @p cache.
 * @post On success, `out->accesses` equals the emitted trace count.
 * @post No ownership changes occur; cache and index remain caller-bound.
 * @note Not thread-safe when bindings are shared.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_replay_stream(const cache_policy_t* pol,
                                  const cb_trace_t*     trace,
                                  cb_cache_t*           cache,
                                  cb_index_t*           index,
                                  cb_result_t*          out)
{
  cb_trace_cursor_t cursor = {};
  if (cb_trace_cursor_open(trace, &cursor) != k_cb_io_ok) {
    return 1;
  }
  uint32_t filled = 0U;
  bool     done   = false;
  while (!done) {
    cb_key_t key = {};
    if (cb_trace_cursor_next(&cursor, &key, &done) != k_cb_io_ok) {
      return 1;
    }
    if (done) {
      break;
    }
    const int32_t found = internal_index_find(index, cache->frames, key);
    out->accesses++;
    if (found != -1) {
      out->hits++;
      if (pol->on_access != nullptr) {
        pol->on_access(cache, (uint32_t)found);
      }
      continue;
    }
    const uint32_t frame      = internal_replay_take_frame(pol, cache, index, &filled, out);
    cache->frames[frame].key  = key;
    cache->frames[frame].live = true;
    internal_index_push(index, frame, key);
    if (pol->on_insert != nullptr) {
      pol->on_insert(cache, frame);
    }
  }
  return (cb_trace_cursor_finish(&cursor) == k_cb_io_ok) ? 0 : 1;
}

int cb_replay(const cache_policy_t* pol,
              const cb_trace_t*     trace,
              uint32_t              capacity,
              cb_workspace_t*       workspace,
              cb_result_t*          out)
{
  if (out != nullptr) {
    *out = (cb_result_t){};
  }
  if ((pol == nullptr) || (trace == nullptr) || (capacity == 0U) || (workspace == nullptr) ||
      (out == nullptr)) {
    return 1;
  }

  cb_frame_t* frames      = nullptr;
  cb_index_t  idx         = {};
  void*       policy_data = nullptr;
  const bool  ready = internal_replay_open(&idx, &frames, &policy_data, pol, capacity, workspace);
  cb_cache_t  cache = {.frames           = frames,
                       .capacity         = capacity,
                       .policy_data      = nullptr,
                       .policy_workspace = policy_data,
                       .policy_workspace_bytes =
                         pol->state_base_bytes + ((size_t)capacity * pol->state_frame_bytes)};
  if (!ready || ((pol->init != nullptr) && (pol->init(&cache) != 0))) {
    internal_replay_close(&idx, frames);
    return 1;
  }

  const int result = internal_replay_stream(pol, trace, &cache, &idx, out);
  if (pol->deinit != nullptr) {
    pol->deinit(&cache);
  }
  internal_replay_close(&idx, frames);
  return result;
}

#ifndef CB_CACHE_BENCH_LIBRARY_ONLY

/**
 * @enum cb_bench_size_t
 * @brief Swept cache capacities (in frames) used on the RAM-budget axis.
 * @details These are the seven capacity points that the benchmark sweeps over.
 *          Each is a power of two chosen to cover the expected SRAM/SDRAM
 *          budget range for the RA8D2 page cache (#147 decision record).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cb_size_64   = 64U,   /**< Smallest evaluated capacity (frames).  */
  k_cb_size_128  = 128U,  /**< 128-frame sweep point.                 */
  k_cb_size_256  = 256U,  /**< Mid-budget representative sweep point. */
  k_cb_size_512  = 512U,  /**< 512-frame sweep point.                 */
  k_cb_size_1024 = 1024U, /**< 1 K-frame sweep point.                 */
  k_cb_size_2048 = 2048U, /**< Largest evaluated capacity (frames).   */
} cb_bench_size_t;

/** @brief Representative mid-budget capacity used in the summary table. */
typedef enum : uint32_t {
  k_cb_mid_cap = 256U, /**< Mid-point capacity (frames) for the summary view. */
} cb_mid_cap_t;

/** @brief Full scale used when computing a hit-rate percentage (integer form). */
typedef enum : uint32_t {
  k_cb_pct_scale = 100U, /**< Divisor to convert a ratio to a percentage. */
} cb_pct_scale_t;

/** @brief Floating-point 100.0 scale factor for hit-rate percentage output. */
static const double s_cb_pct_scale_f = 100.0; /**< double 100.0 for pct maths. */

/** @brief Swept cache capacities (frames) -- the RAM-budget axis. */
static const uint32_t s_cb_sizes[] = {
  32U,
  (uint32_t)k_cb_size_64,
  (uint32_t)k_cb_size_128,
  (uint32_t)k_cb_size_256,
  (uint32_t)k_cb_size_512,
  (uint32_t)k_cb_size_1024,
  (uint32_t)k_cb_size_2048,
};

/**
 * @brief Print the per-trace hit-rate matrix (policies x cache sizes).
 * @details Emits a markdown section for @p tr: a header naming the workload,
 *          then one row per registered policy giving its hit-rate percentage at
 *          each swept capacity in ::s_cb_sizes. Each cell is produced by a full
 *          ::cb_replay of the trace at that size (0.0 when no accesses ran).
 * @param[in] tr Trace to report (name, key stream, footprint).
 * @param[in,out] workspace Reusable exact replay workspace.
 * @param[in,out] sink Report destination.
 * @pre @p tr is non-NULL with valid reset and next operations.
 * @pre ::g_cb_policies / ::g_cb_policy_count are initialized.
 * @post One markdown table for @p tr is written to @p sink.
 * @post @p tr and every policy are left unmodified (replays are self-contained).
 * @return Zero after complete publication, otherwise one.
 * @retval 0 Every table fragment was accepted by @p sink.
 * @retval 1 A replay or sink operation failed.
 * @note Not thread-safe: writes @p sink and runs replays. Benchmark thread only.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_report_trace(const cb_trace_t* tr, cb_workspace_t* workspace, cb_sink_t* sink)
{
  const uint32_t nsz = (uint32_t)(sizeof(s_cb_sizes) / sizeof(s_cb_sizes[0]));
  if (cb_sink_format(sink,
                     "\n### %s  (%llu accesses, footprint %u pages)\n\n",
                     tr->name,
                     (unsigned long long)tr->n,
                     tr->footprint) != k_cb_io_ok ||
      cb_sink_format(sink, "| policy |") != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t s = 0U; s < nsz; ++s) {
    if (cb_sink_format(sink, " %u |", s_cb_sizes[s]) != k_cb_io_ok) {
      return 1;
    }
  }
  if (cb_sink_format(sink, "\n|--------|") != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t s = 0U; s < nsz; ++s) {
    if (cb_sink_format(sink, "------|") != k_cb_io_ok) {
      return 1;
    }
  }
  if (cb_sink_format(sink, "\n") != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t p = 0U; p < g_cb_policy_count; ++p) {
    if (cb_sink_format(sink, "| %-14s |", g_cb_policies[p]->name) != k_cb_io_ok) {
      return 1;
    }
    for (uint32_t s = 0U; s < nsz; ++s) {
      cb_result_t r = {};
      if (cb_replay(g_cb_policies[p], tr, s_cb_sizes[s], workspace, &r) != 0) {
        return 1;
      }
      const double hit =
        (r.accesses == 0U) ? 0.0 : (s_cb_pct_scale_f * (double)r.hits / (double)r.accesses);
      if (cb_sink_format(sink, " %5.1f |", hit) != k_cb_io_ok) {
        return 1;
      }
    }
    if (cb_sink_format(sink, "\n") != k_cb_io_ok) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Print the cross-workload summary (WCET + metadata + mean hit rate).
 * @details For each policy, replays every trace at the fixed mid-budget
 *          capacity ::k_cb_mid_cap, then prints the mean hit rate across
 *          workloads, the worst per-eviction scan depth seen (a WCET proxy),
 *          and the policy's per-frame metadata cost.
 * @param[in] traces Array of @p ntr workloads to average over.
 * @param[in] ntr    Number of traces in @p traces (> 0).
 * @param[in,out] workspace Reusable exact replay workspace.
 * @param[in,out] sink Report destination.
 * @pre @p traces is non-NULL with @p ntr valid entries.
 * @pre ::g_cb_policies / ::g_cb_policy_count are initialized.
 * @post One markdown summary table is written to @p sink.
 * @post No trace or policy state is mutated by the reporting.
 * @return Zero after complete publication, otherwise one.
 * @retval 0 Every summary row was replayed and accepted by @p sink.
 * @retval 1 A replay or sink operation failed.
 * @note Not thread-safe: writes @p sink and runs replays. Benchmark thread only.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_report_summary(const cb_trace_t* traces,
                                   uint32_t          ntr,
                                   cb_workspace_t*   workspace,
                                   cb_sink_t*        sink)
{
  const uint32_t mid_cap = (uint32_t)k_cb_mid_cap;
  if (cb_sink_format(sink, "\n## Summary at %u frames (mean over all workloads)\n\n", mid_cap) !=
        k_cb_io_ok ||
      cb_sink_format(sink, "| policy | mean hit %% | worst scan/evict | meta bytes/frame |\n") !=
        k_cb_io_ok ||
      cb_sink_format(sink, "|--------|-----------:|-----------------:|-----------------:|\n") !=
        k_cb_io_ok) {
    return 1;
  }
  for (uint32_t p = 0U; p < g_cb_policy_count; ++p) {
    double   sum_hit = 0.0;
    uint32_t worst   = 0U;
    for (uint32_t t = 0U; t < ntr; ++t) {
      cb_result_t r = {};
      if (cb_replay(g_cb_policies[p], &traces[t], mid_cap, workspace, &r) != 0) {
        return 1;
      }
      sum_hit +=
        (r.accesses == 0U) ? 0.0 : (s_cb_pct_scale_f * (double)r.hits / (double)r.accesses);
      if (r.worst_scan > worst) {
        worst = r.worst_scan;
      }
    }
    if (cb_sink_format(sink,
                       "| %-14s | %10.2f | %16u | %16zu |\n",
                       g_cb_policies[p]->name,
                       sum_hit / (double)ntr,
                       worst,
                       g_cb_policies[p]->meta_bytes) != k_cb_io_ok) {
      return 1;
    }
  }
  return 0;
}

/** @brief Capacity of the extra captured-trace table filled from argv. */
typedef enum : uint8_t {
  k_cb_max_loaded          = 8U, /**< Most `<name>=<path>` traces accepted per run. */
  k_cb_output_prefix_bytes = 9U, /**< Bytes in the literal `--output=` prefix.      */
} cb_loaded_cap_t;

typedef enum : size_t {
  k_cb_composition_workspace_bytes = 131072U, /**< Maximum exact metadata budget. */
} cb_composition_limit_t;

alignas(max_align_t) static uint8_t s_cb_composition_workspace[k_cb_composition_workspace_bytes];
typedef struct {
  uint64_t before;                              /**< Detect setup underflow. */
  alignas(max_align_t) uint8_t bytes[1048576U]; /**< Semantic cache budget.  */
  uint64_t after;                               /**< Detect setup overflow.  */
} cb_sweep_backing_t;

static cb_sweep_backing_t s_cb_sweep_backing;

typedef enum : uint64_t {
  k_cb_sweep_canary_before = 0x0CACEB00C0FFEE11ULL, /**< Leading guard value.  */
  k_cb_sweep_canary_after  = 0xA11CE55E0BADC0DEULL, /**< Trailing guard value. */
} cb_sweep_canary_t;

/**
 * @brief Load the extra captured traces named on the command line.
 * @details Each argv of the form `<name>=<path>` (e.g. `hw-reader=t.trace`)
 *          is split in place and loaded via ::cb_trace_load; arguments
 *          without `=` are ignored (they are mode flags). Traces that fail
 *          to load (n == 0) are dropped silently, exactly as before.
 * @param[in]  argc   Argument count from main.
 * @param[in]  argv   Argument vector from main (mutated at the `=` split).
 * @param[out] loaded Receives up to ::k_cb_max_loaded loaded traces.
 * @param[out] sources Receives the corresponding open host bindings.
 * @return uint32_t Number of traces actually loaded (0 .. ::k_cb_max_loaded).
 * @retval 0     No argv held a loadable `<name>=<path>` pair.
 * @retval other The count of successfully loaded traces.
 * @pre @p loaded has capacity ::k_cb_max_loaded.
 * @pre @p argv is writable (the `=` is overwritten with a NUL).
 * @post Entries `loaded[0..return)` all have `n > 0`.
 * @post Every processed `<name>=<path>` argv has its `=` replaced by a NUL.
 * @note Not thread-safe (mutates argv in place).
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
internal_load_argv_traces(int argc, char** argv, cb_trace_t* loaded, cb_host_source_t* sources)
{
  uint32_t nloaded = 0U;
  for (int a = 1; (a < argc) && (nloaded < (uint32_t)k_cb_max_loaded); ++a) {
    const char* eq = strchr(argv[a], '=');
    if ((eq == nullptr) || (strncmp(argv[a], "--output=", (size_t)k_cb_output_prefix_bytes) == 0)) {
      continue;
    }
    cb_source_t source = {};
    if ((cb_host_source_open(eq + 1, &sources[nloaded], &source) == k_cb_io_ok) &&
        (cb_trace_bind(&source, argv[a], (size_t)(eq - argv[a]), &loaded[nloaded]) == k_cb_io_ok)) {
      nloaded++;
    } else if (sources[nloaded].fd >= 0) {
      (void)cb_host_source_close(&sources[nloaded]);
    }
  }
  return nloaded;
}

/**
 * @brief Find the optional report destination in the argument vector.
 * @details Scans arguments after argv[0] and returns the bytes following the
 *          first `--output=` prefix without copying or taking ownership.
 * @param[in] argc Number of entries in @p argv.
 * @param[in] argv Process argument vector.
 * @return Borrowed destination path, or NULL when the option is absent.
 * @retval NULL No `--output=` argument was present.
 * @retval other Pointer into the matching argument.
 * @pre @p argv names at least @p argc readable pointers.
 * @pre Each inspected argument is NUL-terminated.
 * @post @p argv and its strings are not modified.
 * @post Any non-NULL result remains valid while the argument vector lives.
 * @note The empty path from a bare `--output=` is returned for open validation.
 * @since 0.1.0
 */
RA8_INTERNAL
static const char* internal_output_path(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--output=", (size_t)k_cb_output_prefix_bytes) == 0) {
      return &argv[i][k_cb_output_prefix_bytes];
    }
  }
  return nullptr;
}

/**
 * @brief Compose scratch, cache, and workspace bindings and execute block mode.
 * @details Opens one host scratch transaction, guards the fixed cache backing
 *          with canaries, runs ::cb_sweep_block, then closes the scratch seam.
 * @param[in,out] output Sweep report destination.
 * @param[in,out] error Diagnostic destination.
 * @return Zero on a complete sweep, otherwise one.
 * @retval 0 The sweep, canary checks, and scratch close all succeeded.
 * @retval 1 Composition, sweep, canary, or close validation failed.
 * @pre @p output and @p error are bound writable sinks.
 * @pre The benchmark runs on its single composition thread.
 * @post The scratch descriptor is closed on every successful open path.
 * @post The fixed cache backing remains owned by this translation unit.
 * @note The two sink bindings may refer to distinct borrowed descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_sweep(cb_sink_t* output, cb_sink_t* error)
{
  cb_host_scratch_t scratch_binding = {.fd = -1};
  cb_scratch_t      scratch         = {};
  if (cb_host_scratch_open(&scratch_binding, &scratch) != k_cb_io_ok) {
    return 1;
  }
  s_cb_sweep_backing.before = (uint64_t)k_cb_sweep_canary_before;
  s_cb_sweep_backing.after  = (uint64_t)k_cb_sweep_canary_after;
  cb_sweep_config_t config  = {.cache_backing      = s_cb_sweep_backing.bytes,
                               .cache_capacity     = sizeof(s_cb_sweep_backing.bytes),
                               .workspace          = s_cb_composition_workspace,
                               .workspace_capacity = sizeof(s_cb_composition_workspace),
                               .scratch            = &scratch,
                               .output             = output,
                               .error              = error};
  int               result  = cb_sweep_block(&config);
  if ((s_cb_sweep_backing.before != (uint64_t)k_cb_sweep_canary_before) ||
      (s_cb_sweep_backing.after != (uint64_t)k_cb_sweep_canary_after) ||
      (cb_host_scratch_close(&scratch_binding) != k_cb_io_ok)) {
    result = 1;
  }
  return result;
}

/**
 * @brief Close every captured source, preserving any close failure.
 * @details Attempts all @p count closes even after one fails so no later
 *          borrowed host binding is skipped during teardown.
 * @param[in,out] sources Array of open host source bindings.
 * @param[in] count Number of entries to close.
 * @return Zero when every close succeeds, otherwise one.
 * @retval 0 All descriptors closed successfully.
 * @retval 1 At least one close operation failed.
 * @pre @p sources contains @p count initialized bindings.
 * @pre Each binding is closed at most once by this call.
 * @post Every entry has been passed to ::cb_host_source_close.
 * @post A failure does not prevent later entries from being attempted.
 * @note Not thread-safe with concurrent users of the same descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_close_sources(cb_host_source_t* sources, uint32_t count)
{
  int result = 0;
  for (uint32_t index = 0U; index < count; ++index) {
    if (cb_host_source_close(&sources[index]) != k_cb_io_ok) {
      result = 1;
    }
  }
  return result;
}

/**
 * @brief Execute the capacity report over synthetic and captured traces.
 * @details Binds the fixed synthetic corpus, opens optional captured sources,
 *          publishes per-trace and summary tables, and closes every source.
 * @param[in] argc Number of entries in @p argv.
 * @param[in,out] argv Writable process argument vector.
 * @param[in,out] output Report destination.
 * @param[in,out] error Diagnostic destination used for close failures.
 * @return Zero on complete publication and teardown, otherwise one.
 * @retval 0 All replays, sink writes, and source closes succeeded.
 * @retval 1 A trace, workspace, sink, or close operation failed.
 * @pre @p argv names @p argc writable, NUL-terminated argument strings.
 * @pre @p output and @p error are bound writable sinks.
 * @post Every successfully opened captured source is closed.
 * @post The fixed composition workspace remains owned by this translation unit.
 * @note This function mutates accepted `<name>=<path>` argument separators.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_capacity(int argc, char** argv, cb_sink_t* output, cb_sink_t* error)
{
  cb_trace_t traces[k_cb_synthetic_trace_count] = {};
  cb_traces_synthetic(traces);
  cb_trace_t       loaded[k_cb_max_loaded]  = {};
  cb_host_source_t sources[k_cb_max_loaded] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_cb_max_loaded; ++i) {
    sources[i].fd = -1;
  }
  const uint32_t nloaded   = internal_load_argv_traces(argc, argv, loaded, sources);
  cb_workspace_t workspace = {.data     = s_cb_composition_workspace,
                              .capacity = sizeof(s_cb_composition_workspace)};
  int            result =
    (cb_sink_format(output, "# #147 eviction-policy benchmark\n") == k_cb_io_ok &&
     cb_sink_format(output, "\nHit rate (%%) by cache size (frames). Higher is better.\n") ==
       k_cb_io_ok)
      ? 0
      : 1;
  for (uint32_t t = 0U; (t < (uint32_t)k_cb_synthetic_trace_count) && (result == 0); ++t) {
    result = internal_report_trace(&traces[t], &workspace, output);
  }
  for (uint32_t t = 0U; (t < nloaded) && (result == 0); ++t) {
    result = internal_report_trace(&loaded[t], &workspace, output);
  }
  if (result == 0) {
    result =
      internal_report_summary(traces, (uint32_t)k_cb_synthetic_trace_count, &workspace, output);
  }
  result |= internal_close_sources(sources, nloaded);
  if (result != 0) {
    (void)cb_sink_format(error,
                         "cache_bench: run failed (workspace required=%zu supplied=%zu)\n",
                         workspace.required,
                         workspace.capacity);
  }
  return result;
}

int main(int argc, char** argv)
{
  cb_sink_t error = {};
  cb_host_standard_sinks(nullptr, &error);
  cb_host_output_t output_binding = {};
  cb_sink_t        output         = {};
  if (cb_host_output_open(internal_output_path(argc, argv), &output_binding, &output) !=
      k_cb_io_ok) {
    (void)cb_sink_format(&error, "cache_bench: output open failed\n");
    return 1;
  }
  const int result = ((argc > 1) && (strcmp(argv[1], "--sweep-block") == 0))
                       ? internal_run_sweep(&output, &error)
                       : internal_run_capacity(argc, argv, &output, &error);
  if ((result == 0) && (cb_host_output_commit(&output_binding) == k_cb_io_ok)) {
    return 0;
  }
  cb_host_output_abort(&output_binding);
  return 1;
}
#endif
