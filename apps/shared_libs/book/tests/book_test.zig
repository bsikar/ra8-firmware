//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for book.zig (RA8FW-1251): the CRC-32 seam, the resident-blob
//! validator's gates (magic, version, feature bits, extents, pixel depths,
//! trailer CRC), the shared "RBKC" container-header seams, the chunk-table
//! walk, and the full open pipeline driven by a byte-copy stand-in inflater.

const std = @import("std");
const book = @import("book");
const wire = book.wire;

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_range_check_failed: u16 = 0x503;
const k_ra8_err_null_ptr: u16 = 0x504;

const k_blob_total: u32 = 256;
const k_string_off: u32 = 100;
const k_string_size: u32 = 40;
const k_pool_off: u32 = 140;

/// A minimal valid 256-byte `.rabook` blob: header + a 40-byte string pool,
/// every table count 0. `flags` and `pixel_format` let callers inject faults.
const BlobSpec = struct {
    flags: u32 = 0,
    format_version: u32 = 1,
    pixel_format: u8 = 1, // k_book_pixfmt_gray8
    with_image: bool = false,
    total: u32 = k_blob_total,
    corrupt_crc: bool = false,
    string_off: u32 = k_string_off,
    string_size: u32 = k_string_size,
};

/// Build a valid blob into `buf` (len >= 256) and return the body CRC.
fn makeBlob(buf: []u8, spec: BlobSpec) u32 {
    const total = spec.total;
    for (buf[0..total]) |*b| {
        b.* = 0;
    }
    var hdr = std.mem.zeroes(wire.BookHeader);
    hdr.magic = "RABOOK1\x00".*;
    hdr.format_version = spec.format_version;
    hdr.total_size = total;
    hdr.flags = spec.flags;
    hdr.string_off = spec.string_off;
    hdr.string_size = spec.string_size;
    hdr.image_pool_off = k_pool_off;
    hdr.image_pool_size = 0;

    if (spec.with_image) {
        hdr.image_off = k_pool_off;
        hdr.image_count = 1;
    }

    // Body pattern: string pool bytes plus filler.
    for (buf[@min(k_string_off, total)..total], 0..) |*b, i| {
        b.* = @truncate(i * 7 + 3);
    }
    if (spec.with_image) {
        var img = std.mem.zeroes(book.BookImageForTest);
        img.pixel_format = spec.pixel_format;
        @memcpy(buf[k_pool_off..][0..24], std.mem.asBytes(&img));
    }
    buf[k_string_off] = 'b';
    buf[k_string_off + 1] = 'o';
    buf[k_string_off + 2] = 'o';
    buf[k_string_off + 3] = 'k';

    @memcpy(buf[0..100], std.mem.asBytes(&hdr));
    const body_len: usize = total - 100;
    var crc = book.priv_book_crc32_extend(0, buf[100..].ptr, body_len);
    if (spec.corrupt_crc) {
        crc ^= 1;
    }
    @memcpy(buf[96..100], std.mem.asBytes(&crc));
    return crc;
}

/// Byte-copy stand-in inflater: "compressed" chunks are raw slices.
fn fakeInflate(
    src: [*]const u8,
    src_len: usize,
    dst: [*]u8,
    dst_cap: usize,
    out_len: *usize,
) callconv(.c) u16 {
    const n = @min(src_len, dst_cap);
    @memcpy(dst[0..n], src[0..n]);
    out_len.* = n;
    return k_ra8_ok;
}

/// Stand-in inflater that always inflates one byte short (wrong-span guard).
fn fakeInflateShort(
    src: [*]const u8,
    src_len: usize,
    dst: [*]u8,
    dst_cap: usize,
    out_len: *usize,
) callconv(.c) u16 {
    const n = @min(src_len, dst_cap) - 1;
    @memcpy(dst[0..n], src[0..n]);
    out_len.* = n;
    return k_ra8_ok;
}

/// Stand-in inflater that fails outright (error propagation).
fn fakeInflateError(
    src: [*]const u8,
    src_len: usize,
    dst: [*]u8,
    dst_cap: usize,
    out_len: *usize,
) callconv(.c) u16 {
    _ = src;
    _ = src_len;
    _ = dst;
    _ = dst_cap;
    _ = out_len;
    return k_ra8_err_invalid_arg;
}

/// Pack an "RBKC" container around `blob` with `chunk_bytes`-byte raw chunks.
/// `table_overrides` rewrites table entries after the standard build.
fn makeContainer(buf: []u8, blob: []const u8, chunk_bytes: u32) usize {
    const n_chunks = (blob.len + chunk_bytes - 1) / chunk_bytes;
    const table_len = (n_chunks + 1) * 8;
    const file_len = 24 + table_len + blob.len;

    @memcpy(buf[0..4], "RBKC");
    std.mem.writeInt(u32, buf[4..8], chunk_bytes, .little);
    std.mem.writeInt(u64, buf[8..16], blob.len, .little);
    std.mem.writeInt(u32, buf[16..20], @intCast(n_chunks), .little);
    std.mem.writeInt(u32, buf[20..24], 0, .little);

    var off: u64 = 0;
    var i: usize = 0;
    while (i <= n_chunks) : (i += 1) {
        std.mem.writeInt(u64, buf[24 + i * 8 ..][0..8], off, .little);
        off = @min(off + chunk_bytes, @as(u64, blob.len));
    }
    @memcpy(buf[24 + table_len ..][0..blob.len], blob);
    return file_len;
}

// ---- CRC-32 -----------------------------------------------------------------

test "crc32 check value 123456789 is 0xCBF43926" {
    const data = "123456789";
    try std.testing.expectEqual(@as(u32, 0xCBF43926), book.priv_book_crc32_extend(0, data.ptr, data.len));
}

test "crc32 empty first span is zero" {
    const data = "x";
    try std.testing.expectEqual(@as(u32, 0), book.priv_book_crc32_extend(0, data.ptr, 0));
}

test "crc32 chains incrementally like zlib.crc32" {
    const full = "the quick brown fox";
    const whole = book.priv_book_crc32_extend(0, full.ptr, full.len);
    const split = book.priv_book_crc32_extend(book.priv_book_crc32_extend(0, full.ptr, 4), full.ptr + 4, full.len - 4);
    try std.testing.expectEqual(whole, split);
}

// ---- Extent checks ----------------------------------------------------------

test "table_fits accepts exact fit and empty table" {
    try std.testing.expect(book.testableTableFits(0, 0, 24, 0));
    try std.testing.expect(book.testableTableFits(4, 4, 24, 100));
    try std.testing.expect(book.testableTableFits(0, 0, 1, 5));
}

test "table_fits rejects overflow past total" {
    try std.testing.expect(!book.testableTableFits(8, 0xFFFFFFFF, 2, 100));
    try std.testing.expect(!book.testableTableFits(101, 0, 1, 100));
    try std.testing.expect(!book.testableTableFits(8, 12, 24, 100));
}

// ---- Resident blob validation ----------------------------------------------

fn validateBlob(buf: []u8, spec: BlobSpec) u16 {
    _ = makeBlob(buf, spec);
    return book.book_validate(@ptrCast(buf.ptr), spec.total);
}

test "validate accepts a well-formed blob" {
    var buf: [k_blob_total]u8 = undefined;
    try std.testing.expectEqual(k_ra8_ok, validateBlob(&buf, .{}));
}

test "validate null base reports null_ptr" {
    try std.testing.expectEqual(k_ra8_err_null_ptr, book.book_validate(null, 256));
}

test "validate short buffer reports invalid_size" {
    var buf: [99]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_invalid_size, book.book_validate(@ptrCast(&buf), 99));
}

test "validate rejects bad magic, version and unknown feature bits" {
    var buf: [k_blob_total]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_invalid_arg, validateBlob(&buf, .{ .format_version = 2 }));

    buf[0] = 'X';
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.book_validate(@ptrCast(&buf), k_blob_total));

    try std.testing.expectEqual(k_ra8_err_invalid_arg, validateBlob(&buf, .{ .flags = 0x2 }));
}

test "validate rejects a total_size larger than the buffer" {
    var buf: [k_blob_total]u8 = undefined;
    _ = makeBlob(&buf, .{});
    std.mem.writeInt(u32, buf[12..16], k_blob_total + 1, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_size, book.book_validate(@ptrCast(&buf), k_blob_total));
    try std.testing.expectEqual(k_ra8_err_invalid_size, validateBlob(&buf, .{ .string_off = 300 }));
}

test "validate rejects an unknown pixel depth" {
    var buf: [k_blob_total]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_invalid_arg, validateBlob(&buf, .{ .with_image = true, .pixel_format = 2 }));
}

test "validate rejects a corrupted CRC trailer" {
    var buf: [k_blob_total]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_range_check_failed, validateBlob(&buf, .{ .corrupt_crc = true }));
}

// ---- Container header seams --------------------------------------------------

test "container header fields parse a good header" {
    var blob: [64]u8 = undefined;
    for (&blob, 0..) |*b, i| {
        b.* = @truncate(i);
    }
    var file: [128]u8 = undefined;
    _ = makeContainer(&file, &blob, 64);
    var chunk_bytes: u32 = 0;
    var total: u64 = 0;
    var count: u32 = 0;
    try std.testing.expectEqual(k_ra8_ok, book.priv_book_container_header_fields(&file, &chunk_bytes, &total, &count));
    try std.testing.expectEqual(@as(u32, 64), chunk_bytes);
    try std.testing.expectEqual(@as(u64, 64), total);
    try std.testing.expectEqual(@as(u32, 1), count);
}

test "container header fields reject magic, zero fields, reserved and count mismatch" {
    var hdr: [32]u8 = undefined;
    _ = makeContainer(&hdr, "", 64);
    var chunk_bytes: u32 = 0;
    var total: u64 = 0;
    var count: u32 = 0;

    hdr[0] = 'X';
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.priv_book_container_header_fields(&hdr, &chunk_bytes, &total, &count));
    _ = makeContainer(&hdr, "", 64);
    std.mem.writeInt(u32, hdr[4..8], 0, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.priv_book_container_header_fields(&hdr, &chunk_bytes, &total, &count));
    _ = makeContainer(&hdr, "", 64);
    std.mem.writeInt(u32, hdr[20..24], 7, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.priv_book_container_header_fields(&hdr, &chunk_bytes, &total, &count));
    _ = makeContainer(&hdr, "", 64);
    std.mem.writeInt(u32, hdr[16..20], 9, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.priv_book_container_header_fields(&hdr, &chunk_bytes, &total, &count));

    try std.testing.expectEqual(k_ra8_err_null_ptr, book.priv_book_container_header_fields(null, &chunk_bytes, &total, &count));
    try std.testing.expectEqual(k_ra8_err_null_ptr, book.priv_book_container_header_fields(&hdr, null, &total, &count));
}

test "container table entry reads unaligned little-endian u64" {
    var table: [16]u8 = undefined;
    for (&table, 0..) |*b, i| {
        b.* = @truncate(i + 1);
    }
    try std.testing.expectEqual(@as(u64, 0x100F0E0D0C0B0A09), book.priv_book_container_table_entry(&table, 1));
}

// ---- Container view + inflate + open -----------------------------------------

test "container view rejects short file, small scratch and bad table shape" {
    var blob: [k_blob_total]u8 = undefined;
    _ = makeBlob(&blob, .{});
    var file: [512]u8 = undefined;
    const file_len = makeContainer(&file, &blob, 64);

    var view: book.TestView = undefined;
    try std.testing.expectEqual(k_ra8_ok, book.testableView(&file, file_len, 256, &view));

    try std.testing.expectEqual(k_ra8_err_invalid_size, book.testableView(&file, 23, 256, &view));
    try std.testing.expectEqual(k_ra8_err_invalid_size, book.testableView(&file, file_len, 255, &view));

    var bad = file;
    std.mem.writeInt(u64, bad[24..32], 8, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.testableView(&bad, file_len, 256, &view));

    bad = file;
    std.mem.writeInt(u64, bad[32..40], 0, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.testableView(&bad, file_len, 256, &view));

    bad = file;
    std.mem.writeInt(u64, bad[24 + 4 * 8 ..][0..8], 255, .little);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.testableView(&bad, file_len, 256, &view));
}

test "inflate chunks reject a chunk inflated to the wrong span" {
    var blob: [k_blob_total]u8 = undefined;
    _ = makeBlob(&blob, .{});
    var file: [512]u8 = undefined;
    const file_len = makeContainer(&file, &blob, 64);
    var view: book.TestView = undefined;
    try std.testing.expectEqual(k_ra8_ok, book.testableView(&file, file_len, 256, &view));

    var scratch: [256]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_invalid_size, book.testableInflate(&view, fakeInflateShort, &scratch));
}

test "inflate chunks propagate the inflater's error" {
    var blob: [k_blob_total]u8 = undefined;
    _ = makeBlob(&blob, .{});
    var file: [512]u8 = undefined;
    const file_len = makeContainer(&file, &blob, 64);
    var view: book.TestView = undefined;
    try std.testing.expectEqual(k_ra8_ok, book.testableView(&file, file_len, 256, &view));

    var scratch: [256]u8 = undefined;
    try std.testing.expectEqual(k_ra8_err_invalid_arg, book.testableInflate(&view, fakeInflateError, &scratch));
}

test "book_open round-trips a container into a validated blob" {
    var blob: [k_blob_total]u8 = undefined;
    _ = makeBlob(&blob, .{});
    var file: [512]u8 = undefined;
    const file_len = makeContainer(&file, &blob, 64);

    var scratch: [256]u8 align(8) = undefined;
    var out_base: ?*const anyopaque = null;
    var out_size: usize = 0;
    try std.testing.expectEqual(k_ra8_ok, book.book_open(@ptrCast(&file), file_len, fakeInflate, &scratch, scratch.len, &out_base, &out_size));
    try std.testing.expectEqual(@as(usize, 256), out_size);
    try std.testing.expectEqual(@intFromPtr(&scratch), @intFromPtr(out_base.?));
    try std.testing.expectEqualSlices(u8, &blob, scratch[0..out_size]);
}

test "book_open null gates" {
    var file: [512]u8 = undefined;
    var scratch: [256]u8 = undefined;
    var out_base: ?*const anyopaque = null;
    var out_size: usize = 0;
    const f = file[0..1].ptr;
    const s = scratch[0..1].ptr;

    try std.testing.expectEqual(k_ra8_err_null_ptr, book.book_open(null, 512, fakeInflate, &scratch, 256, &out_base, &out_size));
    try std.testing.expectEqual(k_ra8_err_null_ptr, book.book_open(f, 512, null, &scratch, 256, &out_base, &out_size));
    try std.testing.expectEqual(k_ra8_err_null_ptr, book.book_open(f, 512, fakeInflate, null, 256, &out_base, &out_size));
    try std.testing.expectEqual(k_ra8_err_null_ptr, book.book_open(f, 512, fakeInflate, &scratch, 256, null, &out_size));
    try std.testing.expectEqual(k_ra8_err_null_ptr, book.book_open(f, 512, fakeInflate, &scratch, 256, &out_base, null));
    _ = s;
}

test "book_open rejects a scratch smaller than the inflated total" {
    var blob: [k_blob_total]u8 = undefined;
    _ = makeBlob(&blob, .{});
    var file: [512]u8 = undefined;
    const file_len = makeContainer(&file, &blob, 64);
    var scratch: [255]u8 = undefined;
    var out_base: ?*const anyopaque = null;
    var out_size: usize = 0;
    try std.testing.expectEqual(k_ra8_err_invalid_size, book.book_open(@ptrCast(&file), file_len, fakeInflate, &scratch, scratch.len, &out_base, &out_size));
}
