/**
 * @file inc/sweep_block_internal.h
 * @brief Module-private seams shared by the #208 sweep translation units.
 *
 * @details
 * The `--sweep-block` mode is split across three translation units to keep
 * each under the maintainability line cap:
 *
 *  - `src/sweep_block.c`          -- the sweep core: the real ::ra8_vmem cache
 *    bundle, the timed drive loop, per-(backend, size) orchestration, and
 *    the `cb_sweep_block()` entry point.
 *  - `src/sweep_block_backends.c` -- the two in-tree ::cbs_backend_t
 *    implementations (`mem` and `rbkc-z9`), the byte-counting meter shim,
 *    the deterministic text filler, and the wall-clock helper.
 *  - `src/sweep_block_report.c`   -- the machine-parseable row printer and the
 *    human summary (per-leg tables + the measured knee / crossover verdict).
 *
 * This header carries the sweep geometry constants, the backend DIP seam
 * (::cbs_backend_t), the result-row type (::cbs_row_t), the counting meter
 * (::cbs_meter_t), and the `RA8_PRIV` declarations of every helper shared
 * across those translation units. Nothing here is part of the tool's public
 * surface: `src/cache_bench.c` consumes only `inc/sweep_block.h`.
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

#include "ra8_attributes.h"
#include "ra8_vsource.h"
#include "sweep_block.h"

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
  int (*setup)(cbs_backend_t*      be,
               ra8_vsource_read_fn payload_read,
               void*               payload_ctx,
               uint32_t            blob_bytes,
               uint32_t            block_bytes,
               cb_sweep_config_t*  config);
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

/** @brief Serializable pseudo-text generator cursor. */
typedef struct {
  uint64_t rng;            /**< PRNG state.                            */
  uint32_t word_index;     /**< Selected word or sentinel.             */
  uint32_t word_offset;    /**< Byte position in the selected word.    */
  uint32_t sentence_words; /**< Words emitted in the current sentence. */
  uint8_t  phase;          /**< Word/separator phase.                  */
} cbs_payload_state_t;

/** @brief Resettable exact pseudo-text byte source with bounded checkpoints. */
typedef struct {
  const void*         checkpoints;      /**< Caller-owned generator-state index.     */
  uint32_t            checkpoint_count; /**< Entries in the index.                   */
  uint64_t            cursor_offset;    /**< End offset cached for sequential reads. */
  cbs_payload_state_t cursor_state;     /**< State at @ref cursor_offset.            */
} cbs_payload_t;

/**
 * @brief Return exact workspace bytes required by the pseudo-text source index.
 * @details Sizes one serialized generator checkpoint per bounded regeneration span.
 * @return Exact checkpoint-index byte count.
 * @retval other Non-zero fixed payload-index requirement.
 * @pre Fixed payload and checkpoint geometries are internally consistent.
 * @pre `sizeof(cbs_payload_state_t)` fits in `size_t` multiplication.
 * @post The result covers all checkpoints including offset zero.
 * @post No storage or global state is modified.
 * @note Thread-safe: this is fixed-geometry arithmetic.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_payload_workspace_required(void);

/**
 * @brief Build the payload source index into caller-owned storage.
 * @details Serializes deterministic generator state at each bounded seek span
 *          and initializes the sequential cursor at offset zero.
 * @param[out] payload Payload binding to initialize.
 * @param[in,out] workspace Aligned caller-owned checkpoint storage.
 * @param[in] capacity Supplied workspace byte count.
 * @return Zero on success, otherwise one.
 * @retval 0 @p payload is ready for exact reads.
 * @retval 1 A binding, alignment, or capacity check failed.
 * @pre @p payload and @p workspace are non-NULL.
 * @pre @p workspace is aligned for ::cbs_payload_state_t.
 * @post On success, every checkpoint is initialized deterministically.
 * @post Workspace ownership remains with the caller.
 * @note Initialization is bounded by fixed payload geometry.
 * @since 0.1.0
 */
RA8_PRIV int priv_payload_init(cbs_payload_t* payload, void* workspace, size_t capacity);

/**
 * @brief Read exact historical pseudo-text bytes at any bounded offset.
 * @details Restores the nearest checkpoint when needed, regenerates at most one
 *          checkpoint span, and caches the resulting sequential cursor.
 * @param[in,out] ctx Bound ::cbs_payload_t.
 * @param[in] offset Logical payload offset.
 * @param[out] buffer Destination buffer.
 * @param[in] length Exact byte count.
 * @return Repository error code.
 * @retval k_ra8_ok The complete range was generated.
 * @retval k_ra8_err_null_ptr A required binding is NULL.
 * @retval k_ra8_err_out_of_range The requested range exceeds the fixed payload.
 * @pre @p buffer is writable for @p length bytes.
 * @pre @p ctx was initialized by ::priv_payload_init.
 * @post On success, @p buffer contains the deterministic historical bytes.
 * @post The cached cursor names the end of the completed read.
 * @note Distinct payload bindings may be read independently.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_payload_read(void* ctx, uint64_t offset, uint8_t* buffer, uint32_t length);

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
RA8_PRIV uint64_t priv_now_ns(void);

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
RA8_PRIV ra8_err_t priv_meter_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

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
RA8_PRIV cbs_backend_t* priv_backends(uint32_t* out_count);

/**
 * @brief Print one machine-parseable result row (`sweep-block key=value ...`).
 *
 * @details Emits every counter of @p r on one line plus derived MiB/s and
 *          ns/read, clamping a zero wall time to 1 ns so the derived rates
 *          never divide by zero. Rows with zero reads print nothing.
 *
 * @param[in] r Finished row to print (NULL is tolerated as a no-op).
 * @param[in,out] sink Report destination.
 *
 * @return 0 on success, or 1 when the sink rejects the row.
 * @retval 0 The row was empty or published completely.
 * @retval 1 The sink rejected a report fragment.
 *
 * @pre @p sink is bound and writable.
 * @pre @p r, when non-NULL, is a finished row (its counters are final).
 * @post One `sweep-block ...` line was printed for a non-empty row.
 * @post No row data is modified (pure reader).
 *
 * @note Not thread-safe: writes @p sink.
 * @since 0.1.0
 */
RA8_PRIV int priv_print_row(cb_sink_t* sink, const cbs_row_t* r);

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
 * @param[in,out] sink Report destination.
 *
 * @return 0 on success, or 1 when the sink rejects output.
 * @retval 0 The complete sequential table was published.
 * @retval 1 The sink rejected a table fragment.
 *
 * @pre Rows for @p be exist for both legs at each size (partial rows skip).
 * @pre @p sink is bound and writable.
 * @post One markdown table for @p be was printed.
 * @post No row data is modified.
 *
 * @note Not thread-safe: writes @p sink.
 * @since 0.1.0
 */
RA8_PRIV int priv_print_seq_table(cb_sink_t*       sink,
                                  const cbs_row_t* rows,
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
 * @param[in,out] sink Report destination.
 *
 * @return 0 on success, or 1 when the sink rejects output.
 * @retval 0 The complete hot-read table was published.
 * @retval 1 The sink rejected a table fragment.
 *
 * @pre Hot rows for @p be exist at each size (missing rows are skipped).
 * @pre @p sink is bound and writable.
 * @post One markdown table for @p be was printed.
 * @post No row data is modified.
 *
 * @note Not thread-safe: writes @p sink.
 * @since 0.1.0
 */
RA8_PRIV int priv_print_hot_table(cb_sink_t*       sink,
                                  const cbs_row_t* rows,
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
 * @param[in,out] sink Report destination.
 *
 * @return 0 on success, or 1 when the sink rejects output.
 * @retval 0 The crossover summary was published.
 * @retval 1 The sink rejected a summary fragment.
 *
 * @pre rbkc-z9 seq rows exist (the function prints nothing useful otherwise).
 * @pre @p blocks is sorted ascending.
 * @post The crossover paragraph was written to @p sink.
 * @post No row data is modified.
 *
 * @note Not thread-safe: writes @p sink.
 * @since 0.1.0
 */
RA8_PRIV int priv_print_crossover(cb_sink_t*       sink,
                                  const cbs_row_t* rows,
                                  uint32_t         nrows,
                                  const uint32_t*  blocks,
                                  uint32_t         nblocks);
