/**
 * @file ra8_epub_entry.h
 * @brief Iterative, bounded-RAM ZIP-entry extraction for the EPUB reader (#231).
 * @ingroup grp_ereader
 *
 * @details
 * Split out of `ra8_epub.h` so that header stays inside the 1000-line
 * maintainability cap. This is the forward-streaming cursor over a single ZIP
 * entry: `ra8_epub_open()` / `ra8_epub_open_streamed()` give you a book, and
 * `ra8_epub_entry_open()` / `_read()` / `_close()` inflate one archive entry a
 * bounded block at a time instead of materialising it whole (as
 * `ra8_epub_get_resource()` does). `ra8_epub_entry_pread()` is the positioned
 * read for *stored* (uncompressed) entries. The implementation lives in
 * `ra8_epub_entry.c`.
 *
 * This header depends on `ra8_epub.h` (for `ra8_epub_book_t`) and includes it,
 * so the dependency is one-directional: `ra8_epub.h` does NOT include this
 * header. A translation unit that needs the entry cursor includes this header
 * directly (`ra8_epub.h` alone is not sufficient for the entry API).
 *
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 *
 * @see ra8_epub.h
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_epub.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Public API -- iterative (bounded-RAM) ZIP-entry extraction (#231)
 * ===========================================================================
 */

/**
 * @enum ra8_epub_entry_reader_layout_t
 * @brief Fixed-layout constants for @ref ra8_epub_entry_reader_t.
 *
 * @details
 * Names the trailing padding that rounds the reader's single-byte `done`
 * flag up to the struct's 8-byte alignment, so the layout carries no bare
 * numeric literal.
 *
 * @invariant `k_ra8_epub_entry_reader_reserved_bytes` keeps `done` plus the
 *            padding at 8 bytes, matching the alignment of the leading 64-bit
 *            members.
 * @see ra8_epub_entry_reader_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_epub_entry_reader_reserved_bytes =
    7, /**< Padding bytes after `done`; rounds the tail to 8-byte alignment. */
} ra8_epub_entry_reader_layout_t;

/**
 * @struct ra8_epub_entry_reader_t
 * @brief Forward streaming cursor over one ZIP entry -- inflate in bounded RAM (#231).
 *
 * @details
 * `ra8_epub_get_resource()` (and the cover / chapter / font accessors) extract a
 * whole entry via `mz_zip_reader_extract_to_mem` into a caller buffer sized to the
 * entry's *uncompressed* size. That is fine for the small text/CSS/font entries,
 * but a full-page manga scan in an all-image EPUB inflates to 20-50 MB -- too big
 * to hold whole on a device with a ~10 MB working-set budget.
 *
 * This cursor drives miniz's iterator (`mz_zip_reader_extract_iter_*`) so the
 * caller pulls one entry in fixed-size chunks: the resident inflate state is
 * bounded (a 32 KiB LZ dictionary + a bounded compressed-read buffer + the
 * inflator), *independent of the entry's uncompressed size*. Works for both a
 * resident book (`ra8_epub_open()`) and a streamed book
 * (`ra8_epub_open_streamed()`); in the streamed case each compressed block is
 * fetched on demand through the book's seek+read backing, so the whole archive is
 * never resident either.
 *
 * Treat every field as private; zero-initialise (`= {}`) before
 * `ra8_epub_entry_open()`.
 *
 * @invariant `iter != NULL` between a successful open and the matching close.
 * @see ra8_epub_entry_open()
 * @see ra8_epub_entry_read()
 * @see ra8_epub_entry_close()
 * @since 0.1.0
 */
typedef struct {
  void*            iter;     /**< Opaque `mz_zip_reader_extract_iter_state*`; NULL when closed. */
  ra8_epub_book_t* book;     /**< Owning book (archive + allocator); borrowed.                  */
  uint64_t         total;    /**< Entry uncompressed size, bytes.                               */
  uint64_t         consumed; /**< Bytes delivered to the caller so far.                         */
  uint8_t          done;     /**< 1 once EOF has been reached / reported.                       */
  /** Padding; keep zero. */
  uint8_t reserved[k_ra8_epub_entry_reader_reserved_bytes];
} ra8_epub_entry_reader_t;

/**
 * @brief Begin a bounded-RAM streaming extraction of one archive entry (#231).
 *
 * @details
 * Resolves @p path the same way `ra8_epub_get_resource()` does -- first joined onto
 * the OPF directory (`book->opf_dir`), then as a bare archive-rooted path -- then
 * starts a miniz extract-iterator over the located entry. No entry bytes are
 * inflated yet; the caller pulls them with `ra8_epub_entry_read()`. The entry's
 * uncompressed size is reported so the caller can size a progress bar or a tile
 * grid without materialising the entry.
 *
 * @param[in]  book       Open book (`in_use == 1`, archive active).
 * @param[in]  path       Entry path, OPF-dir-relative or archive-rooted, NUL-terminated.
 * @param[out] out_reader Cursor to populate (zero-initialised by the caller).
 * @param[out] out_size   Receives the entry's uncompressed size in bytes (may be NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Cursor open; read with `ra8_epub_entry_read()`.
 * @retval k_ra8_err_null_ptr          @p book, @p path, or @p out_reader is NULL.
 * @retval k_ra8_err_not_initialized   Book not open / archive inactive.
 * @retval k_ra8_err_not_found         No entry at @p path (prefixed or bare).
 * @retval k_ra8_err_validation_failed The iterator could not be started (corrupt entry).
 *
 * @pre `book->in_use == 1` and the archive is active.
 * @pre @p out_reader points at writable, zero-initialised storage.
 * @post On success `out_reader->iter != NULL` and `*out_size` (if given) is the size.
 * @post On any error `*out_reader` is zeroed and no iterator leaks.
 * @note Not thread-safe; the reader serialises archive access (one open cursor at a time).
 * @see ra8_epub_entry_read()
 * @see ra8_epub_entry_close()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_entry_open(ra8_epub_book_t*         book,
                                            const char*              path,
                                            ra8_epub_entry_reader_t* out_reader,
                                            uint64_t*                out_size);

/**
 * @brief Pull the next chunk of a streaming entry into a bounded caller buffer (#231).
 *
 * @details
 * Inflates up to @p cap more bytes of the entry into @p buf. A short read
 * (`*got < cap`) means end-of-entry has been reached; a subsequent call reports
 * `*got == 0`. The caller reuses the same fixed @p buf across calls, so the
 * high-water resident footprint is @p cap -- constant regardless of how large the
 * entry inflates to.
 *
 * @param[in]  reader Cursor from `ra8_epub_entry_open()`.
 * @param[out] buf    Destination chunk buffer (@p cap writable bytes).
 * @param[in]  cap    Capacity of @p buf, bytes (> 0).
 * @param[out] got    Bytes written this call (0 at end-of-entry).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Chunk delivered (or clean EOF with `*got == 0`).
 * @retval k_ra8_err_null_ptr          @p reader, @p buf, or @p got is NULL.
 * @retval k_ra8_err_not_initialized   @p reader is closed / never opened.
 * @retval k_ra8_err_invalid_size      `cap == 0`.
 * @retval k_ra8_err_validation_failed The compressed stream is corrupt (bad CRC / LZ).
 *
 * @pre @p reader came from a successful `ra8_epub_entry_open()`.
 * @pre @p buf holds @p cap writable bytes.
 * @post On success `reader->consumed` advanced by `*got`.
 * @post `*got == 0` iff the whole entry has now been delivered.
 * @note Not thread-safe.
 * @see ra8_epub_entry_open()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epub_entry_read(ra8_epub_entry_reader_t* reader, uint8_t* buf, size_t cap, size_t* got);

/**
 * @brief Tear down a streaming-entry cursor and release its inflate state (#231).
 *
 * @details
 * Frees the miniz iterator (returning its LZ dictionary + read buffer to the
 * allocator). If the entire entry had been read, the entry's CRC/size are verified
 * as a corruption check; an early close (before EOF) is legal and simply releases
 * resources. Idempotent on an already-closed cursor.
 *
 * @param[in,out] reader Cursor from `ra8_epub_entry_open()`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Cursor closed and state released.
 * @retval k_ra8_err_null_ptr          @p reader is NULL.
 * @retval k_ra8_err_validation_failed The fully-read entry failed CRC/size verification.
 *
 * @pre @p reader is a cursor (open or already closed).
 * @post `reader->iter == NULL` on return.
 * @post No inflate state remains allocated for this cursor.
 * @note Not thread-safe.
 * @see ra8_epub_entry_open()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_entry_close(ra8_epub_entry_reader_t* reader);

/**
 * @brief Positioned read of a *stored* (uncompressed) archive entry -- windowed
 *        random access in bounded RAM (#231).
 *
 * @details
 * For an entry stored with no compression (ZIP method 0 -- the natural choice for
 * already-compressed pixel data or a display-native tile atlas), the uncompressed
 * bytes lie contiguously in the archive, so any window `[offset, offset+len)` can
 * be read directly off the backing without inflating from the start. This is the
 * random-access primitive the tile source (`ra8_epub_img_tiles`) uses to page a
 * single tile without holding the whole image. A short read at the entry's tail is
 * reported via `*got`.
 *
 * Deflated entries are rejected (`k_ra8_err_not_supported`): random access into a
 * DEFLATE stream requires inflating from the start, which the forward cursor
 * (`ra8_epub_entry_read()`) already provides.
 *
 * @param[in]  book   Open book (`in_use == 1`, archive active).
 * @param[in]  path   Entry path, OPF-dir-relative or archive-rooted, NUL-terminated.
 * @param[in]  offset Byte offset into the entry's uncompressed data.
 * @param[out] buf    Destination buffer (@p len writable bytes).
 * @param[in]  len    Bytes requested.
 * @param[out] got    Bytes actually read (0 at/after the entry's end).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Window read (possibly short at EOF; see `*got`).
 * @retval k_ra8_err_null_ptr          @p book, @p path, @p buf, or @p got is NULL.
 * @retval k_ra8_err_not_initialized   Book not open / archive inactive.
 * @retval k_ra8_err_not_found         No entry at @p path (prefixed or bare).
 * @retval k_ra8_err_not_supported     The entry is DEFLATE-compressed (use the cursor).
 * @retval k_ra8_err_validation_failed The local header could not be read / is corrupt.
 *
 * @pre `book->in_use == 1` and the archive is active.
 * @pre @p buf holds @p len writable bytes.
 * @post On success `*got <= len` bytes are written to @p buf.
 * @post On any error `*got == 0`.
 * @note Not thread-safe.
 * @see ra8_epub_entry_read()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_entry_pread(ra8_epub_book_t* book,
                                             const char*      path,
                                             uint64_t         offset,
                                             uint8_t*         buf,
                                             size_t           len,
                                             size_t*          got);

#ifdef __cplusplus
}
#endif
