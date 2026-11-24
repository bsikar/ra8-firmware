//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-side tests for the strict stream-validator port (RA8FW-1248). The C
//! suite (test_book_stream.c, test_book_stream_mcdc.c) keeps covering the
//! validator from the outside through the unchanged C ABI; these exercise
//! the port's own core paths: argument gates, header/scratch checks, the
//! semantic table passes, image geometry and pool tiling, and the body
//! CRC, over built-in-memory canonical RABOOK1 blobs.

const std = @import("std");
const stream = @import("stream");
const wire = stream.wire;

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_range_check_failed: u16 = 0x503;
const k_ra8_err_null_ptr: u16 = 0x504;

const k_book_nil: u32 = 0xFFFFFFFF;
const k_stream_hdr_crc: usize = 96;

/// The shared incremental CRC-32/ISO-HDLC from book.c, which stays C; the
/// test module links no C archive, so the stub mirrors book.c's algorithm
/// (reflected 0xEDB88320, init and final XOR 0xFFFFFFFF, check value
/// 0xCBF43926 over "123456789").
export fn priv_book_crc32_extend(crc: u32, data: [*]const u8, len: usize) callconv(.c) u32 {
    var state = crc ^ 0xFFFFFFFF;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        state ^= data[i];
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const mask = (state & 1) * 0xEDB88320;
            state = (state >> 1) ^ mask;
        }
    }
    return state ^ 0xFFFFFFFF;
}

// ---- Built-in-memory RABOOK1 blobs ----------------------------------------

/// The interned-string pool every blob shares: offset 0 is the empty-string
/// sentinel, offset 1 names the non-empty string "name", and offsets 6 and
/// 7 name empty strings at valid boundaries.
const k_strings = [8]u8{ 0, 'n', 'a', 'm', 'e', 0, 0, 0 };
const k_string_name: u32 = 1;
const k_string_empty_tail: u32 = 6;

const k_stream_hdr_version: usize = 8;
const k_stream_hdr_total: usize = 12;
const k_stream_hdr_flags: usize = 16;
const k_stream_hdr_title: usize = 20;
const k_stream_hdr_author: usize = 24;
const k_stream_hdr_language: usize = 28;
const k_stream_hdr_identifier: usize = 32;
const k_stream_hdr_cover: usize = 36;
const k_stream_hdr_chapter_count: usize = 40;
const k_stream_hdr_chapter_off: usize = 44;
const k_stream_hdr_node_count: usize = 48;
const k_stream_hdr_node_off: usize = 52;
const k_stream_hdr_attr_count: usize = 56;
const k_stream_hdr_attr_off: usize = 60;
const k_stream_hdr_style_count: usize = 64;
const k_stream_hdr_style_off: usize = 68;
const k_stream_hdr_image_count: usize = 72;
const k_stream_hdr_image_off: usize = 76;
const k_stream_hdr_string_off: usize = 80;
const k_stream_hdr_string_size: usize = 84;
const k_stream_hdr_pool_off: usize = 88;
const k_stream_hdr_pool_size: usize = 92;

const Blob = struct {
    bytes: [4096]u8 = @splat(0),
    len: usize = 0,

    fn put(self: *Blob, raw: []const u8) void {
        @memcpy(self.bytes[self.len..][0..raw.len], raw);
        self.len += raw.len;
    }

    fn setU8(self: *Blob, off: usize, v: u8) void {
        self.bytes[off] = v;
    }

    fn setU16(self: *Blob, off: usize, v: u16) void {
        std.mem.writeInt(u16, self.bytes[off..][0..2], v, .little);
    }

    fn setU32(self: *Blob, off: usize, v: u32) void {
        std.mem.writeInt(u32, self.bytes[off..][0..4], v, .little);
    }

    /// Recompute the body CRC after any table or metadata patch.
    fn fixCrc(self: *Blob) void {
        self.setU32(k_stream_hdr_crc, crc32(self.bytes[100..self.len]));
    }
};

fn crc32(data: []const u8) u32 {
    return priv_book_crc32_extend(0, data.ptr, data.len);
}

fn chapterRec(title: u32, id: u32, root: u32) [12]u8 {
    var rec: [12]u8 = @splat(0);
    std.mem.writeInt(u32, rec[0..4], title, .little);
    std.mem.writeInt(u32, rec[4..8], id, .little);
    std.mem.writeInt(u32, rec[8..12], root, .little);
    return rec;
}

fn nodeElement(
    name: u32,
    text: u32,
    first_attr: u32,
    attr_count: u16,
    first_child: u32,
    next_sibling: u32,
) [24]u8 {
    var rec: [24]u8 = @splat(0);
    rec[0] = 0; // k_book_node_element
    rec[1] = 0;
    std.mem.writeInt(u16, rec[2..4], attr_count, .little);
    std.mem.writeInt(u32, rec[4..8], name, .little);
    std.mem.writeInt(u32, rec[8..12], text, .little);
    std.mem.writeInt(u32, rec[12..16], first_attr, .little);
    std.mem.writeInt(u32, rec[16..20], first_child, .little);
    std.mem.writeInt(u32, rec[20..24], next_sibling, .little);
    return rec;
}

fn nodeText(text: u32) [24]u8 {
    var rec: [24]u8 = @splat(0);
    rec[0] = 1; // k_book_node_text
    std.mem.writeInt(u32, rec[8..12], text, .little);
    std.mem.writeInt(u32, rec[12..16], k_book_nil, .little); // first_attr
    std.mem.writeInt(u32, rec[16..20], k_book_nil, .little); // first_child
    std.mem.writeInt(u32, rec[20..24], k_book_nil, .little); // next_sibling
    return rec;
}

fn attrRec(name: u32, value: u32) [8]u8 {
    var rec: [8]u8 = @splat(0);
    std.mem.writeInt(u32, rec[0..4], name, .little);
    std.mem.writeInt(u32, rec[4..8], value, .little);
    return rec;
}

fn styleRec(src: u32, scope: u32) [8]u8 {
    var rec: [8]u8 = @splat(0);
    std.mem.writeInt(u32, rec[0..4], src, .little);
    std.mem.writeInt(u32, rec[4..8], scope, .little);
    return rec;
}

fn rasterRec(id: u32, w: u16, h: u16, data_off: u32, data_size: u32) [24]u8 {
    var rec: [24]u8 = @splat(0);
    std.mem.writeInt(u32, rec[0..4], id, .little);
    std.mem.writeInt(u16, rec[4..6], w, .little);
    std.mem.writeInt(u16, rec[6..8], h, .little);
    rec[8] = 0; // k_book_image_gray4
    rec[9] = 0; // k_book_pixfmt_gray4
    rec[10] = 0;
    rec[11] = 0;
    std.mem.writeInt(u32, rec[12..16], data_off, .little);
    std.mem.writeInt(u32, rec[16..20], data_size, .little);
    std.mem.writeInt(u32, rec[20..24], data_size, .little);
    return rec;
}

fn svgRec(id: u32, size: u32) [24]u8 {
    var rec: [24]u8 = @splat(0);
    std.mem.writeInt(u32, rec[0..4], id, .little);
    rec[8] = 1; // k_book_image_svg
    rec[9] = 0;
    std.mem.writeInt(u32, rec[12..16], 0, .little); // data_off tiles from zero
    std.mem.writeInt(u32, rec[16..20], size, .little);
    std.mem.writeInt(u32, rec[20..24], size, .little);
    return rec;
}

/// Assemble a canonical blob: header, then the gap-free table order
/// (chapters, nodes, attrs, stylesheets, images, strings, image pool),
/// then the CRC over everything after the header.
fn buildBlob(
    title: u32,
    author: u32,
    language: u32,
    identifier: u32,
    cover: u32,
    chapters: []const [12]u8,
    nodes: []const [24]u8,
    attrs: []const [8]u8,
    styles: []const [8]u8,
    images: []const [24]u8,
    strings: []const u8,
    pool: []const u8,
    blob: *Blob,
) void {
    blob.* = .{};
    blob.len = 100; // the fixed header is zeroed but reserved up front
    @memcpy(blob.bytes[0..8], "RABOOK1\x00"); // the magic is written in place
    var cursor: usize = 100;
    const chapter_off = cursor;
    cursor += chapters.len * 12;
    const node_off = cursor;
    cursor += nodes.len * 24;
    const attr_off = cursor;
    cursor += attrs.len * 8;
    const style_off = cursor;
    cursor += styles.len * 8;
    const image_off = cursor;
    cursor += images.len * 24;
    const string_off = cursor;
    cursor += strings.len;
    const pool_off = cursor;
    cursor += pool.len;

    blob.setU32(k_stream_hdr_version, 1);
    blob.setU32(k_stream_hdr_total, @intCast(cursor));
    blob.setU32(k_stream_hdr_flags, 0);
    blob.setU32(k_stream_hdr_title, title);
    blob.setU32(k_stream_hdr_author, author);
    blob.setU32(k_stream_hdr_language, language);
    blob.setU32(k_stream_hdr_identifier, identifier);
    blob.setU32(k_stream_hdr_cover, cover);
    blob.setU32(k_stream_hdr_chapter_count, @intCast(chapters.len));
    blob.setU32(k_stream_hdr_chapter_off, @intCast(chapter_off));
    blob.setU32(k_stream_hdr_node_count, @intCast(nodes.len));
    blob.setU32(k_stream_hdr_node_off, @intCast(node_off));
    blob.setU32(k_stream_hdr_attr_count, @intCast(attrs.len));
    blob.setU32(k_stream_hdr_attr_off, @intCast(attr_off));
    blob.setU32(k_stream_hdr_style_count, @intCast(styles.len));
    blob.setU32(k_stream_hdr_style_off, @intCast(style_off));
    blob.setU32(k_stream_hdr_image_count, @intCast(images.len));
    blob.setU32(k_stream_hdr_image_off, @intCast(image_off));
    blob.setU32(k_stream_hdr_string_off, @intCast(string_off));
    blob.setU32(k_stream_hdr_string_size, @intCast(strings.len));
    blob.setU32(k_stream_hdr_pool_off, @intCast(pool_off));
    blob.setU32(k_stream_hdr_pool_size, @intCast(pool.len));
    for (chapters) |rec| blob.put(&rec);
    for (nodes) |rec| blob.put(&rec);
    for (attrs) |rec| blob.put(&rec);
    for (styles) |rec| blob.put(&rec);
    for (images) |rec| blob.put(&rec);
    blob.put(strings);
    blob.put(pool);
    blob.fixCrc();
}

/// The minimal valid book: one chapter whose root is one element node, an
/// empty string tail, and no attributes, stylesheets, or images.
fn buildMinimal(blob: *Blob) void {
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &.{},
        &k_strings,
        &.{},
        blob,
    );
}

fn memRead(ctx: ?*anyopaque, off: u64, dst: [*]u8, len: u32) callconv(.c) u16 {
    const blob: *const Blob = @ptrCast(@alignCast(ctx.?));
    if ((off + len) > blob.len) {
        return k_ra8_err_invalid_size;
    }
    @memcpy(dst[0..len], blob.bytes[off..][0..len]);
    return k_ra8_ok;
}

const Harness = struct {
    blob: Blob = .{},
    scratch: [64]u8 = @splat(0),
    hdr: wire.BookHeader = std.mem.zeroes(wire.BookHeader),
};

fn strictRun(h: *Harness) u16 {
    return stream.book_validate_stream_strict(
        memRead,
        &h.blob,
        h.blob.len,
        &h.scratch,
        h.scratch.len,
        &h.hdr,
    );
}

// ---- Tests ----------------------------------------------------------------

test "valid minimal book validates and decodes" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    try std.testing.expectEqual(k_ra8_ok, strictRun(&h));
    try std.testing.expectEqual(@as(u32, 1), h.hdr.chapter_count);
    try std.testing.expectEqual(@as(u32, 1), h.hdr.node_count);
    try std.testing.expectEqual(@as(u32, 144), h.hdr.total_size);
    try std.testing.expectEqualStrings("RABOOK1\x00", &h.hdr.magic);
}

test "null argument gates fail closed in documented order" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        stream.book_validate_stream_strict(memRead, &h.blob, h.blob.len, &h.scratch, h.scratch.len, null),
    );
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        stream.book_validate_stream_strict(null, &h.blob, h.blob.len, &h.scratch, h.scratch.len, &h.hdr),
    );
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        stream.book_validate_stream_strict(memRead, &h.blob, h.blob.len, null, h.scratch.len, &h.hdr),
    );
}

test "short source and zero scratch cap fail closed" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        stream.book_validate_stream_strict(memRead, &h.blob, 99, &h.scratch, h.scratch.len, &h.hdr),
    );
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        stream.book_validate_stream_strict(memRead, &h.blob, h.blob.len, &h.scratch, 0, &h.hdr),
    );
}

test "failed validation leaves the output header zeroed" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU8(0, 'X'); // corrupt the magic
    h.blob.setU32(k_stream_hdr_total, 999); // and keep the CRC stale
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
    try std.testing.expectEqual(@as(u32, 0), h.hdr.total_size);
}

test "chapter strings and root geometry gate" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU32(100, k_string_empty_tail); // title -> empty tail is fine...
    h.blob.setU32(96 + 0, crc32(h.blob.bytes[100..h.blob.len]));
    try std.testing.expectEqual(k_ra8_ok, strictRun(&h));

    var bad: Harness = .{};
    buildMinimal(&bad.blob);
    bad.blob.setU32(100 + 4, 0); // chapter id -> empty string
    bad.blob.setU32(k_stream_hdr_crc, crc32(bad.blob.bytes[100..bad.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&bad));

    var oob: Harness = .{};
    buildMinimal(&oob.blob);
    oob.blob.setU32(100 + 8, 1); // root beyond node_count
    oob.blob.setU32(k_stream_hdr_crc, crc32(oob.blob.bytes[100..oob.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&oob));
}

test "chapter root must be a sibling-free element" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU8(112 + 0, 1); // root is a text node
    h.blob.setU32(k_stream_hdr_crc, crc32(h.blob.bytes[100..h.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));

    var sib: Harness = .{};
    buildMinimal(&sib.blob);
    sib.blob.setU32(112 + 20, 0); // root has a next_sibling link
    sib.blob.setU32(k_stream_hdr_crc, crc32(sib.blob.bytes[100..sib.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&sib));
}

test "duplicate chapter roots fail the ownership bitset" {
    var h: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{ chapterRec(k_string_name, k_string_name, 0), chapterRec(k_string_name, k_string_name, 0) },
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &.{},
        &k_strings,
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
}

test "node reserved field must be zero" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU8(112 + 1, 7);
    h.blob.setU32(k_stream_hdr_crc, crc32(h.blob.bytes[100..h.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
}

test "element-only fields gate text on elements" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU32(112 + 8, k_string_name); // element with a text run
    h.blob.setU32(k_stream_hdr_crc, crc32(h.blob.bytes[100..h.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
}

test "element attribute span must sit at the canonical cursor" {
    var h: Harness = .{};
    const attrs = [_][8]u8{attrRec(k_string_name, k_string_empty_tail)};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, 1, 1, k_book_nil, k_book_nil)}, // first != cursor
        &attrs,
        &.{},
        &.{},
        &k_strings,
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));

    var none: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &attrs, // one attr nobody consumes
        &.{},
        &.{},
        &k_strings,
        &.{},
        &none.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&none));
}

test "element count zero requires nil first attribute" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU32(112 + 12, 0); // attr_count 0 but first_attr = 0
    h.blob.setU32(k_stream_hdr_crc, crc32(h.blob.bytes[100..h.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
}

test "text nodes reject element-only fields" {
    var h: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{
            nodeElement(k_string_name, 0, k_book_nil, 0, 1, k_book_nil),
            nodeText(k_string_name),
        },
        &.{},
        &.{},
        &.{},
        &k_strings,
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_ok, strictRun(&h));

    var named: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{
            nodeElement(k_string_name, 0, k_book_nil, 0, 1, k_book_nil),
            nodeText(k_string_empty_tail),
        },
        &.{},
        &.{},
        &.{},
        &k_strings,
        &.{},
        &named.blob,
    );
    named.blob.setU32(112 + 24 + 4, k_string_name); // text node with a name
    named.blob.setU32(k_stream_hdr_crc, crc32(named.blob.bytes[100..named.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&named));
}

test "uncovered node fails the ownership pass" {
    var h: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{
            nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil),
            nodeText(k_string_name),
        },
        &.{},
        &.{},
        &.{},
        &k_strings,
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
}

test "forward links must go strictly forward and stay unique" {
    var back: Harness = .{};
    buildMinimal(&back.blob);
    back.blob.setU32(112 + 16, 0); // first_child links to itself
    back.blob.setU32(k_stream_hdr_crc, crc32(back.blob.bytes[100..back.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&back));

    var dup: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{ chapterRec(k_string_name, k_string_name, 0), chapterRec(k_string_name, k_string_name, 1) },
        &.{
            nodeElement(k_string_name, 0, k_book_nil, 0, 2, k_book_nil),
            nodeElement(k_string_name, 0, k_book_nil, 0, 2, k_book_nil),
            nodeText(k_string_name),
        },
        &.{},
        &.{},
        &.{},
        &k_strings,
        &.{},
        &dup.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&dup));
}

test "attribute records require non-empty names" {
    var h: Harness = .{};
    const attrs = [_][8]u8{attrRec(0, k_string_empty_tail)};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, 0, 1, k_book_nil, k_book_nil)},
        &attrs,
        &.{},
        &.{},
        &k_strings,
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));
}

test "stylesheet scopes bound to existing chapters" {
    var h: Harness = .{};
    const styles = [_][8]u8{styleRec(k_string_name, 1)}; // scope == chapter_count
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &styles,
        &.{},
        &k_strings,
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&h));

    var ok: Harness = .{};
    const scoped = [_][8]u8{styleRec(k_string_name, k_book_nil)};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &scoped,
        &.{},
        &k_strings,
        &.{},
        &ok.blob,
    );
    try std.testing.expectEqual(k_ra8_ok, strictRun(&ok));
}

test "raster images carry exact packed-pixel geometry" {
    var h: Harness = .{};
    const images = [_][24]u8{rasterRec(k_string_name, 2, 2, 0, 2)};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &images,
        &k_strings,
        &.{ 0xAA, 0xBB },
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_ok, strictRun(&h));

    var wide: Harness = .{};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &images,
        &k_strings,
        &.{ 0xAA, 0xBB },
        &wide.blob,
    );
    wide.blob.setU16(wide.blob.len - 2 - 8 - 24 + 4, 0); // width 0
    wide.blob.setU32(k_stream_hdr_crc, crc32(wide.blob.bytes[100..wide.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, strictRun(&wide));
}

test "svg images carry zero geometry and raw-storage parity" {
    var h: Harness = .{};
    const images = [_][24]u8{svgRec(k_string_name, 3)};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &images,
        &k_strings,
        &.{ '<', 's', 'v' },
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_ok, strictRun(&h));

    var gap: Harness = .{};
    const tilted = [_][24]u8{svgRec(k_string_name, 3)};
    buildBlob(
        k_string_name,
        k_string_name,
        0,
        k_string_empty_tail,
        k_book_nil,
        &.{chapterRec(k_string_name, k_string_name, 0)},
        &.{nodeElement(k_string_name, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &tilted,
        &k_strings,
        &.{ '<', 's', 'v' },
        &gap.blob,
    );
    gap.blob.setU32(gap.blob.len - 3 - 8 - 24 + 12, 1); // data_off 1 leaves a pool gap
    gap.blob.setU32(k_stream_hdr_crc, crc32(gap.blob.bytes[100..gap.blob.len]));
    try std.testing.expectEqual(k_ra8_err_invalid_size, strictRun(&gap));
}

test "body CRC failure reports range_check_failed" {
    var h: Harness = .{};
    buildMinimal(&h.blob);
    h.blob.setU32(k_stream_hdr_crc, crc32(h.blob.bytes[100..h.blob.len]) ^ 1);
    try std.testing.expectEqual(k_ra8_err_range_check_failed, strictRun(&h));
}

test "empty string pool fails the envelope pass" {
    var h: Harness = .{};
    buildBlob(
        0,
        0,
        0,
        0,
        k_book_nil,
        &.{chapterRec(0, 0, 0)},
        &.{nodeElement(0, 0, k_book_nil, 0, k_book_nil, k_book_nil)},
        &.{},
        &.{},
        &.{},
        &.{},
        &.{},
        &h.blob,
    );
    try std.testing.expectEqual(k_ra8_err_invalid_size, strictRun(&h));
}
