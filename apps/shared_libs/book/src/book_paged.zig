//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Paged book-source layer: copy-out reads over ra8_vmem (RA8FW-1249).
//! Ported 1:1 from apps/shared_libs/book/src/book_paged.c, deleted in the
//! same commit. A source is either resident (a validated blob base; reads
//! are a memcpy) or paged (an ra8_vmem cache fronting an ra8_vsource
//! object; reads fault frames in and copy the requested slice out).
//! book_src_read is the single primitive the image and DOM-walk paths build
//! on, and the paged path pins at most one frame at a time, so the resident
//! working set stays bounded by the cache budget.
//!
//! The seven C ABI symbols (book_src_resident, book_src_paged,
//! book_src_read, book_src_image, book_src_image_rect,
//! book_chapter_text_src, book_src_prefetch_chapter) stay exported for the
//! C test suites (test_book_paged_edge.c, test_book_image_rect.c) and every
//! C caller. The shared leaf seams (priv_book_is_block,
//! priv_book_emit_text, priv_book_emit_break) and the resident fast path
//! (book_chapter_text) still live in book_xhtml.c / book.c, which stay C;
//! the port externs them until those TUs port, and the vmem cache API
//! stays extern to libs/ra8_mem.

const std = @import("std");
const wire = @import("book_stream_wire.zig");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_state: ra8_err_t = 0x104;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_out_of_range: ra8_err_t = 0x208;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// Byte length of one record (inc/book.h) and the fixed 100-byte header.
const k_book_sizeof_header: u32 = 100;
const k_book_sizeof_chapter: u32 = 12;
const k_book_sizeof_node: u32 = 24;
const k_book_sizeof_image: u32 = 24;

/// Absent index (inc/book.h).
const k_book_nil: u32 = 0xFFFFFFFF;
const k_book_node_text: u8 = 1;
const k_book_image_gray4: u8 = 0;
const k_book_pixfmt_gray4: u8 = 0;
const k_book_pixfmt_gray8: u8 = 1;

// ---- Loop-bound headroom (book_paged_bound_t, NASA Rule 2) -----------------
/// A copy of `len` bytes spans at most `ceil(len/frame_bytes) + 1` frames
/// (a leading partial frame plus full frames); the +2 guard covers the
/// partial head and tail so the bounded loop can never spin.
const k_book_paged_span_pad: u32 = 2;
/// Smallest accepted frame size (Rule-2 bound).
const k_book_paged_min_frame: u32 = 2;

// ---- 4bpp nibble packing + the per-read pixel budget (no magic numbers) ----
const k_book_gray4_nib_lo: u8 = 0x0F;
const k_book_gray4_nib_sh: u3 = 4;
const k_book_gray4_ppb: u32 = 2;
const k_book_rect_chunk_px: u32 = 512;
const k_book_rect_chunk_pad: u32 = 2;

// ---- Staging sizes and loop bounds for the paged text walk -----------------
const k_book_paged_strbuf: u32 = 256;
const k_book_paged_tagbuf: u32 = 64;
const k_book_paged_pad: u32 = 2;
const k_book_xhtml_stack: u32 = 512;
const k_book_xhtml_iter_x: u32 = 4;

/// `ra8_vmem_t` stays opaque: the cache lives in libs/ra8_mem, still C.
pub const BookImage = extern struct {
    id_off: u32,
    width: u16,
    height: u16,
    format: u8,
    pixel_format: u8,
    reserved2: u16,
    data_off: u32,
    data_size: u32,
    raw_size: u32,
};

pub const BookNode = extern struct {
    kind: u8,
    reserved: u8,
    attr_count: u16,
    name_off: u32,
    text_off: u32,
    first_attr: u32,
    first_child: u32,
    next_sibling: u32,
};

pub const BookChapter = extern struct {
    title_off: u32,
    href_off: u32,
    root_node: u32,
};

/// Host-order mirror of `book_src_t` (inc/book_paged.h).
pub const BookSrc = extern struct {
    base: ?[*]const u8,
    vm: ?*anyopaque,
    object_id: u32,
    frame_bytes: u32,
    size: u32,
    hdr: wire.BookHeader,

    comptime {
        if (@sizeOf(@This()) != 8 + 8 + 4 + 4 + 4 + k_book_sizeof_header) {
            @compileError("book_src_t layout drift");
        }
    }
};

// The shared leaf seams and the resident fast path live in book_xhtml.c /
// book.c, which stay C; the port externs them until those TUs port.
extern fn priv_book_is_block(name: [*:0]const u8) callconv(.c) bool;
extern fn priv_book_emit_text(out: [*]u8, cap: usize, pos: *usize, str: [*:0]const u8, at_break: *bool) callconv(.c) bool;
extern fn priv_book_emit_break(out: [*]u8, cap: usize, pos: *usize, at_break: *bool) callconv(.c) bool;
extern fn book_chapter_text(base: [*]const u8, chapter_idx: u32, out: [*]u8, cap: usize, out_len: *usize) callconv(.c) ra8_err_t;
extern fn ra8_vmem_get(vm: *anyopaque, object_id: u32, offset: u64, out_page: *?*anyopaque) callconv(.c) ra8_err_t;
extern fn ra8_vmem_put(vm: *anyopaque, page: *anyopaque) callconv(.c) ra8_err_t;
extern fn ra8_vmem_prefetch(vm: *anyopaque, object_id: u32, offset: u64) callconv(.c) ra8_err_t;

// ---- Source binding --------------------------------------------------------

/// Bind a resident (already-inflated, fully resident) book as a source: the
/// zero-copy fast path, reads become `base + off`.
pub export fn book_src_resident(out: ?*BookSrc, base: ?*const anyopaque, size: u32) callconv(.c) ra8_err_t {
    if (out == null) return k_ra8_err_null_ptr;
    if (base == null) return k_ra8_err_null_ptr;
    if (size == 0) return k_ra8_err_invalid_size;
    const o = out.?;
    o.base = @ptrCast(base.?);
    o.vm = null;
    o.object_id = 0;
    o.frame_bytes = 0;
    o.size = size;
    const raw: [*]const u8 = @ptrCast(base.?);
    @memcpy(std.mem.asBytes(&o.hdr), raw[0..k_book_sizeof_header]);
    return k_ra8_ok;
}

/// Bind a paged book source over an ra8_vmem cache + ra8_vsource object:
/// reads fault frames in through the cache and copy the requested slice out.
pub export fn book_src_paged(out: ?*BookSrc, vm: ?*anyopaque, object_id: u32, frame_bytes: u32, size: u32) callconv(.c) ra8_err_t {
    if (out == null) return k_ra8_err_null_ptr;
    if (vm == null) return k_ra8_err_null_ptr;
    // frame_bytes >= 2 keeps the copy-out loop bound (len/frame_bytes + 2)
    // provably non-overflowing for any uint32_t len.
    if ((size == 0) or (frame_bytes < k_book_paged_min_frame)) {
        return k_ra8_err_invalid_size;
    }
    const o = out.?;
    o.base = null;
    o.vm = vm;
    o.object_id = object_id;
    o.frame_bytes = frame_bytes;
    o.size = size;
    // Cache the header so its fields never fault during a walk.
    return book_src_read(o, 0, @ptrCast(&o.hdr), k_book_sizeof_header);
}

// ---- Copy-out reads --------------------------------------------------------

/// Copy a byte range out of a paged source, frame by frame: walks `len`
/// bytes at `off` through the cache, pinning the frame holding the current
/// offset, copying the overlapping slice, and releasing it before advancing.
fn internalBookSrcReadPaged(src: *const BookSrc, off: u32, dst: [*]u8, len: u32) ra8_err_t {
    var d = dst;
    var cur: u32 = off;
    var remaining: u32 = len;
    const max_iter: u32 = (len / src.frame_bytes) + k_book_paged_span_pad;
    var it: u32 = 0;
    while ((it < max_iter) and (remaining > 0)) : (it += 1) {
        var page: ?*anyopaque = null;
        const ge = ra8_vmem_get(src.vm.?, src.object_id, cur, &page);
        if (ge != k_ra8_ok) {
            return ge;
        }
        const in_frame: u32 = cur % src.frame_bytes;
        const avail: u32 = src.frame_bytes - in_frame;
        const n: u32 = if (remaining < avail) remaining else avail;
        const bytes: [*]const u8 = @ptrCast(page.?);
        @memcpy(d[0..n], bytes[in_frame..][0..n]);
        const pe = ra8_vmem_put(src.vm.?, page.?);
        if (pe != k_ra8_ok) {
            return pe;
        }
        d += n;
        cur += n;
        remaining -= n;
    }
    return if (remaining == 0) k_ra8_ok else k_ra8_err_out_of_range;
}

/// The single copy-out primitive the walkers build on: resident mode is a
/// memcpy, paged mode faults frames in one at a time.
pub export fn book_src_read(src: ?*const BookSrc, off: u32, dst: ?*anyopaque, len: u32) callconv(.c) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    if (dst == null) return k_ra8_err_null_ptr;
    if ((@as(u64, off) + @as(u64, len)) > src.?.size) {
        return k_ra8_err_out_of_range;
    }
    if (len == 0) {
        return k_ra8_ok;
    }
    if (src.?.base != null) {
        const d: [*]u8 = @ptrCast(dst.?);
        @memcpy(d[0..len], src.?.base.?[off..][0..len]);
        return k_ra8_ok;
    }
    if (src.?.vm == null) {
        return k_ra8_err_invalid_state;
    }
    return internalBookSrcReadPaged(src.?, off, @ptrCast(dst.?), len);
}

// ---- Image sub-rect reads --------------------------------------------------
// The image-pool addressing contract -- descriptor stride, pool base, 4bpp
// nibble packing, and odd-width parity -- lives here in the library that
// owns the format, not open-coded in every image renderer.

/// Unpack one horizontal gray4 run `[x, x+w)` of source row `ry` to gray8.
/// The high nibble is the even flat index, the low nibble the odd; a 4-bit
/// sample expands to gray8 by replicating the nibble into both halves.
fn internalBookImageRow(src: *const BookSrc, img: *const BookImage, x: u32, ry: u32, w: u32, out: [*]u8) ra8_err_t {
    const row_flat: u32 = (ry *% @as(u32, img.width)) +% x;
    var packed_buf: [(k_book_rect_chunk_px / k_book_gray4_ppb) + 1]u8 = undefined;
    var done: u32 = 0;
    const max_iter: u32 = (w / k_book_rect_chunk_px) + k_book_rect_chunk_pad;
    var it: u32 = 0;
    while (it < max_iter) : (it += 1) {
        if (done >= w) {
            break; // whole run unpacked; the count bound above is the loop invariant
        }
        const remain: u32 = w - done;
        const cpx: u32 = if (remain < k_book_rect_chunk_px) remain else k_book_rect_chunk_px;
        const flat0: u32 = row_flat + done;
        const first: u32 = flat0 >> 1;
        const last: u32 = (flat0 +% cpx -% 1) >> 1;
        const n: u32 = (last - first) + 1;
        const off: u64 = @as(u64, src.hdr.image_pool_off) + @as(u64, img.data_off) + @as(u64, first);
        if ((off + n) > src.size) {
            return k_ra8_err_out_of_range; // descriptor / geometry points past the blob
        }
        const re = book_src_read(src, @intCast(off), @ptrCast(&packed_buf), n);
        if (re != k_ra8_ok) {
            return re;
        }
        var i: u32 = 0;
        while (i < cpx) : (i += 1) {
            const flat: u32 = flat0 +% i;
            const byte: u8 = packed_buf[(flat >> 1) - first];
            const nib: u8 = if ((flat & 1) != 0) (byte & k_book_gray4_nib_lo) else (byte >> k_book_gray4_nib_sh);
            out[done + i] = (nib << k_book_gray4_nib_sh) | nib;
        }
        done += cpx;
    }
    return if (done == w) k_ra8_ok else k_ra8_err_out_of_range;
}

/// Copy one horizontal gray8 run `[x, x+w)` of source row `ry` into `out`:
/// no nibble packing, the run maps to a single contiguous pool span.
fn internalBookImageRowGray8(src: *const BookSrc, img: *const BookImage, x: u32, ry: u32, w: u32, out: [*]u8) ra8_err_t {
    const row_flat: u32 = (ry *% @as(u32, img.width)) +% x;
    const off: u64 = @as(u64, src.hdr.image_pool_off) + @as(u64, img.data_off) + @as(u64, row_flat);
    // Single exit: a span leaving the blob is an out-of-range read.
    var re: ra8_err_t = k_ra8_err_out_of_range;
    if ((off + w) <= src.size) {
        re = book_src_read(src, @intCast(off), @ptrCast(out), w);
    }
    return re;
}

/// Copy one image descriptor out of the blob's image table by index.
pub export fn book_src_image(src: ?*const BookSrc, idx: u32, out_img: ?*BookImage) callconv(.c) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    if (out_img == null) return k_ra8_err_null_ptr;
    if (idx >= src.?.hdr.image_count) {
        return k_ra8_err_invalid_arg;
    }
    const off: u32 = src.?.hdr.image_off +% (idx *% k_book_sizeof_image);
    return book_src_read(src, off, out_img, k_book_sizeof_image);
}

/// Read a sub-rectangle of a bound image as unpacked gray8: gray4
/// nibble-unpacks 2px/byte, gray8 copies 1px/byte verbatim; SVG has no
/// pixel grid and is refused.
pub export fn book_src_image_rect(src: ?*const BookSrc, img: ?*const BookImage, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    if (img == null) return k_ra8_err_null_ptr;
    if (out == null) return k_ra8_err_null_ptr;
    // Readable raster of a known depth (one decision, single exit):
    // book_validate() already refuses any other depth; this also guards a
    // corrupt descriptor.
    const pf: u8 = img.?.pixel_format;
    if ((img.?.format != k_book_image_gray4) or
        ((pf != k_book_pixfmt_gray4) and (pf != k_book_pixfmt_gray8)))
    {
        return k_ra8_err_invalid_arg;
    }
    if ((w == 0) or (h == 0) or (out_stride < w)) {
        return k_ra8_err_invalid_arg;
    }
    if ((@as(u64, x) + @as(u64, w)) > img.?.width or
        (@as(u64, y) + @as(u64, h)) > img.?.height)
    {
        return k_ra8_err_out_of_range; // sub-rect not fully inside the image
    }
    const is_gray8 = pf == k_book_pixfmt_gray8;
    var row: u32 = 0;
    while (row < h) : (row += 1) {
        const dst = out.? + (@as(usize, row) * @as(usize, out_stride));
        const re = if (is_gray8)
            internalBookImageRowGray8(src.?, img.?, x, y + row, w, dst)
        else
            internalBookImageRow(src.?, img.?, x, y + row, w, dst);
        if (re != k_ra8_ok) {
            return re;
        }
    }
    return k_ra8_ok;
}

// ---- Paged plain-text extraction ------------------------------------------
// Same output as book_chapter_text but the DOM is read frame-by-frame
// through a book_src_t / ra8_vmem cache, so a book that exceeds the
// resident budget is walked with a bounded working set. The resident path
// delegates to the untouched, byte-identical fast path in book.c.

/// Read one DOM node record out of a paged/resident book source by index.
fn internalPagedNode(src: *const BookSrc, idx: u32, out: *BookNode) ra8_err_t {
    const off: u32 = src.hdr.node_off +% (idx *% k_book_sizeof_node);
    return book_src_read(src, off, @ptrCast(out), k_book_sizeof_node);
}

/// Copy a short NUL-terminated string (a tag name) out into a buffer, NUL-
/// terminating at the first embedded NUL or at the buffer end.
fn internalPagedStrShort(src: *const BookSrc, abs_off: u32, buf: [*]u8, cap: u32) ra8_err_t {
    if (abs_off >= src.size) {
        return k_ra8_err_out_of_range;
    }
    const remain: u32 = src.size - abs_off;
    const want: u32 = cap - 1;
    const chunk: u32 = if (remain < want) remain else want;
    const re = book_src_read(src, abs_off, @ptrCast(buf), chunk);
    if (re != k_ra8_ok) {
        return re;
    }
    var nlen: u32 = 0;
    while ((nlen < chunk) and (buf[nlen] != 0)) {
        nlen += 1;
    }
    buf[nlen] = 0;
    return k_ra8_ok;
}

/// Emit one text run from a paged source, collapsing whitespace: streams
/// the NUL-terminated run at `abs_off` through a staging buffer in
/// strbuf-sized chunks, calling the shared priv_book_emit_text on each.
fn internalPagedEmitRun(src: *const BookSrc, abs_off: u32, out: [*]u8, cap: usize, pos: *usize, at_break: *bool, io_err: *ra8_err_t) bool {
    var buf: [k_book_paged_strbuf]u8 = undefined;
    var off: u32 = abs_off;
    const max_iter: u32 = (src.size / (k_book_paged_strbuf - 1)) + k_book_paged_pad;
    var it: u32 = 0;
    while (it < max_iter) : (it += 1) {
        if (off >= src.size) {
            return true; // Ran to blob end without a NUL (degenerate); treat as done.
        }
        const remain: u32 = src.size - off;
        const want: u32 = k_book_paged_strbuf - 1;
        const chunk: u32 = if (remain < want) remain else want;
        const re = book_src_read(src, off, @ptrCast(&buf), chunk);
        if (re != k_ra8_ok) {
            io_err.* = re;
            return false;
        }
        var nlen: u32 = 0;
        while ((nlen < chunk) and (buf[nlen] != 0)) {
            nlen += 1;
        }
        buf[nlen] = 0;
        if (!priv_book_emit_text(out, cap, pos, @ptrCast(&buf), at_break)) {
            return false; // Output overflow.
        }
        if (nlen < chunk) {
            return true; // Hit the real NUL terminator.
        }
        off += nlen; // nlen == chunk: advance and read the next chunk.
    }
    return true;
}

/// Process one popped node in the paged text walk: reads the node, pushes
/// its sibling, then either emits a text run or, for an element, inserts a
/// block break (priv_book_is_block over a staged tag name) and pushes its
/// first child.
fn internalPagedVisitNode(src: *const BookSrc, n: u32, out: [*]u8, cap: usize, pos: *usize, at_break: *bool, stack: [*]u32, sp: *usize, io_err: *ra8_err_t) bool {
    var node: BookNode = std.mem.zeroes(BookNode);
    const ne = internalPagedNode(src, n, &node);
    if (ne != k_ra8_ok) {
        io_err.* = ne;
        return false;
    }
    if (sp.* >= k_book_xhtml_stack) {
        return false;
    }
    stack[sp.*] = node.next_sibling; // sibling chain after this subtree
    sp.* += 1;
    if (node.kind == k_book_node_text) {
        return internalPagedEmitRun(src, src.hdr.string_off +% node.text_off, out, cap, pos, at_break, io_err);
    }
    var tag: [k_book_paged_tagbuf]u8 = std.mem.zeroes([k_book_paged_tagbuf]u8);
    const te = internalPagedStrShort(src, src.hdr.string_off +% node.name_off, &tag, k_book_paged_tagbuf);
    if (te != k_ra8_ok) {
        io_err.* = te;
        return false;
    }
    var ok = true;
    if (priv_book_is_block(@ptrCast(&tag))) {
        ok = priv_book_emit_break(out, cap, pos, at_break);
    }
    if (ok and (sp.* < k_book_xhtml_stack)) {
        stack[sp.*] = node.first_child; // descend, pre-order
        sp.* += 1;
    }
    return ok;
}

/// Explicit DFS stack (2 KiB) kept in module-static storage so the walk
/// frame stays within the stack-usage budget; iterative (no recursion) and
/// single-threaded, so the shared buffer never overlaps.
var s_paged_stack: [k_book_xhtml_stack]u32 = undefined;

/// Bounded pre-order text walk over a paged book source: identical
/// iterative, recursion-free pre-order traversal as the resident walk,
/// dispatching each popped node through the paged visitors. At most one
/// cache frame is pinned at a time.
fn internalWalkTextPaged(src: *const BookSrc, root: u32, node_count: u32, out: [*]u8, cap: usize, pos: *usize, io_err: *ra8_err_t) bool {
    const stack: [*]u32 = &s_paged_stack;
    var sp: usize = 0;
    var ok = true;
    var at_break = true;
    stack[0] = root;
    sp += 1;

    const max_iter: u32 = (node_count *% k_book_xhtml_iter_x) +% k_book_xhtml_stack;
    var guard: u32 = 0;
    while ((sp > 0) and ok and (guard < max_iter)) {
        guard += 1;
        sp -= 1;
        const n = stack[sp];
        if (n == k_book_nil) {
            continue;
        }
        ok = internalPagedVisitNode(src, n, out, cap, pos, &at_break, stack, &sp, io_err);
    }
    return ok and (guard < max_iter);
}

/// Chapter plain text over a bound source: resident sources delegate to
/// the untouched fast path (book_chapter_text), paged sources read the
/// chapter record then walk the DOM frame-by-frame.
pub export fn book_chapter_text_src(src: ?*const BookSrc, chapter_idx: u32, out: ?[*]u8, cap: usize, out_len: ?*usize) callconv(.c) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    if (out == null) return k_ra8_err_null_ptr;
    if (out_len == null) return k_ra8_err_null_ptr;

    if (chapter_idx >= src.?.hdr.chapter_count) {
        return k_ra8_err_invalid_arg;
    }
    // Resident source: delegate to the untouched, byte-identical fast path.
    if (src.?.base != null) {
        return book_chapter_text(src.?.base.?, chapter_idx, out.?, cap, out_len.?);
    }

    // Paged source: read the chapter record, then walk the DOM frame-by-frame.
    var chap: BookChapter = std.mem.zeroes(BookChapter);
    const coff: u32 = src.?.hdr.chapter_off +% (chapter_idx *% k_book_sizeof_chapter);
    const ce = book_src_read(src, coff, @ptrCast(&chap), k_book_sizeof_chapter);
    if (ce != k_ra8_ok) {
        return ce;
    }
    var pos: usize = 0;
    var io_err: ra8_err_t = k_ra8_ok;
    if (!internalWalkTextPaged(src.?, chap.root_node, src.?.hdr.node_count, out.?, cap, &pos, &io_err)) {
        return if (io_err != k_ra8_ok) io_err else k_ra8_err_invalid_size;
    }
    out_len.?.* = pos;
    return k_ra8_ok;
}

/// Warm a paged cache for a chapter: resolves the chapter's root DOM node
/// (the first content range the walk reads) and prefetches it; a resident
/// (or unbound) source has no cache.
pub export fn book_src_prefetch_chapter(src: ?*const BookSrc, chapter_idx: u32) callconv(.c) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    // Prefetch warms a paged cache; a resident (or unbound) source has none.
    if (src.?.vm == null) {
        return k_ra8_err_invalid_state;
    }
    if (chapter_idx >= src.?.hdr.chapter_count) {
        return k_ra8_err_invalid_arg;
    }
    var chap: BookChapter = std.mem.zeroes(BookChapter);
    const coff: u32 = src.?.hdr.chapter_off +% (chapter_idx *% k_book_sizeof_chapter);
    const ce = book_src_read(src, coff, @ptrCast(&chap), k_book_sizeof_chapter);
    if (ce != k_ra8_ok) {
        return ce;
    }
    // u64 offset math cannot overflow; an out-of-range root_node simply
    // faults in the loader and is returned verbatim (best-effort).
    const noff: u64 = @as(u64, src.?.hdr.node_off) + (@as(u64, chap.root_node) * k_book_sizeof_node);
    return ra8_vmem_prefetch(src.?.vm.?, src.?.object_id, noff);
}
