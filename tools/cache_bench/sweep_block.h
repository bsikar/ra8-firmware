/**
 * @file sweep_block.h
 * @brief #208 block/frame-size sweep: the byte-size axis the capacity sweep
 *        never touches, so the chunked `.rabook` chunk size and the ra_vmem
 *        `frame_bytes` constant are picked from measurement, not intuition.
 *
 * @details
 * `cache_bench`'s default mode sweeps cache CAPACITY (frames) only; every
 * frame is an abstract (object, page) key with no size in bytes. This mode
 * sweeps the missing axis -- the block/frame size itself -- using the
 * methodology of Memory Systems Ch 23.2.1/23.2.2:
 *
 *  (a) **Sequential whole-object scan** at each block size, which isolates
 *      where per-request overhead stops dominating and sustained streaming
 *      throughput takes over (the knee names the chunk size).
 *  (b) **Repeated same-block reads**, which isolate the pure cache-hit path
 *      and show it is independent of the block size.
 *
 * Unlike the capacity sweep (a re-modelled policy harness), this mode drives
 * the REAL firmware stack: ::ra_vmem (SLRU page cache, Layer 2) over
 * ::ra_vsource (Layer 1) with the swept size as `frame_bytes`, fed through
 * the ::cbs_backend_t seam below. Two synthetic host backends ship in-tree:
 *
 *  - `mem`     -- plain memcpy from a resident blob (the harness floor).
 *  - `rbkc-z9` -- a real "RBKC" chunked `.rabook` container built in memory
 *                 with zlib level-9 streams (mirroring `tools/epub_compile`),
 *                 read through ::ra_book_chunked_read, so every miss pays a
 *                 genuine staged read + tinfl inflate of one chunk. This is
 *                 the decompress-per-miss number that picks the chunk size.
 *
 * The SD-over-SPI hardware leg of #208 plugs in later as a third
 * ::cbs_backend_t whose `read` issues real card reads; nothing in the sweep
 * core changes.
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

#include "ra_vsource.h"

/**
 * @struct cbs_backend_t
 * @brief One byte-addressed backing store the sweep reads through -- the
 *        backend DIP seam the #208 hardware leg will implement.
 *
 * @details `setup` prepares a backing for one (blob, block size) pair and
 *          publishes `read`/`read_ctx` (an ::ra_vsource_read_fn, so the
 *          backing plugs straight into ::ra_vsource_add_paged); `teardown`
 *          releases whatever `setup` acquired. `backing_bytes` reports the
 *          on-medium size of the backing (the container size for a chunked
 *          backend), and `src_bytes`, when non-NULL, points at a counter of
 *          raw medium bytes transferred (compressed stream bytes for the
 *          RBKC backend) so the report can separate bytes-on-the-wire from
 *          bytes-delivered-to-the-cache.
 *
 * @invariant After a successful `setup`, `read` is non-NULL until `teardown`.
 * @invariant `setup`/`teardown` calls alternate (no nested setups).
 *
 * @see cb_sweep_block()  The driver that exercises a backend at every size.
 * @since 0.1.0
 */
typedef struct cbs_backend cbs_backend_t;
struct cbs_backend {
  const char* name; /**< Backend name for the report rows. */
  /** @brief Build the backing for @p blob at @p block_bytes; 0 on success. */
  int (*setup)(cbs_backend_t* be, const uint8_t* blob, uint32_t blob_bytes, uint32_t block_bytes);
  /** @brief Release everything `setup` acquired (idempotent). */
  void (*teardown)(cbs_backend_t* be);
  ra_vsource_read_fn read;          /**< Byte reader over the backing (set by `setup`).    */
  void*              read_ctx;      /**< Context for @ref read (set by `setup`).           */
  uint64_t           backing_bytes; /**< On-medium size of the backing, in bytes.          */
  const uint64_t*    src_bytes;     /**< Raw medium-byte counter, or NULL if == delivered. */
};

/**
 * @struct cbs_row_t
 * @brief One measured (backend, leg, block size) result row.
 *
 * @details Filled by the sweep driver; `be_calls`/`be_bytes` are counted at
 *          the ::ra_vsource_read_fn seam (one call per cache miss), while
 *          `src_bytes` counts raw medium traffic (compressed bytes for the
 *          RBKC backend; equal to `be_bytes` for uncompressed backends).
 *
 * @invariant `hits + misses == reads` for every completed row.
 *
 * @see cb_sweep_block()
 * @since 0.1.0
 */
typedef struct {
  const char* backend;       /**< Backend name (::cbs_backend_t.name).          */
  const char* leg;           /**< Workload leg: "seq" or "hot".                 */
  uint32_t    block_bytes;   /**< Swept block / frame / chunk size in bytes.    */
  uint32_t    frames;        /**< Cache frames at this size (budget / block).   */
  uint64_t    reads;         /**< Reader requests issued.                       */
  uint64_t    hits;          /**< ra_vmem hits.                                 */
  uint64_t    misses;        /**< ra_vmem misses.                               */
  uint64_t    evictions;     /**< ra_vmem evictions.                            */
  uint64_t    be_calls;      /**< Backend read calls (storage commands).        */
  uint64_t    be_bytes;      /**< Bytes delivered to the cache by the backend.  */
  uint64_t    src_bytes;     /**< Raw medium bytes moved (compressed for RBKC). */
  uint64_t    backing_bytes; /**< On-medium backing size at this block size.    */
  uint64_t    wall_ns;       /**< Wall-clock time of the timed loop, in ns.     */
} cbs_row_t;

/**
 * @brief Run the #208 block/frame-size sweep and print the report.
 *
 * @details
 * For every registered backend and every swept block size (512 B .. 256 KiB),
 * builds the backing, stands up a real ::ra_vmem cache with `frame_bytes`
 * equal to the swept size under a constant byte budget, and drives legs (a)
 * sequential whole-object scan and (b) same-block re-read. Prints one
 * machine-parseable `sweep-block ...` line per row, then a human summary
 * with the measured knee and a chunk-size recommendation versus the current
 * 64 KiB `.rabook` default (#204). Every returned byte is verified against
 * the source blob, so a lying backend fails the run instead of skewing it.
 *
 * @return int Process-style status.
 * @retval 0 Sweep completed; every row verified byte-identical.
 * @retval 1 Allocation, backend setup, or data-verification failure.
 *
 * @pre The process has heap headroom for the blob + largest container
 *      (roughly 3x the 8 MiB payload).
 * @pre Stdout is line-buffered or better (rows are printed as produced).
 * @post On 0, one row per (backend, leg, size) was printed plus the summary.
 * @post No heap is leaked on either path (backends tear down on failure).
 *
 * @note Not thread-safe (single-threaded host tool; static backend state).
 *
 * @see cbs_backend_t  The seam a future SD-over-SPI hardware leg implements.
 * @since 0.1.0
 */
int cb_sweep_block(void);
