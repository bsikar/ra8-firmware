/**
 * @file sweep_block_internal.h
 * @brief Module-private seams shared by the #208 sweep translation units.
 *
 * @details
 * The `--sweep-block` mode is split across three translation units to keep
 * each under the maintainability line cap:
 *
 *  - `sweep_block.c`          -- the sweep core: the real ::ra8_vmem cache
 *    bundle, the timed drive loop, per-(backend, size) orchestration, and
 *    the `cb_sweep_block()` entry point.
 *  - `sweep_block_backends.c` -- the two in-tree ::cbs_backend_t
 *    implementations (`mem` and `rbkc-z9`), the byte-counting meter shim,
 *    the deterministic text filler, and the wall-clock helper.
 *  - `sweep_block_report.c`   -- the machine-parseable row printer and the
 *    human summary (per-leg tables + the measured knee / crossover verdict).
 *
 * This header carries the sweep geometry constants, the backend DIP seam
 * (::cbs_backend_t), the result-row type (::cbs_row_t), the counting meter
 * (::cbs_meter_t), and the `RA8_PRIV` declarations of every helper shared
 * across those translation units. Nothing here is part of the tool's public
 * surface: `cache_bench.c` consumes only `sweep_block.h`.
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

#include "ra8_attributes.h"
#include "ra8_vsource.h"

/**
 * @enum cbs_dim_t
 * @brief Workload geometry and fixed sweep parameters.
 * @details The payload is a multiple of every swept block size and of the
 *          reader request grain, so no leg ever sees a partial-block edge;
 *          the cache byte budget is held constant across sizes (same RAM
 *          spend, different frame counts) for an honest comparison.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cbs_blob_bytes    = 8388608U,    /**< 8 MiB payload (2^23; all sizes divide it). */
  k_cbs_cache_bytes   = 1048576U,    /**< 1 MiB resident cache budget (constant).    */
  k_cbs_req_bytes     = 256U,        /**< Reader request grain (divides every size). */
  k_cbs_seq_passes    = 4U,          /**< Whole-object passes in the seq leg.        */
  k_cbs_hot_reads     = 1048576U,    /**< Accesses in the same-block re-read leg.    */
  k_cbs_bucket_min    = 16U,         /**< Minimum hash-bucket count for tiny caches. */
  k_cbs_default_chunk = 65536U,      /**< #204 `.rabook` chunk-size default.         */
  k_cbs_knee_pct      = 90U,         /**< %% of peak throughput that names the knee. */
  k_cbs_words_per_dot = 11U,         /**< Words per sentence in the text filler.     */
  k_cbs_kib           = 1024U,       /**< Bytes per KiB (block-size labels).         */
  k_cbs_ns_per_us     = 1000U,       /**< Nanoseconds per microsecond.               */
  k_cbs_ns_per_s      = 1000000000U, /**< Nanoseconds per second.                    */
  k_cbs_max_rows      = 24U,         /**< 2 backends x 6 sizes x 2 legs.             */
} cbs_dim_t;

/**
 * @struct cbs_backend_t
 * @brief One byte-addressed backing store the sweep reads through -- the
 *        backend DIP seam the #208 hardware leg will implement.
 *
 * @details `setup` prepares a backing for one (blob, block size) pair and
 *          publishes `read`/`read_ctx` (an ::ra8_vsource_read_fn, so the
 *          backing plugs straight into ::ra8_vsource_add_paged); `teardown`
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
  ra8_vsource_read_fn read;          /**< Byte reader over the backing (set by `setup`).    */
  void*               read_ctx;      /**< Context for @ref read (set by `setup`).           */
  uint64_t            backing_bytes; /**< On-medium size of the backing, in bytes.          */
  const uint64_t*     src_bytes;     /**< Raw medium-byte counter, or NULL if == delivered. */
};

/**
 * @struct cbs_row_t
 * @brief One measured (backend, leg, block size) result row.
 *
 * @details Filled by the sweep driver; `be_calls`/`be_bytes` are counted at
 *          the ::ra8_vsource_read_fn seam (one call per cache miss), while
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
  uint64_t    hits;          /**< ra8_vmem hits.                                */
  uint64_t    misses;        /**< ra8_vmem misses.                              */
  uint64_t    evictions;     /**< ra8_vmem evictions.                           */
  uint64_t    be_calls;      /**< Backend read calls (storage commands).        */
  uint64_t    be_bytes;      /**< Bytes delivered to the cache by the backend.  */
  uint64_t    src_bytes;     /**< Raw medium bytes moved (compressed for RBKC). */
  uint64_t    backing_bytes; /**< On-medium backing size at this block size.    */
  uint64_t    wall_ns;       /**< Wall-clock time of the timed loop, in ns.     */
} cbs_row_t;

/**
 * @struct cbs_meter_t
 * @brief Counting shim around an ::ra8_vsource_read_fn.
 * @details Forwards to `inner` and tallies calls + bytes. One instance sits
 *          at the vsource seam (counting storage commands = cache misses);
 *          the RBKC backend nests a second one under its container file to
 *          count raw compressed bytes.
 * @invariant `inner` is non-NULL whenever the meter is registered as a reader.
 * @since 0.1.0
 */
typedef struct {
  ra8_vsource_read_fn inner;     /**< Wrapped reader.                */
  void*               inner_ctx; /**< Context for @ref inner.        */
  uint64_t            calls;     /**< Read calls forwarded so far.   */
  uint64_t            bytes;     /**< Bytes served through the shim. */
} cbs_meter_t;

/**
 * @brief Monotonic wall-clock in nanoseconds.
 *
 * @details Reads `CLOCK_MONOTONIC` and folds seconds + nanoseconds into one
 *          64-bit nanosecond count; the sweep core brackets each timed leg
 *          with two calls and reports the difference.
 *
 * @return uint64_t Monotonic time in nanoseconds since an arbitrary epoch.
 * @retval other The current CLOCK_MONOTONIC reading folded to nanoseconds.
 *
 * @pre The host provides `CLOCK_MONOTONIC` (POSIX; true on macOS + Linux).
 * @pre No argument is required; the call takes none.
 * @post The returned value never decreases across calls in one process.
 * @post No argument or global state is read or written.
 *
 * @note Thread-safe (stateless syscall wrapper).
 * @since 0.1.0
 */
RA8_PRIV uint64_t cbs_priv_now_ns(void);

/**
 * @brief Fill @p blob with deterministic book-like pseudo-text.
 *
 * @details Draws words from a fixed pool with a fixed-seed xorshift64 PRNG,
 *          inserting sentence punctuation on a fixed cadence, so the RBKC
 *          backend's zlib streams see a compression ratio representative of
 *          real reflowed chapter text (roughly 2.5-3x). Runs are
 *          byte-identical across hosts and across the two backends.
 *
 * @param[out] blob Buffer to fill (NULL is tolerated as a no-op).
 * @param[in]  len  Bytes to write into @p blob (0 is a no-op).
 *
 * @return void.
 *
 * @pre @p blob covers @p len writable bytes when non-NULL.
 * @pre Called on the single benchmark thread.
 * @post On a non-NULL @p blob, all @p len bytes are printable ASCII text.
 * @post The output is byte-identical for a given @p len across runs and hosts.
 *
 * @note Not thread-safe with itself on the same buffer (caller-owned buffer).
 * @since 0.1.0
 */
RA8_PRIV void cbs_priv_fill_text(uint8_t* blob, uint32_t len);

/**
 * @brief `ra8_vsource_read_fn` forwarding through a ::cbs_meter_t.
 *
 * @details Counts one call plus @p len bytes against the meter, then
 *          forwards to the wrapped reader. Registered at the vsource seam by
 *          the sweep core (counting storage commands == cache misses) and
 *          under the RBKC container file (counting raw compressed traffic).
 *
 * @param[in]  ctx    The ::cbs_meter_t to charge (as `void*`).
 * @param[in]  offset Byte offset forwarded to the wrapped reader.
 * @param[out] buf    Destination buffer forwarded to the wrapped reader.
 * @param[in]  len    Bytes requested.
 *
 * @return ra8_err_t Forwarded result of the wrapped reader.
 * @retval k_ra8_err_null_ptr @p ctx or its wrapped reader is NULL.
 *
 * @pre @p ctx points at a ::cbs_meter_t with a live `inner` reader.
 * @pre @p buf covers @p len writable bytes for the wrapped reader.
 * @post On success, `calls` grew by one and `bytes` by @p len.
 * @post The wrapped reader's result is returned unchanged to the caller.
 *
 * @note Not thread-safe (unsynchronized counters; single-threaded tool).
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t cbs_priv_meter_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

/**
 * @brief Expose the registered sweep backends (the seam the HW leg extends).
 *
 * @details Returns the module's backend registry: `mem` first (the harness
 *          floor), then `rbkc-z9`. Entries are mutable because `setup` binds
 *          `read`/`read_ctx`/`backing_bytes` in place per (blob, size) pair.
 *
 * @param[out] out_count Receives the number of registered backends.
 *
 * @return cbs_backend_t* The registry array (never NULL).
 *
 * @pre @p out_count is non-NULL.
 * @post `*out_count` is the registry length (>= 2).
 *
 * @note Not thread-safe (the registry is written in place by `setup`).
 * @since 0.1.0
 */
RA8_PRIV cbs_backend_t* cbs_priv_backends(uint32_t* out_count);

/**
 * @brief Print one machine-parseable result row (`sweep-block key=value ...`).
 *
 * @details Emits every counter of @p r on one line plus derived MiB/s and
 *          ns/read, clamping a zero wall time to 1 ns so the derived rates
 *          never divide by zero. Rows with zero reads print nothing.
 *
 * @param[in] r Finished row to print (NULL is tolerated as a no-op).
 *
 * @return void.
 *
 * @pre Stdout is writable.
 * @pre @p r, when non-NULL, is a finished row (its counters are final).
 * @post One `sweep-block ...` line was printed for a non-empty row.
 * @post No row data is modified (pure reader).
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
RA8_PRIV void cbs_priv_print_row(const cbs_row_t* r);

/**
 * @brief Print one backend's sequential-scan summary table (leg a).
 *
 * @details Derives an estimated per-miss fill cost by subtracting the
 *          independently measured hit-path cost (the hot leg's ns/read at
 *          the same size) from the sequential wall time and dividing the
 *          remainder across the misses -- the Ch 23.2 method of separating
 *          per-request overhead from streaming cost.
 *
 * @param[in] rows    All finished rows.
 * @param[in] nrows   Number of rows.
 * @param[in] be      Backend name to summarise.
 * @param[in] blocks  The swept sizes, ascending.
 * @param[in] nblocks Number of swept sizes.
 *
 * @return void.
 *
 * @pre Rows for @p be exist for both legs at each size (partial rows skip).
 * @pre Stdout is writable.
 * @post One markdown table for @p be was printed.
 * @post No row data is modified.
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
RA8_PRIV void cbs_priv_print_seq_table(const cbs_row_t* rows,
                                       uint32_t         nrows,
                                       const char*      be,
                                       const uint32_t*  blocks,
                                       uint32_t         nblocks);

/**
 * @brief Print one backend's same-block re-read summary table (leg b).
 *
 * @details Shows the pure cache-hit path per block size; a flat ns/read
 *          column is the expected result (hit cost independent of the block
 *          size), with exactly one cold miss per row.
 *
 * @param[in] rows    All finished rows.
 * @param[in] nrows   Number of rows.
 * @param[in] be      Backend name to summarise.
 * @param[in] blocks  The swept sizes, ascending.
 * @param[in] nblocks Number of swept sizes.
 *
 * @return void.
 *
 * @pre Hot rows for @p be exist at each size (missing rows are skipped).
 * @pre Stdout is writable.
 * @post One markdown table for @p be was printed.
 * @post No row data is modified.
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
RA8_PRIV void cbs_priv_print_hot_table(const cbs_row_t* rows,
                                       uint32_t         nrows,
                                       const char*      be,
                                       const uint32_t*  blocks,
                                       uint32_t         nblocks);

/**
 * @brief Name the measured crossover and print the chunk-size recommendation.
 *
 * @details The knee is the smallest block size whose sequential throughput on
 *          the chunked (rbkc-z9) backend reaches ::k_cbs_knee_pct percent of
 *          the peak across all sizes -- i.e. where per-request overhead has
 *          stopped dominating (Memory Systems Ch 23.2.1). The verdict names
 *          the knee against the current 64 KiB `.rabook` default and states
 *          the host-measurement caveat for the SD hardware leg.
 *
 * @param[in] rows    All finished rows.
 * @param[in] nrows   Number of rows.
 * @param[in] blocks  The swept sizes, ascending.
 * @param[in] nblocks Number of swept sizes.
 *
 * @return void.
 *
 * @pre rbkc-z9 seq rows exist (the function prints nothing useful otherwise).
 * @pre @p blocks is sorted ascending.
 * @post The crossover paragraph was printed to stdout.
 * @post No row data is modified.
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
RA8_PRIV void cbs_priv_print_crossover(const cbs_row_t* rows,
                                       uint32_t         nrows,
                                       const uint32_t*  blocks,
                                       uint32_t         nblocks);
