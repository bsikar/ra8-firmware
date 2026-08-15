/**
 * @file ra8_jof_audit.h
 * @brief No-heap, backing-agnostic structural audit for JOF atlases.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * This library-domain engine reads through ::ra8_jof_pread_fn and borrows
 * every record and decode buffer from its caller. The same code therefore
 * audits a host file, VFS stream, SD entry, or RAM image without libc file
 * APIs or allocation. Host tools are adapters to this contract rather than
 * owners of a second JOF implementation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_jof.h"

/**
 * @struct ra8_jof_audit_record_t
 * @brief One decoded tile's stored window and content evidence.
 * @since 0.1.0
 */
typedef struct ra8_jof_audit_record_t {
  uint32_t offset;       /**< Absolute stored-stream offset.         */
  uint32_t length;       /**< Stored-stream length.                  */
  uint32_t payload;      /**< Exact decoded payload bytes.           */
  uint32_t content_hash; /**< FNV-1a over decoded bytes.             */
  uint16_t width;        /**< Edge-clamped decoded width.            */
  uint16_t height;       /**< Edge-clamped decoded height.           */
  bool     uniform;      /**< All decoded bytes have the same value. */
} ra8_jof_audit_record_t;

/**
 * @struct ra8_jof_audit_requirements_t
 * @brief Exact caller-storage requirements derived from a parsed atlas.
 * @since 0.1.0
 */
typedef struct ra8_jof_audit_requirements_t {
  uint32_t record_count;  /**< Record entries required.              */
  uint32_t tile_bytes;    /**< Decoded tile-buffer bytes required.   */
  uint32_t scratch_bytes; /**< Stored-stream scratch (zero for raw). */
} ra8_jof_audit_requirements_t;

/**
 * @struct ra8_jof_audit_workspace_t
 * @brief Caller-owned buffers consumed by one audit.
 * @since 0.1.0
 */
typedef struct ra8_jof_audit_workspace_t {
  ra8_jof_audit_record_t* records;     /**< Tile record array.            */
  uint32_t                record_cap;  /**< Entries available at records. */
  uint8_t*                tile;        /**< Reusable decoded tile.        */
  uint32_t                tile_cap;    /**< Bytes available at tile.      */
  uint8_t*                scratch;     /**< Reusable compressed staging.  */
  uint32_t                scratch_cap; /**< Bytes available at scratch.   */
} ra8_jof_audit_workspace_t;

/**
 * @struct ra8_jof_audit_result_t
 * @brief Geometry plus anomaly counts emitted by a complete audit.
 * @since 0.1.0
 */
typedef struct ra8_jof_audit_result_t {
  ra8_jof_info_t info;                 /**< Parsed atlas geometry.                     */
  uint32_t       decoded_tiles;        /**< Tiles decoded and hashed.                  */
  uint32_t       coverage_errors;      /**< Gap/overlap/order/end mismatches.          */
  uint32_t       geometry_errors;      /**< Decoded size disagreed with tile geometry. */
  uint32_t       duplicate_candidates; /**< Matching non-uniform hash/size evidence.   */
} ra8_jof_audit_result_t;

/**
 * @brief Parse an atlas and report exact caller-storage requirements
 * @details Validates the JOF header/footer/index geometry through the injected
 * reader and derives the exact record, decoded-tile, and compressed-scratch
 * capacities needed by ::ra8_jof_audit.
 * @param[in]  pread       Positioned-read backend.
 * @param[in]  pread_ctx   Backend context.
 * @param[in]  total_size  Complete atlas byte length.
 * @param[out] out         Receives exact capacities.
 * @return Requirement-query status.
 * @retval k_ra8_ok Exact capacities were published.
 * @retval k_ra8_err_null_ptr A callback or output pointer is null.
 * @retval k_ra8_err_invalid_size Parsed geometry cannot fit bounded capacities.
 * @pre @p pread reads only from the immutable object described by @p
 * total_size.
 * @pre @p out is writable and does not alias the backing object.
 * @post No dynamic allocation or backend mutation occurs.
 * @post Failure does not publish a usable requirements record.
 * @note Thread-safe when the injected reader and backing are thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_jof_audit_requirements(ra8_jof_pread_fn              pread,
                                                   void*                         pread_ctx,
                                                   uint64_t                      total_size,
                                                   ra8_jof_audit_requirements_t* out);

/**
 * @brief Audit coverage, geometry and duplicate-content evidence in bounded RAM
 * @details Parses and decodes every indexed tile through the shared JOF reader,
 * verifies exact stored-stream coverage and decoded dimensions, and records
 * diagnostic duplicate fingerprints without treating a hash match as proof.
 * @param[in]     pread      Positioned-read backend.
 * @param[in]     pread_ctx  Backend context.
 * @param[in]     total_size Complete atlas byte length.
 * @param[in,out] workspace  Exact-or-larger caller-owned buffers.
 * @param[out]    out        Receives parsed geometry and anomaly counts.
 * @return Audit status.
 * @retval k_ra8_ok The atlas passed every structural audit.
 * @retval k_ra8_err_validation_failed Coverage or geometry is inconsistent.
 * @retval k_ra8_err_null_ptr A callback, output, or required workspace is null.
 * @retval k_ra8_err_invalid_size A caller workspace is too small.
 * @retval k_ra8_err_invalid_arg Writable workspace or result spans overlap.
 * @pre Capacities satisfy ::ra8_jof_audit_requirements for the same backing.
 * @pre Workspace spans and @p out remain exclusively owned during the call;
 *      overlap is rejected before any workspace or output byte is changed.
 * @post The function performs no allocation and retains no caller pointer.
 * @post Success reports one decoded record for every parsed tile.
 * @post A complete structural audit publishes diagnostics on success or
 *       validation failure; callback/decode failures preserve @p out.
 * @note Matching tile hashes increment
 *       ::ra8_jof_audit_result_t::duplicate_candidates for diagnostics,
 *       but never reject an atlas: repeated image tiles are valid and a hash
 *       match alone is not proof that two decoded tiles are identical.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_jof_audit(ra8_jof_pread_fn           pread,
                                      void*                      pread_ctx,
                                      uint64_t                   total_size,
                                      ra8_jof_audit_workspace_t* workspace,
                                      ra8_jof_audit_result_t*    out);

#ifdef __cplusplus
}
#endif
