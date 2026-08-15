/**
 * @file ra8_fmt_stream.h
 * @brief Caller-workspace I/O contracts for portable format-tool engines.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Format engines depend only on positioned input, append-only artifact output,
 * durable transaction control, and an injected text sink. Host file
 * descriptors, firmware VFS streams, and in-memory tests bind the same
 * contracts.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_audit.h"

/** @brief Revalidate one positioned source against its captured immutable view.
 */
typedef ra8_err_t (*ra8_fmt_source_validate_fn)(void *ctx,
                                                uint64_t expected_size);

/** @brief Immutable, randomly readable input object. */
typedef struct {
  ra8_jof_pread_fn read_at;            /**< Positioned-read callback.    */
  ra8_fmt_source_validate_fn validate; /**< Optional stability callback. */
  void *ctx;                           /**< Backend-owned context.       */
  uint64_t size;                       /**< Exact object byte length.    */
} ra8_fmt_source_t;

/** @brief Append text or binary bytes to a bounded backend. */
typedef ra8_err_t (*ra8_fmt_sink_write_fn)(void *ctx, const uint8_t *bytes,
                                           size_t len);

/** @brief Injected append-only sink. */
typedef struct {
  ra8_fmt_sink_write_fn write; /**< Exact append callback. */
  void *ctx;                   /**< Backend-owned context. */
} ra8_fmt_sink_t;

/** @brief Seal an exact scratch artifact for immutable positioned reads. */
typedef ra8_err_t (*ra8_fmt_spool_seal_fn)(void *ctx, uint64_t expected_size);

/** @brief Caller-owned scratch artifact with append, seal, and read seams. */
typedef struct {
  ra8_jof_pread_fn read_at;     /**< Positioned reader after seal. */
  ra8_fmt_sink_write_fn append; /**< Append before seal.           */
  ra8_fmt_spool_seal_fn seal;   /**< Seal exact produced bytes.    */
  void *ctx;                    /**< Backend-owned state.          */
} ra8_fmt_spool_t;

/** @brief Durable artifact-transaction operations. */
typedef struct {
  ra8_fmt_sink_write_fn append;   /**< Append artifact bytes.       */
  ra8_err_t (*commit)(void *ctx); /**< Sync and atomically install. */
  void (*abort)(void *ctx);       /**< Discard owned staging data.  */
} ra8_fmt_transaction_ops_t;

/** @brief One caller-owned artifact transaction. */
typedef struct {
  const ra8_fmt_transaction_ops_t *ops; /**< Transaction implementation. */
  void *ctx;                            /**< Backend-owned state.        */
} ra8_fmt_transaction_t;

/** @brief Source geometry and exact arenas required by one JOF conversion. */
typedef struct {
  uint16_t width;           /**< Declared source width in pixels.       */
  uint16_t height;          /**< Declared source height in pixels.      */
  uint16_t tile_width;      /**< Width selected for emitted tiles.      */
  uint16_t tile_height;     /**< Height selected for emitted tiles.     */
  uint32_t work_bytes;      /**< Exact streaming-producer arena bytes.  */
  uint32_t webp_work_bytes; /**< Exact WebP whole-frame bytes, or zero. */
} ra8_fmt_jof_convert_requirements_t;

/** @brief Caller-owned arenas supplied to the streaming JOF converter. */
typedef struct {
  uint8_t *work;          /**< Streaming producer arena.   */
  uint32_t work_cap;      /**< Bytes available at @p work. */
  uint8_t *webp_work;     /**< Optional WebP decode arena. */
  uint32_t webp_work_cap; /**< Bytes at @p webp_work.      */
} ra8_fmt_jof_convert_workspace_t;

/** @brief Exact phase-reused storage requirements for JOF verification. */
typedef struct {
  uint16_t width;                /**< Source width in pixels.             */
  uint16_t height;               /**< Source height in pixels.            */
  uint8_t bpp;                   /**< Decoder output bytes per pixel.     */
  uint16_t band_height;          /**< Subject tile height under test.     */
  uint32_t reference_work_bytes; /**< One-row reference producer arena.   */
  uint32_t banded_work_bytes;    /**< Banded subject producer arena.      */
  uint32_t webp_work_bytes;      /**< Whole-frame WebP arena, or zero.    */
  uint32_t band_tile_bytes;      /**< Largest decoded subject tile.       */
  uint32_t scratch_bytes;        /**< Largest stored-tile staging buffer. */
  uint32_t row_bytes;            /**< One decoded reference row.          */
} ra8_fmt_jof_verify_requirements_t;

/**
 * @brief Caller-owned phase-overlaid arenas for one bounded JOF verification.
 * @details Producer and comparison spans may overlap because both producer
 * passes finish and both spools seal before tile comparison starts.
 */
typedef struct {
  uint8_t *work;          /**< Reused producer work arena.      */
  uint32_t work_cap;      /**< Bytes available at @p work.      */
  uint8_t *webp_work;     /**< Optional WebP whole-frame arena. */
  uint32_t webp_work_cap; /**< Bytes at @p webp_work.           */
  uint8_t *band_tile;     /**< Decoded banded-subject tile.     */
  uint32_t band_tile_cap; /**< Bytes at @p band_tile.           */
  uint8_t *scratch;       /**< Reusable stored-tile staging.    */
  uint32_t scratch_cap;   /**< Bytes at @p scratch.             */
  uint8_t *row;           /**< Decoded one-row reference tile.  */
  uint32_t row_cap;       /**< Bytes at @p row.                 */
} ra8_fmt_jof_verify_workspace_t;

/**
 * @struct ra8_fmt_jof_inspect_workspace_t
 * @brief Exact caller-owned storage used by streaming JOF inspection.
 * @since 0.1.0
 */
typedef struct {
  ra8_jof_audit_record_t *records; /**< Audit record array.          */
  uint32_t record_cap;             /**< Record entries available.    */
  uint8_t *tile;                   /**< Reusable decoded tile.       */
  uint32_t tile_cap;               /**< Decoded tile capacity.       */
  uint8_t *scratch;                /**< Compressed tile staging.     */
  uint32_t scratch_cap;            /**< Compressed staging capacity. */
} ra8_fmt_jof_inspect_workspace_t;

/** @brief Caller-owned storage for strict streamed RBKC/RABOOK1 inspection. */
typedef struct {
  uint64_t *table;         /**< Decoded RBKC offset entries.    */
  uint32_t table_cap;      /**< Entries available at @p table. */
  uint8_t *compressed;     /**< One compressed chunk.           */
  uint32_t compressed_cap; /**< Compressed chunk capacity.      */
  uint8_t *chunk;          /**< One inflated chunk.             */
  uint32_t chunk_cap;      /**< Inflated chunk capacity.        */
  uint8_t *scratch;        /**< Strict validator workspace.     */
  uint32_t scratch_cap;    /**< Validator workspace capacity.   */
} ra8_fmt_rabook_inspect_workspace_t;

/**
 * @brief Inspect one JOF source through callbacks and caller storage.
 * @param[in]     source    Immutable JOF input.
 * @param[in]     verbose   Emit header/footer hex and the full tile table.
 * @param[in,out] workspace Exact-or-larger audit buffers.
 * @param[in,out] report    Human-readable text sink.
 * @return Audit or sink status.
 * @post No allocation occurs and no backend pointer is retained.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fmt_jof_inspect_stream(const ra8_fmt_source_t *source, bool verbose,
                           ra8_fmt_jof_inspect_workspace_t *workspace,
                           const ra8_fmt_sink_t *report);

/**
 * @brief Strictly inspect one streamed RBKC container and its RABOOK1 payload.
 * @param[in] source Immutable positioned container source.
 * @param[in] verbose Emit the bounded chunk inventory.
 * @param[in,out] workspace Exact-or-larger table and validator buffers.
 * @param[in,out] report Human-readable injected text sink.
 * @return Container, decompression, inner-validation, or sink status.
 * @pre All workspace spans are disjoint and remain live for the call.
 * @post Every compressed stream and every inner reference has been validated
 * on success; no source pointer or workspace pointer is retained.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fmt_rabook_inspect_stream(const ra8_fmt_source_t *source, bool verbose,
                              ra8_fmt_rabook_inspect_workspace_t *workspace,
                              const ra8_fmt_sink_t *report);

/**
 * @brief Derive source geometry and exact JOF conversion workspace needs.
 * @param[in]  source Immutable JPEG, PNG, or WebP input.
 * @param[out] out    Receives exact arena requirements and chosen band
 * geometry.
 * @return Probe or sizing status.
 * @retval k_ra8_ok Requirements are complete and non-zero where applicable.
 * @retval k_ra8_err_not_supported Source is not a supported baseline image.
 * @retval k_ra8_err_invalid_size Geometry, source length, or arena sizing is
 * invalid.
 * @pre @p source provides a stable positioned-read callback for `source->size`
 * bytes.
 * @pre @p out is writable.
 * @post Success initializes every field of @p out.
 * @post No source byte or backend position is changed.
 * @note Thread-safe for independent source contexts and caller workspaces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fmt_jof_convert_requirements(const ra8_fmt_source_t *source,
                                 ra8_fmt_jof_convert_requirements_t *out);

/**
 * @brief Stream one encoded image into a durably published JOF artifact.
 * @param[in]     source      Immutable JPEG, PNG, or WebP input.
 * @param[in]     requirements Exact requirements derived for @p source.
 * @param[in,out] workspace   Caller-owned exact-or-larger producer arenas.
 * @param[in,out] transaction Active append-only durable output transaction.
 * @param[in,out] report      Human-readable report sink.
 * @param[in]     output_name Stable destination spelling for the report.
 * @return Producer, transaction, or report status.
 * @retval k_ra8_ok Complete atlas was durably committed and reported.
 * @retval k_ra8_err_invalid_size A supplied arena is below its exact
 * requirement.
 * @pre @p requirements came from ::ra8_fmt_jof_convert_requirements for @p
 * source.
 * @pre @p transaction owns an unpublished sibling stage.
 * @post Success commits exactly one complete JOF artifact.
 * @post Any pre-commit failure aborts the stage and preserves the prior
 * destination.
 * @note Performs no allocation and retains no caller pointer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_jof_convert_stream(
    const ra8_fmt_source_t *source,
    const ra8_fmt_jof_convert_requirements_t *requirements,
    ra8_fmt_jof_convert_workspace_t *workspace,
    ra8_fmt_transaction_t *transaction, const ra8_fmt_sink_t *report,
    const char *output_name);

/**
 * @brief Derive exact producer and comparison storage for bounded JOF
 * verification.
 * @param[in] source Stable JPEG, PNG, or WebP source.
 * @param[out] out Receives exact phase requirements.
 * @return Probe or sizing status.
 * @pre @p source provides immutable positioned reads for its declared size.
 * @post Success initializes every requirement field without creating a spool.
 * @note Source-kind parsing is bounded by source size and caller storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fmt_jof_verify_requirements(const ra8_fmt_source_t *source,
                                ra8_fmt_jof_verify_requirements_t *out);

/**
 * @brief Verify banded JOF pixels against an independently decoded row
 * reference.
 * @details Encodes two independent source contexts into sealed scratch atlases:
 * one-row reference tiles remove inter-row band accumulation, while 256-row
 * subject tiles exercise the production band path. Tiles are decoded and
 * compared one row at a time; an optional PPM is transactionally published.
 * @param[in] reference_source First immutable encoded-source context.
 * @param[in] banded_source Independent context for the same encoded object.
 * @param[in] requirements Exact requirements derived for the source.
 * @param[in,out] workspace Caller-owned phase-reused arenas.
 * @param[in,out] reference_spool Empty scratch artifact for reference JOF
 * bytes.
 * @param[in,out] banded_spool Empty scratch artifact for subject JOF bytes.
 * @param[in,out] dump Optional durable PPM transaction; null disables the dump.
 * @param[in] dump_name Destination spelling used in the legacy report line.
 * @param[in,out] report Human-readable raw byte sink.
 * @return Exact-verdict, validation, producer, spool, or source status.
 * @retval k_ra8_ok Every compared byte matched.
 * @retval k_ra8_err_validation_failed At least one raster byte differed.
 * @pre Source contexts are distinct, stable views of one encoded object.
 * @pre Spools are empty and their callbacks remain bound for the call.
 * @pre Workspace capacities meet @p requirements; phase-overlap is permitted.
 * @post Success sealed both spools and compared the complete decoded raster.
 * @post An enabled dump is committed on successful reconstruction even when
 * pixels differ, matching the legacy diagnostic-dump behavior.
 * @post Any dump write failure aborts its stage and does not change the
 * verdict.
 * @note No dynamic storage, formatted stream, hidden singleton, or whole raster
 * is used.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_jof_verify_stream(
    const ra8_fmt_source_t *reference_source,
    const ra8_fmt_source_t *banded_source,
    const ra8_fmt_jof_verify_requirements_t *requirements,
    ra8_fmt_jof_verify_workspace_t *workspace, ra8_fmt_spool_t *reference_spool,
    ra8_fmt_spool_t *banded_spool, ra8_fmt_transaction_t *dump,
    const char *dump_name, const ra8_fmt_sink_t *report);
