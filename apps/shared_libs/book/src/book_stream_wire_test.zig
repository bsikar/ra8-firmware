//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-side tests for the book wire port (RA8FW-1242). The C suite
//! (book_stream_test) keeps covering the MC/DC seams from the outside;
//! these exercise the port's own core paths: header decode, canonical
//! layout walk, exact-read guard and the string-boundary checks.

const std = @import("std");
const wire = @import("book_stream_wire.zig");

const StreamValidate = wire.StreamValidate;
const BookHeader = wire.BookHeader;

/// Minimal valid RABOOK1 blob: 100-byte header, one chapter, empty
/// node/attr/stylesheet/image tables, a two-byte string pool and an
/// empty image pool.
const blob = [114]u8{
    'R', 'A', 'B', 'O', 'O', 'K', '1', 0, // magic[8]
    1, 0, 0, 0, // format_version = 1
    114, 0, 0, 0, // total_size = 114
    0, 0, 0, 0, // flags = 0
    0, 0, 0, 0, // title_off = 0
    0, 0, 0, 0, // author_off = 0
    0, 0, 0, 0, // language_off = 0
    0, 0, 0, 0, // identifier_off = 0
    0xFF, 0xFF, 0xFF, 0xFF, // cover_image_index = nil
    1, 0, 0, 0, // chapter_count = 1
    100, 0, 0, 0, // chapter_off = 100
    0, 0, 0, 0, // node_count = 0
    112, 0, 0, 0, // node_off = 112
    0, 0, 0, 0, // attr_count = 0
    112, 0, 0, 0, // attr_off = 112
    0, 0, 0, 0, // stylesheet_count = 0
    112, 0, 0, 0, // stylesheet_off = 112
    0, 0, 0, 0, // image_count = 0
    112, 0, 0, 0, // image_off = 112
    112, 0, 0, 0, // string_off = 112
    2, 0, 0, 0, // string_size = 2
    114, 0, 0, 0, // image_pool_off = 114
    0, 0, 0, 0, // image_pool_size = 0
    0, 0,   0, 0, // crc32_val = 0 (unchecked here)
    // [100..112) one chapter record (unchecked here)
    0, 0,   0, 0,
    0, 0,   0, 0,
    0, 0,   0, 0,
    // [112..114) string pool: NUL sentinel, then one non-NUL byte
    0, 'a',
};

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_size: u16 = 0x105;

var backing: []const u8 = &.{};

fn testRead(ctx: ?*anyopaque, offset: u64, buf: [*]u8, len: u32) callconv(.c) u16 {
    _ = ctx;
    if (offset + len > backing.len) {
        return k_ra8_err_invalid_size;
    }
    @memcpy(buf[0..len], backing[offset..][0..len]);
    return k_ra8_ok;
}

fn ctxFor(source: []const u8, scratch_cap: u32) StreamValidate {
    return .{
        .read = testRead,
        .read_ctx = null,
        .source_size = source.len,
        .scratch = null,
        .scratch_cap = scratch_cap,
        .hdr = std.mem.zeroes(BookHeader),
    };
}

test "valid minimal blob decodes and validates" {
    backing = &blob;
    var v = ctxFor(&blob, 0);
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read_validate_header(&v));
    try std.testing.expectEqualStrings("RABOOK1\x00", &v.hdr.magic);
    try std.testing.expectEqual(@as(u32, 1), v.hdr.format_version);
    try std.testing.expectEqual(@as(u32, 114), v.hdr.total_size);
    try std.testing.expectEqual(@as(u32, 1), v.hdr.chapter_count);
    try std.testing.expectEqual(@as(u32, 100), v.hdr.chapter_off);
    try std.testing.expectEqual(@as(u32, 2), v.hdr.string_size);
}

test "exact-read guard rejects out-of-range spans" {
    backing = &blob;
    const v = ctxFor(&blob, 0);
    var out: [4]u8 = @splat(0);
    // Past the end.
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        wire.priv_book_stream_read(&v, blob.len, &out, 1),
    );
    // Exactly at the end is still a zero-length success.
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read(&v, blob.len, &out, 0));
    // In range works and fills the destination.
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read(&v, 100, &out, 4));
    try std.testing.expectEqual(@as(u8, 0), out[3]);
}

test "bad magic, version and unknown flag bits are rejected" {
    var bad_magic = blob;
    bad_magic[6] = '2';
    backing = &bad_magic;
    var v = ctxFor(&bad_magic, 0);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        wire.priv_book_stream_read_validate_header(&v),
    );

    var bad_version = blob;
    bad_version[8] = 2;
    backing = &bad_version;
    v = ctxFor(&bad_version, 0);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        wire.priv_book_stream_read_validate_header(&v),
    );

    var unknown_flag = blob;
    unknown_flag[16] = 0x80; // bit outside k_book_flag_mask_known
    backing = &unknown_flag;
    v = ctxFor(&unknown_flag, 0);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        wire.priv_book_stream_read_validate_header(&v),
    );
}

test "total-size and scratch-capacity mismatches are invalid-size" {
    var short = blob;
    short[12] = 113; // total_size != source_size
    backing = &short;
    var v = ctxFor(&short, 0);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        wire.priv_book_stream_read_validate_header(&v),
    );

    // node_mark_bytes for node_count=0 is 0, so any capacity passes; force a
    // node_count that needs more mark bytes than the scratch offers.
    var marked = blob;
    marked[48] = 9; // node_count = 9 -> (9 + 7) / 8 = 2 mark bytes
    backing = &marked;
    v = ctxFor(&marked, 1);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        wire.priv_book_stream_read_validate_header(&v),
    );
}

test "layout walk rejects gaps and 32-bit overflow" {
    // Gap: push the string pool past the chapter table's end.
    var gapped = blob;
    gapped[80] = 113; // string_off = 113, cursor is at 112
    backing = &gapped;
    var v = ctxFor(&gapped, 0);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        wire.priv_book_stream_read_validate_header(&v),
    );

    // Overflow: a chapter count whose table end passes UINT32_MAX
    // (0x20000000 chapters * 12 bytes = 0x180000000 > 2^32).
    var huge = blob;
    huge[43] = 0x20; // chapter_count = 0x20000000
    backing = &huge;
    v = ctxFor(&huge, 0);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        wire.priv_book_stream_read_validate_header(&v),
    );
}

test "string references obey boundaries and sentinels" {
    backing = &blob;
    var v = ctxFor(&blob, 0);
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read_validate_header(&v));

    // The empty-string sentinel at zero is always fine.
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_string_ref(&v, 0));
    // Offset 1 is a real boundary (preceded by the NUL at pool offset 0).
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_string_ref(&v, 1));
    // Past the pool is out of range.
    try std.testing.expectEqual(k_ra8_err_invalid_arg, wire.priv_book_stream_string_ref(&v, 2));
    // Offset 1 names a non-empty string ('a').
    try std.testing.expectEqual(
        k_ra8_ok,
        wire.priv_book_stream_nonempty_string_ref(&v, 1),
    );

    // A mid-string reference (no preceding NUL) is rejected.
    var mid = blob;
    mid[112] = 'x'; // pool byte 0 no longer NUL
    backing = &mid;
    v = ctxFor(&mid, 0);
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read_validate_header(&v));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, wire.priv_book_stream_string_ref(&v, 1));
}

test "string envelope requires both sentinels and a non-empty pool" {
    // Valid pool: leading NUL, trailing 'a' -> the last sentinel fails.
    backing = &blob;
    var v = ctxFor(&blob, 0);
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read_validate_header(&v));
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        wire.priv_book_stream_validate_string_envelope(&v),
    );

    // Both sentinels NUL pass.
    var both = blob;
    both[113] = 0;
    backing = &both;
    v = ctxFor(&both, 0);
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read_validate_header(&v));
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_validate_string_envelope(&v));

    // Empty pool is invalid-size before any read. Drop the pool entirely:
    // the blob shrinks to header + chapter, the pool offsets move to the
    // table edge and the total shrinks with it.
    var empty = blob[0..112].*;
    empty[12] = 112; // total_size = 112
    empty[80] = 112; // string_off = 112
    empty[84] = 0; // string_size = 0
    empty[88] = 112; // image_pool_off = 112
    backing = &empty;
    v = ctxFor(&empty, 0);
    try std.testing.expectEqual(k_ra8_ok, wire.priv_book_stream_read_validate_header(&v));
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        wire.priv_book_stream_validate_string_envelope(&v),
    );
}
