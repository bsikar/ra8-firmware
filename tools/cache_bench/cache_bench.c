/**
 * @file cache_bench.c
 * @brief #147 eviction-policy benchmark: the exact-accounting replay engine.
 *
 * @details
 * Replays one access trace through one registered ::cache_policy_t at one
 * cache capacity. The resident-set lookup is an exact key->frame chained hash
 * (one node per frame, no tombstones) so hit accounting is precise; only
 * eviction ordering is delegated to the policy under test, and every byte of
 * replay state is carved from a caller-owned workspace.
 *
 * This unit is the replay ENGINE and carries no command line: the swept-
 * capacity markdown report, the `--sweep-block` mode dispatch (see
 * sweep_block.h) and `main` live in cache_bench_report.c, which reaches the
 * engine only through the public cache_bench.h surface.
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

#include "cache_bench_io.h"
#include "ra8_attributes.h"
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
