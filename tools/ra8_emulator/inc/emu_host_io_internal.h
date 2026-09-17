/**
 * @file emu_host_io_internal.h
 * @brief Bounded raw-descriptor I/O seam for the RA8 emulator.
 * @details Defines the private result, operation-injection, exact-transfer,
 * formatting, file-open, and sibling-transaction contracts used by the host tool.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "ra8_attributes.h"

/** @brief Result status for an emulator host-I/O operation. */
typedef enum : uint8_t {
  k_emu_io_ok = 0U,   /**< The complete operation succeeded.                   */
  k_emu_io_invalid,   /**< A pointer, descriptor, size, or path was invalid.   */
  k_emu_io_truncated, /**< Formatted text exceeded the bounded scratch buffer. */
  k_emu_io_eof,       /**< Input ended before the requested byte count.        */
  k_emu_io_error,     /**< A host syscall failed; os_error carries errno.      */
} emu_io_status_t;

/** @brief Complete, caller-visible result of a raw host-I/O operation. */
typedef struct {
  emu_io_status_t status;      /**< Semantic completion status.                   */
  size_t          transferred; /**< Bytes transferred before completion/failure.  */
  int             os_error;    /**< Captured errno for k_emu_io_error, else zero. */
} emu_io_result_t;

/** @brief Injectable raw operations used by the exact-transfer loops. */
typedef struct {
  ssize_t (*read_fn)(int fd, void* buf, size_t count);                    /**< read().   */
  ssize_t (*write_fn)(int fd, const void* buf, size_t count);             /**< write().  */
  ssize_t (*pread_fn)(int fd, void* buf, size_t count, off_t offset);     /**< pread().  */
  ssize_t (*pwrite_fn)(int fd, const void* buf, size_t count, off_t off); /**< pwrite(). */
} emu_io_ops_t;

/** @brief Raw descriptor plus its stat-derived byte length. */
typedef struct {
  int     fd;   /**< Owned descriptor, or -1 when closed. */
  int64_t size; /**< Non-negative regular-file length.    */
} emu_io_file_t;

/** @brief Sibling temporary file used for failure-atomic publication. */
typedef struct {
  int  fd;         /**< Owned temporary descriptor, or -1 when inactive.  */
  char final[512]; /**< Final publication path.                           */
  char temp[544];  /**< Sibling mkstemp template / active temporary path. */
} emu_io_txn_t;

/**
 * @brief Inject process output descriptors and optional raw transfer hooks.
 * @details Selects composition-owned destinations and production or test raw operations.
 * @param[in] out_fd Descriptor used by output-sink calls.
 * @param[in] err_fd Descriptor used by error-sink calls.
 * @param[in] ops Complete operation table, or nullptr for production syscalls.
 * @pre @p out_fd and @p err_fd are valid for their intended calls.
 * @pre Non-null @p ops supplies all four callbacks.
 * @post Subsequent sink calls use the supplied descriptors.
 * @post Subsequent transfer calls use @p ops or the production table.
 * @note Process-wide composition step; not thread-safe with active transfers.
 * @since 0.1.0
 */
RA8_PRIV void priv_emu_io_configure(int out_fd, int err_fd, const emu_io_ops_t* ops);
/** @brief Read exactly @p count sequential bytes or report EOF/fault progress. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_read_exact(int fd, void* buf, size_t count);
/** @brief Write exactly @p count sequential bytes or report fault progress. */
RA8_PRIV [[nodiscard]] emu_io_result_t
priv_emu_io_write_exact(int fd, const void* buf, size_t count);
/** @brief Read exactly @p count bytes at @p offset without changing the cursor. */
RA8_PRIV [[nodiscard]] emu_io_result_t
priv_emu_io_pread_exact(int fd, void* buf, size_t count, off_t offset);
/** @brief Write exactly @p count bytes at @p offset without changing the cursor. */
RA8_PRIV [[nodiscard]] emu_io_result_t
priv_emu_io_pwrite_exact(int fd, const void* buf, size_t count, off_t offset);
/** @brief Write a NUL-terminated literal to the injected output descriptor. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_out_text(const char* text);
/** @brief Write a NUL-terminated literal to the injected error descriptor. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_err_text(const char* text);
/** @brief Write an exact non-NUL-terminated byte fragment to the error sink. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_err_bytes(const void* bytes, size_t length);
/** @brief Format bounded text and write it to the injected output descriptor. */
[[gnu::format(printf, 1, 2)]] RA8_PRIV [[nodiscard]] emu_io_result_t
priv_emu_io_outf(const char* format, ...);
/** @brief Format bounded text and write it to the injected error descriptor. */
[[gnu::format(printf, 1, 2)]] RA8_PRIV [[nodiscard]] emu_io_result_t
priv_emu_io_errf(const char* format, ...);
/** @brief Write a NUL-terminated literal to an explicit raw descriptor. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_file_text(int fd, const char* text);
/** @brief Write one byte to an explicit raw descriptor. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_file_char(int fd, char value);
/** @brief Format bounded text and write it to an explicit raw descriptor. */
[[gnu::format(printf, 2, 3)]] RA8_PRIV [[nodiscard]] emu_io_result_t
priv_emu_io_filef(int fd, const char* format, ...);
/** @brief Open and stat a regular file for bounded raw reading. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_open_read(const char* path, emu_io_file_t* file);
/** @brief Close and invalidate an owned raw file descriptor. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_close(emu_io_file_t* file);
/** @brief Create a sibling temporary output for failure-atomic publication. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_txn_begin(const char* path, emu_io_txn_t* txn);
/** @brief Sync, close, and atomically rename an active sibling transaction. */
RA8_PRIV [[nodiscard]] emu_io_result_t priv_emu_io_txn_commit(emu_io_txn_t* txn);
/**
 * @brief Close and unlink an active sibling transaction without publication.
 * @details Releases any temporary descriptor/path while preserving the final target.
 * @param[in,out] txn Transaction to abandon; nullptr is accepted.
 * @pre Non-null @p txn was zero-initialised or passed to priv_emu_io_txn_begin().
 * @pre The caller no longer intends to commit @p txn.
 * @post Any owned descriptor is closed and set to -1.
 * @post Any named sibling temporary is unlinked and cleared.
 * @note Does not modify the final target path.
 * @since 0.1.0
 */
RA8_PRIV void priv_emu_io_txn_abort(emu_io_txn_t* txn);
