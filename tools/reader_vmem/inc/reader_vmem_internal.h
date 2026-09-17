/**
 * @file inc/reader_vmem_internal.h
 * @brief Private host sink and caller-owned workspace contracts for reader_vmem
 *
 * @details
 * The workload remains independent of hosted streams and allocation. The
 * standalone composition root supplies one explicitly bounded RAM workspace,
 * while trace publication is isolated behind a descriptor-backed host edge.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_vmem.h"

#ifndef READER_VMEM_MAX_BUDGET
/** @brief Maximum frames reserved by the standalone composition root. */
#define READER_VMEM_MAX_BUDGET (1024U)
#endif

/** @brief Fixed composition and host-path limits. */
typedef enum : uint32_t {
  k_rv_frame_bytes   = 4096U, /**< Bytes per cache frame.   */
  k_rv_bucket_count  = 1024U, /**< Cache hash-bucket count. */
  k_rv_host_path_cap = 4096U, /**< Hosted path capacity.    */
  k_rv_host_name_cap = 256U,  /**< Hosted leaf capacity.    */
} rv_internal_limit_t;

/** @brief Exact byte regions required for one cache budget. */
typedef struct {
  size_t frame_offset;  /**< Frame-pool region offset.   */
  size_t frame_bytes;   /**< Frame-pool region size.     */
  size_t meta_offset;   /**< Metadata region offset.     */
  size_t meta_bytes;    /**< Metadata region size.       */
  size_t key_offset;    /**< Key region offset.          */
  size_t key_bytes;     /**< Key region size.            */
  size_t bucket_offset; /**< Hash-bucket region offset.  */
  size_t bucket_bytes;  /**< Hash-bucket region size.    */
  size_t total_bytes;   /**< Exact workspace high-water. */
} rv_workspace_need_t;

/** @brief Typed views carved from a caller's workspace. */
typedef struct {
  uint8_t*          frame_mem; /**< Page-frame pool.         */
  ra8_vmem_frame_t* meta;      /**< Per-frame metadata.      */
  ra8_vmem_key_t*   keys;      /**< Per-frame key storage.   */
  int32_t*          buckets;   /**< Hash-bucket index heads. */
} rv_workspace_t;

/** @brief Caller-owned atomic trace-publication state. */
typedef struct {
  char     final_name[k_rv_host_name_cap]; /**< Destination leaf.        */
  char     temp_name[k_rv_host_name_cap];  /**< Temporary leaf.          */
  int      directory_fd;                   /**< Parent directory handle. */
  int      trace_fd;                       /**< Temporary trace handle.  */
  uint64_t offset;                         /**< Next trace byte offset.  */
  bool     temp_exists;                    /**< Temp cleanup guard.      */
  bool     io_failed;                      /**< Sticky exact-I/O fault.  */
} rv_trace_t;

/** @brief Query exact split-region requirements for one frame budget. */
[[nodiscard]] RA8_PRIV bool priv_rv_workspace_require(uint32_t budget, rv_workspace_need_t* need);

/** @brief Bind typed cache views into an aligned caller-owned backing. */
[[nodiscard]] RA8_PRIV bool priv_rv_workspace_bind(void*                      backing,
                                                   size_t                     backing_bytes,
                                                   const rv_workspace_need_t* need,
                                                   rv_workspace_t*            workspace);

/** @brief Begin one unpublished same-directory trace transaction. */
[[nodiscard]] RA8_PRIV bool priv_rv_trace_begin(const char* path, rv_trace_t* trace);

/** @brief Append one exact object/frame reference to a trace transaction. */
[[nodiscard]] RA8_PRIV bool
priv_rv_trace_reference(rv_trace_t* trace, uint32_t object_id, uint32_t frame);

/** @brief Durably and atomically publish a complete trace. */
[[nodiscard]] RA8_PRIV bool priv_rv_trace_commit(rv_trace_t* trace);

/**
 * @brief Close and unlink an unpublished trace.
 * @details Idempotently releases every partially acquired transaction resource.
 * @param[in,out] trace Trace state returned by the begin operation.
 * @pre @p trace is non-null and may be partially initialized.
 * @pre No further append or commit uses the same state concurrently.
 * @post Every owned descriptor is closed.
 * @post Any owned private leaf is removed; the final trace is unchanged.
 * @note Not thread-safe through one trace state.
 * @since 0.1.0
 */
RA8_PRIV void priv_rv_trace_abort(rv_trace_t* trace);

/**
 * @brief Write one best-effort diagnostic fragment to standard error.
 * @details Retries interrupted and short raw-descriptor writes without stdio.
 * @param[in] text NUL-terminated fragment; null is ignored.
 * @pre @p text is null or NUL-terminated.
 * @pre Standard error may accept or reject output.
 * @post The complete non-null fragment was attempted.
 * @post No cache, trace, or filesystem state changed.
 * @note Fragments can interleave with another process.
 * @since 0.1.0
 */
RA8_PRIV void priv_rv_diag(const char* text);

/**
 * @brief Write one unsigned decimal diagnostic without stdio.
 * @details Converts through a fixed local buffer before one bounded fragment write.
 * @param[in] value Unsigned value to render in base ten.
 * @pre The fixed digit buffer covers every `uint64_t` value.
 * @pre Standard error may accept or reject output.
 * @post The complete decimal spelling was attempted.
 * @post No cache, trace, or filesystem state changed.
 * @note Fragments can interleave with another process.
 * @since 0.1.0
 */
RA8_PRIV void priv_rv_diag_u64(uint64_t value);
