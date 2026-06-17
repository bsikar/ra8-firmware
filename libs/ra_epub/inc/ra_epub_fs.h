/**
 * @file ra_epub_fs.h
 * @brief `ra_fs` -> `ra_epub` bridge: open a `.epub` straight off a filesystem.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: S}
 *
 * @details
 * The target-side adapter the `ra_epub.h` overview calls "Phase 4.2": it reads an
 * entire `.epub` file from a mounted `ra_fs` volume (FAT12/16/32 over an SD card
 * via `ra_sdmmc_spi`, or any other `ra_fs_backend_t`) into a caller-owned buffer,
 * then hands that buffer to `ra_epub_open()` as an in-memory media blob. So the
 * e-reader can enumerate and read a book that lives on storage with one call,
 * with no heap (NASA Rule 3): the caller owns the backing buffer.
 *
 * Keeping the bridge in its own translation unit (and behind
 * `__has_include("ra_fs.h")`) means the pure `ra_epub` core stays free of any
 * `ra_fs` dependency for hosts/apps that only use the in-memory media path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "ra_epub.h"
#include "ra_err.h"
#include "ra_fs.h"

/**
 * @brief Open an EPUB book directly from a file on a mounted `ra_fs` volume.
 *
 * @details
 * Opens @p path for reading on @p mount, validates that the file fits in @p cap,
 * reads the whole `.epub` into @p buf, closes the file, and then opens the book
 * from that buffer via `ra_epub_open()`. On success @p out_book owns the parsed
 * spine/metadata and @p buf must outlive it (the book's zip archive points into
 * @p buf). The file is always closed before returning, on success or failure.
 *
 * @param[in]  mount    Mounted volume handle from `ra_fs_mount()`. Non-NULL.
 * @param[in]  path     Absolute `.epub` path on the volume (e.g. "BOOK.EPUB"). Non-NULL.
 * @param[out] buf      Caller-owned buffer that receives the file bytes. Non-NULL.
 * @param[in]  cap      Capacity of @p buf, bytes. Must be >= the file size.
 * @param[out] out_book Receives the opened book on success. Non-NULL.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok               Book read from @p path and opened.
 * @retval k_ra_err_null_ptr     Any pointer argument is NULL.
 * @retval k_ra_err_invalid_size @p cap is 0.
 * @retval k_ra_err_no_mem       The file is larger than @p cap.
 * @retval k_ra_err_hw_error     A short read (file shrank / backend error).
 * @retval other                 Propagated from `ra_fs_open/size/read` or
 *                               `ra_epub_open` (e.g. not_found, invalid archive).
 *
 * @pre @p mount is a live mount; @p buf has @p cap bytes; @p path names a `.epub`.
 * @post On `k_ra_ok`, `*out_book` is open and `ra_epub_close()` must be called.
 * @post The `ra_fs` file handle is closed on every path.
 *
 * @note Not thread-safe vs. concurrent use of the same mount. Reads the whole
 *       file into RAM (the book is then served from @p buf with no further I/O).
 * @see ra_epub_open()
 * @see ra_epub_close()
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_open_fs(ra_fs_mount_t*  mount,
                                       const char*     path,
                                       uint8_t*        buf,
                                       size_t          cap,
                                       ra_epub_book_t* out_book);

#ifdef __cplusplus
}
#endif
