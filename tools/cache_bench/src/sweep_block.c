/**
 * @file src/sweep_block.c
 * @brief Core of the #208 block/frame-size sweep (`--sweep-block`).
 *
 * @details
 * Drives the REAL ::ra8_vmem SLRU page cache (not a re-modelled policy) with
 * `frame_bytes` swept from 512 B to 256 KiB under a constant byte budget,
 * over the backends published by ::priv_backends() (the ::cbs_backend_t
 * seam; implementations live in src/sweep_block_backends.c). Leg (a) scans the
 * whole object sequentially; leg (b) re-reads one block. Every byte handed
 * back by the cache is verified against the source blob. Row printing and
 * the summary report live in src/sweep_block_report.c.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include "sweep_block.h"

#include <string.h>

#include "miniz.h"
#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "sweep_block_internal.h"

/**
 * @enum cbs_block_size_t
 * @brief The swept block / frame / chunk sizes, in bytes.
 * @details Powers of two from one SD sector up to a quarter MiB, bracketing
 *          the 64 KiB `.rabook` chunk default (#204) by two octaves on each
 *          side so the knee is visible whichever way it falls.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cbs_block_512b   = 512U,    /**< One SD sector.                       */
  k_cbs_block_1kib   = 1024U,   /**< 1 KiB.                               */
  k_cbs_block_4kib   = 4096U,   /**< Classic VM page size.                */
  k_cbs_block_16kib  = 16384U,  /**< 16 KiB.                              */
  k_cbs_block_64kib  = 65536U,  /**< The current `.rabook` chunk default. */
  k_cbs_block_256kib = 262144U, /**< 256 KiB (frame RAM gets expensive).  */
} cbs_block_size_t;

/** @brief Log backend stub so ra8_check's RA8_CHECK_* macros link host-side. */
void ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  (void)message;
}

/** @brief Valued log backend stub (present for the linker, as reader_vmem). */
void ra8_log_emit_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)tag;
  (void)message;
  (void)value;
}

/**
 * @brief Round @p v up to a power of two (>= 1).
 *
 * @details Starts at 1 and left-shifts until the running value reaches or
 *          exceeds @p v, yielding the smallest power of two not less than
 *          @p v. Used to size the ::ra8_vmem hash-bucket table for a swept
 *          frame count.
 *
 * @param[in] v Target to round up; both 0 and 1 map to 1.
 *
 * @return uint32_t The smallest power of two that is >= @p v (never 0).
 * @retval 1     @p v was 0 or 1.
 * @retval other The next power of two at or above @p v.
 *
 * @pre @p v <= 2^31 so the next power of two fits in 32 bits.
 * @pre Called on the single benchmark thread.
 * @post The result is an exact power of two.
 * @post The result is >= @p v and >= 1; no shared state is touched.
 *
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

/* ----------------------------------------------------------- the sweep -- */

/**
 * @struct cbs_cache_t
 * @brief One ::ra8_vmem instance sized for a swept block size + its storage.
 * @details Frame storage, metadata, and buckets are carved from explicit
 *          caller bindings per leg so every leg starts cold; ::cbs_meter_t counts
 *          storage commands (== misses) and delivered bytes.
 * @invariant `frames == k_cbs_cache_bytes / frame_bytes` (>= 1).
 * @since 0.1.0
 */
typedef struct {
  ra8_vmem_t        vm;        /**< The real SLRU page cache under test. */
  ra8_vsource_t     vs;        /**< Object-source registry (one object). */
  ra8_vsource_obj_t objs[1];   /**< The single registered object's slot. */
  cbs_meter_t       meter;     /**< Vsource-seam storage-command meter.  */
  uint8_t*          frame_mem; /**< `frames * block` page storage.       */
  ra8_vmem_frame_t* meta;      /**< Per-frame metadata array.            */
  ra8_vmem_key_t*   keys;      /**< Per-frame key-storage array.         */
  int32_t*          buckets;   /**< Hash-bucket heads.                   */
  uint32_t          frames;    /**< Frame count at this block size.      */
  uint32_t          object_id; /**< Registered object id.                */
} cbs_cache_t;

/**
 * @brief End a ::cbs_cache_t's borrowed workspace bindings (idempotent).
 *
 * @details Frees the frame storage, per-frame metadata, key-storage, and bucket
 *          arrays, then zeroes the bundle so a repeat call is a safe no-op.
 *          Called on every leg exit and on any partial ::internal_cache_open failure.
 *
 * @param[in,out] c Cache bundle to release (NULL tolerated as a no-op).
 *
 * @pre @p c is NULL, or its buffers came from ::internal_cache_open.
 * @pre Called on the single benchmark thread.
 * @post @p c is all-zero; caller-owned backing remains reusable.
 * @post The embedded ::ra8_vmem is no longer usable until reopened.
 *
 * @note Safe for distinct cache/workspace bindings.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_cache_close(cbs_cache_t* c)
{
  if (c == nullptr) {
    return;
  }
  *c = (cbs_cache_t){};
}

/**
 * @brief Round a workspace span to maximum fundamental alignment.
 * @details Applies the power-of-two alignment mask used by every metadata
 *          partition in this bounded sweep composition.
 * @param[in] value Unaligned byte count.
 * @return Smallest aligned byte count not less than @p value.
 * @retval other Aligned span used for workspace partitioning.
 * @pre @p value plus the alignment bias fits in `size_t` by sweep geometry.
 * @pre `alignof(max_align_t)` is a non-zero power of two.
 * @post The result is a multiple of `alignof(max_align_t)`.
 * @post No storage is read or modified.
 * @note Thread-safe: this is a pure arithmetic helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t internal_align_size(size_t value)
{
  const size_t alignment = alignof(max_align_t);
  return (value + alignment - 1U) & ~(alignment - 1U);
}

/**
 * @brief Take one aligned region from the caller-owned sweep workspace.
 * @details Advances @p used only when the aligned request fits completely.
 * @param[in,out] config Sweep configuration carrying workspace storage.
 * @param[in,out] used Current cursor, advanced on success.
 * @param[in] bytes Requested region size before alignment.
 * @return Borrowed region pointer, or NULL when capacity is insufficient.
 * @retval NULL The aligned request does not fit.
 * @retval other Pointer to the start of the reserved region.
 * @pre @p config and @p used are non-NULL.
 * @pre `config->workspace` is valid for `workspace_capacity` bytes.
 * @post On success, @p used advances by the aligned span.
 * @post On failure, @p used and workspace bytes are unchanged.
 * @note Returned storage remains owned by the caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static void* internal_workspace_take(cb_sweep_config_t* config, size_t* used, size_t bytes)
{
  const size_t span = internal_align_size(bytes);
  if ((*used > config->workspace_capacity) || (span > (config->workspace_capacity - *used))) {
    return nullptr;
  }
  void* result = &config->workspace[*used];
  *used += span;
  return result;
}

/**
 * @brief Carve and clear exact cache metadata from caller-owned bindings.
 * @details Derives frame and bucket counts for @p block_bytes, binds the fixed
 *          cache backing, and partitions metadata from the small workspace.
 * @param[out] c Cache bundle receiving all borrowed regions.
 * @param[in] block_bytes Swept cache-frame size.
 * @param[in,out] config Caller bindings and exact-capacity diagnostics.
 * @param[out] out_buckets Receives the hash-bucket count.
 * @return Zero when all regions fit, otherwise one.
 * @retval 0 Cache storage is bound and cleared.
 * @retval 1 Caller workspace is too small.
 * @pre All pointers are non-NULL and cache backing satisfies `cache_capacity`.
 * @pre @p block_bytes is one of the non-zero swept sizes.
 * @post `config->workspace_required` records the attempted high-water mark.
 * @post On success, cache bytes and metadata are zero initialized.
 * @note No storage ownership changes.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_cache_storage(cbs_cache_t*       c,
                                  uint32_t           block_bytes,
                                  cb_sweep_config_t* config,
                                  uint32_t*          out_buckets)
{
  c->frames = (uint32_t)k_cbs_cache_bytes / block_bytes;
  if (c->frames == 0U) {
    c->frames = 1U;
  }
  uint32_t buckets = internal_pow2_ceil(c->frames * 2U);
  if (buckets < (uint32_t)k_cbs_bucket_min) {
    buckets = (uint32_t)k_cbs_bucket_min;
  }
  size_t used  = config->workspace_used;
  c->frame_mem = config->cache_backing;
  c->meta =
    (ra8_vmem_frame_t*)internal_workspace_take(config,
                                               &used,
                                               (size_t)c->frames * sizeof(ra8_vmem_frame_t));
  c->keys    = (ra8_vmem_key_t*)internal_workspace_take(config,
                                                        &used,
                                                        (size_t)c->frames * sizeof(ra8_vmem_key_t));
  c->buckets = (int32_t*)internal_workspace_take(config, &used, (size_t)buckets * sizeof(int32_t));
  config->workspace_required = used;
  if ((c->meta == nullptr) || (c->keys == nullptr) || (c->buckets == nullptr)) {
    return 1;
  }
  memset(config->cache_backing, 0, config->cache_capacity);
  memset(c->meta, 0, (size_t)c->frames * sizeof(ra8_vmem_frame_t));
  memset(c->keys, 0, (size_t)c->frames * sizeof(ra8_vmem_key_t));
  *out_buckets = buckets;
  return 0;
}

/**
 * @brief Stand up a real ::ra8_vmem cache with `frame_bytes == block_bytes`.
 *
 * @details The byte budget ::k_cbs_cache_bytes is constant across sizes, so
 *          the frame count is `budget / block` (min 1) and the bucket count
 *          is the next power of two above twice the frames (min
 *          ::k_cbs_bucket_min). The backend is wired in through the
 *          vsource-seam meter so `meter.calls` counts storage commands.
 *
 * @param[out] c           Cache bundle to populate.
 * @param[in]  be          Backend with a live `read` (post-setup).
 * @param[in]  blob_bytes  Object length in bytes.
 * @param[in]  block_bytes Swept frame size in bytes.
 * @param[in,out] config Caller cache/workspace bindings and diagnostics.
 *
 * @return int 0 on success, 1 on capacity or init failure.
 * @retval 0 @p c is cold and ready for ::ra8_vmem_get.
 * @retval 1 A NULL/zero argument, exact capacity, or init step failed.
 *
 * @pre `be->setup` succeeded for this block size.
 * @pre @p c is writable.
 * @post On 0, `c->vm` is cold and ready for ::ra8_vmem_get.
 * @post On 1, no caller-owned storage is retained.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_cache_open(cbs_cache_t*         c,
                               const cbs_backend_t* be,
                               uint32_t             blob_bytes,
                               uint32_t             block_bytes,
                               cb_sweep_config_t*   config)
{
  if ((c == nullptr) || (be == nullptr) || (be->read == nullptr) || (block_bytes == 0U) ||
      (config == nullptr) || (config->cache_backing == nullptr) ||
      (config->cache_capacity != (size_t)k_cbs_cache_bytes)) {
    return 1;
  }
  *c                = (cbs_cache_t){};
  uint32_t nbuckets = 0U;
  if (internal_cache_storage(c, block_bytes, config, &nbuckets) != 0) {
    internal_cache_close(c);
    return 1;
  }
  c->meter = (cbs_meter_t){.inner = be->read, .inner_ctx = be->read_ctx};
  if (ra8_vsource_init(&c->vs, c->objs, 1U) != k_ra8_ok) {
    internal_cache_close(c);
    return 1;
  }
  if (ra8_vsource_add_paged(&c->vs, priv_meter_read, &c->meter, 0U, blob_bytes, &c->object_id) !=
      k_ra8_ok) {
    internal_cache_close(c);
    return 1;
  }
  const ra8_vmem_cfg_t cfg = {.frame_mem    = c->frame_mem,
                              .frame_bytes  = block_bytes,
                              .frame_count  = c->frames,
                              .meta         = c->meta,
                              .keys         = c->keys,
                              .buckets      = c->buckets,
                              .bucket_count = nbuckets,
                              .loader       = ra8_vsource_loader,
                              .loader_ctx   = &c->vs};
  if (ra8_vmem_init(&c->vm, &cfg) != k_ra8_ok) {
    internal_cache_close(c);
    return 1;
  }
  return 0;
}

/**
 * @brief Drive one timed leg: @p n_reads requests of ::k_cbs_req_bytes each,
 *        at offsets `(i * req) % wrap_bytes`, verifying every byte returned.
 *
 * @details `wrap_bytes == blob_bytes` yields sequential whole-object passes
 *          (leg a); `wrap_bytes == block_bytes` pins the loop inside block 0
 *          (leg b, the pure hit path after one cold fill). The wall clock
 *          brackets only this loop -- backend setup and container packing
 *          are never timed.
 *
 * @param[in,out] c          An open, cold ::cbs_cache_t.
 * @param[in]     payload_read Ground-truth byte-source callback.
 * @param[in]     payload_ctx Ground-truth callback context.
 * @param[in]     n_reads    Requests to issue.
 * @param[in]     wrap_bytes Offset wrap span (see details).
 * @param[out]    row        Row receiving reads / stats / wall time.
 *
 * @return int 0 on success, 1 on any get/put/verify failure.
 * @retval 0 Every request hit correct bytes; @p row is fully filled.
 * @retval 1 A NULL argument, a get/put error, or a byte mismatch occurred.
 *
 * @pre @p c was opened by ::internal_cache_open and is unused (cold).
 * @pre @p wrap_bytes is a non-zero multiple of ::k_cbs_req_bytes.
 * @post `row->wall_ns`, `row->reads`, and the hit/miss/eviction counters are
 *       filled from the cache's own accounting.
 * @post The cache holds no pins (every get was put back).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_drive(cbs_cache_t*        c,
                          ra8_vsource_read_fn payload_read,
                          void*               payload_ctx,
                          uint64_t            n_reads,
                          uint64_t            wrap_bytes,
                          cbs_row_t*          row)
{
  if ((c == nullptr) || (payload_read == nullptr) || (row == nullptr) || (wrap_bytes == 0U)) {
    return 1;
  }
  const uint64_t req   = (uint64_t)k_cbs_req_bytes;
  const uint64_t block = (uint64_t)c->vm.cfg.frame_bytes;
  uint64_t       bad   = 0U;
  const uint64_t t0    = priv_now_ns();
  for (uint64_t i = 0U; i < n_reads; ++i) {
    const uint64_t off  = (i * req) % wrap_bytes;
    void*          page = nullptr;
    if (ra8_vmem_get(&c->vm, c->object_id, off, &page) != k_ra8_ok) {
      bad++;
      continue;
    }
    const uint8_t* piece = &((const uint8_t*)page)[off % block];
    uint8_t        expected[k_cbs_req_bytes];
    if ((payload_read(payload_ctx, off, expected, (uint32_t)req) != k_ra8_ok) ||
        (memcmp(piece, expected, (size_t)req) != 0)) {
      bad++;
    }
    if (ra8_vmem_put(&c->vm, page) != k_ra8_ok) {
      bad++;
    }
  }
  row->wall_ns  = priv_now_ns() - t0;
  row->reads    = n_reads;
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  if (ra8_vmem_stats(&c->vm, &hits, &miss, &evic) != k_ra8_ok) {
    return 1;
  }
  row->hits      = (uint64_t)hits;
  row->misses    = (uint64_t)miss;
  row->evictions = (uint64_t)evic;
  if (bad != 0U) {
    return 1;
  }
  return 0;
}

/** @brief Leg indices for ::internal_run_block's per-leg loop. */
typedef enum : uint8_t {
  k_cbs_leg_seq   = 0U, /**< Sequential whole-object scan. */
  k_cbs_leg_hot   = 1U, /**< Same-block re-read loop.      */
  k_cbs_leg_count = 2U, /**< Number of legs.               */
} cbs_leg_t;

/**
 * @var s_cbs_leg_names
 * @brief Report names for the two workload legs, indexed by ::cbs_leg_t.
 * @details Read-only.
 * @note Only the run/report functions read this.
 * @since 0.1.0
 */
static const char* const s_cbs_leg_names[k_cbs_leg_count] = {"seq", "hot"};

/**
 * @brief Run both legs of one (backend, block size) combination.
 *
 * @details Sets the backend up once for the size, then runs each leg on a
 *          fresh cold cache; per-leg backend counters are taken as deltas so
 *          open-time header/table reads never pollute a leg's traffic
 *          numbers. Each finished row is appended and printed immediately.
 *
 * @param[in,out] be          Backend to exercise.
 * @param[in]     payload_read Ground-truth byte-source callback.
 * @param[in]     payload_ctx Ground-truth callback context.
 * @param[in]     block_bytes Swept block size.
 * @param[in,out] rows        Row array (capacity ::k_cbs_max_rows).
 * @param[in,out] nrows       Row count; incremented per finished leg.
 * @param[in,out] config Caller workspace, scratch, and report sinks.
 *
 * @return int 0 on success, 1 on any setup / leg failure.
 * @retval 0 Both legs ran; ::k_cbs_leg_count rows were appended and printed.
 * @retval 1 A NULL argument, a backend setup, an open, or a drive failed.
 *
 * @pre @p rows has space for ::k_cbs_leg_count more rows.
 * @pre @p be has `setup` and `teardown` bound.
 * @post The backend binding is ended on every path; caller storage is retained.
 * @post On 0, ::k_cbs_leg_count rows were appended and printed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_run_block(cbs_backend_t*      be,
                              ra8_vsource_read_fn payload_read,
                              void*               payload_ctx,
                              uint32_t            block_bytes,
                              cbs_row_t*          rows,
                              uint32_t*           nrows,
                              cb_sweep_config_t*  config)
{
  if ((be == nullptr) || (payload_read == nullptr) || (rows == nullptr) || (nrows == nullptr) ||
      (config == nullptr)) {
    return 1;
  }
  config->workspace_used = config->workspace_floor;
  if (be->setup(be, payload_read, payload_ctx, (uint32_t)k_cbs_blob_bytes, block_bytes, config) !=
      0) {
    return 1;
  }
  int rc = 0;
  for (uint8_t leg = 0U; leg < (uint8_t)k_cbs_leg_count; ++leg) {
    cbs_cache_t c = {};
    if (internal_cache_open(&c, be, (uint32_t)k_cbs_blob_bytes, block_bytes, config) != 0) {
      rc = 1;
      break;
    }
    const uint64_t src0 = (be->src_bytes != nullptr) ? *be->src_bytes : 0U;
    const bool     seq  = (leg == (uint8_t)k_cbs_leg_seq);
    const uint64_t n =
      seq ? ((uint64_t)k_cbs_seq_passes * (uint64_t)k_cbs_blob_bytes / (uint64_t)k_cbs_req_bytes)
          : (uint64_t)k_cbs_hot_reads;
    const uint64_t wrap = seq ? (uint64_t)k_cbs_blob_bytes : (uint64_t)block_bytes;
    cbs_row_t*     row  = &rows[*nrows];
    *row                = (cbs_row_t){.backend       = be->name,
                                      .leg           = s_cbs_leg_names[leg],
                                      .block_bytes   = block_bytes,
                                      .frames        = c.frames,
                                      .backing_bytes = be->backing_bytes};
    const int drc       = internal_drive(&c, payload_read, payload_ctx, n, wrap, row);
    row->be_calls       = c.meter.calls;
    row->be_bytes       = c.meter.bytes;
    row->src_bytes      = (be->src_bytes != nullptr) ? (*be->src_bytes - src0) : c.meter.bytes;
    internal_cache_close(&c);
    if (drc != 0) {
      rc = 1;
      break;
    }
    (*nrows)++;
    if (priv_print_row(config->output, row) != 0) {
      rc = 1;
      break;
    }
  }
  be->teardown(be);
  return rc;
}

/**
 * @var s_cbs_blocks
 * @brief The swept block sizes, ascending (drives loops + knee search).
 * @details Values come from ::cbs_block_size_t.
 * @note Read-only.
 * @since 0.1.0
 */
static const uint32_t s_cbs_blocks[] = {
  (uint32_t)k_cbs_block_512b,
  (uint32_t)k_cbs_block_1kib,
  (uint32_t)k_cbs_block_4kib,
  (uint32_t)k_cbs_block_16kib,
  (uint32_t)k_cbs_block_64kib,
  (uint32_t)k_cbs_block_256kib,
};

/**
 * @brief Execute every backend and block-size pair and append its result rows.
 * @details Traverses the bounded backend-by-size matrix in stable order and
 *          stops at the first failed setup, drive, verification, or report.
 * @param[in,out] config Bound sweep composition.
 * @param[in,out] payload Deterministic payload source.
 * @param[in,out] backends Backend table.
 * @param[in] backend_count Number of backend entries.
 * @param[out] rows Fixed result-row array.
 * @param[in,out] row_count Number of valid rows appended.
 * @return Zero after the complete matrix, otherwise one.
 * @retval 0 Every matrix point produced both workload rows.
 * @retval 1 One matrix point failed.
 * @pre Output arrays have capacity ::k_cbs_max_rows.
 * @pre @p backend_count and the static size count fit that row capacity.
 * @post On success, @p row_count equals the full matrix row count.
 * @post On failure, rows before @p row_count remain complete and ordered.
 * @note Backends are executed serially on the benchmark thread.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_execute(cb_sweep_config_t* config,
                            cbs_payload_t*     payload,
                            cbs_backend_t*     backends,
                            uint32_t           backend_count,
                            cbs_row_t*         rows,
                            uint32_t*          row_count)
{
  const uint32_t block_count = (uint32_t)(sizeof(s_cbs_blocks) / sizeof(s_cbs_blocks[0]));
  int            result      = 0;
  for (uint32_t backend = 0U; (backend < backend_count) && (result == 0); ++backend) {
    for (uint32_t size = 0U; (size < block_count) && (result == 0); ++size) {
      result = internal_run_block(&backends[backend],
                                  priv_payload_read,
                                  payload,
                                  s_cbs_blocks[size],
                                  rows,
                                  row_count,
                                  config);
    }
  }
  return result;
}

/**
 * @brief Publish all human summary tables and the crossover verdict.
 * @details Emits the fixed report heading, both workload tables for each
 *          backend, and the measured chunk-size crossover conclusion.
 * @param[in,out] config Bound output composition.
 * @param[in] backends Completed backend table.
 * @param[in] backend_count Number of backend entries.
 * @param[in] rows Completed result rows.
 * @param[in] row_count Number of valid rows.
 * @return Zero after complete publication, otherwise one.
 * @retval 0 Every summary fragment was accepted by the sink.
 * @retval 1 A table or verdict publication failed.
 * @pre @p config and @p backends are non-NULL.
 * @pre @p rows contains @p row_count complete matrix rows.
 * @post No result row or backend is modified.
 * @post On success, the human-readable report is complete.
 * @note Timing values are informational and do not select process exit status.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_summary(cb_sweep_config_t*   config,
                            const cbs_backend_t* backends,
                            uint32_t             backend_count,
                            const cbs_row_t*     rows,
                            uint32_t             row_count)
{
  const uint32_t block_count = (uint32_t)(sizeof(s_cbs_blocks) / sizeof(s_cbs_blocks[0]));
  if (cb_sink_format(config->output,
                     "\n## Summary (payload 8 MiB, cache budget 1 MiB, %u B reader requests)\n",
                     (unsigned)k_cbs_req_bytes) != k_cb_io_ok) {
    return 1;
  }
  for (uint32_t backend = 0U; backend < backend_count; ++backend) {
    if ((priv_print_seq_table(config->output,
                              rows,
                              row_count,
                              backends[backend].name,
                              s_cbs_blocks,
                              block_count) != 0) ||
        (priv_print_hot_table(config->output,
                              rows,
                              row_count,
                              backends[backend].name,
                              s_cbs_blocks,
                              block_count) != 0)) {
      return 1;
    }
  }
  return priv_print_crossover(config->output, rows, row_count, s_cbs_blocks, block_count);
}

int cb_sweep_block(cb_sweep_config_t* config)
{
  if ((config == nullptr) || (config->workspace == nullptr) || (config->output == nullptr) ||
      (config->error == nullptr) || (config->scratch == nullptr)) {
    return 1;
  }
  config->cache_required     = (size_t)k_cbs_cache_bytes;
  const size_t payload_bytes = priv_payload_workspace_required();
  config->workspace_required = payload_bytes;
  cbs_payload_t payload      = {};
  if ((payload_bytes > config->workspace_capacity) ||
      (priv_payload_init(&payload, config->workspace, config->workspace_capacity) != 0)) {
    return 1;
  }
  config->workspace_floor = internal_align_size(payload_bytes);
  config->workspace_used  = config->workspace_floor;
  if (cb_sink_format(config->output, "# #208 block/frame-size sweep\n\n") != k_cb_io_ok ||
      cb_sink_format(config->output,
                     "payload=%u cache_budget=%u req=%u seq_passes=%u hot_reads=%u zlib_level=%d "
                     "cache=ra8_vmem(SLRU)\n\n",
                     (unsigned)k_cbs_blob_bytes,
                     (unsigned)k_cbs_cache_bytes,
                     (unsigned)k_cbs_req_bytes,
                     (unsigned)k_cbs_seq_passes,
                     (unsigned)k_cbs_hot_reads,
                     MZ_BEST_COMPRESSION) != k_cb_io_ok) {
    return 1;
  }

  cbs_row_t      rows[k_cbs_max_rows] = {};
  uint32_t       row_count            = 0U;
  uint32_t       backend_count        = 0U;
  cbs_backend_t* backends             = priv_backends(&backend_count);
  int            rc = internal_execute(config, &payload, backends, backend_count, rows, &row_count);
  if (rc == 0) {
    rc = internal_summary(config, backends, backend_count, rows, row_count);
  }
  if (rc != 0) {
    (void)cb_sink_format(config->error,
                         "sweep-block: sweep aborted (cache required=%zu supplied=%zu; "
                         "workspace required=%zu supplied=%zu)\n",
                         config->cache_required,
                         config->cache_capacity,
                         config->workspace_required,
                         config->workspace_capacity);
  }
  return rc;
}
