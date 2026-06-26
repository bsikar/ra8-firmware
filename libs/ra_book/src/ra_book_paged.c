/**
 * @file ra_book_paged.c
 * @brief Paged book-source layer: copy-out reads over ra_vmem (#163).
 *
 * @details
 * Implements the @ref ra_book_src_t seam from ra_book_paged.h. A source is
 * either resident (a validated blob base; reads are a `memcpy`) or paged (an
 * @ref ra_vmem cache fronting an @ref ra_vsource object; reads fault frames in
 * and copy the requested slice out). ::ra_book_src_read is the single primitive
 * the DOM walkers build on; the paged path pins at most one frame at a time, so
 * the resident working set stays bounded by the cache budget irrespective of how
 * much of the book a chapter walk touches.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "ra_book_paged.h"

#include <string.h>

#include "ra_book.h"
#include "ra_check.h"
#include "ra_vmem.h"

/** @brief Log tag for paged-source diagnostics. */
static const char* const s_tag_paged = "ra_book_paged";

/**
 * @enum ra_book_paged_bound_t
 * @brief Loop-bound headroom for the frame-by-frame copy-out (NASA Rule 2).
 * @details A copy of `len` bytes spans at most `ceil(len/frame_bytes) + 1`
 *          frames (a leading partial frame plus full frames); the `+2` guard
 *          covers the partial head and tail so the bounded loop can never spin.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra_book_paged_span_pad  = 2U, /**< Extra frame iterations over the exact span.  */
  k_ra_book_paged_min_frame = 2U, /**< Smallest accepted frame size (Rule-2 bound). */
} ra_book_paged_bound_t;

ra_err_t ra_book_src_resident(ra_book_src_t* out, const void* base, uint32_t size)
{
  RA_CHECK_NULL_PTR(out, s_tag_paged, "resident: null out");
  RA_CHECK_NULL_PTR(base, s_tag_paged, "resident: null base");
  if (size == 0U) {
    return k_ra_err_invalid_size;
  }
  out->base        = (const uint8_t*)base;
  out->vm          = nullptr;
  out->object_id   = 0U;
  out->frame_bytes = 0U;
  out->size        = size;
  (void)memcpy(&out->hdr, base, sizeof(ra_book_header_t));
  return k_ra_ok;
}

ra_err_t ra_book_src_paged(ra_book_src_t* out,
                           ra_vmem_t*     vm,
                           uint32_t       object_id,
                           uint32_t       frame_bytes,
                           uint32_t       size)
{
  RA_CHECK_NULL_PTR(out, s_tag_paged, "paged: null out");
  RA_CHECK_NULL_PTR(vm, s_tag_paged, "paged: null vm");
  /* frame_bytes >= 2 keeps the copy-out loop bound (len/frame_bytes + 2)
   * provably non-overflowing for any uint32_t len. */
  if ((size == 0U) || (frame_bytes < (uint32_t)k_ra_book_paged_min_frame)) {
    return k_ra_err_invalid_size;
  }
  out->base        = nullptr;
  out->vm          = vm;
  out->object_id   = object_id;
  out->frame_bytes = frame_bytes;
  out->size        = size;
  /* Cache the header so its fields never fault during a walk. */
  return ra_book_src_read(out, 0U, &out->hdr, (uint32_t)sizeof(ra_book_header_t));
}

/**
 * @brief Copy a byte range out of a paged source, frame by frame.
 *
 * @details The demand-fetch worker behind ::ra_book_src_read: walks @p len bytes
 *          at @p off through the cache, pinning the frame holding the current
 *          offset (::ra_vmem_get), copying the overlapping slice, and releasing
 *          it (::ra_vmem_put) before advancing -- one frame pinned at a time.
 *          The loop is bounded by `len / frame_bytes + 2` (frame_bytes >= 2 is
 *          enforced at bind time, so the bound never overflows).
 *
 * @param[in]  src Paged source (`src->vm` non-NULL, `src->base` NULL).
 * @param[in]  off Byte offset within the blob (range already validated).
 * @param[out] dst Destination buffer receiving @p len bytes.
 * @param[in]  len Number of bytes to copy (non-zero).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               All @p len bytes copied into @p dst.
 * @retval k_ra_err_out_of_range The loop drained before @p len bytes (unreachable
 *                               for a valid range / `frame_bytes >= 2`).
 * @retval k_ra_err_*            A ::ra_vmem_get / ::ra_vmem_put fault (verbatim).
 *
 * @pre  @p src is paged and `off + len <= src->size`.
 * @pre  @p dst addresses at least @p len writable bytes.
 * @post On success `dst[0..len)` holds the blob bytes at @p off; no pin is held.
 * @post On any non-ok return @p dst content is unspecified and no pin is held.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t
priv_book_src_read_paged(const ra_book_src_t* src, uint32_t off, void* dst, uint32_t len)
{
  uint8_t*       d         = (uint8_t*)dst;
  uint32_t       cur       = off;
  uint32_t       remaining = len;
  const uint32_t max_iter  = (len / src->frame_bytes) + (uint32_t)k_ra_book_paged_span_pad;
  for (uint32_t it = 0U; (it < max_iter) && (remaining > 0U); ++it) {
    void*          page = nullptr;
    const ra_err_t ge   = ra_vmem_get(src->vm, src->object_id, (uint64_t)cur, &page);
    if (ge != k_ra_ok) {
      return ge;
    }
    const uint32_t in_frame = cur % src->frame_bytes;
    const uint32_t avail    = src->frame_bytes - in_frame;
    const uint32_t n        = (remaining < avail) ? remaining : avail;
    (void)memcpy(d, (const uint8_t*)page + in_frame, n);
    const ra_err_t pe = ra_vmem_put(src->vm, page);
    if (pe != k_ra_ok) {
      return pe;
    }
    d += n;
    cur += n;
    remaining -= n;
  }
  return (remaining == 0U) ? k_ra_ok : k_ra_err_out_of_range;
}

ra_err_t ra_book_src_read(const ra_book_src_t* src, uint32_t off, void* dst, uint32_t len)
{
  RA_CHECK_NULL_PTR(src, s_tag_paged, "read: null src");
  RA_CHECK_NULL_PTR(dst, s_tag_paged, "read: null dst");
  if (((uint64_t)off + (uint64_t)len) > (uint64_t)src->size) {
    return k_ra_err_out_of_range;
  }
  if (len == 0U) {
    return k_ra_ok;
  }
  if (src->base != nullptr) {
    (void)memcpy(dst, src->base + off, len);
    return k_ra_ok;
  }
  if (src->vm == nullptr) {
    return k_ra_err_invalid_state;
  }
  return priv_book_src_read_paged(src, off, dst, len);
}
