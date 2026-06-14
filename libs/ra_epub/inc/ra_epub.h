/**
 * @file ra_epub.h
 * @brief EPUB (.epub) reader and chapter iterator for ra8d2-firmware.
 *
 * @details
 * `ra_epub` is a small, dependency-light domain layer that opens an EPUB
 * (.epub) file and exposes an iterator over its chapters, plus a handful
 * of metadata accessors and a glyph rasteriser that reaches through to
 * `stb_truetype`.
 *
 * Internally `ra_epub`:
 *
 *   1. Treats the .epub as a ZIP container and reads the central
 *      directory via `miniz` (`mz_zip_reader_init_mem`).
 *   2. Locates `META-INF/container.xml`, parses it with `tinyxml2`, and
 *      follows the `<rootfile full-path="...">` to the OPF document
 *      (typically `OEBPS/content.opf`).
 *   3. Parses the OPF manifest + spine to build a fixed-size,
 *      statically-allocated chapter list (`k_ra_epub_max_chapters`).
 *   4. Pulls the Dublin Core metadata block (`<dc:title>`,
 *      `<dc:creator>`, `<dc:language>`) into the book record.
 *   5. Resolves the `cover-image` manifest item (or the legacy
 *      `<meta name="cover" content="...">`) and exposes its raw bytes
 *      so the caller can decode them with `stb_image`.
 *
 * The `media` parameter to `ra_epub_open()` is intentionally opaque:
 *
 *   - On the host unit-test build, `media` points at an
 *     `ra_epub_mem_media_t` describing an in-memory `.epub` blob.
 *   - On the target firmware, the FileX adapter (Phase 4.2) wraps an
 *     `FX_FILE` and reads the entire file into a caller-owned buffer
 *     before handing it to `ra_epub_open()`.
 *
 * ## Static-allocation footprint
 *
 *   - Chapter list:   `k_ra_epub_max_chapters` * `k_ra_epub_max_path_len`
 *   - Metadata:       3 * `k_ra_epub_meta_len`
 *   - Backing zip:    one `mz_zip_archive` embedded in the book record
 *                     (no heap; size validated at compile time).
 *   - Font slot:      one `(uint8_t* font_data, size_t font_len)` pair.
 *
 * NASA Rule 3 (zero malloc/free in firmware) is honored: the book
 * record holds the entire `mz_zip_archive` inline as a byte buffer
 * sized for the type, the OPF scratch is a single `static` slot, and
 * the chapter table is a fixed-size 2D array. The only residual
 * allocation under this driver is internal to `tinyxml2`'s parser
 * (vendored), which is documented as a deviation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra_epub_limits_t
 * @brief Static-allocation caps for the EPUB reader.
 *
 * @details
 * The reader holds at most `k_ra_epub_max_chapters` chapter entries; an
 * EPUB with a longer spine is truncated and `ra_epub_open()` returns
 * `k_ra_err_no_mem`. `k_ra_epub_max_path_len` covers the longest
 * `<item href="...">` string we can store; `k_ra_epub_meta_len` is the
 * cap for any single Dublin Core metadata field (title, creator,
 * language).
 */
typedef enum : uint16_t {
  k_ra_epub_max_chapters = 64,  /**< Max spine length we accept.            */
  k_ra_epub_max_toc      = 64,  /**< Max table-of-contents entries we keep. */
  k_ra_epub_max_path_len = 192, /**< Max `href` length (incl. NUL).         */
  k_ra_epub_meta_len     = 128, /**< Max bytes per metadata field.          */
  k_ra_epub_zip_archive_bytes =
    256, /**< Inline storage for `mz_zip_archive` (sizeof on miniz 3.0.2 is 112; cushion for upstream growth; static_assert in .c). */
} ra_epub_limits_t;

/**
 * @enum ra_epub_toc_kind_t
 * @brief Which navigation document the table of contents was parsed from.
 *
 * @details
 * EPUB 2 books ship an NCX (`toc.ncx`, referenced by the spine's `toc`
 * attribute); EPUB 3 books ship an XHTML navigation document (the
 * manifest item carrying `properties="nav"`). When both are present the
 * reader prefers the EPUB 3 nav document. `k_ra_epub_toc_none` means no
 * usable TOC was found (the book is still readable via the spine).
 */
typedef enum : uint8_t {
  k_ra_epub_toc_none = 0, /**< No table of contents parsed.            */
  k_ra_epub_toc_ncx  = 1, /**< Parsed from an EPUB 2 NCX document.     */
  k_ra_epub_toc_nav  = 2, /**< Parsed from an EPUB 3 nav.xhtml.        */
} ra_epub_toc_kind_t;

/**
 * @struct ra_epub_toc_entry_t
 * @brief One titled entry in the table of contents.
 *
 * @details
 * `href` is stored exactly as the navigation document wrote it, relative
 * to the OPF directory (the same convention as
 * `ra_epub_book_t::chapter_paths`), and may carry a `#fragment` that
 * points at an anchor inside a chapter. `depth` is the nesting level: a
 * top-level entry is `0`, a sub-entry `1`, and so on, so a renderer can
 * indent a hierarchical TOC.
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  char title[k_ra_epub_meta_len]; /**< Navigation label text ("" if none).  */
  // cppcheck-suppress unusedStructMember
  char href[k_ra_epub_max_path_len]; /**< Target href (rel. to OPF dir).    */
  // cppcheck-suppress unusedStructMember
  uint8_t depth; /**< Nesting level; 0 == top level.                   */
} ra_epub_toc_entry_t;

/**
 * @enum ra_epub_render_t
 * @brief Pixel-format selectors for `ra_epub_render_glyph()`.
 *
 * @details
 * Currently the reader exposes a single 8-bit grayscale rasteriser; the
 * enum exists so future backends (subpixel-AA, MVE-vectorised) can be
 * added without changing the public function signature.
 */
typedef enum : uint8_t {
  k_ra_epub_render_alpha8 = 0, /**< 8 bits per pixel, alpha mask.          */
} ra_epub_render_t;

/* ===========================================================================
 * Opaque media interface
 * ===========================================================================
 */

/**
 * @struct ra_epub_mem_media_t
 * @brief In-memory EPUB media descriptor.
 *
 * @details
 * Used by the host-test build (and by any caller that has already
 * loaded the entire `.epub` into RAM). Pass the address of an instance
 * of this struct as the `media` argument to `ra_epub_open()`.
 *
 * @invariant `data` non-NULL, `size > 0`.
 *
 * @see ra_epub_open()
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  const uint8_t* data; /**< Pointer to the EPUB byte stream.        */
  // cppcheck-suppress unusedStructMember
  size_t size; /**< Length of the EPUB byte stream, bytes.  */
} ra_epub_mem_media_t;

/* ===========================================================================
 * Book handle
 * ===========================================================================
 */

/**
 * @struct ra_epub_book_t
 * @brief Opened EPUB book.
 *
 * @details
 * Populated by `ra_epub_open()`. The struct is exposed (not behind a
 * pointer) so callers can statically allocate it. Treat all fields as
 * read-only; modify only via the `ra_epub_*` API.
 *
 * @invariant `in_use == 1` while a book is open; cleared by
 *            `ra_epub_close()`.
 *
 * @see ra_epub_open()
 * @see ra_epub_close()
 */
typedef struct {
  /* --- Backing storage (caller-owned) ---------------------------------- */
  // cppcheck-suppress unusedStructMember
  const uint8_t* zip_bytes; /**< Pointer to the EPUB blob.           */
  // cppcheck-suppress unusedStructMember
  size_t zip_size; /**< Length of the EPUB blob.            */

  /* --- miniz state ----------------------------------------------------- */
  /* Inline storage for `mz_zip_archive`. Sized via static_assert in
   * ra_epub_open.c; the book record owns the archive in-place rather
   * than allocating it on the heap (NASA Rule 3). Cast through
   * `(mz_zip_archive*)&book->zip_archive_storage[0]` inside the
   * implementation. The exact upstream type would force this header
   * to include `miniz.h`; we hold an opaque byte buffer instead.
   *
   * `alignas(max_align_t)` is mandatory: ``mz_zip_archive`` carries
   * pointer-typed and ``uint64_t`` fields that require 8-byte
   * alignment. Without the alignment specifier the cast is
   * undefined behaviour on strict-alignment architectures. */
  // cppcheck-suppress unusedStructMember
  alignas(max_align_t) uint8_t zip_archive_storage[k_ra_epub_zip_archive_bytes];
  // cppcheck-suppress unusedStructMember
  uint8_t zip_archive_active; /**< 1 = mz_zip_reader_init succeeded. */

  /* --- Chapter table --------------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  uint16_t chapter_count; /**< Spine length actually stored. */
  // cppcheck-suppress unusedStructMember
  char chapter_paths[k_ra_epub_max_chapters]
                    [k_ra_epub_max_path_len]; /**< Manifest hrefs (relative to OPF dir). */

  /* --- Metadata -------------------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  char title[k_ra_epub_meta_len]; /**< Dublin Core `<dc:title>`.     */
  // cppcheck-suppress unusedStructMember
  char author[k_ra_epub_meta_len]; /**< Dublin Core `<dc:creator>`.   */
  // cppcheck-suppress unusedStructMember
  char language[k_ra_epub_meta_len]; /**< Dublin Core `<dc:language>`.  */

  /* --- Cover ----------------------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  char cover_path[k_ra_epub_max_path_len]; /**< Path to cover image (or empty). */

  /* --- OPF base directory --------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  char opf_dir[k_ra_epub_max_path_len]; /**< Directory portion of the OPF. */

  /* --- Table of contents ---------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  char toc_path[k_ra_epub_max_path_len]; /**< Nav/NCX href (rel. to OPF dir); "" if none. */
  // cppcheck-suppress unusedStructMember
  uint8_t toc_kind; /**< `ra_epub_toc_kind_t`: source of the TOC.      */
  // cppcheck-suppress unusedStructMember
  uint16_t toc_count; /**< Number of entries stored in `toc`.          */
  // cppcheck-suppress unusedStructMember
  ra_epub_toc_entry_t toc[k_ra_epub_max_toc]; /**< Flattened TOC, document order. */

  /* --- Optional embedded font for glyph rendering --------------------- */
  // cppcheck-suppress unusedStructMember
  const uint8_t* font_data; /**< TTF blob; NULL if unset.        */
  // cppcheck-suppress unusedStructMember
  size_t font_size; /**< TTF blob length.                */

  /* --- Lifecycle ------------------------------------------------------- */
  // cppcheck-suppress unusedStructMember
  uint8_t in_use; /**< 1 = open, 0 = closed.                          */
} ra_epub_book_t;

/* ===========================================================================
 * Metadata struct
 * ===========================================================================
 */

/**
 * @struct ra_epub_metadata_t
 * @brief Dublin Core metadata bundle returned by `ra_epub_get_metadata()`.
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  const char* title; /**< NUL-terminated title; "" if absent.      */
  // cppcheck-suppress unusedStructMember
  const char* author; /**< NUL-terminated creator; "" if absent.    */
  // cppcheck-suppress unusedStructMember
  const char* language; /**< NUL-terminated language tag; "" if absent.*/
} ra_epub_metadata_t;

/* ===========================================================================
 * Public API -- lifecycle
 * ===========================================================================
 */

/**
 * @brief Open an EPUB book from an opaque media handle.
 *
 * @details
 * Treats `media` as a pointer to `ra_epub_mem_media_t`, opens the ZIP
 * via miniz, follows `META-INF/container.xml` to the OPF document,
 * parses metadata + manifest + spine, and populates `*out_book`.
 *
 * Algorithm:
 *   1. Validate args, zero `*out_book`.
 *   2. `mz_zip_reader_init_mem()` against the in-memory blob.
 *   3. Extract `META-INF/container.xml` to the local stack buffer.
 *   4. Parse with tinyxml2; pull the first `<rootfile>` `full-path`.
 *   5. Extract the OPF file; parse the `<metadata>`, `<manifest>`,
 *      and `<spine>` blocks.
 *   6. Walk the spine in document order; for each `<itemref idref="X">`,
 *      look up the manifest entry with id="X" and copy its `href` into
 *      `chapter_paths[chapter_count++]`.
 *
 * @param[in]  media     Opaque pointer; currently expected to be a
 *                       `ra_epub_mem_media_t*`.
 * @param[in]  path      Cosmetic file path for diagnostic logs; may be
 *                       NULL. Not used to read bytes -- the caller is
 *                       responsible for loading the blob into `media`.
 * @param[out] out_book  Populated book on success.
 *
 * @return ra_err_t
 * @retval k_ra_ok                    Book opened.
 * @retval k_ra_err_null_ptr          `media` or `out_book` is NULL.
 * @retval k_ra_err_invalid_arg       Media payload invalid.
 * @retval k_ra_err_no_mem            Spine longer than
 *                                    `k_ra_epub_max_chapters`.
 * @retval k_ra_err_validation_failed ZIP/XML/OPF could not be parsed.
 *
 * @pre `media` non-NULL.
 * @pre `out_book` non-NULL.
 * @post On success, `out_book->in_use == 1` and `chapter_count >= 0`.
 * @post On failure, `*out_book` is zero-initialized.
 *
 * @note Not thread-safe. Single-threaded init context.
 *
 * @see ra_epub_close()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_open(const void* media, const char* path, ra_epub_book_t* out_book);

/**
 * @brief Close a previously opened EPUB book.
 *
 * @param[in,out] book Book opened by `ra_epub_open()`.
 *
 * @retval k_ra_ok                  Book closed and slot released.
 * @retval k_ra_err_null_ptr        `book` is NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL.
 * @pre `book->in_use == 1`.
 * @post `book->in_use == 0`.
 * @post `book->zip_archive == NULL`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_close(ra_epub_book_t* book);

/* ===========================================================================
 * Public API -- chapter iteration
 * ===========================================================================
 */

/**
 * @brief Report how many chapters the spine contains.
 *
 * @param[in]  book      Open book.
 * @param[out] out_count Chapter count.
 *
 * @retval k_ra_ok                  Reported.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL, `out_count` non-NULL.
 * @post `*out_count <= k_ra_epub_max_chapters`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_get_chapter_count(const ra_epub_book_t* book, uint16_t* out_count);

/**
 * @brief Extract the raw XHTML bytes of one chapter into the caller's buffer.
 *
 * @param[in]  book     Open book.
 * @param[in]  idx      Chapter index, `[0, chapter_count)`.
 * @param[out] out_xhtml Destination buffer (caller-owned).
 * @param[in]  max_len  Capacity of `out_xhtml`, bytes.
 * @param[out] got_len  Bytes actually written (0 on failure).
 *
 * @retval k_ra_ok                  Chapter copied (got_len <= max_len).
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 * @retval k_ra_err_out_of_range    `idx >= chapter_count`.
 * @retval k_ra_err_invalid_size    `max_len == 0`.
 * @retval k_ra_err_no_mem          Chapter does not fit in `max_len`.
 * @retval k_ra_err_not_found       Manifest references a missing zip entry.
 *
 * @pre `book` non-NULL, `book->in_use == 1`.
 * @post On success, `*got_len > 0`. On failure, `*got_len == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_load_chapter(ra_epub_book_t* book,
                                            uint16_t        idx,
                                            uint8_t*        out_xhtml,
                                            size_t          max_len,
                                            size_t*         got_len);

/* ===========================================================================
 * Public API -- table of contents
 * ===========================================================================
 */

/**
 * @brief Report which navigation document the TOC was parsed from.
 *
 * @param[in]  book     Open book.
 * @param[out] out_kind Receives a `ra_epub_toc_kind_t` value.
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Reported.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL, `out_kind` non-NULL.
 * @pre `book->in_use == 1`.
 * @post `*out_kind` is one of the `ra_epub_toc_kind_t` enumerators.
 * @post `*out_kind == k_ra_epub_toc_none` iff `toc_count == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_get_toc_kind(const ra_epub_book_t* book, uint8_t* out_kind);

/**
 * @brief Report how many table-of-contents entries the book exposes.
 *
 * @details
 * The count is the flattened (depth-first) entry total parsed from the
 * book's NCX or nav document; `0` means no usable TOC was found. Use
 * `ra_epub_get_toc_entry()` to read each entry's title, href, and depth.
 *
 * @param[in]  book      Open book.
 * @param[out] out_count Entry count.
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Reported.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL, `out_count` non-NULL.
 * @pre `book->in_use == 1`.
 * @post `*out_count <= k_ra_epub_max_toc`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_get_toc_count(const ra_epub_book_t* book, uint16_t* out_count);

/**
 * @brief Copy one table-of-contents entry into the caller's struct.
 *
 * @param[in]  book      Open book.
 * @param[in]  idx       Entry index, `[0, toc_count)`.
 * @param[out] out_entry Destination entry (title, href, depth) on success.
 *
 * @return ra_err_t
 * @retval k_ra_ok                  Entry copied.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 * @retval k_ra_err_out_of_range    `idx >= toc_count`.
 *
 * @pre `book` non-NULL, `out_entry` non-NULL.
 * @pre `book->in_use == 1`.
 * @post On success, `*out_entry` is a NUL-terminated, bounded copy.
 * @post On failure, `*out_entry` is left unmodified.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_epub_get_toc_entry(const ra_epub_book_t* book, uint16_t idx, ra_epub_toc_entry_t* out_entry);

/* ===========================================================================
 * Public API -- metadata + cover + glyph rasterise
 * ===========================================================================
 */

/**
 * @brief Return Dublin Core metadata strings.
 *
 * @details
 * The returned `const char*` pointers alias `book` storage and are
 * valid until `ra_epub_close()` is called. Missing fields point at the
 * empty string `""`, never NULL.
 *
 * @param[in]  book     Open book.
 * @param[out] out_meta Populated metadata struct on success.
 *
 * @retval k_ra_ok                  Reported.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_get_metadata(const ra_epub_book_t* book,
                                            ra_epub_metadata_t*   out_meta);

/**
 * @brief Copy the raw bytes of the cover image into the caller's buffer.
 *
 * @details
 * The image is returned in its on-disk encoding (PNG / JPEG / GIF). The
 * caller decodes it with `stb_image` (see `libs/third_party/stb`) or
 * any other decoder.
 *
 * @param[in]  book    Open book.
 * @param[out] out_buf Destination buffer.
 * @param[in]  max_len Capacity of `out_buf`, bytes.
 * @param[out] got_len Bytes actually written.
 *
 * @retval k_ra_ok                  Cover copied.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 * @retval k_ra_err_invalid_size    `max_len == 0`.
 * @retval k_ra_err_not_found       No cover image declared.
 * @retval k_ra_err_no_mem          Cover does not fit in `max_len`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_epub_get_cover_image(ra_epub_book_t* book, uint8_t* out_buf, size_t max_len, size_t* got_len);

/**
 * @brief Attach a TTF font blob to be used by `ra_epub_render_glyph()`.
 *
 * @details
 * `ra_epub` does not embed a default font; the caller (or the host
 * application) must point the book at a TTF blob that lives for the
 * lifetime of the book. Typical sources are:
 *   - A font baked into the firmware image as a `static const`.
 *   - A `font/`-prefixed manifest entry inside the EPUB, extracted
 *     via `ra_epub_load_chapter()` style code into a caller buffer.
 *
 * @param[in,out] book      Open book.
 * @param[in]     font_data TTF buffer; must outlive `book`.
 * @param[in]     font_size Length of `font_data`, bytes.
 *
 * @retval k_ra_ok                  Font installed.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0`.
 * @retval k_ra_err_invalid_size    `font_size < 16`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_epub_set_font(ra_epub_book_t* book, const uint8_t* font_data, size_t font_size);

/**
 * @brief Rasterise a single Unicode code point into an alpha-8 bitmap.
 *
 * @details
 * Wraps `stbtt_GetCodepointBitmap()` against the font installed via
 * `ra_epub_set_font()`. The resulting bitmap is 8 bits per pixel,
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
 * @retval k_ra_ok                  Glyph rasterised.
 * @retval k_ra_err_null_ptr        Any pointer NULL.
 * @retval k_ra_err_not_initialized `book->in_use == 0` or no font set.
 * @retval k_ra_err_invalid_arg     `font_size <= 0`.
 * @retval k_ra_err_no_mem          Glyph does not fit in `max_pixels`.
 * @retval k_ra_err_validation_failed Font blob malformed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_epub_render_glyph(const ra_epub_book_t* book,
                                            int32_t               codepoint,
                                            float                 font_size,
                                            uint8_t*              out_bitmap,
                                            size_t                max_pixels,
                                            uint32_t*             out_w,
                                            uint32_t*             out_h);

#ifdef __cplusplus
}
#endif
