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
 * Include `ra8_epub.h` to get this API transitively (it includes this header);
 * a translation unit that only needs the entry cursor may include this header
 * directly.
 *
 * @see ra8_epub.h
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

typedef struct {
  // cppcheck-suppress unusedStructMember
  void* iter; /**< Opaque `mz_zip_reader_extract_iter_state*`; NULL when closed. */
  // cppcheck-suppress unusedStructMember
  ra8_epub_book_t* book; /**< Owning book (archive + allocator); borrowed. */
  // cppcheck-suppress unusedStructMember
  uint64_t total; /**< Entry uncompressed size, bytes. */
  // cppcheck-suppress unusedStructMember
  uint64_t consumed; /**< Bytes delivered to the caller so far. */
  // cppcheck-suppress unusedStructMember
  uint8_t done; /**< 1 once EOF has been reached / reported. */
  // cppcheck-suppress unusedStructMember
  uint8_t reserved[k_ra8_epub_entry_reader_reserved_bytes]; /**< Padding; keep zero. */
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

/**
 * @brief Number of `<manifest>` `<item>` entries retained (#151).
 *
 * @details
 * `ra8_epub_open()` records every manifest item -- id, href, media-type -- in OPF
 * document order, up to ::k_ra8_epub_max_manifest. The on-device compiler walks
 * them in that order to emit the stylesheet and image tables, matching the
 * desktop `epub_compile.py` (`manifest.items()`), which keeps the blob
 * byte-identical.
 *
 * @param[in] book Open book (`in_use == 1`).
 * @return Item count (0 .. ::k_ra8_epub_max_manifest), or 0 if @p book is NULL.
 * @retval 0 No items, a closed book, or a NULL handle.
 * @pre @p book was populated by `ra8_epub_open()`.
 * @pre The caller treats the count as a read-only snapshot.
 * @post No state is mutated.
 * @note Not thread-safe; single-threaded reader context.
 * @see ra8_epub_manifest_item()
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_epub_manifest_count(const ra8_epub_book_t* book);

/**
 * @brief Borrow the manifest item at @p index (OPF document order, #151).
 *
 * @details
 * Returns a const pointer into the book's retained manifest array; the storage
 * lives in @p book and stays valid until `ra8_epub_close()`. The caller must not
 * write through the pointer.
 *
 * @param[in] book  Open book (`in_use == 1`).
 * @param[in] index Zero-based item index, `< ra8_epub_manifest_count(book)`.
 * @return Pointer to the item, or NULL if @p book is NULL or @p index is out of range.
 * @retval NULL @p book is NULL or @p index >= the retained count.
 * @pre @p book was populated by `ra8_epub_open()`.
 * @pre @p index is less than ::ra8_epub_manifest_count for @p book.
 * @post No state is mutated; the returned storage is owned by @p book.
 * @note Not thread-safe; single-threaded reader context.
 * @see ra8_epub_manifest_count()
 * @since 0.1.0
 */
[[nodiscard]] const ra8_epub_manifest_item_t* ra8_epub_manifest_item(const ra8_epub_book_t* book,
                                                                     uint16_t               index);

/**
 * @brief Count the fonts the EPUB ships in its OPF manifest (#109).
 *
 * @details
 * During `ra8_epub_open()` the manifest is scanned for `<item>` entries whose
 * `media-type` is a known font type (`application/font-sfnt`,
 * `application/vnd.ms-opentype`, `font/ttf`, `font/otf`,
 * `application/x-font-ttf`); their hrefs are recorded (up to
 * ::k_ra8_epub_max_fonts). This returns how many were found so the caller can
 * iterate `ra8_epub_get_embedded_font()`. Zero is normal -- many books rely on
 * the reading-system font.
 *
 * @param[in]  book      Open book.
 * @param[out] out_count Receives the embedded font count (0 .. ::k_ra8_epub_max_fonts).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Count written.
 * @retval k_ra8_err_null_ptr        @p book or @p out_count is NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book->in_use == 1`.
 * @pre @p out_count is non-NULL.
 * @post `*out_count <= k_ra8_epub_max_fonts`.
 * @post @p book is unmodified.
 *
 * @note Not thread-safe; single-threaded init-context use.
 * @see ra8_epub_get_embedded_font()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_embedded_font_count(const ra8_epub_book_t* book,
                                                         uint16_t*              out_count);

/**
 * @brief Extract one EPUB-shipped font's bytes into a caller buffer (#109).
 *
 * @details
 * Joins the recorded font href onto the OPF directory and extracts the resource
 * from the open ZIP (same path as `ra8_epub_get_cover_image()`). The returned
 * bytes can be validated + bound into the reflow engine via
 * `ra8_reflow_bind_font()`, or attached with `ra8_epub_set_font()`.
 *
 * @param[in,out] book    Open book.
 * @param[in]     idx     Font index, `0 .. ra8_epub_get_embedded_font_count()-1`.
 * @param[out]    out_buf Destination buffer for the raw font bytes.
 * @param[in]     max_len Capacity of @p out_buf, bytes.
 * @param[out]    got_len Bytes actually written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Font copied; `*got_len > 0`.
 * @retval k_ra8_err_null_ptr        @p book, @p out_buf, or @p got_len is NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 * @retval k_ra8_err_invalid_size    `max_len == 0`.
 * @retval k_ra8_err_out_of_range    `idx >= embedded_font_count`.
 * @retval k_ra8_err_no_mem          Font does not fit in @p max_len.
 * @retval k_ra8_err_not_found       Recorded font href missing from the archive.
 *
 * @pre `book->in_use == 1` and `idx < ra8_epub_get_embedded_font_count()`.
 * @pre @p out_buf and @p got_len are non-NULL, `max_len > 0`.
 * @post On success `*got_len` holds the font length; on failure `*got_len == 0`.
 *
 * @note Not thread-safe; single-threaded init-context use.
 * @see ra8_epub_get_embedded_font_count(), ra8_reflow_bind_font()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_embedded_font(ra8_epub_book_t* book,
                                                   uint16_t         idx,
                                                   uint8_t*         out_buf,
                                                   size_t           max_len,
                                                   size_t*          got_len);

/**
 * @brief Attach a TTF font blob to be used by `ra8_epub_render_glyph()`.
 *
 * @details
 * `ra8_epub` does not embed a default font; the caller (or the host
 * application) must point the book at a TTF blob that lives for the
 * lifetime of the book. Typical sources are:
 *   - A font baked into the firmware image as a `static const`.
 *   - A `font/`-prefixed manifest entry inside the EPUB, extracted
 *     via `ra8_epub_load_chapter()` style code into a caller buffer.
 *
 * @param[in,out] book      Open book.
 * @param[in]     font_data TTF buffer; must outlive `book`.
 * @param[in]     font_size Length of `font_data`, bytes.
 *
 * @retval k_ra8_ok                  Font installed.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 * @retval k_ra8_err_invalid_size    `font_size < 16`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epub_set_font(ra8_epub_book_t* book, const uint8_t* font_data, size_t font_size);

/**
 * @brief Rasterise a single Unicode code point into an alpha-8 bitmap.
 *
 * @details
 * Wraps `stbtt_GetCodepointBitmap()` against the font installed via
 * `ra8_epub_set_font()`. The resulting bitmap is 8 bits per pixel,
 * grayscale, with the glyph's advance and side-bearing dropped (the
 * caller positions glyphs on the page).
 *
 * @param[in]  book       Open book with a font installed.
 * @param[in]  codepoint  Unicode code point (e.g. `'A'`).
 * @param[in]  font_size  Pixel size for the rasteriser.
 * @param[out] out_bitmap Destination buffer (caller-owned).
 * @param[in]  max_pixels Capacity of `out_bitmap`, in bytes.
 * @param[out] out_w      Glyph width in pixels.
 * @param[out] out_h      Glyph height in pixels.
 *
 * @retval k_ra8_ok                  Glyph rasterised.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0` or no font set.
 * @retval k_ra8_err_invalid_arg     `font_size <= 0`.
 * @retval k_ra8_err_no_mem          Glyph does not fit in `max_pixels`.
 * @retval k_ra8_err_validation_failed Font blob malformed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_render_glyph(const ra8_epub_book_t* book,
                                              int32_t                codepoint,
                                              float                  font_size,
                                              uint8_t*               out_bitmap,
                                              size_t                 max_pixels,
                                              uint32_t*              out_w,
                                              uint32_t*              out_h);

#ifdef __cplusplus
}
#endif
