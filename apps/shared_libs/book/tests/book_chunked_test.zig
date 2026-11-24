//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-side tests for the chunk-reader port (RA8FW-1244). The C suite
//! (test_book_stream / test_book_paged_edge) keeps covering the reader from
//! the outside through the unchanged C ABI; these exercise the port's own
//! core paths: the table-shape validation, the staged open geometry and the
//! chunk-aligned read contract. The shared header parser
//! (priv_book_container_header_fields, which stays C in book.c) is stubbed
//! here with the same semantics it has in book.c.

const std = @import("std");
const chunked = @import("chunked");

const BookChunked = chunked.BookChunked;

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_state: u16 = 0x104;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_out_of_range: u16 = 0x208;
const k_ra8_err_null_ptr: u16 = 0x504;

const k_header_len: u32 = 24;
const k_entry_len: u32 = 8;

// ---- Test fixtures ---------------------------------------------------------

/// The stub of priv_book_container_header_fields with book.c's semantics:
/// "RBKC" magic, non-zero chunk size and inflated total, zero reserved word,
/// and a chunk count that agrees with ceil(total / chunk_bytes).
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

var backing: []const u8 = &.{};

fn testRead(ctx: ?*anyopaque, offset: u64, buf: [*]u8, len: u32) callconv(.c) u16 {
    _ = ctx;
    if ((offset > backing.len) or (@as(u64, len) > (backing.len - offset))) {
        return k_ra8_err_invalid_size;
    }
    @memcpy(buf[0..len], backing[@intCast(offset)..][0..len]);
    return k_ra8_ok;
}

/// Inflate fixture: fill the destination with a marker byte and report the
/// full capacity as produced (the "compressed" stream is a run-length one).
var marker: u8 = 0x5A;
var short_produced: bool = false;

fn testInflate(src: ?*const anyopaque, src_len: usize, dst: ?*anyopaque, dst_cap: usize, out_len: *usize) callconv(.c) u16 {
    _ = src;
    _ = src_len;
    const out: [*]u8 = @ptrCast(dst.?);
    @memset(out[0..dst_cap], marker);
    out_len.* = if (short_produced) (dst_cap - 1) else dst_cap;
    return k_ra8_ok;
}

/// Build one container file into `buf`: fixed header, the chunk table as
/// uint64 LE entries, then the payload streams. Returns the live slice.
fn buildFile(
    buf: []u8,
    chunk_bytes: u32,
    total: u64,
    count: u32,
    reserved: u32,
    table: []const u64,
    payload: []const u8,
) []u8 {
    std.debug.assert(table.len == count + 1);
    const table_bytes = table.len * k_entry_len;
    const file_len = k_header_len + table_bytes + payload.len;
    std.debug.assert(file_len <= buf.len);
    const f = buf[0..file_len];
    @memset(f, 0);
    @memcpy(f[0..4], "RBKC");
    std.mem.writeInt(u32, f[4..8], chunk_bytes, .little);
    std.mem.writeInt(u64, f[8..16], total, .little);
    std.mem.writeInt(u32, f[16..20], count, .little);
    std.mem.writeInt(u32, f[20..24], reserved, .little);
    for (table, 0..) |entry, i| {
        std.mem.writeInt(u64, f[k_header_len + i * k_entry_len ..][0..8], entry, .little);
    }
    @memcpy(f[k_header_len + table_bytes ..], payload);
    return f;
}

const happy_table = [_]u64{ 0, 3, 6 };
const happy_payload = [_]u8{ 1, 2, 3, 4, 5, 6 };

// ---- Tests -----------------------------------------------------------------

test "open rejects each null argument" {
    var file_buf: [64]u8 = undefined;
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    var rd: BookChunked = undefined;
    var table: [8]u64 = undefined;
    var staging: [8]u8 = undefined;

    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        chunked.book_chunked_open(null, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        chunked.book_chunked_open(&rd, null, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, null, &table, table.len, &staging, staging.len),
    );
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, null, table.len, &staging, staging.len),
    );
    try std.testing.expectEqual(
        k_ra8_err_null_ptr,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, null, staging.len),
    );
}

test "open binds the reader and reads serve both chunks" {
    var file_buf: [64]u8 = undefined;
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    var rd: BookChunked = undefined;
    var table: [8]u64 = undefined;
    var staging: [8]u8 = undefined;
    const err = chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqual(@as(u32, 4), rd.chunk_bytes);
    try std.testing.expectEqual(@as(u64, 6), rd.inflated_total);
    try std.testing.expectEqual(@as(u32, 2), rd.chunk_count);
    try std.testing.expectEqual(@as(u64, 24 + 3 * 8), rd.payload_off);

    var out: [8]u8 = undefined;
    try std.testing.expectEqual(k_ra8_ok, chunked.book_chunked_read(&rd, 0, &out, 4));
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x5A, 0x5A, 0x5A, 0x5A }, out[0..4]);
    marker = 0xA5;
    try std.testing.expectEqual(k_ra8_ok, chunked.book_chunked_read(&rd, 4, &out, 2));
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xA5, 0xA5 }, out[0..2]);
    marker = 0x5A;
}

test "read rejects unbound, out-of-range, unaligned and wrong-length reads" {
    var file_buf: [64]u8 = undefined;
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    var rd: BookChunked = undefined;
    var table: [8]u64 = undefined;
    var staging: [8]u8 = undefined;
    var out: [8]u8 = undefined;

    try std.testing.expectEqual(k_ra8_err_null_ptr, chunked.book_chunked_read(null, 0, &out, 4));
    try std.testing.expectEqual(k_ra8_err_null_ptr, chunked.book_chunked_read(&rd, 0, null, 4));
    // Unbound reader: the zeroed struct's table is null.
    var fresh: BookChunked = std.mem.zeroes(BookChunked);
    try std.testing.expectEqual(k_ra8_err_invalid_state, chunked.book_chunked_read(&fresh, 0, &out, 4));

    _ = chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len);
    try std.testing.expectEqual(k_ra8_err_out_of_range, chunked.book_chunked_read(&rd, 6, &out, 4));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, chunked.book_chunked_read(&rd, 2, &out, 4));
    try std.testing.expectEqual(k_ra8_err_invalid_arg, chunked.book_chunked_read(&rd, 0, &out, 3));
}

test "read propagates a short file read and a wrong inflate length" {
    var file_buf: [64]u8 = undefined;
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    var rd: BookChunked = undefined;
    var table: [8]u64 = undefined;
    var staging: [8]u8 = undefined;
    var out: [8]u8 = undefined;
    _ = chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len);

    // A reader whose payload region is one byte short fails the chunk read.
    backing = backing[0 .. backing.len - 1];
    try std.testing.expectEqual(k_ra8_err_invalid_size, chunked.book_chunked_read(&rd, 4, &out, 2));

    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    short_produced = true;
    try std.testing.expectEqual(k_ra8_err_invalid_size, chunked.book_chunked_read(&rd, 0, &out, 4));
    short_produced = false;
}

test "open rejects a bad magic, a non-zero reserved word and a short file" {
    var file_buf: [64]u8 = undefined;
    var rd: BookChunked = undefined;
    var table: [8]u64 = undefined;
    var staging: [8]u8 = undefined;

    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    file_buf[3] = 'X';
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );
    try std.testing.expectEqual(@as(?[*]const u64, null), rd.table);

    backing = buildFile(&file_buf, 4, 6, 2, 1, &happy_table, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );

    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        chunked.book_chunked_open(&rd, testRead, null, 16, testInflate, &table, table.len, &staging, staging.len),
    );
    // Short files: below the fixed header, then below the header plus table.
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        chunked.book_chunked_open(&rd, testRead, null, 16, testInflate, &table, table.len, &staging, staging.len),
    );
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        chunked.book_chunked_open(&rd, testRead, null, 30, testInflate, &table, table.len, &staging, staging.len),
    );
}

test "open rejects an ill-formed table" {
    var file_buf: [64]u8 = undefined;
    var rd: BookChunked = undefined;
    var table: [8]u64 = undefined;
    var staging: [8]u8 = undefined;

    // table[0] != 0.
    const shifted = [_]u64{ 1, 3, 6 };
    backing = buildFile(&file_buf, 4, 6, 2, 0, &shifted, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );

    // Non-monotonic table.
    const flat = [_]u64{ 0, 3, 3 };
    backing = buildFile(&file_buf, 4, 6, 2, 0, &flat, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );

    // The table's end offset disagrees with the payload extent.
    const over = [_]u64{ 0, 3, 5 };
    backing = buildFile(&file_buf, 4, 6, 2, 0, &over, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, staging.len),
    );

    // A compressed chunk larger than the staging budget: the happy table's
    // 3-byte streams do not fit a 2-byte staging buffer.
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, table.len, &staging, 2),
    );

    // A caller table buffer too small for the entries.
    backing = buildFile(&file_buf, 4, 6, 2, 0, &happy_table, &happy_payload);
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        chunked.book_chunked_open(&rd, testRead, null, backing.len, testInflate, &table, 2, &staging, staging.len),
    );
}
