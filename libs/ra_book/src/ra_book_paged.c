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
#include "ra_book_internal.h"
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

/* ===========================================================================
 * Paged plain-text extraction (#163): same output as ra_book_chapter_text but
 * the DOM is read frame-by-frame through an ra_book_src_t / ra_vmem cache, so a
 * book that exceeds the resident budget is walked with a bounded working set.
 * The resident path above is untouched; these helpers reuse the shared,
 * source-agnostic emit_text / emit_break / is_block leaf helpers on staging
 * buffers, so paged output is byte-for-byte identical to the resident walk.
 * =========================================================================== */

/**
 * @enum ra_book_paged_walk_bound_t
 * @brief Staging sizes and loop bounds for the paged text walk (no magic numbers).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra_book_paged_strbuf = 256U, /**< Staging chunk for a paged text run.       */
  k_ra_book_paged_tagbuf = 64U,  /**< Staging for one (short) element tag name. */
  k_ra_book_paged_pad    = 2U,   /**< Loop-bound headroom over the exact span.  */
} ra_book_paged_walk_bound_t;

/**
 * @brief Read one DOM node record out of a paged/resident book source by index.
 *
 * @details Translates the node index to its blob byte offset
 *          (`node_off + idx * sizeof(ra_book_node_t)`) and copies the 24-byte
 *          record out via ::ra_book_src_read, which bounds-checks the range and
 *          faults the frame in for a paged source.
 *
 * @param[in]  src Bound book source.
 * @param[in]  idx Node-table index to read.
 * @param[out] out Receives the node record.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Node copied into @p out.
 * @retval k_ra_err_out_of_range @p idx lies outside the node table.
 * @retval k_ra_err_*            A ::ra_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p out is writable.
 * @pre  @p idx is intended to index a valid node (verified by the read bounds).
 * @post On success @p out holds the node at @p idx.
 * @post On any non-ok return @p out content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t ra_book_paged_node(const ra_book_src_t* src, uint32_t idx, ra_book_node_t* out)
{
  const uint32_t off = src->hdr.node_off + (idx * (uint32_t)sizeof(ra_book_node_t));
  return ra_book_src_read(src, off, out, (uint32_t)sizeof(ra_book_node_t));
}

/**
 * @brief Copy a short NUL-terminated string (a tag name) out into a buffer.
 *
 * @details Reads up to `cap - 1` bytes at @p abs_off and NUL-terminates at the
 *          first embedded NUL or at the buffer end. Used for element tag names,
 *          which are short; a name longer than the buffer is truncated, which at
 *          worst makes ::ra_book_is_block treat it as inline (no break) -- never
 *          a correctness issue for real markup.
 *
 * @param[in]  src     Bound book source.
 * @param[in]  abs_off Absolute blob offset of the string start.
 * @param[out] buf     Destination buffer (>= @p cap bytes).
 * @param[in]  cap     Capacity of @p buf in bytes (>= 1).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               String (possibly truncated) copied + terminated.
 * @retval k_ra_err_out_of_range @p abs_off is at or past the blob end.
 * @retval k_ra_err_*            A ::ra_book_src_read fault (returned verbatim).
 *
 * @pre  @p src is bound and @p buf has room for @p cap bytes.
 * @pre  @p cap is at least one (for the NUL terminator).
 * @post On success @p buf is a NUL-terminated string.
 * @post On any non-ok return @p buf content is unspecified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t
ra_book_paged_str_short(const ra_book_src_t* src, uint32_t abs_off, char* buf, uint32_t cap)
{
  if (abs_off >= src->size) {
    return k_ra_err_out_of_range;
  }
  const uint32_t remain = src->size - abs_off;
  const uint32_t want   = cap - 1U;
  const uint32_t chunk  = (remain < want) ? remain : want;
  const ra_err_t re     = ra_book_src_read(src, abs_off, buf, chunk);
  if (re != k_ra_ok) {
    return re;
  }
  uint32_t nlen = 0U;
  while ((nlen < chunk) && (buf[nlen] != '\0')) {
    ++nlen;
  }
  buf[nlen] = '\0';
  return k_ra_ok;
}

/**
 * @brief Emit one text run from a paged source, collapsing whitespace.
 *
 * @details Streams the NUL-terminated run at @p abs_off through a staging buffer
 *          in `k_ra_book_paged_strbuf`-sized chunks, calling the shared
 *          ::ra_book_emit_text on each chunk. Chunking keeps the resident
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
 * @post On true with `*io_err == k_ra_ok` the run is fully emitted.
 * @post On a fault `*io_err` holds the fault code and false is returned.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static bool ra_book_paged_emit_run(const ra_book_src_t* src,
                                   uint32_t             abs_off,
                                   char*                out,
                                   size_t               cap,
                                   size_t*              pos,
                                   bool*                at_break,
                                   ra_err_t*            io_err)
{
  char           buf[k_ra_book_paged_strbuf];
  uint32_t       off = abs_off;
  const uint32_t max_iter =
    (src->size / (uint32_t)(k_ra_book_paged_strbuf - 1U)) + (uint32_t)k_ra_book_paged_pad;
  for (uint32_t it = 0U; it < max_iter; ++it) {
    if (off >= src->size) {
      return true; /* Ran to blob end without a NUL (degenerate); treat as done. */
    }
    const uint32_t remain = src->size - off;
    const uint32_t want   = (uint32_t)(k_ra_book_paged_strbuf - 1U);
    const uint32_t chunk  = (remain < want) ? remain : want;
    const ra_err_t re     = ra_book_src_read(src, off, buf, chunk);
    if (re != k_ra_ok) {
      *io_err = re;
      return false;
    }
    uint32_t nlen = 0U;
    while ((nlen < chunk) && (buf[nlen] != '\0')) {
      ++nlen;
    }
    buf[nlen] = '\0';
    if (!ra_book_emit_text(out, cap, pos, buf, at_break)) {
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
 * @details The per-node body of ::ra_book_walk_text_paged, split out to keep both
 *          functions within the size/complexity budget. Reads the node, pushes
 *          its sibling, then either emits a text run (::ra_book_paged_emit_run)
 *          or, for an element, inserts a block break (::ra_book_is_block over a
 *          staged tag name) and pushes its first child. Mutates the caller's
 *          stack/sp and whitespace-collapse @p at_break in place.
 *
 * @param[in]     src      Bound (paged) book source.
 * @param[in]     n        Node index to visit (already non-nil).
 * @param[out]    out      Destination plain-text buffer.
 * @param[in]     cap      Capacity of @p out in bytes.
 * @param[in,out] pos      Current write offset; advanced as text is emitted.
 * @param[in,out] at_break Whitespace-collapse carry flag.
 * @param[in,out] stack    Caller walk stack of length @c k_ra_book_xhtml_stack.
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
static bool priv_paged_visit_node(const ra_book_src_t* src,
                                  uint32_t             n,
                                  char*                out,
                                  size_t               cap,
                                  size_t*              pos,
                                  bool*                at_break,
                                  uint32_t*            stack,
                                  uint32_t*            sp,
                                  ra_err_t*            io_err)
{
  ra_book_node_t node = {};
  const ra_err_t ne   = ra_book_paged_node(src, n, &node);
  if (ne != k_ra_ok) {
    *io_err = ne;
    return false;
  }
  if (*sp >= k_ra_book_xhtml_stack) {
    return false;
  }
  stack[(*sp)++] = node.next_sibling; /* sibling chain after this subtree */
  if (node.kind == (uint8_t)k_ra_book_node_text) {
    return ra_book_paged_emit_run(src,
                                  src->hdr.string_off + node.text_off,
                                  out,
                                  cap,
                                  pos,
                                  at_break,
                                  io_err);
  }
  char           tag[k_ra_book_paged_tagbuf] = {};
  const ra_err_t te = ra_book_paged_str_short(src,
                                              src->hdr.string_off + node.name_off,
                                              tag,
                                              (uint32_t)k_ra_book_paged_tagbuf);
  if (te != k_ra_ok) {
    *io_err = te;
    return false;
  }
  bool ok = true;
  if (ra_book_is_block(tag)) {
    ok = ra_book_emit_break(out, cap, pos, at_break);
  }
  if (ok && (*sp < k_ra_book_xhtml_stack)) {
    stack[(*sp)++] = node.first_child; /* descend, pre-order */
  }
  return ok;
}

/**
 * @brief Bounded pre-order text walk over a paged book source.
 *
 * @details The paged counterpart of ra_book_walk_text(): identical iterative,
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
 * @param[out]    io_err     Set to the fault code on a read fault; k_ra_ok otherwise.
 *
 * @return bool Walk result.
 * @retval true  Subtree fully serialised (check @p io_err for a clean run).
 * @retval false Overflow, stack/guard exhaustion, or a read fault (see @p io_err).
 *
 * @pre  @p src is a paged source bound by ra_book_src_paged().
 * @pre  @p root is a node index within the blob.
 * @post On true with `*io_err == k_ra_ok`, @p out holds the chapter text.
 * @post On a fault `*io_err` carries the fault code.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static bool ra_book_walk_text_paged(const ra_book_src_t* src,
                                    uint32_t             root,
                                    uint32_t             node_count,
                                    char*                out,
                                    size_t               cap,
                                    size_t*              pos,
                                    ra_err_t*            io_err)
{
  /* Explicit DFS stack (2 KiB) kept in module-static storage so this frame
   * stays within the stack-usage budget; iterative (no recursion) and
   * single-threaded, so the shared buffer never overlaps. */
  static uint32_t s_paged_stack[k_ra_book_xhtml_stack];
  uint32_t*       stack    = s_paged_stack;
  uint32_t        sp       = 0U;
  bool            ok       = true;
  bool            at_break = true;
  stack[sp++]              = root;

  const uint32_t max_iter = (node_count * k_ra_book_xhtml_iter_x) + k_ra_book_xhtml_stack;
  uint32_t       guard    = 0U;
  while ((sp > 0U) && ok && (guard < max_iter)) {
    ++guard;
    const uint32_t n = stack[--sp];
    if (n == k_ra_book_nil) {
      continue;
    }
    ok = priv_paged_visit_node(src, n, out, cap, pos, &at_break, stack, &sp, io_err);
  }
  return ok && (guard < max_iter);
}

ra_err_t ra_book_chapter_text_src(const ra_book_src_t* src,
                                  uint32_t             chapter_idx,
                                  char*                out,
                                  size_t               cap,
                                  size_t*              out_len)
{
  RA_CHECK_NULL_PTR(src, s_tag_paged, "text_src: null src");
  RA_CHECK_NULL_PTR(out, s_tag_paged, "text_src: null out");
  RA_CHECK_NULL_PTR(out_len, s_tag_paged, "text_src: null out_len");

  if (chapter_idx >= src->hdr.chapter_count) {
    return k_ra_err_invalid_arg;
  }
  /* Resident source: delegate to the untouched, byte-identical fast path. */
  if (src->base != nullptr) {
    return ra_book_chapter_text(src->base, chapter_idx, out, cap, out_len);
  }

  /* Paged source: read the chapter record, then walk the DOM frame-by-frame. */
  ra_book_chapter_t chap = {};
  const uint32_t coff = src->hdr.chapter_off + (chapter_idx * (uint32_t)sizeof(ra_book_chapter_t));
  const ra_err_t ce   = ra_book_src_read(src, coff, &chap, (uint32_t)sizeof(ra_book_chapter_t));
  if (ce != k_ra_ok) {
    return ce;
  }
  size_t   pos    = 0U;
  ra_err_t io_err = k_ra_ok;
  if (!ra_book_walk_text_paged(src, chap.root_node, src->hdr.node_count, out, cap, &pos, &io_err)) {
    return (io_err != k_ra_ok) ? io_err : k_ra_err_invalid_size;
  }
  *out_len = pos;
  return k_ra_ok;
}

ra_err_t ra_book_src_prefetch_chapter(const ra_book_src_t* src, uint32_t chapter_idx)
{
  RA_CHECK_NULL_PTR(src, s_tag_paged, "prefetch: null src");
  /* Prefetch warms a paged cache; a resident (or unbound) source has no cache. */
  if (src->vm == nullptr) {
    return k_ra_err_invalid_state;
  }
  if (chapter_idx >= src->hdr.chapter_count) {
    return k_ra_err_invalid_arg;
  }
  /* Resolve the chapter's root DOM node: the first content range
   * ra_book_chapter_text_src() reads for this chapter. Reading the record here
   * also warms the chapter-table frame the same walk touches first. */
  ra_book_chapter_t chap = {};
  const uint32_t coff = src->hdr.chapter_off + (chapter_idx * (uint32_t)sizeof(ra_book_chapter_t));
  const ra_err_t ce   = ra_book_src_read(src, coff, &chap, (uint32_t)sizeof(ra_book_chapter_t));
  if (ce != k_ra_ok) {
    return ce;
  }
  /* uint64 offset math cannot overflow; an out-of-range root_node simply faults
   * in the loader and is returned verbatim (best-effort, nothing warmed). */
  const uint64_t noff =
    (uint64_t)src->hdr.node_off + ((uint64_t)chap.root_node * sizeof(ra_book_node_t));
  return ra_vmem_prefetch(src->vm, src->object_id, noff);
}
