/**
 * @file ra8_epub.h
 * @brief EPUB (.epub) reader and chapter iterator for ra8-firmware.
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_epub` is a small, dependency-light domain layer that opens an EPUB
 * (.epub) file and exposes an iterator over its chapters, plus a handful
 * of metadata accessors and a glyph rasteriser that reaches through to
 * `stb_truetype`.
 *
 * Internally `ra8_epub`:
 *
 *   1. Treats the .epub as a ZIP container and reads the central
 *      directory via `miniz` (`mz_zip_reader_init_mem`).
 *   2. Locates `META-INF/container.xml`, parses it with `tinyxml2`, and
 *      follows the `<rootfile full-path="...">` to the OPF document
 *      (typically `OEBPS/content.opf`).
 *   3. Parses the OPF manifest + spine to build a fixed-size,
 *      statically-allocated chapter list (`k_ra8_epub_max_chapters`).
 *   4. Pulls the Dublin Core metadata block (`<dc:title>`,
 *      `<dc:creator>`, `<dc:language>`) into the book record.
 *   5. Resolves the `cover-image` manifest item (or the legacy
 *      `<meta name="cover" content="...">`) and exposes its raw bytes
 *      so the caller can decode them with `stb_image`.
 *
 * The `media` parameter to `ra8_epub_open()` is intentionally opaque:
 *
 *   - On the host unit-test build, `media` points at an
 *     `ra8_epub_mem_media_t` describing an in-memory `.epub` blob.
 *   - On the target firmware, the FileX adapter (Phase 4.2) wraps an
 *     `FX_FILE` and reads the entire file into a caller-owned buffer
 *     before handing it to `ra8_epub_open()`.
 *
 * ## Static-allocation footprint
 *
 *   - Chapter list:   `k_ra8_epub_max_chapters` * `k_ra8_epub_max_path_len`
 *   - Metadata:       3 * `k_ra8_epub_meta_len`
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

#include "ra8_err.h"

/* ===========================================================================
 * Compile-time limits
 * ===========================================================================
 */

/**
 * @enum ra8_epub_limits_t
 * @brief Static-allocation caps for the EPUB reader.
 *
 * @details
 * The reader holds at most `k_ra8_epub_max_chapters` chapter entries; an
 * EPUB with a longer spine is truncated and `ra8_epub_open()` returns
 * `k_ra8_err_no_mem`. `k_ra8_epub_max_path_len` covers the longest
 * `<item href="...">` string we can store; `k_ra8_epub_meta_len` is the
 * cap for any single Dublin Core metadata field (title, creator,
 * language).
 */
typedef enum : uint16_t {
  k_ra8_epub_max_chapters = 64,  /**< Max spine length we accept.                         */
  k_ra8_epub_max_toc      = 64,  /**< Max table-of-contents entries we keep.              */
  k_ra8_epub_max_path_len = 192, /**< Max `href` length (incl. NUL).                      */
  k_ra8_epub_meta_len     = 128, /**< Max bytes per metadata field.                       */
  k_ra8_epub_max_fonts    = 8,   /**< Max embedded font manifest items kept.              */
  k_ra8_epub_max_manifest = 96,  /**< Max `<manifest>` `<item>` entries kept (OPF order). */
  k_ra8_epub_id_len       = 64,  /**< Max manifest item id length (incl. NUL).            */
  k_ra8_epub_media_len    = 48,  /**< Max `media-type` length (incl. NUL).                */
  k_ra8_epub_zip_archive_bytes =
    256, /**< Storage for `mz_zip_archive` (miniz 3.0.2 sizeof=112; margin; static_assert in .c). */
} ra8_epub_limits_t;

/**
 * @enum ra8_epub_toc_kind_t
 * @brief Which navigation document the table of contents was parsed from.
 *
 * @details
 * EPUB 2 books ship an NCX (`toc.ncx`, referenced by the spine's `toc`
 * attribute); EPUB 3 books ship an XHTML navigation document (the
 * manifest item carrying `properties="nav"`). When both are present the
 * reader prefers the EPUB 3 nav document. `k_ra8_epub_toc_none` means no
 * usable TOC was found (the book is still readable via the spine).
 */
typedef enum : uint8_t {
  k_ra8_epub_toc_none = 0, /**< No table of contents parsed.        */
  k_ra8_epub_toc_ncx  = 1, /**< Parsed from an EPUB 2 NCX document. */
  k_ra8_epub_toc_nav  = 2, /**< Parsed from an EPUB 3 nav.xhtml.    */
} ra8_epub_toc_kind_t;

/**
 * @struct ra8_epub_toc_entry_t
 * @brief One titled entry in the table of contents.
 *
 * @details
 * `href` is stored exactly as the navigation document wrote it, relative
 * to the OPF directory (the same convention as
 * `ra8_epub_book_t::chapter_paths`), and may carry a `#fragment` that
 * points at an anchor inside a chapter. `depth` is the nesting level: a
 * top-level entry is `0`, a sub-entry `1`, and so on, so a renderer can
 * indent a hierarchical TOC.
 */
typedef struct {
  char    title[k_ra8_epub_meta_len];    /**< Navigation label text ("" if none). */
  char    href[k_ra8_epub_max_path_len]; /**< Target href (rel. to OPF dir).      */
  uint8_t depth;                         /**< Nesting level; 0 == top level.      */
} ra8_epub_toc_entry_t;

/**
 * @enum ra8_epub_render_t
 * @brief Pixel-format selectors for `ra8_epub_render_glyph()`.
 *
 * @details
 * Currently the reader exposes a single 8-bit grayscale rasteriser; the
 * enum exists so future backends (subpixel-AA, MVE-vectorised) can be
 * added without changing the public function signature.
 */
typedef enum : uint8_t {
  k_ra8_epub_render_alpha8 = 0, /**< 8 bits per pixel, alpha mask. */
} ra8_epub_render_t;

/* ===========================================================================
 * Opaque media interface
 * ===========================================================================
 */

/**
 * @struct ra8_epub_mem_media_t
 * @brief In-memory EPUB media descriptor.
 *
 * @details
 * Used by the host-test build (and by any caller that has already
 * loaded the entire `.epub` into RAM). Pass the address of an instance
 * of this struct as the `media` argument to `ra8_epub_open()`.
 *
 * @invariant `data` non-NULL, `size > 0`.
 *
 * @see ra8_epub_open()
 */
typedef struct {
  const uint8_t* data; /**< Pointer to the EPUB byte stream.       */
  size_t         size; /**< Length of the EPUB byte stream, bytes. */
} ra8_epub_mem_media_t;

/**
 * @typedef ra8_epub_stream_read_fn
 * @brief Seek+read callback backing a streamed (non-resident) EPUB open (#151).
 *
 * @details
 * The storage seam for `ra8_epub_open_streamed()`: the reader hands the callback
 * an absolute byte @p offset into the `.epub` archive and asks for @p len bytes.
 * The callback is free to seek+read from any backing -- an `ra8_fs` file on the SD
 * card, an `ra8_io` block device, or a page cache (`ra8_vmem`) -- so the reader
 * never needs the whole archive resident. Only the ZIP tail (end-of-central-
 * directory + central directory) and one entry at a time are ever fetched, so a
 * multi-GB book opens inside a fixed, small RAM budget.
 *
 * The signature deliberately mirrors miniz's `mz_file_read_func` (offset+length,
 * bytes-actually-read return) so the reader can drive miniz directly with no
 * copy: a return `< len` is treated as end-of-file / read error, exactly as the
 * in-memory path treats a short read.
 *
 * @param[in]  ctx    Opaque backing context (::ra8_epub_stream_media_t::ctx).
 * @param[in]  offset Absolute byte offset within the `.epub` archive.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Number of bytes requested.
 *
 * @return Bytes actually read (0 at/after EOF or on error; `< len` aborts).
 *
 * @note Not thread-safe; the reader serialises access.
 * @since 0.1.0
 */
typedef size_t (*ra8_epub_stream_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @struct ra8_epub_stream_media_t
 * @brief Seekable EPUB media descriptor -- opens with no whole-file residency (#151).
 *
 * @details
 * Pass the address of an instance of this struct as the @p media argument to
 * `ra8_epub_open_streamed()`. Unlike ::ra8_epub_mem_media_t (which requires the
 * entire `.epub` resident in one buffer), this describes the archive by a
 * seek+read callback plus its total size, so the reader streams the ZIP central
 * directory and each entry on demand. The backing referenced by @p ctx (an open
 * file, a block device, a page cache) must out-live the opened book, exactly as
 * the resident blob must out-live a book opened via `ra8_epub_open()`.
 *
 * @invariant `read` is non-NULL and `size > 0`.
 *
 * @see ra8_epub_open_streamed()
 * @since 0.1.0
 */
typedef struct {
  ra8_epub_stream_read_fn read; /**< Seek+read callback (non-NULL).                         */
  void*                   ctx;  /**< Opaque backing passed to @c read (out-lives the book). */
  uint64_t                size; /**< Total archive length in bytes (> 0).                   */
} ra8_epub_stream_media_t;

/* ===========================================================================
 * Manifest item
 * ===========================================================================
 */

/**
 * @struct ra8_epub_manifest_item_t
 * @brief One `<manifest>` `<item>` entry, retained in OPF document order.
 *
 * @details
 * The on-device EPUB->.rabook compiler iterates the manifest in document order
 * to emit the stylesheet and image tables (and to resolve the cover by id), so
 * the raw `id`, `href`, and `media-type` are kept verbatim. Filtering is done by
 * exact `media_type` string compare, matching the desktop `epub_compile.py`
 * (`manifest.items()` in OPF order), which is what keeps the emitted blob
 * byte-identical.
 *
 * @invariant All three fields are NUL-terminated; absent attributes are "".
 *
 * @see ra8_epub_manifest_count()
 * @see ra8_epub_manifest_item()
 */
typedef struct {
  char id[k_ra8_epub_id_len];            /**< Manifest item `id` (or "" if absent).  */
  char href[k_ra8_epub_max_path_len];    /**< Item `href` (relative to the OPF dir). */
  char media_type[k_ra8_epub_media_len]; /**< Item `media-type` (or "" if absent).   */
} ra8_epub_manifest_item_t;

/* ===========================================================================
 * Book handle
 * ===========================================================================
 */

/**
 * @struct ra8_epub_book_t
 * @brief Opened EPUB book.
 *
 * @details
 * Populated by `ra8_epub_open()`. The struct is exposed (not behind a
 * pointer) so callers can statically allocate it. Treat all fields as
 * read-only; modify only via the `ra8_epub_*` API.
 *
 * @invariant `in_use == 1` while a book is open; cleared by
 *            `ra8_epub_close()`.
 *
 * @see ra8_epub_open()
 * @see ra8_epub_close()
 */
typedef struct {
  /* --- Backing storage (caller-owned) ---------------------------------- */
  const uint8_t* zip_bytes; /**< Pointer to the EPUB blob. */
  size_t         zip_size;  /**< Length of the EPUB blob.  */

  /* --- miniz state ----------------------------------------------------- */
  /* Inline storage for `mz_zip_archive`. Sized via static_assert in
   * ra8_epub_open.c; the book record owns the archive in-place rather
   * than allocating it on the heap (NASA Rule 3). Cast through
   * `(mz_zip_archive*)&book->zip_archive_storage[0]` inside the
   * implementation. The exact upstream type would force this header
   * to include `miniz.h`; we hold an opaque byte buffer instead.
   *
   * `alignas(max_align_t)` is mandatory: ``mz_zip_archive`` carries
   * pointer-typed and ``uint64_t`` fields that require 8-byte
   * alignment. Without the alignment specifier the cast is
   * undefined behaviour on strict-alignment architectures. */
  alignas(max_align_t) uint8_t
    zip_archive_storage[k_ra8_epub_zip_archive_bytes]; /**< Zip archive storage.              */
  uint8_t zip_archive_active;                          /**< 1 = mz_zip_reader_init succeeded. */

  /* --- Streamed backing (caller-seekable, no resident blob) (#151) ----- */
  /* For a book opened via `ra8_epub_open_streamed()`, miniz's `m_pIO_opaque`
   * points at this inline descriptor (a stable address for the book's
   * lifetime), and `zip_bytes == NULL`. For the resident `ra8_epub_open()`
   * path this is left zeroed and unused. */
  ra8_epub_stream_media_t stream_media; /**< Streamed media descriptor; {} for the resident path. */

  /* --- Chapter table --------------------------------------------------- */
  uint16_t chapter_count; /**< Spine length actually stored. */
  char     chapter_paths[k_ra8_epub_max_chapters]
                    [k_ra8_epub_max_path_len]; /**< Manifest hrefs (relative to OPF dir). */

  /* --- Metadata -------------------------------------------------------- */
  char title[k_ra8_epub_meta_len];      /**< Dublin Core `<dc:title>`.                       */
  char author[k_ra8_epub_meta_len];     /**< Dublin Core `<dc:creator>`.                     */
  char language[k_ra8_epub_meta_len];   /**< Dublin Core `<dc:language>`.                    */
  char identifier[k_ra8_epub_meta_len]; /**< Dublin Core `<dc:identifier>` (unique book id). */

  /* --- Cover ----------------------------------------------------------- */
  char cover_path[k_ra8_epub_max_path_len]; /**< Path to cover image (or empty). */

  /* --- Embedded fonts (#109) ------------------------------------------- */
  uint16_t embedded_font_count; /**< Manifest font items found (<= cap). */
  char     embedded_font_paths[k_ra8_epub_max_fonts]
                          [k_ra8_epub_max_path_len]; /**< Font hrefs (rel. to OPF dir). */

  /* --- Manifest (document order, #151) -------------------------------- */
  uint16_t manifest_count; /**< `<manifest>` `<item>` entries stored (<= cap). */
  /** Items, OPF order. */
  ra8_epub_manifest_item_t manifest[k_ra8_epub_max_manifest];

  /* --- OPF base directory --------------------------------------------- */
  char opf_dir[k_ra8_epub_max_path_len]; /**< Directory portion of the OPF. */

  /* --- Table of contents ---------------------------------------------- */
  char     toc_path[k_ra8_epub_max_path_len];   /**< Nav/NCX href (rel. to OPF dir); "" if none. */
  uint8_t  toc_kind;                            /**< `ra8_epub_toc_kind_t`: source of the TOC.   */
  uint16_t toc_count;                           /**< Number of entries stored in `toc`.          */
  ra8_epub_toc_entry_t toc[k_ra8_epub_max_toc]; /**< Flattened TOC, document order.              */

  /* --- Optional embedded font for glyph rendering --------------------- */
  const uint8_t* font_data; /**< TTF blob; NULL if unset. */
  size_t         font_size; /**< TTF blob length.         */

  /* --- Lifecycle ------------------------------------------------------- */
  uint8_t in_use; /**< 1 = open, 0 = closed. */
} ra8_epub_book_t;

/* ===========================================================================
 * Metadata struct
 * ===========================================================================
 */

/**
 * @struct ra8_epub_metadata_t
 * @brief Dublin Core metadata bundle returned by `ra8_epub_get_metadata()`.
 */
typedef struct {
  const char* title;      /**< NUL-terminated title; "" if absent.        */
  const char* author;     /**< NUL-terminated creator; "" if absent.      */
  const char* language;   /**< NUL-terminated language tag; "" if absent. */
  const char* identifier; /**< NUL-terminated unique id; "" if absent.    */
} ra8_epub_metadata_t;

/* ===========================================================================
 * Public API -- lifecycle
 * ===========================================================================
 */

/**
 * @brief Open an EPUB book from an opaque media handle.
 *
 * @details
 * Treats `media` as a pointer to `ra8_epub_mem_media_t`, opens the ZIP
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
 *                       `ra8_epub_mem_media_t*`.
 * @param[in]  path      Cosmetic file path for diagnostic logs; may be
 *                       NULL. Not used to read bytes -- the caller is
 *                       responsible for loading the blob into `media`.
 * @param[out] out_book  Populated book on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Book opened.
 * @retval k_ra8_err_null_ptr          `media` or `out_book` is NULL.
 * @retval k_ra8_err_invalid_arg       Media payload invalid.
 * @retval k_ra8_err_no_mem            Spine longer than
 *                                    `k_ra8_epub_max_chapters`.
 * @retval k_ra8_err_validation_failed ZIP/XML/OPF could not be parsed.
 *
 * @pre `media` non-NULL.
 * @pre `out_book` non-NULL.
 * @post On success, `out_book->in_use == 1` and `chapter_count >= 0`.
 * @post On failure, `*out_book` is zero-initialized.
 *
 * @note Not thread-safe. Single-threaded init context.
 *
 * @see ra8_epub_close()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epub_open(const void* media, const char* path, ra8_epub_book_t* out_book);

/**
 * @brief Open an EPUB book from a seekable stream, with no whole-file residency (#151).
 *
 * @details
 * The streaming counterpart to `ra8_epub_open()`. Instead of a fully-resident
 * blob (::ra8_epub_mem_media_t), it takes a ::ra8_epub_stream_media_t -- a seek+read
 * callback plus the archive size -- and drives miniz's user-read reader
 * (`mz_zip_reader_init`) off it. Only the ZIP tail (end-of-central-directory +
 * central directory) is read at open, and each entry (container.xml, OPF, a
 * chapter, the cover) is inflated on demand through the same callback, so the
 * resident working set is bounded by the largest single entry plus the central
 * directory -- never the whole archive. This is what lets a book far larger than
 * SRAM+SDRAM (e.g. a multi-hundred-MB manga omnibus on the SD card) be opened and
 * parsed at all.
 *
 * The parsed book is identical to one opened via `ra8_epub_open()`: every
 * accessor (`ra8_epub_load_chapter()`, `ra8_epub_get_cover_image()`,
 * `ra8_epub_get_resource()`, ...) works unchanged, streaming each entry from the
 * backing on demand. `ra8_epub_close()` tears the reader down for both paths.
 *
 * @param[in]  media    Seekable media descriptor; `read` non-NULL, `size > 0`.
 *                      The backing referenced by `media->ctx` must out-live the
 *                      opened book.
 * @param[in]  path     Cosmetic path for diagnostics; may be NULL. Not used to
 *                      read bytes -- all I/O goes through `media->read`.
 * @param[out] out_book Populated book on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Book opened; streams from @p media on demand.
 * @retval k_ra8_err_null_ptr          `media` or `out_book` is NULL.
 * @retval k_ra8_err_invalid_arg       `media->read` is NULL or `media->size == 0`.
 * @retval k_ra8_err_no_mem            Spine longer than `k_ra8_epub_max_chapters`.
 * @retval k_ra8_err_validation_failed ZIP/XML/OPF could not be parsed / read.
 *
 * @pre `media` and `out_book` are non-NULL.
 * @pre `media->read` faithfully reads `[offset, offset+len)` of the archive.
 * @post On success `out_book->in_use == 1` and `out_book->zip_bytes == NULL`.
 * @post On failure `*out_book` is zero-initialized.
 *
 * @note Not thread-safe. Single-threaded reader context.
 * @note The whole-file `ra8_epub_open()` stays the right choice for small, already
 *       resident (baked / XIP) books; this path is additive for the large/FS case.
 *
 * @see ra8_epub_open()
 * @see ra8_epub_close()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_open_streamed(const ra8_epub_stream_media_t* media,
                                               const char*                    path,
                                               ra8_epub_book_t*               out_book);

/**
 * @brief Close a previously opened EPUB book.
 *
 * @param[in,out] book Book opened by `ra8_epub_open()`.
 *
 * @retval k_ra8_ok                  Book closed and slot released.
 * @retval k_ra8_err_null_ptr        `book` is NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL.
 * @pre `book->in_use == 1`.
 * @post `book->in_use == 0`.
 * @post `book->zip_archive == NULL`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_close(ra8_epub_book_t* book);

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
 * @retval k_ra8_ok                  Reported.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL, `out_count` non-NULL.
 * @post `*out_count <= k_ra8_epub_max_chapters`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_chapter_count(const ra8_epub_book_t* book,
                                                   uint16_t*              out_count);

/**
 * @brief Extract the raw XHTML bytes of one chapter into the caller's buffer.
 *
 * @param[in]  book     Open book.
 * @param[in]  idx      Chapter index, `[0, chapter_count)`.
 * @param[out] out_xhtml Destination buffer (caller-owned).
 * @param[in]  max_len  Capacity of `out_xhtml`, bytes.
 * @param[out] got_len  Bytes actually written (0 on failure).
 *
 * @retval k_ra8_ok                  Chapter copied (got_len <= max_len).
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 * @retval k_ra8_err_out_of_range    `idx >= chapter_count`.
 * @retval k_ra8_err_invalid_size    `max_len == 0`.
 * @retval k_ra8_err_no_mem          Chapter does not fit in `max_len`.
 * @retval k_ra8_err_not_found       Manifest references a missing zip entry.
 *
 * @pre `book` non-NULL, `book->in_use == 1`.
 * @post On success, `*got_len > 0`. On failure, `*got_len == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_load_chapter(ra8_epub_book_t* book,
                                              uint16_t         idx,
                                              uint8_t*         out_xhtml,
                                              size_t           max_len,
                                              size_t*          got_len);

/* ===========================================================================
 * Public API -- table of contents
 * ===========================================================================
 */

/**
 * @brief Report which navigation document the TOC was parsed from.
 *
 * @param[in]  book     Open book.
 * @param[out] out_kind Receives a `ra8_epub_toc_kind_t` value.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Reported.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL, `out_kind` non-NULL.
 * @pre `book->in_use == 1`.
 * @post `*out_kind` is one of the `ra8_epub_toc_kind_t` enumerators.
 * @post `*out_kind == k_ra8_epub_toc_none` iff `toc_count == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_toc_kind(const ra8_epub_book_t* book, uint8_t* out_kind);

/**
 * @brief Report how many table-of-contents entries the book exposes.
 *
 * @details
 * The count is the flattened (depth-first) entry total parsed from the
 * book's NCX or nav document; `0` means no usable TOC was found. Use
 * `ra8_epub_get_toc_entry()` to read each entry's title, href, and depth.
 *
 * @param[in]  book      Open book.
 * @param[out] out_count Entry count.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Reported.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 *
 * @pre `book` non-NULL, `out_count` non-NULL.
 * @pre `book->in_use == 1`.
 * @post `*out_count <= k_ra8_epub_max_toc`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_toc_count(const ra8_epub_book_t* book, uint16_t* out_count);

/**
 * @brief Copy one table-of-contents entry into the caller's struct.
 *
 * @param[in]  book      Open book.
 * @param[in]  idx       Entry index, `[0, toc_count)`.
 * @param[out] out_entry Destination entry (title, href, depth) on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Entry copied.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 * @retval k_ra8_err_out_of_range    `idx >= toc_count`.
 *
 * @pre `book` non-NULL, `out_entry` non-NULL.
 * @pre `book->in_use == 1`.
 * @post On success, `*out_entry` is a NUL-terminated, bounded copy.
 * @post On failure, `*out_entry` is left unmodified.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epub_get_toc_entry(const ra8_epub_book_t* book, uint16_t idx, ra8_epub_toc_entry_t* out_entry);

/**
 * @brief Resolve a TOC entry to the spine chapter index it points into.
 *
 * @details
 * Makes the parsed TOC navigable: a TOC entry's `href` is an OPF-relative
 * path with an optional `#fragment` (e.g. `ch3.xhtml#sec2`), whereas
 * `ra8_epub_load_chapter()` is indexed by spine position. This strips any
 * fragment from the entry href and returns the index of the first spine
 * chapter (`chapter_paths`) whose path matches, suitable to hand to
 * `ra8_epub_load_chapter()`.
 *
 * @param[in]  book            Open book.
 * @param[in]  toc_idx         TOC entry index, `[0, toc_count)`.
 * @param[out] out_chapter_idx Spine chapter index on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Resolved; `*out_chapter_idx < chapter_count`.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 * @retval k_ra8_err_out_of_range    `toc_idx >= toc_count`.
 * @retval k_ra8_err_not_found       The entry points outside the spine.
 *
 * @pre `book` non-NULL, `out_chapter_idx` non-NULL.
 * @pre `book->in_use == 1`.
 * @post On success, `*out_chapter_idx` indexes a valid spine chapter.
 * @post On failure, `*out_chapter_idx` is left unmodified.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_toc_entry_to_chapter(const ra8_epub_book_t* book,
                                                      uint16_t               toc_idx,
                                                      uint16_t*              out_chapter_idx);

/* ===========================================================================
 * Public API -- metadata + cover + glyph rasterise
 * ===========================================================================
 */

/**
 * @brief Return Dublin Core metadata strings.
 *
 * @details
 * The returned `const char*` pointers alias `book` storage and are
 * valid until `ra8_epub_close()` is called. Missing fields point at the
 * empty string `""`, never NULL.
 *
 * @param[in]  book     Open book.
 * @param[out] out_meta Populated metadata struct on success.
 *
 * @retval k_ra8_ok                  Reported.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_metadata(const ra8_epub_book_t* book,
                                              ra8_epub_metadata_t*   out_meta);

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
 * @retval k_ra8_ok                  Cover copied.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized `book->in_use == 0`.
 * @retval k_ra8_err_invalid_size    `max_len == 0`.
 * @retval k_ra8_err_not_found       No cover image declared.
 * @retval k_ra8_err_no_mem          Cover does not fit in `max_len`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epub_get_cover_image(ra8_epub_book_t* book, uint8_t* out_buf, size_t max_len, size_t* got_len);

/**
 * @brief Copy the raw bytes of an arbitrary archive resource into the caller's
 *        buffer (#140 external stylesheets, and any href-referenced resource).
 *
 * @details
 * Generic by-path extraction from the open ZIP -- the same path the cover,
 * embedded fonts, and chapters use. @p path is resolved relative to the OPF
 * directory (`book->opf_dir`), with a fall-back to @p path taken as an
 * archive-rooted path; so `"style.css"`, `"css/main.css"`, and a bare
 * `"OEBPS/style.css"` all resolve. The bytes are returned exactly as stored
 * (decompressed); the caller owns @p out_buf and interprets them (e.g. CSS text
 * fed to `ra8_css_parse()` via a `ra8_reflow` css-loader).
 *
 * @param[in]  book    Open book (`in_use == 1`, archive active).
 * @param[in]  path    Resource path, OPF-dir-relative or archive-rooted, NUL-terminated.
 * @param[out] out_buf Destination buffer.
 * @param[in]  max_len Capacity of @p out_buf, bytes.
 * @param[out] got_len Bytes actually written.
 *
 * @retval k_ra8_ok                  Resource copied.
 * @retval k_ra8_err_null_ptr        Any pointer NULL.
 * @retval k_ra8_err_not_initialized Book not open / archive inactive.
 * @retval k_ra8_err_invalid_size    `max_len == 0`.
 * @retval k_ra8_err_not_found       No entry at @p path (prefixed or bare).
 * @retval k_ra8_err_no_mem          Resource does not fit in @p max_len.
 *
 * @pre `book->in_use == 1` and the archive is active.
 * @pre @p path, @p out_buf, @p got_len are non-NULL; @p max_len > 0.
 * @post On success `*got_len` bytes are written to @p out_buf.
 * @post On any error `*got_len == 0`.
 * @note Not thread-safe; single-threaded reader context.
 * @see ra8_epub_get_cover_image(), ra8_epub_get_embedded_font()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_get_resource(ra8_epub_book_t* book,
                                              const char*      path,
                                              uint8_t*         out_buf,
                                              size_t           max_len,
                                              size_t*          got_len);

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
