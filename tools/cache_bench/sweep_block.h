/**
 * @file sweep_block.h
 * @brief #208 block/frame-size sweep: the byte-size axis the capacity sweep
 *        never touches, so the chunked `.rabook` chunk size and the ra8_vmem
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
 * the REAL firmware stack: ::ra8_vmem (SLRU page cache, Layer 2) over
 * ::ra8_vsource (Layer 1) with the swept size as `frame_bytes`, fed through
 * the ::cbs_backend_t seam (declared in sweep_block_internal.h, implemented
 * in sweep_block_backends.c). Two synthetic host backends ship in-tree:
 *
 *  - `mem`     -- plain memcpy from a resident blob (the harness floor).
 *  - `rbkc-z9` -- a real "RBKC" chunked `.rabook` container built in memory
 *                 with zlib level-9 streams (mirroring `tools/epub_compile`),
 *                 read through ::ra8_book_chunked_read, so every miss pays a
 *                 genuine staged read + tinfl inflate of one chunk. This is
 *                 the decompress-per-miss number that picks the chunk size.
 *
 * The SD-over-SPI hardware leg of #208 plugs in later as a third
 * ::cbs_backend_t whose `read` issues real card reads; nothing in the sweep
 * core changes.
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

#include "cache_bench_io.h"

/** @brief Caller-owned bindings for the block sweep. */
typedef struct {
  uint8_t*      cache_backing;      /**< Exactly the measured resident-cache budget. */
  size_t        cache_capacity;     /**< Supplied cache bytes.                       */
  size_t        cache_required;     /**< Exact semantic cache requirement.           */
  uint8_t*      workspace;          /**< Small metadata/codec-input workspace.       */
  size_t        workspace_capacity; /**< Supplied workspace bytes.                   */
  size_t        workspace_required; /**< Exact latest requirement.                   */
  size_t        workspace_floor;    /**< Persistent payload-index prefix.            */
  size_t        workspace_used;     /**< Current phase workspace usage.              */
  cb_scratch_t* scratch;            /**< Host-composed streamed RBKC transaction.    */
  cb_sink_t*    output;             /**< Report destination.                         */
  cb_sink_t*    error;              /**< Diagnostic destination.                     */
} cb_sweep_config_t;

/**
 * @brief Run the #208 block/frame-size sweep and print the report.
 *
 * @details
 * For every registered backend and every swept block size (512 B .. 256 KiB),
 * builds the backing, stands up a real ::ra8_vmem cache with `frame_bytes`
 * equal to the swept size under a constant byte budget, and drives legs (a)
 * sequential whole-object scan and (b) same-block re-read. Prints one
 * machine-parseable `sweep-block ...` line per row, then a human summary
 * with the measured knee and a chunk-size recommendation versus the current
 * 64 KiB `.rabook` default (#204). Every returned byte is verified against
 * the source blob, so a lying backend fails the run instead of skewing it.
 *
 * @param[in,out] config Caller-owned cache, workspace, scratch, and sinks.
 *
 * @return int Process-style status.
 * @retval 0 Sweep completed; every row verified byte-identical.
 * @retval 1 Capacity, backend setup, I/O, or data-verification failure.
 *
 * @pre @p config supplies exactly 1 MiB of aligned cache backing, bounded
 *      metadata workspace, a scratch transaction, and output/error sinks.
 * @pre The scratch callbacks complete exact positional transfers.
 * @post On 0, one row per (backend, leg, size) was printed plus the summary.
 * @post No storage ownership changes; scratch and workspace remain caller-owned.
 *
 * @note Not thread-safe (single-threaded host tool; static backend state).
 *
 * @see cbs_backend_t  The seam a future SD-over-SPI hardware leg implements.
 * @since 0.1.0
 */
int cb_sweep_block(cb_sweep_config_t* config);
