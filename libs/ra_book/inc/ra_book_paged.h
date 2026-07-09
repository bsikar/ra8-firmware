/**
 * @file ra_book_paged.h
 * @brief Paged (demand-fetched) accessor mode for ra_book over ra_vmem (#163).
 *
 * @details
 * The default ra_book accessors (ra_book.h) are pure offset arithmetic over a
 * fully-resident inflated blob -- the right, zero-copy fast path for a book that
 * fits the resident budget (and XIP-friendly). This sub-header adds the *paged*
 * mode the #147/#162 memory hierarchy was built for: node / string / chapter
 * lookups copy the needed bytes out of an @ref ra_vmem page cache on demand, so
 * a GB-class EPUB / CBZ that does not fit RAM is read frame-by-frame with a
 * bounded resident working set instead of a whole-blob inflate.
 *
 * An @ref ra_book_src_t is the seam: it wraps *either* a resident blob base
 * (zero-copy, identical to ra_book.h) *or* an @ref ra_vmem cache fronting an
 * @ref ra_vsource object (paged). Reads flow through ::ra_book_src_read in both
 * modes, and the DOM walkers (::ra_book_chapter_text_src) dispatch on the mode,
 * so the reader uses one API and selects paged only when a book exceeds budget.
 * The resident path is byte-for-byte identical to ra_book_chapter_text(), so the
 * ereader golden render stays unchanged for RAM-fitting books.
 *
 * @note Including this header pulls in ra_vmem; the resident ra_book.h API has
 *       no such dependency. Only consumers that page books need this file.
 *
 * @see ra_book.h        Resident (XIP) accessors + the on-disk format.
 * @see ra_vmem.h        The SLRU page cache the paged mode reads through.
 * @see ra_vsource.h     The backing-object registry the cache loads from.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra_book.h"
#include "ra_err.h"
#include "ra_vmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra_book_src_t
 * @brief A book data source: resident (zero-copy) or paged (demand-fetched).
 *
 * @details Exactly one mode is active. Resident mode sets @ref base (the
 *          validated blob) with @ref vm NULL: reads are `base + off`, identical
 *          to the ra_book.h accessors. Paged mode sets @ref vm (an initialised
 *          ::ra_vmem fronting the book's ::ra_vsource object) with @ref base
 *          NULL: reads fault frames in through the cache and copy the requested
 *          slice out. The 100-byte header is copied into @ref hdr at bind time
 *          in both modes so header fields are always available without a fault.
 *          Populate via ::ra_book_src_resident / ::ra_book_src_paged; treat the
 *          fields as read-only afterwards.
 *
 * @invariant Exactly one of (`base != NULL`) and (`vm != NULL`) holds.
 * @invariant `hdr` is a copy of the blob's ::ra_book_header_t.
 *
 * @see ra_book_src_resident()
 * @see ra_book_src_paged()
 * @since Version 0.1.0
 */
typedef struct {
  const uint8_t*   base;        /**< Resident blob base, or NULL in paged mode.   */
  ra_vmem_t*       vm;          /**< Paged page cache, or NULL in resident mode.  */
  uint32_t         object_id;   /**< Paged: the book's ::ra_vsource object id.    */
  uint32_t         frame_bytes; /**< Paged: ::ra_vmem frame size, for slice math. */
  uint32_t         size;        /**< Total blob length in bytes.                  */
  ra_book_header_t hdr;         /**< Resident copy of the blob header.            */
} ra_book_src_t;

/**
 * @brief Bind a resident (already-inflated, fully resident) book as a source.
 *
 * @details The zero-copy fast path: @ref ra_book_src_t::base is set to @p base
 *          and reads become `base + off`, byte-for-byte identical to the
 *          ra_book.h accessors. The header is copied into @ref ra_book_src_t::hdr.
 *
 * @param[out] out  Source to populate (caller-owned).
 * @param[in]  base Validated `.rabook` blob base (non-NULL), as from ra_book_open().
 * @param[in]  size Blob length in bytes (`> 0`, == header total_size).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Source bound in resident mode.
 * @retval k_ra_err_null_ptr     @p out or @p base was NULL.
 * @retval k_ra_err_invalid_size @p size was zero.
 *
 * @pre  @p base was accepted by ra_book_validate() / ra_book_open().
 * @pre  @p out points at writable storage out-living the reads.
 * @post On success `out->base == base`, `out->vm == NULL`, `out->hdr` is the header.
 * @post On any non-ok return @p out is left unbound.
 *
 * @note Not thread-safe; reads over the immutable blob are otherwise re-entrant.
 * @see ra_book_src_paged()
 * @since Version 0.1.0
 */
[[nodiscard]] ra_err_t ra_book_src_resident(ra_book_src_t* out, const void* base, uint32_t size);

/**
 * @brief Bind a paged book source over an ::ra_vmem cache + ::ra_vsource object.
 *
 * @details The demand-fetch path for books that do not fit the resident budget.
 *          The caller has already registered the book's bytes as an ::ra_vsource
 *          paged (or XIP) object and initialised @p vm with ::ra_vsource_loader.
 *          This reads the 100-byte header through the cache into
 *          @ref ra_book_src_t::hdr so header fields never fault thereafter.
 *
 * @param[out] out         Source to populate (caller-owned).
 * @param[in]  vm          Initialised page cache fronting the book object (non-NULL).
 * @param[in]  object_id   The book's ::ra_vsource object id within @p vm's loader.
 * @param[in]  frame_bytes @p vm's frame size in bytes (`>= 2`); used for slice math.
 * @param[in]  size        Blob length in bytes (`> 0`, == header total_size).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Source bound in paged mode; header cached.
 * @retval k_ra_err_null_ptr     @p out or @p vm was NULL.
 * @retval k_ra_err_invalid_size @p size was zero or @p frame_bytes was `< 2`.
 * @retval k_ra_err_*            A page fault while reading the header (verbatim).
 *
 * @pre  @p vm was initialised with the book registered at @p object_id.
 * @pre  @p frame_bytes equals the frame size @p vm was configured with.
 * @post On success `out->vm == vm`, `out->base == NULL`, `out->hdr` is the header.
 * @post On any non-ok return @p out is left unbound.
 *
 * @note Not thread-safe; @p vm's pin/evict state is mutated by reads.
 * @see ra_book_src_resident()
 * @since Version 0.1.0
 */
[[nodiscard]] ra_err_t ra_book_src_paged(ra_book_src_t* out,
                                         ra_vmem_t*     vm,
                                         uint32_t       object_id,
                                         uint32_t       frame_bytes,
                                         uint32_t       size);

/**
 * @brief Copy a byte range out of a book source (resident memcpy or paged fault).
 *
 * @details The single read primitive both modes share. Resident mode is a
 *          `memcpy(dst, base + off, len)`. Paged mode walks the range frame by
 *          frame: ::ra_vmem_get pins the frame holding the current offset, the
 *          overlapping slice is copied to @p dst, and ::ra_vmem_put releases it
 *          before advancing -- so at most one frame is pinned at a time and the
 *          resident working set stays bounded regardless of @p len.
 *
 * @param[in]  src Bound book source.
 * @param[in]  off Byte offset within the blob (`off + len <= src->size`).
 * @param[out] dst Destination buffer receiving @p len bytes (non-NULL).
 * @param[in]  len Number of bytes to copy.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Bytes copied into @p dst.
 * @retval k_ra_err_null_ptr      @p src or @p dst was NULL.
 * @retval k_ra_err_invalid_state @p src is bound to neither mode.
 * @retval k_ra_err_out_of_range  @p off + @p len exceeds @p src->size.
 * @retval k_ra_err_*             A page fault (paged mode only; returned verbatim).
 *
 * @pre  @p src was populated by ra_book_src_resident() / ra_book_src_paged().
 * @pre  @p dst addresses at least @p len writable bytes.
 * @post On success `dst[0..len)` holds the blob bytes at @p off.
 * @post On any non-ok return @p dst content is unspecified and no pin is held.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
[[nodiscard]] ra_err_t
ra_book_src_read(const ra_book_src_t* src, uint32_t off, void* dst, uint32_t len);

/**
 * @brief Extract one chapter's plain text from a book source (resident or paged).
 *
 * @details The source-aware counterpart of ra_book_chapter_text(): same output
 *          contract (whitespace-collapsed text runs, a newline at each
 *          block-level element, not NUL-terminated), but reads the DOM through
 *          @p src so a paged book is walked frame-by-frame. In resident mode it
 *          delegates to ra_book_chapter_text() and is byte-for-byte identical;
 *          in paged mode it copies each visited node and string out on demand,
 *          pinning only what it is reading.
 *
 * @param[in]  src         Bound book source.
 * @param[in]  chapter_idx Spine chapter index (`< src->hdr.chapter_count`).
 * @param[out] out         Destination text buffer (non-NULL).
 * @param[in]  cap         Capacity of @p out in bytes.
 * @param[out] out_len     Receives the text byte length written.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Chapter text extracted.
 * @retval k_ra_err_null_ptr     @p src, @p out, or @p out_len was NULL.
 * @retval k_ra_err_invalid_arg  @p chapter_idx is out of range.
 * @retval k_ra_err_invalid_size Output did not fit @p cap, or DOM nesting
 *                               exceeded the bounded walk stack.
 * @retval k_ra_err_*            A page fault (paged mode only; returned verbatim).
 *
 * @pre  @p src was populated by ra_book_src_resident() / ra_book_src_paged().
 * @pre  @p chapter_idx is less than `src->hdr.chapter_count`.
 * @post On k_ra_ok, `out[0..*out_len)` is the plain-text run (not NUL-terminated).
 * @post On error, @p out content is unspecified.
 *
 * @note Not thread-safe.
 * @see ra_book_chapter_text()
 * @since Version 0.1.0
 */
[[nodiscard]] ra_err_t ra_book_chapter_text_src(const ra_book_src_t* src,
                                                uint32_t             chapter_idx,
                                                char*                out,
                                                size_t               cap,
                                                size_t*              out_len);

/**
 * @brief Warm the cache frame holding a chapter's first content bytes, without
 *        pinning it (single-threaded read-ahead for the display-flush idle window).
 *
 * @details The reader-facing wrapper #207 wires into the flush-idle window: after
 *          rendering the current page and issuing the panel flush, warm the
 *          adjacent chapters (N+1 for forward reading, N-1 for a back-flip) so the
 *          next chapter-crossing page turn finds them resident. Resolves @p
 *          chapter_idx to the byte offset of its root DOM node -- the first content
 *          range ::ra_book_chapter_text_src reads for that chapter -- and hands it
 *          to ::ra_vmem_prefetch, which does a bounded get+put so the page ends
 *          resident but unpinned (SLRU/2Q probationary: a wrong read-ahead guess
 *          ages out before hot data, no prefetch backfire on a fast skim).
 *          Best-effort and transparent: warming only changes cache residency, never
 *          the bytes a later read returns, so rendered output is unchanged.
 *          Resident sources are already wholly in RAM and are rejected as a no-op.
 *
 * @param[in] src         Bound (paged) book source.
 * @param[in] chapter_idx Spine chapter index to warm (`< src->hdr.chapter_count`).
 *
 * @return ra_err_t Error code (callers on the idle path typically discard it).
 * @retval k_ra_ok                The chapter's root-node frame is resident, unpinned.
 * @retval k_ra_err_null_ptr      @p src was NULL.
 * @retval k_ra_err_invalid_state @p src is resident (or unbound): nothing to page.
 * @retval k_ra_err_invalid_arg   @p chapter_idx is out of range.
 * @retval k_ra_err_*             A chapter-record read or the warm faulted (verbatim).
 *
 * @pre  @p src was populated by ra_book_src_paged() (paged mode, `src->vm != NULL`).
 * @pre  Called from the single owning context (e.g. the reader idle window).
 * @post On k_ra_ok the chapter's first content frame is resident with a net-zero
 *       change to its pin count.
 * @post On any return no cache frame is left pinned by this call.
 *
 * @note Not thread-safe. Single-threaded read-ahead only.
 * @note Warming is transparent: it never alters the bytes a subsequent read sees.
 *
 * @see ra_vmem_prefetch()          The bounded get+put warm this maps a chapter to.
 * @see ra_book_chapter_text_src()  The demand read this warms ahead of.
 * @since Version 0.1.0
 */
[[nodiscard]] ra_err_t ra_book_src_prefetch_chapter(const ra_book_src_t* src, uint32_t chapter_idx);

#ifdef __cplusplus
}
#endif
