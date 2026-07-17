/**
 * @file ra8_epub_fs.h
 * @brief `ra8_fs` -> `ra8_epub` bridge: open a `.epub` straight off a filesystem.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: S}
 *
 * @details
 * The target-side adapter that opens a book living on a mounted `ra8_fs` volume
 * (FAT12/16/32 over an SD card via `ra8_sdmmc_spi`, or any other
 * `ra8_fs_backend_t`) by STREAMING it (#151/#230): `ra8_epub_open_streamed_fs()`
 * hands `ra8_epub_open_streamed()` a seek+read backing over the open file, so
 * only the ZIP tail and one entry at a time are ever resident -- no whole-file
 * buffer, no book-size ceiling below the `ra8_fs` 4 GiB offset limit, and no
 * heap (NASA Rule 3). The retired whole-file `ra8_epub_open_fs()` bridge was
 * deleted with #230 when its last consumer moved onto this streamed path.
 *
 * Keeping the bridge in its own translation unit (and behind
 * `__has_include("ra8_fs.h")`) means the pure `ra8_epub` core stays free of any
 * `ra8_fs` dependency for hosts/apps that only use the in-memory media path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"

/**
 * @struct ra8_epub_stream_fs_ctx_t
 * @brief Backing state for a streamed `ra8_fs` EPUB open (#151).
 *
 * @details
 * Holds the open source-file handle for a book opened via
 * `ra8_epub_open_streamed_fs()`. The streamed path keeps the file open for the
 * book's whole lifetime and seeks+reads each ZIP entry on demand -- so this
 * context, and the file it owns, must out-live the book. Release both with
 * `ra8_epub_close_streamed_fs()`.
 *
 * @invariant `file != NULL` between a successful open and the matching close.
 * @see ra8_epub_open_streamed_fs()
 * @see ra8_epub_close_streamed_fs()
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_file_t* file; /**< Open source `.epub`; owned until close. */
} ra8_epub_stream_fs_ctx_t;

/**
 * @brief Stream-open an EPUB directly off a mounted `ra8_fs` volume, no residency (#151).
 *
 * @details
 * Opens @p path for reading and hands `ra8_epub_open_streamed()` a seek+read
 * backing over the open file instead of reading the whole book into RAM (#151):
 * only the ZIP tail and one entry at a time
 * are ever read from the card, so an arbitrarily large book (bounded by `ra8_fs`'s
 * 32-bit file offsets, i.e. < 4 GiB) opens inside a small fixed RAM budget. The
 * file stays open in @p io until `ra8_epub_close_streamed_fs()`.
 *
 * @param[in]  mount    Mounted volume handle from `ra8_fs_mount()`. Non-NULL.
 * @param[in]  path     `.epub` path on the volume. Non-NULL.
 * @param[out] io       Caller-owned stream context; receives the open file. Must
 *                      out-live @p out_book. Non-NULL.
 * @param[out] out_book Receives the opened book on success. Non-NULL.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Book opened; streams from the card on demand.
 * @retval k_ra8_err_null_ptr Any pointer argument is NULL.
 * @retval other             Propagated from `ra8_fs_open` or `ra8_epub_open_streamed`.
 *
 * @pre @p mount is a live mount; @p path names a `.epub`; @p io out-lives @p out_book.
 * @post On `k_ra8_ok`, `*out_book` is open and `io->file` is the open source file.
 * @post On any error the file is closed and `io->file == NULL`.
 * @note Not thread-safe vs. concurrent use of the same mount. The file is read on
 *       every chapter/cover/resource access, not once up front.
 * @see ra8_epub_open_streamed()
 * @see ra8_epub_close_streamed_fs()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_open_streamed_fs(ra8_fs_mount_t*           mount,
                                                  const char*               path,
                                                  ra8_epub_stream_fs_ctx_t* io,
                                                  ra8_epub_book_t*          out_book);

/**
 * @brief Close a book opened by `ra8_epub_open_streamed_fs()` and its source file.
 *
 * @details
 * Tears down the miniz reader (via `ra8_epub_close()`) and closes the open source
 * file held in @p io. Safe to call once after a successful open; after a failed
 * open the file is already closed (`io->file == NULL`) and only the book is reset.
 *
 * @param[in,out] io   Stream context from `ra8_epub_open_streamed_fs()`. Non-NULL.
 * @param[in,out] book Book opened by `ra8_epub_open_streamed_fs()`. Non-NULL.
 *
 * @return ra8_err_t The result of `ra8_epub_close(book)`.
 * @retval k_ra8_ok           Book closed and file released.
 * @retval k_ra8_err_null_ptr @p io or @p book is NULL.
 * @retval other             Propagated from `ra8_epub_close()`.
 *
 * @pre @p io and @p book came from the same `ra8_epub_open_streamed_fs()` call.
 * @post `io->file == NULL` on return.
 * @post `book->in_use == 0` on `k_ra8_ok`.
 * @see ra8_epub_open_streamed_fs()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_close_streamed_fs(ra8_epub_stream_fs_ctx_t* io,
                                                   ra8_epub_book_t*          book);

#ifdef __cplusplus
}
#endif
