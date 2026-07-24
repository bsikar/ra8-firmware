/**
 * @file ra8_book_paged.c
 * @brief Paged book-source layer: copy-out reads over ra8_vmem (#163).
 *
 * @details
 * Implements the @ref ra8_book_src_t seam from ra8_book_paged.h. A source is
 * either resident (a validated blob base; reads are a `memcpy`) or paged (an
 * @ref ra8_vmem cache fronting an @ref ra8_vsource object; reads fault frames in
 * and copy the requested slice out). ::ra8_book_src_read is the single primitive
 * the DOM walkers build on; the paged path pins at most one frame at a time, so
 * the resident working set stays bounded by the cache budget irrespective of how
 * much of the book a chapter walk touches.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "ra8_book_paged.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_book_internal.h"
#include "ra8_check.h"
#include "ra8_vmem.h"

/** @brief Log tag for paged-source diagnostics. */
static const char* const s_tag_paged = "ra8_book_paged";

/**
 * @enum ra8_book_paged_bound_t
 * @brief Loop-bound headroom for the frame-by-frame copy-out (NASA Rule 2).
 * @details A copy of `len` bytes spans at most `ceil(len/frame_bytes) + 1`
 *          frames (a leading partial frame plus full frames); the `+2` guard
 *          covers the partial head and tail so the bounded loop can never spin.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_paged_span_pad  = 2U, /**< Extra frame iterations over the exact span.  */
  k_ra8_book_paged_min_frame = 2U, /**< Smallest accepted frame size (Rule-2 bound). */
} ra8_book_paged_bound_t;

ra8_err_t ra8_book_src_resident(ra8_book_src_t* out, const void* base, uint32_t size)
{
  RA8_CHECK_NULL_PTR(out, s_tag_paged, "resident: null out");
  RA8_CHECK_NULL_PTR(base, s_tag_paged, "resident: null base");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  out->base        = (const uint8_t*)base;
  out->vm          = nullptr;
  out->object_id   = 0U;
  out->frame_bytes = 0U;
  out->size        = size;
  (void)memcpy(&out->hdr, base, sizeof(ra8_book_header_t));
  return k_ra8_ok;
}

ra8_err_t ra8_book_src_paged(ra8_book_src_t* out,
                             ra8_vmem_t*     vm,
                             uint32_t        object_id,
                             uint32_t        frame_bytes,
                             uint32_t        size)
{
  RA8_CHECK_NULL_PTR(out, s_tag_paged, "paged: null out");
  RA8_CHECK_NULL_PTR(vm, s_tag_paged, "paged: null vm");
  /* frame_bytes >= 2 keeps the copy-out loop bound (len/frame_bytes + 2)
   * provably non-overflowing for any uint32_t len. */
  if ((size == 0U) || (frame_bytes < (uint32_t)k_ra8_book_paged_min_frame)) {
    return k_ra8_err_invalid_size;
  }
  out->base        = nullptr;
  out->vm          = vm;
  out->object_id   = object_id;
  out->frame_bytes = frame_bytes;
  out->size        = size;
  /* Cache the header so its fields never fault during a walk. */
  return ra8_book_src_read(out, 0U, &out->hdr, (uint32_t)sizeof(ra8_book_header_t));
}

/**
 * @brief Copy a byte range out of a paged source, frame by frame.
 *
 * @details The demand-fetch worker behind ::ra8_book_src_read: walks @p len bytes
 *          at @p off through the cache, pinning the frame holding the current
 *          offset (::ra8_vmem_get), copying the overlapping slice, and releasing
 *          it (::ra8_vmem_put) before advancing -- one frame pinned at a time.
 *          The loop is bounded by `len / frame_bytes + 2` (frame_bytes >= 2 is
 *          enforced at bind time, so the bound never overflows).
 *
 * @param[in]  src Paged source (`src->vm` non-NULL, `src->base` NULL).
 * @param[in]  off Byte offset within the blob (range already validated).
 * @param[out] dst Destination buffer receiving @p len bytes.
 * @param[in]  len Number of bytes to copy (non-zero).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               All @p len bytes copied into @p dst.
 * @retval k_ra8_err_out_of_range The loop drained before @p len bytes (unreachable
 *                               for a valid range / `frame_bytes >= 2`).
 * @retval k_ra8_err_*            A ::ra8_vmem_get / ::ra8_vmem_put fault (verbatim).
 *
 * @pre  @p src is paged and `off + len <= src->size`.
 * @pre  @p dst addresses at least @p len writable bytes.
 * @post On success `dst[0..len)` holds the blob bytes at @p off; no pin is held.
 * @post On any non-ok return @p dst content is unspecified and no pin is held.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_book_src_read_paged(const ra8_book_src_t* src, uint32_t off, void* dst, uint32_t len)
{
  uint8_t*       d         = (uint8_t*)dst;
  uint32_t       cur       = off;
  uint32_t       remaining = len;
  const uint32_t max_iter  = (len / src->frame_bytes) + (uint32_t)k_ra8_book_paged_span_pad;
  for (uint32_t it = 0U; (it < max_iter) && (remaining > 0U); ++it) {
    void*           page = nullptr;
    const ra8_err_t ge   = ra8_vmem_get(src->vm, src->object_id, (uint64_t)cur, &page);
    if (ge != k_ra8_ok) {
      return ge;
    }
    const uint32_t in_frame = cur % src->frame_bytes;
    const uint32_t avail    = src->frame_bytes - in_frame;
    const uint32_t n        = (remaining < avail) ? remaining : avail;
    (void)memcpy(d, (const uint8_t*)page + in_frame, n);
    const ra8_err_t pe = ra8_vmem_put(src->vm, page);
    if (pe != k_ra8_ok) {
      return pe;
    }
    d += n;
    cur += n;
    remaining -= n;
  }
  return (remaining == 0U) ? k_ra8_ok : k_ra8_err_out_of_range;
}

ra8_err_t ra8_book_src_read(const ra8_book_src_t* src, uint32_t off, void* dst, uint32_t len)
{
  RA8_CHECK_NULL_PTR(src, s_tag_paged, "read: null src");
  RA8_CHECK_NULL_PTR(dst, s_tag_paged, "read: null dst");
  if (((uint64_t)off + (uint64_t)len) > (uint64_t)src->size) {
    return k_ra8_err_out_of_range;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  if (src->base != nullptr) {
    (void)memcpy(dst, src->base + off, len);
    return k_ra8_ok;
  }
  if (src->vm == nullptr) {
    return k_ra8_err_invalid_state;
  }
  return priv_book_src_read_paged(src, off, dst, len);
}

/* ===========================================================================
 * Image sub-rect reads (#342): the image-pool addressing contract -- descriptor
 * stride, pool base, 4bpp nibble packing, and odd-width parity -- lives here in
 * the library that owns the format, not open-coded in every image renderer. The
 * loupe / tile / thumbnail consumers keep only their geometry and blit and call
 * ra8_book_src_image_rect() for "read me this rectangle" as unpacked gray8.
 * =========================================================================== */

/**
 * @enum ra8_book_image_rect_const_t
 * @brief 4bpp nibble packing + the per-read pixel budget (no magic numbers).
 * @details A @ref k_ra8_book_image_gray4 pool byte holds two pixels; the high
 *          nibble is the even flat index, the low nibble the odd. A 4-bit sample
 *          expands to gray8 by replicating the nibble into both halves
 *          (`(nib << 4) | nib`). ::ra8_book_src_image_rect unpacks each row in
 *          spans of at most @ref k_ra8_book_rect_chunk_px pixels so the staging
 *          buffer -- and every ::ra8_book_src_read -- stays small and bounded.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_gray4_nib_lo  = 0x0FU, /**< Low-nibble mask (odd flat index).           */
  k_ra8_book_gray4_nib_sh  = 4U,    /**< Nibble shift and 4->8 bit replicate amount. */
  k_ra8_book_gray4_ppb     = 2U,    /**< Pixels packed per pool byte.                */
  k_ra8_book_rect_chunk_px = 512U,  /**< Max pixels unpacked per bounded span read.  */
  k_ra8_book_rect_chunk_pad =
    2U, /**< Loop-bound headroom over ceil(w / k_ra8_book_rect_chunk_px). */
} ra8_book_image_rect_const_t;

/**
 * @brief Unpack one horizontal gray4 run `[x, x+w)` of source row @p ry to gray8.
 *
 * @details The per-row worker of ::ra8_book_src_image_rect. Walks the run in
 *          spans of at most ::k_ra8_book_rect_chunk_px pixels: each span's flat
 *          nibble index maps to an absolute pool offset
 *          (`image_pool_off + data_off + (flat >> 1)`), the packed span is copied
 *          out via ::ra8_book_src_read (bounds-checked, paged-fault-aware), and
 *          each 4-bit sample is replicated into a gray8 byte. Tracking the
 *          absolute flat index per pixel keeps the high/low nibble parity correct
 *          across span boundaries and for odd image widths.
 *
 * @param[in]  src Bound book source.
 * @param[in]  img Gray4 image descriptor (caller already checked the format).
 * @param[in]  x   Run left edge in source pixels (`x + w <= img->width`).
 * @param[in]  ry  Source row index (`< img->height`).
 * @param[in]  w   Run width in pixels (`> 0`).
 * @param[out] out Destination gray8 run (`>= w` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The run was unpacked into @p out.
 * @retval k_ra8_err_out_of_range A span it addresses leaves the blob.
 * @retval k_ra8_err_*            A ::ra8_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p img is a gray4 descriptor within @p src.
 * @pre  @p out addresses at least @p w writable bytes.
 * @post On k_ra8_ok, `out[i]` is the gray8 expansion of pixel `(x+i, ry)`.
 * @post On any non-ok return @p out content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_book_image_row(const ra8_book_src_t*   src,
                                     const ra8_book_image_t* img,
                                     uint32_t                x,
                                     uint32_t                ry,
                                     uint32_t                w,
                                     uint8_t*                out)
{
  const uint32_t row_flat = (ry * (uint32_t)img->width) + x;
  uint8_t        packed[(k_ra8_book_rect_chunk_px / k_ra8_book_gray4_ppb) + 1U];
  uint32_t       done = 0U;
  const uint32_t max_iter =
    (w / (uint32_t)k_ra8_book_rect_chunk_px) + (uint32_t)k_ra8_book_rect_chunk_pad;
  for (uint32_t it = 0U; it < max_iter; ++it) {
    if (done >= w) {
      break; /* whole run unpacked; the count bound above is the loop invariant */
    }
    const uint32_t remain = w - done;
    const uint32_t cpx =
      (remain < (uint32_t)k_ra8_book_rect_chunk_px) ? remain : (uint32_t)k_ra8_book_rect_chunk_px;
    const uint32_t flat0 = row_flat + done;
    const uint32_t first = flat0 >> 1U;
    const uint32_t last  = (flat0 + cpx - 1U) >> 1U;
    const uint32_t n     = (last - first) + 1U;
    const uint64_t off =
      (uint64_t)src->hdr.image_pool_off + (uint64_t)img->data_off + (uint64_t)first;
    if ((off + (uint64_t)n) > (uint64_t)src->size) {
      return k_ra8_err_out_of_range; /* descriptor / geometry points past the blob */
    }
    const ra8_err_t re = ra8_book_src_read(src, (uint32_t)off, packed, n);
    if (re != k_ra8_ok) {
      return re;
    }
    for (uint32_t i = 0U; i < cpx; ++i) {
      const uint32_t flat = flat0 + i;
      const uint8_t  byte = packed[(flat >> 1U) - first];
      const uint8_t nib = ((flat & 1U) != 0U) ? (uint8_t)(byte & (uint8_t)k_ra8_book_gray4_nib_lo)
                                              : (uint8_t)(byte >> (uint8_t)k_ra8_book_gray4_nib_sh);
      out[done + i]     = (uint8_t)((nib << (uint8_t)k_ra8_book_gray4_nib_sh) | nib);
    }
    done += cpx;
  }
  return (done == w) ? k_ra8_ok : k_ra8_err_out_of_range;
}

ra8_err_t ra8_book_src_image(const ra8_book_src_t* src, uint32_t idx, ra8_book_image_t* out_img)
{
  RA8_CHECK_NULL_PTR(src, s_tag_paged, "image: null src");
  RA8_CHECK_NULL_PTR(out_img, s_tag_paged, "image: null out_img");
  if (idx >= src->hdr.image_count) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t off = src->hdr.image_off + (idx * (uint32_t)sizeof(ra8_book_image_t));
  return ra8_book_src_read(src, off, out_img, (uint32_t)sizeof(ra8_book_image_t));
}

ra8_err_t ra8_book_src_image_rect(const ra8_book_src_t*   src,
                                  const ra8_book_image_t* img,
                                  uint32_t                x,
                                  uint32_t                y,
                                  uint32_t                w,
                                  uint32_t                h,
                                  uint8_t*                out,
                                  uint32_t                out_stride)
{
  RA8_CHECK_NULL_PTR(src, s_tag_paged, "image_rect: null src");
  RA8_CHECK_NULL_PTR(img, s_tag_paged, "image_rect: null img");
  RA8_CHECK_NULL_PTR(out, s_tag_paged, "image_rect: null out");
  if (img->format != (uint8_t)k_ra8_book_image_gray4) {
    return k_ra8_err_invalid_arg; /* only 4bpp packed grayscale is unpackable here */
  }
  if ((w == 0U) || (h == 0U) || (out_stride < w)) {
    return k_ra8_err_invalid_arg;
  }
  if ((((uint64_t)x + (uint64_t)w) > (uint64_t)img->width) ||
      (((uint64_t)y + (uint64_t)h) > (uint64_t)img->height)) {
    return k_ra8_err_out_of_range; /* sub-rect not fully inside the image */
  }
  for (uint32_t row = 0U; row < h; ++row) {
    const ra8_err_t re =
      priv_book_image_row(src, img, x, y + row, w, &out[(size_t)row * (size_t)out_stride]);
    if (re != k_ra8_ok) {
      return re;
    }
  }
  return k_ra8_ok;
}

/* ===========================================================================
 * Paged plain-text extraction (#163): same output as ra8_book_chapter_text but
 * the DOM is read frame-by-frame through an ra8_book_src_t / ra8_vmem cache, so a
 * book that exceeds the resident budget is walked with a bounded working set.
 * The resident path above is untouched; these helpers reuse the shared,
 * source-agnostic emit_text / emit_break / is_block leaf helpers on staging
 * buffers, so paged output is byte-for-byte identical to the resident walk.
 * =========================================================================== */

/**
 * @enum ra8_book_paged_walk_bound_t
 * @brief Staging sizes and loop bounds for the paged text walk (no magic numbers).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_paged_strbuf = 256U, /**< Staging chunk for a paged text run.       */
  k_ra8_book_paged_tagbuf = 64U,  /**< Staging for one (short) element tag name. */
  k_ra8_book_paged_pad    = 2U,   /**< Loop-bound headroom over the exact span.  */
} ra8_book_paged_walk_bound_t;

/**
 * @brief Read one DOM node record out of a paged/resident book source by index.
 *
 * @details Translates the node index to its blob byte offset
 *          (`node_off + idx * sizeof(ra8_book_node_t)`) and copies the 24-byte
 *          record out via ::ra8_book_src_read, which bounds-checks the range and
 *          faults the frame in for a paged source.
 *
 * @param[in]  src Bound book source.
 * @param[in]  idx Node-table index to read.
 * @param[out] out Receives the node record.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Node copied into @p out.
 * @retval k_ra8_err_out_of_range @p idx lies outside the node table.
 * @retval k_ra8_err_*            A ::ra8_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p out is writable.
 * @pre  @p idx is intended to index a valid node (verified by the read bounds).
 * @post On success @p out holds the node at @p idx.
 * @post On any non-ok return @p out content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ra8_book_paged_node(const ra8_book_src_t* src, uint32_t idx, ra8_book_node_t* out)
{
  const uint32_t off = src->hdr.node_off + (idx * (uint32_t)sizeof(ra8_book_node_t));
  return ra8_book_src_read(src, off, out, (uint32_t)sizeof(ra8_book_node_t));
}

/**
 * @brief Copy a short NUL-terminated string (a tag name) out into a buffer.
 *
 * @details Reads up to `cap - 1` bytes at @p abs_off and NUL-terminates at the
 *          first embedded NUL or at the buffer end. Used for element tag names,
 *          which are short; a name longer than the buffer is truncated, which at
 *          worst makes ::ra8_book_is_block treat it as inline (no break) -- never
 *          a correctness issue for real markup.
 *
 * @param[in]  src     Bound book source.
 * @param[in]  abs_off Absolute blob offset of the string start.
 * @param[out] buf     Destination buffer (>= @p cap bytes).
 * @param[in]  cap     Capacity of @p buf in bytes (>= 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               String (possibly truncated) copied + terminated.
 * @retval k_ra8_err_out_of_range @p abs_off is at or past the blob end.
 * @retval k_ra8_err_*            A ::ra8_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p buf has room for @p cap bytes.
 * @pre  @p cap is at least one (for the NUL terminator).
 * @post On success @p buf is a NUL-terminated string.
 * @post On any non-ok return @p buf content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
ra8_book_paged_str_short(const ra8_book_src_t* src, uint32_t abs_off, char* buf, uint32_t cap)
{
  if (abs_off >= src->size) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t  remain = src->size - abs_off;
  const uint32_t  want   = cap - 1U;
  const uint32_t  chunk  = (remain < want) ? remain : want;
  const ra8_err_t re     = ra8_book_src_read(src, abs_off, buf, chunk);
  if (re != k_ra8_ok) {
    return re;
  }
  uint32_t nlen = 0U;
  while ((nlen < chunk) && (buf[nlen] != '\0')) {
    ++nlen;
  }
  buf[nlen] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Emit one text run from a paged source, collapsing whitespace.
 *
 * @details Streams the NUL-terminated run at @p abs_off through a staging buffer
 *          in `k_ra8_book_paged_strbuf`-sized chunks, calling the shared
 *          ::ra8_book_emit_text on each chunk. Chunking keeps the resident
 *          footprint bounded for arbitrarily long runs; the `at_break` flag
 *          threads across chunks so whitespace collapsing is identical to the
 *          resident single-pass walk.
 *
 * @param[in]     src      Bound book source.
 * @param[in]     abs_off  Absolute blob offset of the NUL-terminated run.
 * @param[out]    out      Destination text buffer.
 * @param[in]     cap      Capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; advanced as text is emitted.
 * @param[in,out] at_break Whitespace-collapse carry flag threaded across chunks.
 * @param[out]    io_err   Set to the fault code on a read fault; untouched otherwise.
 *
 * @return bool Emit result.
 * @retval true  The whole run was emitted; @p *io_err is unchanged.
 * @retval false Output buffer overflowed (@p io_err untouched), or a page fault
 *               occurred (@p *io_err set to the fault code).
 *
 * @pre  @p src is bound; @p out has room for @p cap bytes.
 * @pre  @p pos and @p at_break are non-null and valid.
 * @post On true with `*io_err == k_ra8_ok` the run is fully emitted.
 * @post On a fault `*io_err` holds the fault code and false is returned.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool ra8_book_paged_emit_run(const ra8_book_src_t* src,
                                    uint32_t              abs_off,
                                    char*                 out,
                                    size_t                cap,
                                    size_t*               pos,
                                    bool*                 at_break,
                                    ra8_err_t*            io_err)
{
  char           buf[k_ra8_book_paged_strbuf];
  uint32_t       off = abs_off;
  const uint32_t max_iter =
    (src->size / (uint32_t)(k_ra8_book_paged_strbuf - 1U)) + (uint32_t)k_ra8_book_paged_pad;
  for (uint32_t it = 0U; it < max_iter; ++it) {
    if (off >= src->size) {
      return true; /* Ran to blob end without a NUL (degenerate); treat as done. */
    }
    const uint32_t  remain = src->size - off;
    const uint32_t  want   = (uint32_t)(k_ra8_book_paged_strbuf - 1U);
    const uint32_t  chunk  = (remain < want) ? remain : want;
    const ra8_err_t re     = ra8_book_src_read(src, off, buf, chunk);
    if (re != k_ra8_ok) {
      *io_err = re;
      return false;
    }
    uint32_t nlen = 0U;
    while ((nlen < chunk) && (buf[nlen] != '\0')) {
      ++nlen;
    }
    buf[nlen] = '\0';
    if (!ra8_book_emit_text(out, cap, pos, buf, at_break)) {
      return false; /* Output overflow. */
    }
    if (nlen < chunk) {
      return true; /* Hit the real NUL terminator. */
    }
    off += nlen; /* nlen == chunk: advance and read the next chunk. */
  }
  return true;
}

/**
 * @brief Process one popped node in the paged text walk.
 *
 * @details The per-node body of ::ra8_book_walk_text_paged, split out to keep both
 *          functions within the size/complexity budget. Reads the node, pushes
 *          its sibling, then either emits a text run (::ra8_book_paged_emit_run)
 *          or, for an element, inserts a block break (::ra8_book_is_block over a
 *          staged tag name) and pushes its first child. Mutates the caller's
 *          stack/sp and whitespace-collapse @p at_break in place.
 *
 * @param[in]     src      Bound (paged) book source.
 * @param[in]     n        Node index to visit (already non-nil).
 * @param[out]    out      Destination plain-text buffer.
 * @param[in]     cap      Capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; advanced as text is emitted.
 * @param[in,out] at_break Whitespace-collapse carry flag.
 * @param[in,out] stack    Caller walk stack of length @c k_ra8_book_xhtml_stack.
 * @param[in,out] sp       Stack pointer (live entry count); pushed up to twice.
 * @param[out]    io_err   Set to the fault code on a read fault; untouched otherwise.
 *
 * @return bool Visit result.
 * @retval true  Node handled; the walk may continue.
 * @retval false Output overflow, stack full, or a read fault (see @p io_err).
 *
 * @pre  @p src is paged; @p n indexes a node in the blob.
 * @pre  @p stack / @p sp / @p at_break are the walk's live state.
 * @post On true the node's text/break is emitted and children/sibling pushed.
 * @post On false the walk must stop; @p io_err is set iff a fault occurred.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool priv_paged_visit_node(const ra8_book_src_t* src,
                                  uint32_t              n,
                                  char*                 out,
                                  size_t                cap,
                                  size_t*               pos,
                                  bool*                 at_break,
                                  uint32_t*             stack,
                                  uint32_t*             sp,
                                  ra8_err_t*            io_err)
{
  ra8_book_node_t node = {};
  const ra8_err_t ne   = ra8_book_paged_node(src, n, &node);
  if (ne != k_ra8_ok) {
    *io_err = ne;
    return false;
  }
  if (*sp >= k_ra8_book_xhtml_stack) {
    return false;
  }
  stack[(*sp)++] = node.next_sibling; /* sibling chain after this subtree */
  if (node.kind == (uint8_t)k_ra8_book_node_text) {
    return ra8_book_paged_emit_run(src,
                                   src->hdr.string_off + node.text_off,
                                   out,
                                   cap,
                                   pos,
                                   at_break,
                                   io_err);
  }
  char            tag[k_ra8_book_paged_tagbuf] = {};
  const ra8_err_t te = ra8_book_paged_str_short(src,
                                                src->hdr.string_off + node.name_off,
                                                tag,
                                                (uint32_t)k_ra8_book_paged_tagbuf);
  if (te != k_ra8_ok) {
    *io_err = te;
    return false;
  }
  bool ok = true;
  if (ra8_book_is_block(tag)) {
    ok = ra8_book_emit_break(out, cap, pos, at_break);
  }
  if (ok && (*sp < k_ra8_book_xhtml_stack)) {
    stack[(*sp)++] = node.first_child; /* descend, pre-order */
  }
  return ok;
}

/**
 * @brief Bounded pre-order text walk over a paged book source.
 *
 * @details The paged counterpart of ra8_book_walk_text(): identical iterative,
 *          recursion-free pre-order traversal and identical output, dispatching
 *          each popped node to ::priv_paged_visit_node. At most one cache frame
 *          is pinned at a time, so the resident working set stays bounded.
 *
 * @param[in]     src        Bound (paged) book source.
 * @param[in]     root       Node index of the subtree root.
 * @param[in]     node_count Total node count, for the iteration guard.
 * @param[out]    out        Destination plain-text buffer.
 * @param[in]     cap        Capacity of @p out in bytes.
 * @param[in,out] pos        Current write offset; advanced as text is emitted.
 * @param[out]    io_err     Set to the fault code on a read fault; k_ra8_ok otherwise.
 *
 * @return bool Walk result.
 * @retval true  Subtree fully serialised (check @p io_err for a clean run).
 * @retval false Overflow, stack/guard exhaustion, or a read fault (see @p io_err).
 *
 * @pre  @p src is a paged source bound by ra8_book_src_paged().
 * @pre  @p root is a node index within the blob.
 * @post On true with `*io_err == k_ra8_ok`, @p out holds the chapter text.
 * @post On a fault `*io_err` carries the fault code.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool ra8_book_walk_text_paged(const ra8_book_src_t* src,
                                     uint32_t              root,
                                     uint32_t              node_count,
                                     char*                 out,
                                     size_t                cap,
                                     size_t*               pos,
                                     ra8_err_t*            io_err)
{
  /* Explicit DFS stack (2 KiB) kept in module-static storage so this frame
   * stays within the stack-usage budget; iterative (no recursion) and
   * single-threaded, so the shared buffer never overlaps. */
  static uint32_t s_paged_stack[k_ra8_book_xhtml_stack];
  uint32_t*       stack    = s_paged_stack;
  uint32_t        sp       = 0U;
  bool            ok       = true;
  bool            at_break = true;
  stack[sp++]              = root;

  const uint32_t max_iter = (node_count * k_ra8_book_xhtml_iter_x) + k_ra8_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while ((sp > 0U) && ok && (guard < max_iter)) {
    ++guard;
    const uint32_t n = stack[--sp];
    if (n == k_ra8_book_nil) {
      continue;
    }
    ok = priv_paged_visit_node(src, n, out, cap, pos, &at_break, stack, &sp, io_err);
  }
  return ok && (guard < max_iter);
}

ra8_err_t ra8_book_chapter_text_src(const ra8_book_src_t* src,
                                    uint32_t              chapter_idx,
                                    char*                 out,
                                    size_t                cap,
                                    size_t*               out_len)
{
  RA8_CHECK_NULL_PTR(src, s_tag_paged, "text_src: null src");
  RA8_CHECK_NULL_PTR(out, s_tag_paged, "text_src: null out");
  RA8_CHECK_NULL_PTR(out_len, s_tag_paged, "text_src: null out_len");

  if (chapter_idx >= src->hdr.chapter_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Resident source: delegate to the untouched, byte-identical fast path. */
  if (src->base != nullptr) {
    return ra8_book_chapter_text(src->base, chapter_idx, out, cap, out_len);
  }

  /* Paged source: read the chapter record, then walk the DOM frame-by-frame. */
  ra8_book_chapter_t chap = {};
  const uint32_t coff = src->hdr.chapter_off + (chapter_idx * (uint32_t)sizeof(ra8_book_chapter_t));
  const ra8_err_t ce  = ra8_book_src_read(src, coff, &chap, (uint32_t)sizeof(ra8_book_chapter_t));
  if (ce != k_ra8_ok) {
    return ce;
  }
  size_t    pos    = 0U;
  ra8_err_t io_err = k_ra8_ok;
  if (!ra8_book_walk_text_paged(src,
                                chap.root_node,
                                src->hdr.node_count,
                                out,
                                cap,
                                &pos,
                                &io_err)) {
    return (io_err != k_ra8_ok) ? io_err : k_ra8_err_invalid_size;
  }
  *out_len = pos;
  return k_ra8_ok;
}

ra8_err_t ra8_book_src_prefetch_chapter(const ra8_book_src_t* src, uint32_t chapter_idx)
{
  RA8_CHECK_NULL_PTR(src, s_tag_paged, "prefetch: null src");
  /* Prefetch warms a paged cache; a resident (or unbound) source has no cache. */
  if (src->vm == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (chapter_idx >= src->hdr.chapter_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Resolve the chapter's root DOM node: the first content range
   * ra8_book_chapter_text_src() reads for this chapter. Reading the record here
   * also warms the chapter-table frame the same walk touches first. */
  ra8_book_chapter_t chap = {};
  const uint32_t coff = src->hdr.chapter_off + (chapter_idx * (uint32_t)sizeof(ra8_book_chapter_t));
  const ra8_err_t ce  = ra8_book_src_read(src, coff, &chap, (uint32_t)sizeof(ra8_book_chapter_t));
  if (ce != k_ra8_ok) {
    return ce;
  }
  /* uint64 offset math cannot overflow; an out-of-range root_node simply faults
   * in the loader and is returned verbatim (best-effort, nothing warmed). */
  const uint64_t noff =
    (uint64_t)src->hdr.node_off + ((uint64_t)chap.root_node * sizeof(ra8_book_node_t));
  return ra8_vmem_prefetch(src->vm, src->object_id, noff);
}
