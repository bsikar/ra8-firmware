//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Wire decoding and canonical layout validation for streamed books
//! (RA8FW-1242). Ported 1:1 from apps/shared_libs/book/src/book_stream_wire.c,
//! deleted in the same commit: the exact-read guard, fixed-header decoding,
//! gap-free table geometry and interned-string boundary checks used by the
//! semantic stream passes. Keeping wire concerns here leaves book_stream.c
//! responsible for coordinating the chapter, DOM, attribute, stylesheet,
//! image and integrity validators.
//!
//! The five `priv_book_stream_*` C ABI symbols are kept for the callers that
//! remain C (book_stream.c and the book_stream test suite); the archive
//! (libra8_book.a) provides them wherever the C TU used to.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;

/// Wire offsets inside the fixed 100-byte RABOOK1 header.
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
const k_stream_hdr_crc: usize = 96;

/// Byte length of one chapter-table record (inc/book.h).
const k_book_sizeof_header: u64 = 100;
const k_book_sizeof_chapter: u32 = 12;
const k_book_sizeof_node: u32 = 24;
const k_book_sizeof_attr: u32 = 8;
const k_book_sizeof_stylesheet: u32 = 8;
const k_book_sizeof_image: u32 = 24;

/// Current `.rabook` layout revision (inc/book.h).
const k_book_format_version: u32 = 1;
/// Every flag bit this firmware understands (inc/book.h).
const k_book_flag_mask_known: u32 = 0x00000001;

/// Bit geometry shared by wire decoding and the node ownership map.
const k_stream_bits_per_byte: u64 = 8;
const k_stream_mark_round: u64 = 7;

/// Host-order mirror of `book_header_t` (inc/book.h, size pinned at 100).
pub const BookHeader = extern struct {
    magic: [8]u8,
    format_version: u32,
    total_size: u32,
    flags: u32,
    title_off: u32,
    author_off: u32,
    language_off: u32,
    identifier_off: u32,
    cover_image_index: u32,
    chapter_count: u32,
    chapter_off: u32,
    node_count: u32,
    node_off: u32,
    attr_count: u32,
    attr_off: u32,
    stylesheet_count: u32,
    stylesheet_off: u32,
    image_count: u32,
    image_off: u32,
    string_off: u32,
    string_size: u32,
    image_pool_off: u32,
    image_pool_size: u32,
    crc32_val: u32,

    comptime {
        if (@sizeOf(BookHeader) != k_book_sizeof_header) {
            @compileError("book_header_t size pinned at 100 bytes");
        }
    }
};

/// `ra8_vsource_read_fn` (libs/ra8_mem/inc/ra8_vsource.h): the exact-read seam.
const VsourceReadFn = *const fn (?*anyopaque, u64, [*]u8, u32) callconv(.c) ra8_err_t;

/// Host-order mirror of `stream_validate_t` (src/book_stream_internal.h).
pub const StreamValidate = extern struct {
    read: VsourceReadFn,
    read_ctx: ?*anyopaque,
    source_size: u64,
    scratch: ?[*]u8,
    scratch_cap: u32,
    hdr: BookHeader,
};

/// Decode one little-endian 32-bit field from four bytes.
fn le32(raw: []const u8) u32 {
    return @as(u32, raw[0]) | (@as(u32, raw[1]) << 8) |
        (@as(u32, raw[2]) << 16) | (@as(u32, raw[3]) << 24);
}

/// The exported C ABI seam behind `priv_book_stream_read`: one exact,
/// bounded source span, rejected before the callback ever runs.
pub export fn priv_book_stream_read(
    ctx: *const StreamValidate,
    off: u64,
    dst: [*]u8,
    len: u32,
) callconv(.c) ra8_err_t {
    if ((off > ctx.source_size) or (@as(u64, len) > (ctx.source_size - off))) {
        return k_ra8_err_invalid_size;
    }
    return ctx.read(ctx.read_ctx, off, dst, len);
}

/// Decode the fixed header from canonical little-endian wire bytes.
fn decodeHeader(raw: *const [k_book_sizeof_header]u8, hdr: *BookHeader) void {
    hdr.magic = raw[0..8].*;
    hdr.format_version = le32(raw[k_stream_hdr_version..][0..4]);
    hdr.total_size = le32(raw[k_stream_hdr_total..][0..4]);
    hdr.flags = le32(raw[k_stream_hdr_flags..][0..4]);
    hdr.title_off = le32(raw[k_stream_hdr_title..][0..4]);
    hdr.author_off = le32(raw[k_stream_hdr_author..][0..4]);
    hdr.language_off = le32(raw[k_stream_hdr_language..][0..4]);
    hdr.identifier_off = le32(raw[k_stream_hdr_identifier..][0..4]);
    hdr.cover_image_index = le32(raw[k_stream_hdr_cover..][0..4]);
    hdr.chapter_count = le32(raw[k_stream_hdr_chapter_count..][0..4]);
    hdr.chapter_off = le32(raw[k_stream_hdr_chapter_off..][0..4]);
    hdr.node_count = le32(raw[k_stream_hdr_node_count..][0..4]);
    hdr.node_off = le32(raw[k_stream_hdr_node_off..][0..4]);
    hdr.attr_count = le32(raw[k_stream_hdr_attr_count..][0..4]);
    hdr.attr_off = le32(raw[k_stream_hdr_attr_off..][0..4]);
    hdr.stylesheet_count = le32(raw[k_stream_hdr_style_count..][0..4]);
    hdr.stylesheet_off = le32(raw[k_stream_hdr_style_off..][0..4]);
    hdr.image_count = le32(raw[k_stream_hdr_image_count..][0..4]);
    hdr.image_off = le32(raw[k_stream_hdr_image_off..][0..4]);
    hdr.string_off = le32(raw[k_stream_hdr_string_off..][0..4]);
    hdr.string_size = le32(raw[k_stream_hdr_string_size..][0..4]);
    hdr.image_pool_off = le32(raw[k_stream_hdr_pool_off..][0..4]);
    hdr.image_pool_size = le32(raw[k_stream_hdr_pool_size..][0..4]);
    hdr.crc32_val = le32(raw[k_stream_hdr_crc..][0..4]);
}

/// Require one segment to begin at the canonical cursor and advance it:
/// gap-free table layout, with the count-by-element product in 64 bits
/// before a 32-bit wire offset is accepted.
fn layoutSegment(off: u32, count: u32, elem: u32, cursor: *u64) ra8_err_t {
    if (@as(u64, off) != cursor.*) {
        return k_ra8_err_invalid_size;
    }
    const end = cursor.* + (@as(u64, count) * @as(u64, elem));
    if (end > 0xFFFFFFFF) {
        return k_ra8_err_invalid_size;
    }
    cursor.* = end;
    return k_ra8_ok;
}

/// Validate decoded header fields before walking the table layout:
/// magic, supported features, exact source length and scratch
/// ownership-map capacity, without reading any table data.
fn validateHeaderFields(ctx: *const StreamValidate) ra8_err_t {
    const magic = [8]u8{ 'R', 'A', 'B', 'O', 'O', 'K', '1', 0 };
    if (!std.mem.eql(u8, &ctx.hdr.magic, &magic)) {
        return k_ra8_err_invalid_arg;
    }
    if ((ctx.hdr.format_version != k_book_format_version) or
        ((ctx.hdr.flags & ~k_book_flag_mask_known) != 0))
    {
        return k_ra8_err_invalid_arg;
    }
    if (@as(u64, ctx.hdr.total_size) != ctx.source_size) {
        return k_ra8_err_invalid_size;
    }
    const node_mark_bytes = (@as(u64, ctx.hdr.node_count) + k_stream_mark_round) /
        k_stream_bits_per_byte;
    return if (node_mark_bytes > @as(u64, ctx.scratch_cap))
        k_ra8_err_invalid_size
    else
        k_ra8_ok;
}

/// Validate version, flags, exact source length and canonical layout:
/// the decoded fields first, then every table and pool in canonical
/// wire order.
fn validateHeaderLayout(ctx: *const StreamValidate) ra8_err_t {
    var cursor: u64 = k_book_sizeof_header;
    var err = validateHeaderFields(ctx);
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.chapter_off, ctx.hdr.chapter_count, k_book_sizeof_chapter, &cursor);
    }
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.node_off, ctx.hdr.node_count, k_book_sizeof_node, &cursor);
    }
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.attr_off, ctx.hdr.attr_count, k_book_sizeof_attr, &cursor);
    }
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.stylesheet_off, ctx.hdr.stylesheet_count, k_book_sizeof_stylesheet, &cursor);
    }
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.image_off, ctx.hdr.image_count, k_book_sizeof_image, &cursor);
    }
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.string_off, ctx.hdr.string_size, 1, &cursor);
    }
    if (err == k_ra8_ok) {
        err = layoutSegment(ctx.hdr.image_pool_off, ctx.hdr.image_pool_size, 1, &cursor);
    }
    if ((err == k_ra8_ok) and (cursor != @as(u64, ctx.hdr.total_size))) {
        err = k_ra8_err_invalid_size;
    }
    return err;
}

/// The exported C ABI seam behind `priv_book_stream_read_validate_header`:
/// read, decode, and validate the canonical stream header and layout.
pub export fn priv_book_stream_read_validate_header(ctx: *StreamValidate) callconv(.c) ra8_err_t {
    var raw: [k_book_sizeof_header]u8 = @splat(0);
    var err = priv_book_stream_read(ctx, 0, &raw, @sizeOf(@TypeOf(raw)));
    if (err == k_ra8_ok) {
        decodeHeader(&raw, &ctx.hdr);
        err = validateHeaderLayout(ctx);
    }
    return err;
}

/// The exported C ABI seam behind `priv_book_stream_string_ref`: require a
/// referenced offset to name an interned-string boundary (the empty-string
/// sentinel at zero, otherwise in range and preceded by a NUL).
pub export fn priv_book_stream_string_ref(ctx: *const StreamValidate, off: u32) callconv(.c) ra8_err_t {
    if (off >= ctx.hdr.string_size) {
        return k_ra8_err_invalid_arg;
    }
    if (off == 0) {
        return k_ra8_ok;
    }
    var preceding: u8 = 0;
    var err = priv_book_stream_read(ctx, @as(u64, ctx.hdr.string_off) + @as(u64, off) - 1, @ptrCast(&preceding), 1);
    if ((err == k_ra8_ok) and (preceding != 0)) {
        err = k_ra8_err_invalid_arg;
    }
    return err;
}

/// The exported C ABI seam behind `priv_book_stream_nonempty_string_ref`:
/// require a string reference to name a non-empty interned string.
pub export fn priv_book_stream_nonempty_string_ref(ctx: *const StreamValidate, off: u32) callconv(.c) ra8_err_t {
    var err = priv_book_stream_string_ref(ctx, off);
    var first: u8 = 0;
    if (err == k_ra8_ok) {
        err = priv_book_stream_read(ctx, @as(u64, ctx.hdr.string_off) + @as(u64, off), @ptrCast(&first), 1);
    }
    if ((err == k_ra8_ok) and (first == 0)) {
        err = k_ra8_err_invalid_arg;
    }
    return err;
}

/// The exported C ABI seam behind `priv_book_stream_validate_string_envelope`:
/// validate the string pool's leading and trailing NUL sentinels.
pub export fn priv_book_stream_validate_string_envelope(ctx: *const StreamValidate) callconv(.c) ra8_err_t {
    if (ctx.hdr.string_size == 0) {
        return k_ra8_err_invalid_size;
    }
    var first: u8 = 0;
    var last: u8 = 0;
    var err = priv_book_stream_read(ctx, @as(u64, ctx.hdr.string_off), @ptrCast(&first), 1);
    if (err == k_ra8_ok) {
        err = priv_book_stream_read(
            ctx,
            @as(u64, ctx.hdr.string_off) + @as(u64, ctx.hdr.string_size) - 1,
            @ptrCast(&last),
            1,
        );
    }
    if ((err == k_ra8_ok) and ((first != 0) or (last != 0))) {
        err = k_ra8_err_invalid_arg;
    }
    return err;
}
