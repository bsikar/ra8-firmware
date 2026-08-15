/**
 * @file cache_bench_host.h
 * @brief POSIX raw-descriptor composition bindings for cache_bench.
 * @details Publishes the host-only adapters that bind portable benchmark I/O
 *          seams to borrowed or explicitly owned POSIX descriptors.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "cache_bench_io.h"

typedef enum : uint16_t {
  k_cb_host_path_capacity = 4096U, /**< Bounded host path copy. */
} cb_host_limit_t;

/** @brief Borrowed raw descriptor source binding. */
typedef struct {
  int      fd;   /**< Open read-only descriptor. */
  uint64_t size; /**< Size snapshot.             */
} cb_host_source_t;

/** @brief Output transaction or borrowed standard descriptor. */
typedef struct {
  int  fd;                                   /**< Writable descriptor.                  */
  int  dir_fd;                               /**< Parent descriptor for durable rename. */
  bool transactional;                        /**< True when temporary sibling is live.  */
  char destination[k_cb_host_path_capacity]; /**< Final path.                           */
  char temporary[k_cb_host_path_capacity];   /**< Sibling temporary path.               */
} cb_host_output_t;

/** @brief Unlinked raw-descriptor scratch binding. */
typedef struct {
  int fd; /**< Open temporary descriptor. */
} cb_host_scratch_t;

/**
 * @brief Bind the process standard-output and standard-error descriptor sinks.
 * @details Publishes non-owning sink callbacks over descriptors one and two.
 * @param[out] output Standard-output sink; NULL is accepted.
 * @param[out] error Standard-error sink; NULL is accepted.
 * @pre Each non-NULL output pointer is writable.
 * @pre The process standard descriptors have their conventional integer values.
 * @post Each non-NULL output receives a complete sink binding.
 * @post Descriptor ownership remains with the process runtime.
 * @note The bindings share file-local descriptor integers but distinct values.
 * @since 0.1.0
 */
void cb_host_standard_sinks(cb_sink_t* output, cb_sink_t* error);

/**
 * @brief Open a captured-trace path and publish its source seam.
 * @details Opens a non-symlink regular file read-only and snapshots its size for
 *          mutation checks on every injected read.
 * @param[in] path NUL-terminated host path.
 * @param[out] binding Receives the owned descriptor binding.
 * @param[out] source Receives the borrowed source seam.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The source is open and bound.
 * @retval k_cb_io_fault An argument, open, type, or metadata check failed.
 * @pre All pointers are non-NULL.
 * @pre @p path names a regular file directly, not a final symlink.
 * @post On success, @p binding owns one descriptor until close.
 * @post On failure, no descriptor remains owned by @p binding.
 * @note The source callback borrows @p binding and must not outlive it.
 * @since 0.1.0
 */
cb_io_status_t
cb_host_source_open(const char* path, cb_host_source_t* binding, cb_source_t* source);

/**
 * @brief Close a source binding.
 * @details Releases the owned descriptor and marks the binding closed.
 * @param[in,out] binding Open host source binding.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The descriptor closed successfully.
 * @retval k_cb_io_fault The binding was invalid/closed or close failed.
 * @pre @p binding is non-NULL and was initialized by ::cb_host_source_open.
 * @pre No source read is in flight through @p binding.
 * @post `binding->fd` is negative after the close attempt.
 * @post No descriptor ownership remains with @p binding.
 * @note A host close error is preserved in the return status.
 * @since 0.1.0
 */
cb_io_status_t cb_host_source_close(cb_host_source_t* binding);

/**
 * @brief Open a sibling output transaction, or bind descriptor one for NULL.
 * @details Creates a bounded exclusive temporary sibling for pathname output;
 *          standard output remains a borrowed non-transactional binding.
 * @param[in] path Destination path, or NULL for standard output.
 * @param[out] binding Receives transaction state.
 * @param[out] sink Receives the writable sink seam.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The sink is ready for publication.
 * @retval k_cb_io_capacity A path component exceeds fixed storage.
 * @retval k_cb_io_fault An argument or host operation failed.
 * @pre @p binding and @p sink are non-NULL.
 * @pre A non-NULL @p path is NUL-terminated.
 * @post On success, commit or abort must finish a transactional binding.
 * @post The destination is unchanged until commit.
 * @note Temporary-name retries are bounded by ::k_cb_host_retry_limit.
 * @since 0.1.0
 */
cb_io_status_t cb_host_output_open(const char* path, cb_host_output_t* binding, cb_sink_t* sink);

/**
 * @brief Flush and atomically publish a transactional output.
 * @details Syncs and closes the temporary file, renames it over the destination,
 *          then syncs the parent directory.
 * @param[in,out] binding Open output transaction or borrowed standard output.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok Publication completed or no transaction was required.
 * @retval k_cb_io_fault The binding or a durability operation failed.
 * @pre @p binding is non-NULL and initialized by ::cb_host_output_open.
 * @pre No sink write is in flight through @p binding.
 * @post A successful transaction has no live temporary sibling.
 * @post On failure after open, the temporary sibling is unlinked when possible.
 * @note Standard output is neither closed nor synchronized by this function.
 * @since 0.1.0
 */
cb_io_status_t cb_host_output_commit(cb_host_output_t* binding);

/**
 * @brief Abandon a temporary sibling while preserving the destination.
 * @details Closes owned descriptors, unlinks a live sibling, and clears the
 *          transaction marker; NULL and repeated calls are safe.
 * @param[in,out] binding Transaction to abandon; NULL is accepted.
 * @pre @p binding is NULL or initialized output state.
 * @pre No sink write is in flight through @p binding.
 * @post No owned descriptor or live temporary sibling remains in the binding.
 * @post The destination pathname is not replaced by this function.
 * @note Host cleanup failures are intentionally best-effort in the void API.
 * @since 0.1.0
 */
void cb_host_output_abort(cb_host_output_t* binding);

/**
 * @brief Create an unlinked raw-descriptor scratch transaction.
 * @details Creates a host temporary file, immediately unlinks its pathname,
 *          and publishes exact positional read/write callbacks.
 * @param[out] binding Receives the owned scratch descriptor.
 * @param[out] scratch Receives the injected scratch seam.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The unlinked transaction is open and bound.
 * @retval k_cb_io_fault An argument, create, or unlink operation failed.
 * @pre @p binding and @p scratch are non-NULL and writable.
 * @pre The host temporary directory permits secure file creation.
 * @post On success, no pathname refers to the open scratch file.
 * @post On failure, no descriptor remains owned by @p binding.
 * @note Scratch bytes live until ::cb_host_scratch_close.
 * @since 0.1.0
 */
cb_io_status_t cb_host_scratch_open(cb_host_scratch_t* binding, cb_scratch_t* scratch);

/**
 * @brief Close the scratch transaction.
 * @details Releases the sole descriptor backing the already-unlinked file.
 * @param[in,out] binding Open scratch descriptor binding.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok The descriptor closed successfully.
 * @retval k_cb_io_fault The binding was invalid/closed or close failed.
 * @pre @p binding is non-NULL and initialized by ::cb_host_scratch_open.
 * @pre No scratch callback is in flight.
 * @post `binding->fd` is negative after the close attempt.
 * @post The host reclaims the unlinked file when the descriptor closes.
 * @note No pathname cleanup is required.
 * @since 0.1.0
 */
cb_io_status_t cb_host_scratch_close(cb_host_scratch_t* binding);
