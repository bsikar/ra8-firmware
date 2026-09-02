/**
 * @file comic.h
 * @brief Unified demand-paged reader for comic-book archives -- CBZ (ZIP) and CBR (RAR).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * A comic archive is simply a container of page images (JPEG / PNG / GIF / BMP)
 * whose reading order is the sorted entry names. This facade opens either shape
 * -- a `.cbz` (a ZIP of images) or a `.cbr` (a RAR of images) -- behind one
 * interface, so the reader pages through both identically: detect the container,
 * build a sorted page index, then serve each page's *encoded image bytes* on
 * demand for the image decoder (`ra8_img_decode_blit`) to rasterise.
 *
 * @par Streaming, bounded RAM (#151, NASA P10 Rule 3)
 * The archive is never resident in full. The CBZ backend drives miniz's user-read
 * ZIP reader off the same seek+read seam that streams a large `.epub`
 * (`epub_open_streamed`): only the ZIP central directory and one entry at a
 * time are fetched. The CBR backend walks RAR block headers one at a time
 * (`ra8_rar.h`) and streams one member. So a full manga volume (hundreds of MB)
 * opens inside a small fixed budget -- the caller-owned page-index + name arena
 * plus one page's encoded image, never the whole file.
 *
 * The caller supplies all storage: a page-index array, a name arena sized for
 * the archive's page count, and (inside ::comic_t) one per-object bounded
 * miniz workspace. There is no heap (NASA Rule 3).
 *
 * @par Substitutability (SOLID L)
 * CBZ and CBR are drop-in interchangeable behind ::comic_page_read: the reader
 * opens by extension/magic and pages through the result with no knowledge of the
 * container. Both yield the same "sorted list of encoded page images".
 *
 * @code
 * comic_t     comic = {};
 * comic_page_t pages[k_max_pages] = {};
 * char            names[k_name_arena] = {};
 * ra8_err_t err = comic_open(&comic, sd_read, &file, file_len,
 *                              pages, k_max_pages, names, sizeof names);
 * for (uint32_t p = 0U; p < comic_page_count(&comic); ++p) {
 *   size_t got = 0U;
 *   err = comic_page_read(&comic, p, img_buf, sizeof img_buf, &got);
 *   ra8_img_decode_blit(&arena, img_buf, got, 0, 0, fb_w, fb_h, NULL, NULL);
 * }
 * comic_close(&comic);
 * @endcode
 *
 * @note One comic object is not thread-safe; independent objects share no
 *       miniz allocator state and may remain open simultaneously.
 * @note The CBZ backend needs miniz built with its archive APIs -- pull `epub`
 *       into the app `LIBS` (which compiles miniz with the ZIP reader and supplies
 *       the caller-owned `epub_miniz_alloc` arena used on host and target).
 *
 * @see ra8_rar.h                The clean-room RAR walker the CBR backend uses.
 * @see epub.h               The streaming ZIP open this mirrors for CBZ.
 * @see reflow_image.h       `ra8_img_decode_blit`, the page rasteriser.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "epub_miniz_alloc.h"
#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"
#include "unarch_gzip.h"
#include "unarch_tar.h"
#include "unarch_xz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef comic_read_fn
 * @brief Seek+read backing over the comic-archive bytes.
 * @details Identical in shape to the streaming EPUB read seam and
 *          ::ra8_rar_read_fn, so one `ra8_fs` / `ra8_vmem` backing drives EPUB, CBZ,
 *          and CBR alike. A return shorter than @p len signals end-of-file.
 * @param[in]  ctx    Opaque backing context (::comic_t::ctx).
 * @param[in]  offset Absolute byte offset within the archive.
 * @param[out] buf    Destination buffer (@p len writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes actually read (0 at/after EOF or on error).
 * @note Not thread-safe; the reader serialises access.
 * @since Version 0.1.0
 */
typedef size_t (*comic_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @enum comic_kind_t
 * @brief Which container an opened comic is.
 * @details Set by ::comic_open from the file magic; selects the backend that
 *          ::comic_page_read dispatches to.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_comic_kind_none = 0U, /**< Not opened / unrecognised. */
  k_comic_kind_cbz  = 1U, /**< ZIP of images (`.cbz`).    */
  k_comic_kind_cbr  = 2U, /**< RAR of images (`.cbr`).    */
  k_comic_kind_cbt  = 3U, /**< tar of images (`.cbt`).    */
} comic_kind_t;

/**
 * @enum comic_limits_t
 * @brief Fixed sizes for magic detection and the inline ZIP-reader storage.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_comic_magic_len = 8U,   /**< Bytes read to detect the container magic.    */
  k_comic_zip_bytes = 256U, /**< Inline storage for miniz's `mz_zip_archive`. */
} comic_limits_t;

/**
 * @struct comic_page_t
 * @brief One entry in the resident, sorted page index.
 *
 * @details Caller-owned array element populated by ::comic_open. @p name_off /
 *          @p name_len address the page's entry name inside the caller name arena.
 *          @p raw_size is the encoded image's byte length. The remaining fields
 *          are backend bookkeeping used by ::comic_page_read: @p zip_index for
 *          CBZ (the miniz central-directory index) and @p data_off / @p pack_size
 *          for CBR (the RAR member's data area). @p extractable is 0 only for a CBR
 *          member packed with a compressor this reader does not decode (RAR4
 *          legacy); RAR5-compressed members decode and stay extractable. @p rar_method
 *          is the normalised CBR compression method (::ra8_rar_method_t) used to route
 *          the page between the STORE copy and the RAR5 decompressor.
 *
 * @invariant `name_off + name_len <= names_len` of the owning comic.
 * @see comic_open()
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t name_off;    /**< Offset of the page name in the name arena.      */
  uint16_t name_len;    /**< Page name length in bytes.                      */
  uint8_t  extractable; /**< 1 if this reader can decode the page's bytes.   */
  uint8_t  rar_method;  /**< CBR: ::ra8_rar_method_t (0 = store); 0 for CBZ. */
  uint32_t zip_index;   /**< CBZ: miniz central-directory index.             */
  uint64_t data_off;    /**< CBR: absolute offset of the member data area.   */
  uint64_t pack_size;   /**< CBR: packed member size in bytes.               */
  uint64_t raw_size;    /**< Encoded image length in bytes (decoder input).  */
} comic_page_t;

/**
 * @struct comic_stream_t
 * @brief Stable seek+read descriptor bound to the CBZ backend's miniz reader.
 * @details Held inside ::comic_t so miniz's I/O-opaque pointer has a lifetime
 *          address; the CBR backend leaves it unused.
 * @invariant `read != NULL` once ::comic_open has bound a CBZ.
 * @since Version 0.1.0
 */
typedef struct {
  comic_read_fn read; /**< Backing reader (mirrors ::comic_t::read). */
  void*         ctx;  /**< Backing context.                          */
  uint64_t      size; /**< Archive length in bytes.                  */
} comic_stream_t;

/**
 * @struct comic_t
 * @brief One open comic archive: backing, kind, page index, and backend state.
 *
 * @details Populated by ::comic_open; treat every field as read-only afterward
 *          except through the API. @p pages / @p names are the caller-owned index
 *          and name arena; @p zip_storage holds miniz's `mz_zip_archive` inline
 *          (no heap) for a CBZ; @p rar holds the walker for a CBR.
 *
 * @invariant `page_count <= page_cap` and `names_len <= names_cap`.
 * @invariant `kind != k_comic_kind_none` between open and close.
 * @see comic_open()
 * @since Version 0.1.0
 */
typedef struct {
  comic_read_fn    read;       /**< Byte reader over the archive.            */
  void*            ctx;        /**< Context for @ref read.                   */
  uint64_t         size;       /**< Archive length in bytes.                 */
  comic_page_t*    pages;      /**< Caller page-index array.                 */
  uint32_t         page_cap;   /**< Capacity of @ref pages in entries.       */
  uint32_t         page_count; /**< Populated page count.                    */
  char*            names;      /**< Caller name arena.                       */
  uint32_t         names_cap;  /**< Capacity of @ref names in bytes.         */
  uint32_t         names_len;  /**< Bytes used in @ref names.                */
  comic_stream_t   stream;     /**< CBZ miniz I/O descriptor (stable addr).  */
  ra8_rar_t        rar;        /**< CBR walker state.                        */
  ra8_rar5_state_t rar5_state; /**< CBR RAR5 decompressor scratch (no heap). */
  unarch_tar_t     tar;        /**< CBT tar walker state.                    */
  comic_kind_t     kind;       /**< Detected container kind.                 */
  uint8_t          zip_active; /**< 1 = miniz reader is initialised (CBZ).   */
  /** @brief Inline storage for miniz's `mz_zip_archive` (CBZ; no heap). */
  alignas(max_align_t) uint8_t zip_storage[k_comic_zip_bytes];
  /** @brief Per-comic allocator descriptor bound through miniz's opaque pointer. */
  epub_miniz_arena_t miniz_arena;
  /** @brief Per-comic bounded allocator workspace (used only for CBZ). */
  epub_miniz_workspace_t miniz_workspace;
} comic_t;

/**
 * @brief Open a comic archive (CBZ or CBR) and build its sorted page index.
 *
 * @details Reads the leading magic through @p read, detects a ZIP (`.cbz`) or a
 *          RAR (`.cbr`) container, enumerates its image members into @p pages
 *          (filtered to decodable image extensions, names copied into @p names),
 *          and sorts the index by entry name so page 0 is the first page. On
 *          success at least one page is present.
 *
 * @param[out] c          Reader to populate (caller-owned).
 * @param[in]  read       Byte reader over the archive (non-NULL).
 * @param[in]  ctx        Context passed to @p read.
 * @param[in]  size       Archive length in bytes (> 0).
 * @param[in]  pages      Caller page-index array (non-NULL).
 * @param[in]  page_cap   Capacity of @p pages in entries (> 0).
 * @param[in]  names      Caller name arena (non-NULL).
 * @param[in]  names_cap  Capacity of @p names in bytes (> 0).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Comic opened; @p c bound with >= 1 page.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_size  @p size / a capacity is 0, a short magic read,
 *                                or the page index / name arena was too small.
 * @retval k_ra8_err_not_supported The bytes are neither a ZIP nor a RAR archive.
 * @retval k_ra8_err_not_found     A valid archive with no decodable image pages.
 * @retval k_ra8_err_*             A backend (miniz / RAR) or reader error.
 *
 * @pre @p read serves offsets `[0, size)` of the archive.
 * @pre @p pages / @p names out-live @p c and every read from it.
 * @post On k_ra8_ok, `comic_page_count(c) >= 1` and pages are name-sorted.
 * @post On any error @p c is left with `kind == k_comic_kind_none`.
 *
 * @note Not thread-safe. Call ::comic_close to release a CBZ's miniz reader.
 * @see comic_page_read()
 * @see comic_close()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t comic_open(comic_t*      c,
                                   comic_read_fn read,
                                   void*         ctx,
                                   uint64_t      size,
                                   comic_page_t* pages,
                                   uint32_t      page_cap,
                                   char*         names,
                                   uint32_t      names_cap);

/**
 * @brief Number of pages in an open comic.
 * @details Returns the count parsed by ::comic_open, guarding a NULL/unopened
 *          reader so a shelf view can query it safely.
 * @param[in] c Comic bound by ::comic_open (may be NULL).
 * @return The page count, or 0 for a NULL / unopened reader.
 * @retval 0 @p c is NULL or was never opened.
 * @pre @p c was populated by ::comic_open (or is NULL).
 * @pre @p c out-lives the call.
 * @post No state is modified (pure read).
 * @post The result equals the sorted index length.
 * @note Thread-safe: pure read of an immutable count.
 * @since Version 0.1.0
 */
static inline uint32_t comic_page_count(const comic_t* c)
{
  if (c == nullptr) {
    return 0U;
  }
  if (c->kind == k_comic_kind_none) {
    return 0U;
  }
  return c->page_count;
}

/**
 * @brief The detected container kind of an open comic.
 * @details Reports the kind ::comic_open set from the file magic, guarding a
 *          NULL / unopened reader so a caller can branch before a comic is bound.
 * @param[in] c Comic bound by ::comic_open (may be NULL).
 * @return ::comic_kind_t; ::k_comic_kind_none for a NULL / unopened reader.
 * @retval k_comic_kind_none @p c is NULL or was never opened.
 * @pre @p c was populated by ::comic_open (or is NULL).
 * @pre @p c out-lives the call.
 * @post No state is modified (pure read).
 * @post The result is stable for the lifetime of the open comic.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
static inline comic_kind_t comic_kind(const comic_t* c)
{
  if (c == nullptr) {
    return k_comic_kind_none;
  }
  return c->kind;
}

/**
 * @brief Read one page's manifest entry: name, encoded length, decodability.
 *
 * @details Pure index query -- no archive I/O. Copies the page's entry name into
 *          @p name_buf (clamped to @p name_cap) and reports its encoded byte
 *          length and whether this reader can decode it (0 for a compressed CBR
 *          member).
 *
 * @param[in]  c            Comic bound by ::comic_open (non-NULL).
 * @param[in]  page         Page index (`< comic_page_count(c)`).
 * @param[out] name_buf     Buffer for the entry name (may be NULL if @p name_cap 0).
 * @param[in]  name_cap     Capacity of @p name_buf in bytes.
 * @param[out] out_name_len Receives the copied name length (may be NULL).
 * @param[out] out_raw_size Receives the encoded image length (may be NULL).
 * @param[out] out_extractable Receives 1 if the page is decodable (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Outputs populated from the index.
 * @retval k_ra8_err_null_ptr      @p c was NULL.
 * @retval k_ra8_err_invalid_state @p c was never opened.
 * @retval k_ra8_err_out_of_range  @p page is at or past the page count.
 *
 * @pre @p c was populated by ::comic_open.
 * @pre @p name_buf holds @p name_cap writable bytes when @p name_cap > 0.
 * @post On k_ra8_ok every non-NULL output is populated from page @p page.
 * @post On any error no output is modified.
 *
 * @note Thread-safe: pure read of an immutable index.
 * @see comic_page_read()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t comic_page_info(const comic_t* c,
                                        uint32_t       page,
                                        char*          name_buf,
                                        uint16_t       name_cap,
                                        uint16_t*      out_name_len,
                                        uint64_t*      out_raw_size,
                                        uint8_t*       out_extractable);

/**
 * @brief Extract one page's encoded image bytes for the decoder.
 *
 * @details Streams the page's encoded image (JPEG / PNG / GIF / BMP) into @p buf:
 *          for a CBZ, miniz inflates the ZIP entry (STORE or DEFLATE) through the
 *          streaming reader; for a CBR, a STORE member is copied and a RAR5-compressed
 *          member is inflated by the clean-room decompressor, both via the RAR walker.
 *          The result is ready to hand to `ra8_img_decode_blit`. One call is one
 *          page's worth of I/O -- the demand-paged model.
 *
 * @param[in]  c    Comic bound by ::comic_open (non-NULL).
 * @param[in]  page Page index (`< comic_page_count(c)`).
 * @param[out] buf  Destination for the encoded image (non-NULL).
 * @param[in]  cap  Capacity of @p buf in bytes; must be >= the page `raw_size`.
 * @param[out] got  Receives the bytes written (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Page bytes written; `*got == raw_size`.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_state @p c was never opened.
 * @retval k_ra8_err_out_of_range  @p page is at or past the page count.
 * @retval k_ra8_err_not_supported A CBR page packed with a RAR compressor.
 * @retval k_ra8_err_no_mem        @p cap is smaller than the page `raw_size`.
 * @retval k_ra8_err_*             A backend extract / reader error.
 *
 * @pre @p c was populated by ::comic_open.
 * @pre @p buf holds at least @p cap writable bytes.
 * @post On k_ra8_ok, `buf[0..*got)` holds the page's encoded image.
 * @post On any error @p buf contents are unspecified and `*got == 0`.
 *
 * @note Not thread-safe: reuses the backend's decode state across calls.
 * @see comic_page_info()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
comic_page_read(comic_t* c, uint32_t page, uint8_t* buf, size_t cap, size_t* got);

/**
 * @brief Release an open comic's backend resources.
 * @details Ends the CBZ miniz reader (freeing its central directory through the
 *          per-object bounded arena); the CBR backend allocates nothing. Resets @p c to the
 *          unopened state. Idempotent after a failed open.
 * @param[in,out] c Comic from ::comic_open (non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Resources released; @p c reset.
 * @retval k_ra8_err_null_ptr @p c was NULL.
 * @pre @p c was populated by ::comic_open or zero-initialised.
 * @pre @p c out-lives the call.
 * @post `c->kind == k_comic_kind_none` and `c->zip_active == 0`.
 * @post No further ::comic_page_read may be issued until re-opened.
 * @note Not thread-safe.
 * @see comic_open()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t comic_close(comic_t* c);

/**
 * @enum comic_wrap_dims_t
 * @brief Alignment of the unwrap arena a wrapped open consumes.
 * @details ::comic_open_wrapped stores its flat-memory descriptor at the
 *          arena start, so the caller arena must be at least this aligned
 *          (any static or `alignas(8)` buffer qualifies).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_comic_wrap_align = 8U, /**< Required unwrap-arena alignment, bytes. */
} comic_wrap_dims_t;

/**
 * @brief Open a comic that may be gzip- or XZ-wrapped (`.cbt.gz`, `.tar.xz`).
 *
 * @details Probes the leading magic: a bare container (ZIP / RAR / tar)
 *          passes straight through to ::comic_open; a gzip or XZ
 *          wrapper is first decoded whole into the caller @p arena under
 *          the default decompression-limits policy, the unwrapped bytes
 *          are re-probed (a wrapper inside a wrapper is rejected as a
 *          nesting bomb), and the inner container is opened from the
 *          arena. The arena therefore must out-live the comic, exactly
 *          like the page-index and name buffers.
 *
 * @param[out] c           Reader to populate (caller-owned).
 * @param[in]  read        Byte reader over the outer file (non-NULL).
 * @param[in]  ctx         Context passed to @p read.
 * @param[in]  size        Outer file length in bytes (> 0).
 * @param[in]  pages       Caller page-index array (non-NULL).
 * @param[in]  page_cap    Capacity of @p pages in entries (> 0).
 * @param[in]  names       Caller name arena (non-NULL).
 * @param[in]  names_cap   Capacity of @p names in bytes (> 0).
 * @param[in]  arena       Unwrap arena (non-NULL, 8-aligned; out-lives @p c).
 * @param[in]  arena_cap   Capacity of @p arena in bytes.
 * @param[in]  xz_scratch  XZ session scratch (8-aligned,
 *                         > `k_unarch_xz_state_reserve` bytes; may be
 *                         NULL when XZ content is not expected -- an XZ
 *                         file is then rejected fail-closed).
 * @param[in]  xz_scratch_len Scratch length in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Comic opened; @p c bound with >= 1 page.
 * @retval k_ra8_err_null_ptr          A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_size      A zero size/capacity, a short magic
 *                                     read, a misaligned or undersized
 *                                     arena, or full index buffers.
 * @retval k_ra8_err_decomp_depth      The unwrapped bytes are another
 *                                     gzip/XZ wrapper (nesting bomb).
 * @retval k_ra8_err_decomp_*          The wrapper breached the policy.
 * @retval k_ra8_err_checksum_mismatch The gzip integrity check failed.
 * @retval k_ra8_err_not_supported     Not a recognised container or wrapper.
 * @retval k_ra8_err_*                 An unwrap or inner-open error.
 *
 * @pre @p read serves offsets `[0, size)` of the outer file.
 * @pre @p arena (and every other buffer) out-lives @p c and every read.
 * @post On k_ra8_ok the comic serves pages exactly like ::comic_open.
 * @post On any error @p c is left with `kind == k_comic_kind_none`.
 *
 * @note Not thread-safe (shares the single-client XZ pool / gzip state).
 * @see comic_open()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t comic_open_wrapped(comic_t*      c,
                                           comic_read_fn read,
                                           void*         ctx,
                                           uint64_t      size,
                                           comic_page_t* pages,
                                           uint32_t      page_cap,
                                           char*         names,
                                           uint32_t      names_cap,
                                           uint8_t*      arena,
                                           size_t        arena_cap,
                                           void*         xz_scratch,
                                           uint32_t      xz_scratch_len);

#ifdef __cplusplus
}
#endif
