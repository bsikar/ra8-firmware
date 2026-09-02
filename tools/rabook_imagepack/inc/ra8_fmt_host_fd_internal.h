/**
 * @file ra8_fmt_host_fd_internal.h
 * @brief Raw file-descriptor adapters for the portable format-tool contracts.
 * @details Defines caller-owned source, sink, and transaction state; raw host
 * descriptors remain confined to this composition boundary.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"

/** @brief Bounded host path and transaction-name capacities. */
typedef enum : uint32_t {
  k_ra8_fmt_host_path_cap = 1024U, /**< Parent-path storage including NUL. */
  k_ra8_fmt_host_name_cap = 256U,  /**< One leaf name including NUL.       */
} ra8_fmt_host_limit_t;

/** @brief Captured regular-file identity and mutation evidence. */
typedef struct {
  uint64_t device;     /**< Filesystem device identifier.          */
  uint64_t inode;      /**< File object identifier.                */
  uint64_t size;       /**< Captured regular-file extent.          */
  int64_t  mtime_sec;  /**< Modification timestamp seconds.        */
  int64_t  mtime_nsec; /**< Modification timestamp nanoseconds.    */
  int64_t  ctime_sec;  /**< Metadata-change timestamp seconds.     */
  int64_t  ctime_nsec; /**< Metadata-change timestamp nanoseconds. */
} ra8_fmt_host_snapshot_t;

/** @brief Open raw-fd source and its portable view. */
typedef struct {
  int                     fd;       /**< Owned descriptor, or -1 when closed. */
  ra8_fmt_source_t        source;   /**< Portable positioned-read view.       */
  ra8_fmt_host_snapshot_t snapshot; /**< Immutable-open evidence.             */
} ra8_fmt_host_source_t;

/** @brief Append sink backed by a caller-owned descriptor. */
typedef struct {
  int fd; /**< Borrowed writable descriptor. */
} ra8_fmt_host_fd_sink_t;

/** @brief Caller-owned state for one sibling-file transaction. */
typedef struct {
  int      parent_fd;                           /**< Owned parent directory.   */
  int      stage_fd;                            /**< Owned staging descriptor. */
  uint64_t position;                            /**< Bytes appended so far.    */
  char     stage_name[k_ra8_fmt_host_name_cap]; /**< Staging leaf name.        */
  char     final_name[k_ra8_fmt_host_name_cap]; /**< Destination leaf name.    */
  bool     stage_exists;                        /**< Stage still needs unlink. */
  bool     active;                              /**< Transaction is usable.    */
} ra8_fmt_host_transaction_t;

/** @brief Open a bounded, regular, non-symlink input object. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_fmt_host_source_open(const char* path, uint64_t max_size, ra8_fmt_host_source_t* out);

/**
 * @brief Confirm two opens captured the same unchanged regular-file object.
 * @param[in] first First open source context.
 * @param[in] second Independent second open source context.
 * @return Whether identity, extent, and mutation timestamps match.
 * @pre Both sources came from successful host source-open calls.
 * @post Neither descriptor nor snapshot is changed.
 * @note Pure over captured evidence.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_fmt_host_sources_same(const ra8_fmt_host_source_t* first,
                                                       const ra8_fmt_host_source_t* second);

/**
 * @brief Revalidate one open descriptor against its captured snapshot.
 * @param[in] source Open source wrapper.
 * @return Canonical stability status.
 * @retval k_ra8_ok Descriptor still names the captured regular file.
 * @retval k_ra8_err_validation_failed Identity, size, or timestamps changed.
 * @retval k_ra8_fail Host metadata query failed.
 * @pre @p source came from a successful source-open call.
 * @post No descriptor position or captured field changes.
 * @note Detects in-place concurrent mutation between verifier phase boundaries.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_fmt_host_source_unchanged(const ra8_fmt_host_source_t* source);

/**
 * @brief Close an open host source; safe after failed open.
 * @details Releases only the raw descriptor and marks the host wrapper closed.
 * @param[in,out] source Host source state, nullable for cleanup convenience.
 * @pre @p source is null or was initialized by the source-open operation.
 * @pre No positioned read uses the same source concurrently.
 * @post Any owned descriptor is closed and set to -1.
 * @post A null or already-closed source is unchanged.
 * @note Idempotent for sequential cleanup calls.
 * @since 0.1.0
 */
RA8_PRIV void priv_fmt_host_source_close(ra8_fmt_host_source_t* source);

/** @brief Obtain the exact-write portable sink for a raw descriptor. */
RA8_PRIV [[nodiscard]] ra8_fmt_sink_t priv_fmt_host_fd_sink(ra8_fmt_host_fd_sink_t* state);

/**
 * @brief Adapt a logging byte to an injected raw-fd sink.
 * @details Bridges the core logger's no-status byte callback to the same
 * exact-write descriptor adapter used by portable reports.
 * @param[in,out] ctx Bound ::ra8_fmt_host_fd_sink_t.
 * @param[in] byte One log byte.
 * @pre @p ctx points at a writable raw-fd sink for the callback lifetime.
 * @pre The bound descriptor remains open during this call.
 * @post The byte was offered exactly once; host write failure is intentionally dropped.
 * @post No descriptor ownership or caller state changed.
 * @note Matches the no-status byte-sink contract of `ra8_log_set_byte_sink()`.
 * @since 0.1.0
 */
RA8_PRIV void priv_fmt_host_log_byte(void* ctx, uint8_t byte);

/** @brief Begin a sibling-temp durable replacement transaction. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fmt_host_transaction_begin(const char*                 path,
                                                                 ra8_fmt_host_transaction_t* state,
                                                                 ra8_fmt_transaction_t*      out);
