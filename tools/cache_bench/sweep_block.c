/**
 * @file sweep_block.c
 * @brief Core of the #208 block/frame-size sweep (`--sweep-block`).
 *
 * @details
 * Drives the REAL ::ra8_vmem SLRU page cache (not a re-modelled policy) with
 * `frame_bytes` swept from 512 B to 256 KiB under a constant byte budget,
 * over the backends published by ::cbs_priv_backends() (the ::cbs_backend_t
 * seam; implementations live in sweep_block_backends.c). Leg (a) scans the
 * whole object sequentially; leg (b) re-reads one block. Every byte handed
 * back by the cache is verified against the source blob. Row printing and
 * the summary report live in sweep_block_report.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include "sweep_block.h"

#include <stdio.h>
#include <stdlib.h>
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
void internal_ra8_log_error(const char* tag, const char* message)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s\n", tag, message);
}

/** @brief Valued log backend stub (present for the linker, as reader_vmem). */
void internal_ra8_log_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s =%u\n", tag, message, value);
}

/** @brief Round @p v up to a power of two (>= 1). */
static uint32_t cbs_pow2_ceil(uint32_t v)
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
 * @details Frame storage, metadata, and buckets are heap-carved per leg so
 *          every leg starts cold; the vsource-level ::cbs_meter_t counts
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

/** @brief Release a ::cbs_cache_t's heap carvings (idempotent). */
static void cbs_cache_close(cbs_cache_t* c)
{
  if (c == nullptr) {
    return;
  }
  free(c->frame_mem);
  free(c->meta);
  free(c->keys);
  free(c->buckets);
  *c = (cbs_cache_t){};
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
 *
 * @return int 0 on success, 1 on allocation or init failure.
 *
 * @pre `be->setup` succeeded for this block size.
 * @pre @p c is writable.
 * @post On 0, `c->vm` is cold and ready for ::ra8_vmem_get.
 * @post On 1, everything partially acquired is freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int
cbs_cache_open(cbs_cache_t* c, cbs_backend_t* be, uint32_t blob_bytes, uint32_t block_bytes)
{
  if ((c == nullptr) || (be == nullptr) || (be->read == nullptr) || (block_bytes == 0U)) {
    return 1;
  }
  *c        = (cbs_cache_t){};
  c->frames = (uint32_t)k_cbs_cache_bytes / block_bytes;
  if (c->frames == 0U) {
    c->frames = 1U;
  }
  uint32_t nbuckets = cbs_pow2_ceil(c->frames * 2U);
  if (nbuckets < (uint32_t)k_cbs_bucket_min) {
    nbuckets = (uint32_t)k_cbs_bucket_min;
  }
  c->frame_mem = (uint8_t*)malloc((size_t)c->frames * (size_t)block_bytes);
  c->meta      = (ra8_vmem_frame_t*)calloc((size_t)c->frames, sizeof(ra8_vmem_frame_t));
  c->keys      = (ra8_vmem_key_t*)calloc((size_t)c->frames, sizeof(ra8_vmem_key_t));
  c->buckets   = (int32_t*)malloc((size_t)nbuckets * sizeof(int32_t));
  if ((c->frame_mem == nullptr) || (c->meta == nullptr) || (c->keys == nullptr) ||
      (c->buckets == nullptr)) {
    cbs_cache_close(c);
    return 1;
  }
  c->meter = (cbs_meter_t){.inner = be->read, .inner_ctx = be->read_ctx};
  if (ra8_vsource_init(&c->vs, c->objs, 1U) != k_ra8_ok) {
    cbs_cache_close(c);
    return 1;
  }
  if (ra8_vsource_add_paged(&c->vs,
                            cbs_priv_meter_read,
                            &c->meter,
                            0U,
                            blob_bytes,
                            &c->object_id) != k_ra8_ok) {
    cbs_cache_close(c);
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
    cbs_cache_close(c);
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
 * @param[in]     blob       Ground-truth payload for verification.
 * @param[in]     n_reads    Requests to issue.
 * @param[in]     wrap_bytes Offset wrap span (see details).
 * @param[out]    row        Row receiving reads / stats / wall time.
 *
 * @return int 0 on success, 1 on any get/put/verify failure.
 *
 * @pre @p c was opened by ::cbs_cache_open and is unused (cold).
 * @pre @p wrap_bytes is a non-zero multiple of ::k_cbs_req_bytes.
 * @post `row->wall_ns`, `row->reads`, and the hit/miss/eviction counters are
 *       filled from the cache's own accounting.
 * @post The cache holds no pins (every get was put back).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int cbs_drive(cbs_cache_t*   c,
                     const uint8_t* blob,
                     uint64_t       n_reads,
                     uint64_t       wrap_bytes,
                     cbs_row_t*     row)
{
  if ((c == nullptr) || (blob == nullptr) || (row == nullptr) || (wrap_bytes == 0U)) {
    return 1;
  }
  const uint64_t req   = (uint64_t)k_cbs_req_bytes;
  const uint64_t block = (uint64_t)c->vm.cfg.frame_bytes;
  uint64_t       bad   = 0U;
  const uint64_t t0    = cbs_priv_now_ns();
  for (uint64_t i = 0U; i < n_reads; ++i) {
    const uint64_t off  = (i * req) % wrap_bytes;
    void*          page = nullptr;
    if (ra8_vmem_get(&c->vm, c->object_id, off, &page) != k_ra8_ok) {
      bad++;
      continue;
    }
    const uint8_t* piece = &((const uint8_t*)page)[off % block];
    if (memcmp(piece, &blob[off], (size_t)req) != 0) {
      bad++;
    }
    if (ra8_vmem_put(&c->vm, page) != k_ra8_ok) {
      bad++;
    }
  }
  row->wall_ns  = cbs_priv_now_ns() - t0;
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
    (void)fprintf(stderr, "sweep-block: %llu bad accesses\n", (unsigned long long)bad);
    return 1;
  }
  return 0;
}

/** @brief Leg indices for ::cbs_run_block's per-leg loop. */
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
 * @param[in]     blob        Source payload (ground truth).
 * @param[in]     block_bytes Swept block size.
 * @param[in,out] rows        Row array (capacity ::k_cbs_max_rows).
 * @param[in,out] nrows       Row count; incremented per finished leg.
 *
 * @return int 0 on success, 1 on any setup / leg failure.
 *
 * @pre @p rows has space for ::k_cbs_leg_count more rows.
 * @pre @p be has `setup` and `teardown` bound.
 * @post The backend is torn down (buffers freed) on every path.
 * @post On 0, ::k_cbs_leg_count rows were appended and printed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int cbs_run_block(cbs_backend_t* be,
                         const uint8_t* blob,
                         uint32_t       block_bytes,
                         cbs_row_t*     rows,
                         uint32_t*      nrows)
{
  if ((be == nullptr) || (blob == nullptr) || (rows == nullptr) || (nrows == nullptr)) {
    return 1;
  }
  if (be->setup(be, blob, (uint32_t)k_cbs_blob_bytes, block_bytes) != 0) {
    return 1;
  }
  int rc = 0;
  for (uint8_t leg = 0U; leg < (uint8_t)k_cbs_leg_count; ++leg) {
    cbs_cache_t c = {};
    if (cbs_cache_open(&c, be, (uint32_t)k_cbs_blob_bytes, block_bytes) != 0) {
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
    const int drc       = cbs_drive(&c, blob, n, wrap, row);
    row->be_calls       = c.meter.calls;
    row->be_bytes       = c.meter.bytes;
    row->src_bytes      = (be->src_bytes != nullptr) ? (*be->src_bytes - src0) : c.meter.bytes;
    cbs_cache_close(&c);
    if (drc != 0) {
      rc = 1;
      break;
    }
    (*nrows)++;
    cbs_priv_print_row(row);
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

int cb_sweep_block(void)
{
  uint8_t* blob = (uint8_t*)malloc((size_t)k_cbs_blob_bytes);
  if (blob == nullptr) {
    (void)fprintf(stderr, "sweep-block: payload allocation failed\n");
    /* cppcheck-suppress memleak ; false positive: cppcheck 2.13 does not
     * model the C23 nullptr keyword, so it cannot see blob is NULL here. */
    return 1;
  }
  cbs_priv_fill_text(blob, (uint32_t)k_cbs_blob_bytes);

  (void)printf("# #208 block/frame-size sweep\n\n");
  (void)printf("payload=%u cache_budget=%u req=%u seq_passes=%u hot_reads=%u zlib_level=%d "
               "cache=ra8_vmem(SLRU)\n\n",
               (unsigned)k_cbs_blob_bytes,
               (unsigned)k_cbs_cache_bytes,
               (unsigned)k_cbs_req_bytes,
               (unsigned)k_cbs_seq_passes,
               (unsigned)k_cbs_hot_reads,
               MZ_BEST_COMPRESSION);

  static cbs_row_t s_rows[k_cbs_max_rows] = {};
  uint32_t         nrows                  = 0U;
  int              rc                     = 0;
  uint32_t         nbe                    = 0U;
  cbs_backend_t*   backends               = cbs_priv_backends(&nbe);
  const uint32_t   nblocks = (uint32_t)(sizeof(s_cbs_blocks) / sizeof(s_cbs_blocks[0]));
  for (uint32_t b = 0U; (b < nbe) && (rc == 0); ++b) {
    for (uint32_t s = 0U; (s < nblocks) && (rc == 0); ++s) {
      rc = cbs_run_block(&backends[b], blob, s_cbs_blocks[s], s_rows, &nrows);
    }
  }
  if (rc == 0) {
    (void)printf("\n## Summary (payload 8 MiB, cache budget 1 MiB, %u B reader requests)\n",
                 (unsigned)k_cbs_req_bytes);
    for (uint32_t b = 0U; b < nbe; ++b) {
      cbs_priv_print_seq_table(s_rows, nrows, backends[b].name, s_cbs_blocks, nblocks);
      cbs_priv_print_hot_table(s_rows, nrows, backends[b].name, s_cbs_blocks, nblocks);
    }
    cbs_priv_print_crossover(s_rows, nrows, s_cbs_blocks, nblocks);
  } else {
    (void)fprintf(stderr, "sweep-block: sweep aborted\n");
  }
  free(blob);
  return rc;
}
