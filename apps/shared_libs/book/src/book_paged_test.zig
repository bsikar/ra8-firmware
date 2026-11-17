//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-side tests for the paged book-source port (RA8FW-1249). The C
//! suites (test_book_paged_edge.c, test_book_image_rect.c) keep covering
//! the surface from the outside through the unchanged C ABI; these exercise
//! the port's own core paths over built-in-memory RABOOK1 blobs: binding
//! gates, resident and paged copy-out reads (frame-spanning, faults), the
//! image descriptor and sub-rect paths for both gray depths, and the paged
//! DOM text walk with its seam behaviour.

const std = @import("std");
const paged = @import("book_paged.zig");
const wire = @import("book_stream_wire.zig");

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_state: u16 = 0x104;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_out_of_range: u16 = 0x208;
const k_ra8_err_null_ptr: u16 = 0x504;

const k_book_nil: u32 = 0xFFFFFFFF;

// ---- Shared leaf seams (book_xhtml.zig since RA8FW-1250) -------------------
// Faithful mirrors so the walk output matches the resident behaviour the C
// tests pin; this test module links no archive and imports no other book
// TU, so it carries its own copies.

fn internalEmit(out: [*]u8, cap: usize, pos: *usize, bytes: []const u8) bool {
    if (pos.* + bytes.len > cap) return false;
    @memcpy(out[pos.*..][0..bytes.len], bytes);
    pos.* += bytes.len;
    return true;
}

export fn priv_book_is_block(name: [*:0]const u8) callconv(.c) bool {
    const blocks = [_][]const u8{
        "p", "h1", "h2", "h3", "h4", "h5", "h6", "li",
        "ul", "ol", "div", "br", "hr", "section", "tr", "pre",
        "header", "blockquote", "article", "aside", "footer", "figure", "figcaption",
    };
    const s = std.mem.span(name);
    for (blocks) |b| {
        if (std.mem.eql(u8, s, b)) return true;
    }
    return false;
}

export fn priv_book_emit_text(out: [*]u8, cap: usize, pos: *usize, str: [*:0]const u8, at_break: *bool) callconv(.c) bool {
    var i: usize = 0;
    while (str[i] != 0) : (i += 1) {
        const c = str[i];
        const ws = (c == ' ') or (c == '\t') or (c == '\n') or (c == '\r');
        if (!ws) {
            if (!internalEmit(out, cap, pos, &[1]u8{c})) return false;
            at_break.* = false;
            continue;
        }
        if (at_break.*) continue;
        if (!internalEmit(out, cap, pos, " ")) return false;
        at_break.* = true;
    }
    return true;
}

export fn priv_book_emit_break(out: [*]u8, cap: usize, pos: *usize, at_break: *bool) callconv(.c) bool {
    while ((pos.* > 0) and (out[pos.* - 1] == ' ')) {
        pos.* -= 1;
    }
    at_break.* = true;
    if ((pos.* > 0) and (out[pos.* - 1] == '\n')) {
        return true;
    }
    return internalEmit(out, cap, pos, "\n");
}

export fn book_chapter_text(base: [*]const u8, chapter_idx: u32, out: [*]u8, cap: usize, out_len: *usize) callconv(.c) u16 {
    _ = base;
    _ = chapter_idx;
    const s = "RESIDENT";
    if (cap < s.len) return k_ra8_err_invalid_size;
    @memcpy(out[0..s.len], s);
    out_len.* = s.len;
    return k_ra8_ok;
}

// ---- Fake ra8_vmem: one static frame cache over a backing blob -------------

const FakeVm = struct {
    backing: []const u8 = &.{},
    frame_bytes: u32 = 4,
    fault_code: u16 = 0,
    puts: u32 = 0,
    frame: [256]u8 = undefined,
    prefetches: [8]u64 = .{0, 0, 0, 0, 0, 0, 0, 0},
    prefetch_count: usize = 0,
};

var g_vm = FakeVm{};

export fn ra8_vmem_get(vm: *anyopaque, object_id: u32, offset: u64, out_page: *?*anyopaque) callconv(.c) u16 {
    _ = vm;
    _ = object_id;
    if (g_vm.fault_code != 0) {
        const e = g_vm.fault_code;
        g_vm.fault_code = 0;
        return e;
    }
    const fb: u64 = g_vm.frame_bytes;
    const start: u64 = offset - (offset % fb);
    var i: u64 = 0;
    while (i < fb) : (i += 1) {
        const idx = start + i;
        g_vm.frame[@intCast(i)] = if (idx < g_vm.backing.len) g_vm.backing[@intCast(idx)] else 0;
    }
    out_page.* = @ptrCast(&g_vm.frame);
    return k_ra8_ok;
}

export fn ra8_vmem_put(vm: *anyopaque, page: *anyopaque) callconv(.c) u16 {
    _ = vm;
    _ = page;
    g_vm.puts += 1;
    return k_ra8_ok;
}

export fn ra8_vmem_prefetch(vm: *anyopaque, object_id: u32, offset: u64) callconv(.c) u16 {
    _ = vm;
    _ = object_id;
    g_vm.prefetches[g_vm.prefetch_count % g_vm.prefetches.len] = offset;
    g_vm.prefetch_count += 1;
    return k_ra8_ok;
}

// ---- Built-in-memory RABOOK1 blob ------------------------------------------

/// The interned-string pool: offset 0 the empty-string sentinel, 1 "p",
/// 3 "hello world", 15 "span".
const pool = "\x00" ++ "p\x00" ++ "hello world\x00" ++ "span\x00";
const k_str_p: u32 = 1;
const k_str_hello: u32 = 3;
const k_str_span: u32 = 15;

const k_chapter_off: usize = 100;
const k_node_off: usize = 112;
const k_image_off: usize = 184;
const k_string_off: usize = 232;
const k_pool_off: usize = 252;
const k_blob_size: usize = 268;

const pool_bytes: []const u8 = &[_]u8{
    0x12, 0x34, 0x56, 0, 0, 0, 0, 0, // image0 gray4 raster (3 bytes)
    0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0, 0, // image1 gray8 raster at data_off 8
};

var g_blob: [k_blob_size]u8 = undefined;

fn buildBlob() []const u8 {
    @memset(&g_blob, 0);
    var hdr: wire.BookHeader = std.mem.zeroes(wire.BookHeader);
    hdr.chapter_count = 1;
    hdr.chapter_off = k_chapter_off;
    hdr.node_count = 3;
    hdr.node_off = k_node_off;
    hdr.image_count = 2;
    hdr.image_off = k_image_off;
    hdr.string_off = k_string_off;
    hdr.string_size = pool.len;
    hdr.image_pool_off = k_pool_off;
    hdr.image_pool_size = pool_bytes.len;
    @memcpy(g_blob[0..100], std.mem.asBytes(&hdr));

    const chapters = [_]paged.BookChapter{
        .{ .title_off = 0, .href_off = 0, .root_node = 0 },
    };
    const nodes = [_]paged.BookNode{
        .{ .kind = 0, .reserved = 0, .attr_count = 0, .name_off = k_str_p, .text_off = 0, .first_attr = k_book_nil, .first_child = 1, .next_sibling = 2 },
        .{ .kind = 1, .reserved = 0, .attr_count = 0, .name_off = 0, .text_off = k_str_hello, .first_attr = k_book_nil, .first_child = k_book_nil, .next_sibling = k_book_nil },
        .{ .kind = 0, .reserved = 0, .attr_count = 0, .name_off = k_str_span, .text_off = 0, .first_attr = k_book_nil, .first_child = k_book_nil, .next_sibling = k_book_nil },
    };
    const images = [_]paged.BookImage{
        .{ .id_off = 0, .width = 3, .height = 2, .format = 0, .pixel_format = 0, .reserved2 = 0, .data_off = 0, .data_size = 3, .raw_size = 3 },
        .{ .id_off = 0, .width = 3, .height = 2, .format = 0, .pixel_format = 1, .reserved2 = 0, .data_off = 8, .data_size = 6, .raw_size = 6 },
    };
    @memcpy(g_blob[k_chapter_off..][0..12], std.mem.asBytes(&chapters));
    @memcpy(g_blob[k_node_off..][0..72], std.mem.asBytes(&nodes));
    @memcpy(g_blob[k_image_off..][0..48], std.mem.asBytes(&images));
    @memcpy(g_blob[k_string_off..][0..pool.len], pool);
    @memcpy(g_blob[k_pool_off..][0..pool_bytes.len], pool_bytes);
    return g_blob[0..];
}

fn bindResident(src: *paged.BookSrc, blob: []const u8) void {
    const err = paged.book_src_resident(src, @ptrCast(blob.ptr), @intCast(blob.len));
    std.testing.expectEqual(k_ra8_ok, err) catch unreachable;
}

fn bindPaged(src: *paged.BookSrc, blob: []const u8, frame_bytes: u32) void {
    g_vm = .{ .backing = blob, .frame_bytes = frame_bytes };
    const err = paged.book_src_paged(src, @ptrCast(&g_vm), 7, frame_bytes, @intCast(blob.len));
    std.testing.expectEqual(k_ra8_ok, err) catch unreachable;
}

// ---- Tests -----------------------------------------------------------------

test "book_src_resident gates" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_resident(null, @ptrCast(blob.ptr), @intCast(blob.len)));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_resident(&src, null, @intCast(blob.len)));
    try std.testing.expectEqual(k_ra8_err_invalid_size, paged.book_src_resident(&src, @ptrCast(blob.ptr), 0));
}

test "book_src_resident binds and copies the header" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    try std.testing.expect(src.base != null);
    try std.testing.expect(src.vm == null);
    try std.testing.expectEqual(@as(u32, @intCast(blob.len)), src.size);
    try std.testing.expectEqual(@as(u32, 0), src.object_id);
    try std.testing.expectEqualSlices(u8, blob[0..100], std.mem.asBytes(&src.hdr));
}

test "book_src_paged gates" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_paged(null, @ptrCast(&g_vm), 7, 4, @intCast(blob.len)));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_paged(&src, null, 7, 4, @intCast(blob.len)));
    try std.testing.expectEqual(k_ra8_err_invalid_size, paged.book_src_paged(&src, @ptrCast(&g_vm), 7, 4, 0));
    try std.testing.expectEqual(k_ra8_err_invalid_size, paged.book_src_paged(&src, @ptrCast(&g_vm), 7, 1, @intCast(blob.len)));
}

test "book_src_paged binds and caches the header" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindPaged(&src, blob, 4);
    try std.testing.expect(src.base == null);
    try std.testing.expect(src.vm != null);
    try std.testing.expectEqual(@as(u32, 7), src.object_id);
    try std.testing.expectEqualSlices(u8, blob[0..100], std.mem.asBytes(&src.hdr));
}

test "book_src_read resident: whole blob, range gate, zero len, null gates" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    var buf: [k_blob_size]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_read(null, 0, &buf, 4));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_read(&src, 0, null, 4));
    try std.testing.expectEqual(k_ra8_err_out_of_range, paged.book_src_read(&src, @intCast(blob.len - 1), &buf, 2));
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_read(&src, 0, &buf, 0));
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_read(&src, 0, &buf, @intCast(blob.len)));
    try std.testing.expectEqualSlices(u8, blob, buf[0..blob.len]);
}

test "book_src_read unbound source is invalid state" {
    var src: paged.BookSrc = std.mem.zeroes(paged.BookSrc);
    src.size = k_blob_size;
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_invalid_state, paged.book_src_read(&src, 0, &buf, 4));
}

test "book_src_read paged: frames span the copy" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindPaged(&src, blob, 4);
    var buf: [16]u8 = undefined;
    // A 7-byte read at offset 2 crosses four frames (2..9): the bounded
    // copy-out must reassemble it byte-for-byte.
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_read(&src, 2, &buf, 7));
    try std.testing.expectEqualSlices(u8, blob[2..9], buf[0..7]);
    try std.testing.expect(g_vm.puts >= 4);
}

test "book_src_read paged: vmem fault propagates verbatim" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindPaged(&src, blob, 4);
    var buf: [4]u8 = undefined;
    g_vm.fault_code = k_ra8_err_out_of_range;
    try std.testing.expectEqual(k_ra8_err_out_of_range, paged.book_src_read(&src, 0, &buf, 4));
    try std.testing.expectEqual(k_ra8_ok, g_vm.fault_code); // consumed
}

test "book_src_image: gates, index gate, descriptor copy" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    var img: paged.BookImage = undefined;
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_image(null, 0, &img));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_image(&src, 0, null));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_src_image(&src, 2, &img));
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image(&src, 1, &img));
    try std.testing.expectEqual(@as(u16, 3), img.width);
    try std.testing.expectEqual(@as(u8, 1), img.pixel_format);
    try std.testing.expectEqual(@as(u32, 8), img.data_off);
}

test "book_src_image_rect: argument and geometry gates" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    var img: paged.BookImage = undefined;
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image(&src, 0, &img));
    var out: [9]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_image_rect(null, &img, 0, 0, 1, 1, &out, 3));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_image_rect(&src, null, 0, 0, 1, 1, &out, 3));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_image_rect(&src, &img, 0, 0, 1, 1, null, 3));
    // SVG has no pixel grid; a corrupt pixel_format is refused too.
    var svg = img;
    svg.format = 1;
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_src_image_rect(&src, &svg, 0, 0, 1, 1, &out, 3));
    var bad_pf = img;
    bad_pf.pixel_format = 2;
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_src_image_rect(&src, &bad_pf, 0, 0, 1, 1, &out, 3));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_src_image_rect(&src, &img, 0, 0, 0, 1, &out, 3));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_src_image_rect(&src, &img, 0, 0, 1, 1, &out, 0));
    try std.testing.expectEqual(k_ra8_err_out_of_range, paged.book_src_image_rect(&src, &img, 2, 0, 2, 1, &out, 3));
    try std.testing.expectEqual(k_ra8_err_out_of_range, paged.book_src_image_rect(&src, &img, 0, 1, 1, 2, &out, 3));
}

test "book_src_image_rect gray4: nibble parity and odd width" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    var img: paged.BookImage = undefined;
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image(&src, 0, &img));
    var out: [9]u8 = undefined;
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image_rect(&src, &img, 0, 0, 3, 2, &out, 3));
    // Nibble expand: high nibble even index, low nibble odd.
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, out[0..6]);
    // Sub-rect starting on an odd flat index keeps the parity correct.
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image_rect(&src, &img, 1, 0, 2, 1, &out, 3));
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x22, 0x33 }, out[0..2]);
}

test "book_src_image_rect gray8: verbatim rows over the pool" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    var img: paged.BookImage = undefined;
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image(&src, 1, &img));
    var out: [9]u8 = undefined;
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image_rect(&src, &img, 0, 0, 3, 2, &out, 3));
    try std.testing.expectEqualSlices(u8, pool_bytes[8..14], out[0..6]);
    // One row mid-image.
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_image_rect(&src, &img, 0, 1, 3, 1, &out, 3));
    try std.testing.expectEqualSlices(u8, pool_bytes[11..14], out[0..3]);
}

test "book_chapter_text_src: resident delegates to the fast path" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    var buf: [32]u8 = undefined;
    var len: usize = 0;
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_chapter_text_src(null, 0, &buf, buf.len, &len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_chapter_text_src(&src, 0, null, buf.len, &len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_chapter_text_src(&src, 0, &buf, buf.len, null));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_chapter_text_src(&src, 1, &buf, buf.len, &len));
    try std.testing.expectEqual(k_ra8_ok, paged.book_chapter_text_src(&src, 0, &buf, buf.len, &len));
    try std.testing.expectEqualStrings("RESIDENT", buf[0..len]);
}

test "book_chapter_text_src paged: block break then text run" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindPaged(&src, blob, 16);
    var buf: [64]u8 = undefined;
    var len: usize = 0;
    try std.testing.expectEqual(k_ra8_ok, paged.book_chapter_text_src(&src, 0, &buf, buf.len, &len));
    try std.testing.expectEqualStrings("\nhello world", buf[0..len]);
}

test "book_chapter_text_src paged: overflow reports invalid size" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindPaged(&src, blob, 16);
    var buf: [8]u8 = undefined;
    var len: usize = 0;
    try std.testing.expectEqual(k_ra8_err_invalid_size, paged.book_chapter_text_src(&src, 0, &buf, buf.len, &len));
}

test "book_src_prefetch_chapter: gates and root-node warm" {
    var src: paged.BookSrc = undefined;
    const blob = buildBlob();
    bindResident(&src, blob);
    try std.testing.expectEqual(k_ra8_err_null_ptr, paged.book_src_prefetch_chapter(null, 0));
    try std.testing.expectEqual(k_ra8_err_invalid_state, paged.book_src_prefetch_chapter(&src, 0));
    bindPaged(&src, blob, 16);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, paged.book_src_prefetch_chapter(&src, 1));
    g_vm.prefetch_count = 0;
    try std.testing.expectEqual(k_ra8_ok, paged.book_src_prefetch_chapter(&src, 0));
    try std.testing.expectEqual(@as(usize, 1), g_vm.prefetch_count);
    // Chapter 0's root node sits at node_off + 0 * sizeof(node).
    try std.testing.expectEqual(@as(u64, k_node_off), g_vm.prefetches[0]);
}
