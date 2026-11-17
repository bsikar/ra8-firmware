//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-side tests for the strict chunked-validation adapter port
//! (RA8FW-1247). The C suite keeps covering the adapter from the outside
//! through the unchanged C ABI; these exercise the port's own core paths:
//! the argument gates, the fail-closed alias and workspace checks, the
//! reader/table revalidation, and the one-chunk-cache flat read through the
//! strict validator seam. book_validate_stream_strict (which stays C in
//! book_stream.c) is stubbed here with marker semantics.

const std = @import("std");
const chunked = @import("book_chunked.zig");
const validate = @import("book_chunked_validate.zig");
const wire = @import("book_stream_wire.zig");

const BookChunked = chunked.BookChunked;
const BookHeader = wire.BookHeader;

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_state: u16 = 0x104;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_out_of_range: u16 = 0x208;
const k_ra8_err_null_ptr: u16 = 0x504;

// ---- Stub of the strict validator seam (book_stream.c, stays C) -----------

/// When set, the stub asks the adapter's flat read for this out-of-range
/// offset first, to exercise the defensive range guard.
var stub_probe_offset: ?u64 = null;
var stub_calls: u32 = 0;

export fn book_validate_stream_strict(
    read: *const fn (?*anyopaque, u64, [*]u8, u32) callconv(.c) u16,
    read_ctx: ?*anyopaque,
    source_size: u64,
    scratch: [*]u8,
    scratch_cap: u32,
    out_header: *BookHeader,
) callconv(.c) u16 {
    _ = scratch_cap;
    stub_calls += 1;
    if (stub_probe_offset) |off| {
        return read(read_ctx, off, scratch, 4);
    }
    const err = read(read_ctx, 0, scratch, @intCast(source_size));
    if (err != k_ra8_ok) {
        return err;
    }
    // Marker: stamp the total_size field (byte offset 12) so the caller can
    // prove the validated header came back out.
    std.mem.writeInt(u32, std.mem.asBytes(out_header)[12..16], 1234, .little);
    return k_ra8_ok;
}

/// The shared header parser (priv_book_container_header_fields, which stays
/// C in book.c) is stubbed here with the same semantics it has in book.c,
/// because the test module links no C archive.
export fn priv_book_container_header_fields(
    hdr: [*]const u8,
    out_chunk_bytes: *u32,
    out_total: *u64,
    out_count: *u32,
) callconv(.c) u16 {
    if (!std.mem.eql(u8, hdr[0..4], "RBKC")) {
        return k_ra8_err_invalid_arg;
    }
    const chunk_bytes = std.mem.readInt(u32, hdr[4..8], .little);
    const total = std.mem.readInt(u64, hdr[8..16], .little);
    const count = std.mem.readInt(u32, hdr[16..20], .little);
    const reserved = std.mem.readInt(u32, hdr[20..24], .little);
    if ((chunk_bytes == 0) or (total == 0) or (reserved != 0)) {
        return k_ra8_err_invalid_arg;
    }
    if (count != (total + chunk_bytes - 1) / chunk_bytes) {
        return k_ra8_err_invalid_arg;
    }
    out_chunk_bytes.* = chunk_bytes;
    out_total.* = total;
    out_count.* = count;
    return k_ra8_ok;
}

// ---- Fixtures --------------------------------------------------------------

/// One-container fixture: 24-byte header (RBKC, chunk 4, total 8, count 2,
/// reserved 0), a 3-entry table {0, 2, 3}, and 3 payload bytes.
var blob: [51]u8 = undefined;

fn testRead(ctx: ?*anyopaque, offset: u64, buf: [*]u8, len: u32) callconv(.c) u16 {
    _ = ctx;
    if (@as(u64, offset) + @as(u64, len) > blob.len) {
        return k_ra8_err_out_of_range;
    }
    @memcpy(buf[0..len], blob[offset..][0..len]);
    return k_ra8_ok;
}

/// The zlib stand-in: copies the compressed bytes and zero-fills the rest,
/// reporting the full requested length as produced.
fn testInflate(src: ?*const anyopaque, src_len: usize, dst: ?*anyopaque, dst_cap: usize, produced: *usize) callconv(.c) u16 {
    _ = src_len;
    const s: [*]const u8 = @ptrCast(src.?);
    const d: [*]u8 = @ptrCast(dst.?);
    @memset(d[0..dst_cap], 0);
    @memcpy(d[0..3], s[0..3]);
    produced.* = dst_cap;
    return k_ra8_ok;
}

var table_buf: [3]u64 = .{ 0, 2, 3 };
var staging: [16]u8 = undefined;

fn openReader() BookChunked {
    @memset(&blob, 0);
    @memcpy(blob[0..4], "RBKC");
    std.mem.writeInt(u32, blob[4..8], 4, .little);
    std.mem.writeInt(u64, blob[8..16], 8, .little);
    std.mem.writeInt(u32, blob[16..20], 2, .little);
    std.mem.writeInt(u32, blob[20..24], 0, .little);
    std.mem.writeInt(u64, blob[24..32], 0, .little);
    std.mem.writeInt(u64, blob[32..40], 2, .little);
    std.mem.writeInt(u64, blob[40..48], 3, .little);
    blob[48] = 0xAA;
    blob[49] = 0xBB;
    blob[50] = 0xCC;
    table_buf = .{ 0, 2, 3 };
    var rd: BookChunked = std.mem.zeroes(BookChunked);
    const err = chunked.book_chunked_open(&rd, testRead, null, blob.len, testInflate, &table_buf, table_buf.len, &staging, staging.len);
    std.testing.expectEqual(k_ra8_ok, err) catch unreachable;
    return rd;
}

// ---- Tests -----------------------------------------------------------------

test "null out_header is rejected without touching the reader" {
    var rd = openReader();
    var chunk_buf: [8]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, null);
    try std.testing.expectEqual(k_ra8_err_null_ptr, err);
}

test "null reader zeroes the header and returns null_ptr" {
    _ = openReader();
    var out: BookHeader = undefined;
    var chunk_buf: [8]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    const err = validate.book_chunked_validate_strict(null, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, &out);
    try std.testing.expectEqual(k_ra8_err_null_ptr, err);
    try std.testing.expectEqual(@as(u128, 0), @as(u128, @bitCast(std.mem.readInt(u128, std.mem.asBytes(&out)[0..16], .little))));
}

test "output aliasing a workspace is rejected fail-closed" {
    var rd = openReader();
    // Aligned so the aliased header pointer is well-formed on its own.
    var chunk_buf: [128]u8 align(@alignOf(BookHeader)) = undefined;
    var scratch_buf: [64]u8 = undefined;
    const out: *BookHeader = @ptrCast(&chunk_buf);
    @memset(&chunk_buf, 0);
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, out);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, err);
    // The aliased storage is untouched (the C contract: rejected without
    // modifying the aliased span).
    try std.testing.expectEqual(@as(u8, 0), chunk_buf[0]);
}

test "workspaces overlapping each other are rejected" {
    var rd = openReader();
    var workspace: [256]u8 = undefined;
    var out: BookHeader = undefined;
    // chunk (cached at rd.chunk_bytes = 4) and scratch drawn from the same
    // buffer overlap by construction.
    const err = validate.book_chunked_validate_strict(&rd, workspace[0..8], 8, workspace[2..][0..64], 64, &out);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, err);
}

test "bad table shape is revalidated and rejected" {
    var rd = openReader();
    table_buf[1] = 3;
    table_buf[2] = 2;
    var chunk_buf: [8]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    var out: BookHeader = undefined;
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, &out);
    try std.testing.expectEqual(k_ra8_err_invalid_state, err);
}

test "zero capacities are rejected" {
    var rd = openReader();
    var chunk_buf: [8]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    var out: BookHeader = undefined;
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, 0, &out);
    try std.testing.expectEqual(k_ra8_err_invalid_size, err);
}

test "chunk cache smaller than the chunk size is rejected" {
    var rd = openReader();
    var chunk_buf: [2]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    var out: BookHeader = undefined;
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, &out);
    try std.testing.expectEqual(k_ra8_err_invalid_size, err);
}

test "happy path streams the flat blob and returns the validated header" {
    var rd = openReader();
    var chunk_buf: [8]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    var out: BookHeader = undefined;
    stub_calls = 0;
    stub_probe_offset = null;
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, &out);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqual(@as(u32, 1), stub_calls);
    // The marker the stub stamped after a successful flat read came back out.
    try std.testing.expectEqual(@as(u32, 1234), std.mem.readInt(u32, std.mem.asBytes(&out)[12..16], .little));
}

test "the defensive flat-read range guard still returns out_of_range" {
    var rd = openReader();
    var chunk_buf: [8]u8 = undefined;
    var scratch_buf: [64]u8 = undefined;
    var out: BookHeader = undefined;
    stub_probe_offset = 100;
    const err = validate.book_chunked_validate_strict(&rd, &chunk_buf, chunk_buf.len, &scratch_buf, scratch_buf.len, &out);
    try std.testing.expectEqual(k_ra8_err_out_of_range, err);
}
