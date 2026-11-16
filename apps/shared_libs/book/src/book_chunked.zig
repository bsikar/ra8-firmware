//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Demand-paged "RBKC" chunk reader (RA8FW-1244). Ported 1:1 from
//! apps/shared_libs/book/src/book_chunked.c, deleted in the same commit: the
//! caller-driven open (fixed header + chunk table through the file reader),
//! the table-shape validation against the payload extent, and the per-chunk
//! stage-and-inflate read. Geometry parsing stays shared with the resident
//! open through priv_book_container_header_fields (book.c), which remains the
//! single in-firmware definition of the container format.
//!
//! The book_chunked_open / book_chunked_read C ABI is kept for the callers
//! that remain C (book_chunked_validate.c and the book test suite); the
//! archive (libra8_book.a) provides them wherever the C TU used to. One
//! delta from the C: the RA8_CHECK_NULL_PTR diagnostic log line on a null
//! argument is not replicated (the C macro's log emission is
//! build-config-dependent); the returned error codes are identical.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_state: ra8_err_t = 0x104;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_out_of_range: ra8_err_t = 0x208;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// Fixed header bytes ahead of the chunk table (inc/book.h).
const k_book_container_header_len: u64 = 24;
/// One chunk-table entry: uint64 LE offset (inc/book.h).
const k_book_container_entry_len: u64 = 8;

/// `ra8_vsource_read_fn` (libs/ra8_mem/inc/ra8_vsource.h): the container
/// byte-reader seam. Addresses the compressed on-disk file bytes.
const VsourceReadFn = *const fn (?*anyopaque, u64, [*]u8, u32) callconv(.c) ra8_err_t;

/// `book_inflate_fn` (inc/book.h): the caller-supplied DEFLATE inflater.
const InflateFn = *const fn (?*const anyopaque, usize, ?*anyopaque, usize, *usize) callconv(.c) ra8_err_t;

/// Host-order mirror of `book_chunked_t` (inc/book_chunked.h): parsed
/// geometry plus the caller's file-reader, table and staging storage.
pub const BookChunked = extern struct {
    file_read: VsourceReadFn,
    file_ctx: ?*anyopaque,
    inflate_cb: InflateFn,
    table: ?[*]const u64,
    table_cap_entries: u32,
    staging: ?[*]u8,
    staging_cap: u32,
    payload_off: u64,
    inflated_total: u64,
    chunk_bytes: u32,
    chunk_count: u32,
};

/// The shared container-header parser (book.c): the one in-firmware
/// definition of the "RBKC" header format.
extern fn priv_book_container_header_fields(
    hdr: [*]const u8,
    out_chunk_bytes: *u32,
    out_total: *u64,
    out_count: *u32,
) ra8_err_t;

/// Require the loaded chunk table's shape against the payload extent:
/// `table[0] == 0`, strictly increasing entries, an end offset equal to
/// `payload_len` (the streams exactly tile the rest of the file), and every
/// chunk's compressed length within the staging budget.
fn tableCheck(rd: *const BookChunked, table_buf: [*]const u64, payload_len: u64) ra8_err_t {
    if (table_buf[0] != 0) {
        return k_ra8_err_invalid_arg;
    }
    var max_clen: u64 = 0;
    var i: u32 = 0;
    while (i < rd.chunk_count) : (i += 1) {
        if (table_buf[i + 1] <= table_buf[i]) {
            return k_ra8_err_invalid_arg;
        }
        const clen = table_buf[i + 1] - table_buf[i];
        if (clen > max_clen) {
            max_clen = clen;
        }
    }
    if (table_buf[rd.chunk_count] != payload_len) {
        return k_ra8_err_invalid_arg;
    }
    if (max_clen > @as(u64, rd.staging_cap)) {
        return k_ra8_err_invalid_size;
    }
    return k_ra8_ok;
}

/// Load + validate the chunk table through the file reader: size it from the
/// parsed header fields, bound it against the caller buffer, the file length
/// and the one-call uint32 read limit, read it in one burst, then walk it
/// once. On success `rd.table` and `rd.payload_off` are bound.
fn loadTable(rd: *BookChunked, file_len: u64, table_buf: [*]u64, table_cap_entries: u32) ra8_err_t {
    const entries: u64 = @as(u64, rd.chunk_count) + 1;
    const table_bytes = entries * k_book_container_entry_len;
    if (entries > @as(u64, table_cap_entries)) {
        return k_ra8_err_invalid_size;
    }
    if (table_bytes > 0xFFFFFFFF) {
        return k_ra8_err_invalid_size;
    }
    if (file_len < k_book_container_header_len + table_bytes) {
        return k_ra8_err_invalid_size;
    }
    const err = rd.file_read(
        rd.file_ctx,
        k_book_container_header_len,
        @ptrCast(table_buf),
        @intCast(table_bytes),
    );
    if (err != k_ra8_ok) {
        return err;
    }
    const payload_off = k_book_container_header_len + table_bytes;
    const shape = tableCheck(rd, table_buf, file_len - payload_off);
    if (shape != k_ra8_ok) {
        return shape;
    }
    rd.table = table_buf;
    rd.table_cap_entries = table_cap_entries;
    rd.payload_off = payload_off;
    return k_ra8_ok;
}

/// The argument-checked body of book_chunked_open: read and parse the fixed
/// header, then load + validate the chunk table.
fn chunkedOpenBody(rd: *BookChunked, file_len: u64, table_buf: [*]u64, table_cap_entries: u32) ra8_err_t {
    if (file_len < k_book_container_header_len) {
        return k_ra8_err_invalid_size;
    }
    var hdr: [k_book_container_header_len]u8 = @splat(0);
    var err = rd.file_read(rd.file_ctx, 0, &hdr, @intCast(k_book_container_header_len));
    if (err != k_ra8_ok) {
        return err;
    }
    err = priv_book_container_header_fields(&hdr, &rd.chunk_bytes, &rd.inflated_total, &rd.chunk_count);
    if (err != k_ra8_ok) {
        return err;
    }
    return loadTable(rd, file_len, table_buf, table_cap_entries);
}

/// The exported C ABI seam behind book_chunked_open: bind the reader's
/// callbacks and caller storage, then validate the container geometry.
pub export fn book_chunked_open(
    rd: ?*BookChunked,
    file_read: ?VsourceReadFn,
    file_ctx: ?*anyopaque,
    file_len: u64,
    inflate: ?InflateFn,
    table_buf: ?[*]u64,
    table_cap_entries: u32,
    staging: ?[*]u8,
    staging_cap: u32,
) callconv(.c) ra8_err_t {
    if (rd == null) {
        return k_ra8_err_null_ptr;
    }
    if (file_read == null) {
        return k_ra8_err_null_ptr;
    }
    if (inflate == null) {
        return k_ra8_err_null_ptr;
    }
    if (table_buf == null) {
        return k_ra8_err_null_ptr;
    }
    if (staging == null) {
        return k_ra8_err_null_ptr;
    }

    rd.?.* = std.mem.zeroes(BookChunked);
    rd.?.file_read = file_read.?;
    rd.?.file_ctx = file_ctx;
    rd.?.inflate_cb = inflate.?;
    rd.?.staging = staging;
    rd.?.staging_cap = staging_cap;
    return chunkedOpenBody(rd.?, file_len, table_buf.?, table_cap_entries);
}

/// Stage one chunk's compressed stream and inflate it into `buf`: the I/O
/// tail of book_chunked_read after the geometry checks. Requires the
/// produced length to equal the requested span.
fn stageAndInflate(rd: *const BookChunked, idx: u32, buf: [*]u8, len: u32) ra8_err_t {
    const table = rd.table.?;
    const clen = table[idx + 1] - table[idx];
    var err = rd.file_read(rd.file_ctx, rd.payload_off + table[idx], rd.staging.?, @intCast(clen));
    if (err != k_ra8_ok) {
        return err;
    }
    var produced: usize = 0;
    err = rd.inflate_cb(rd.staging.?, @as(usize, @intCast(clen)), buf, @as(usize, len), &produced);
    if (err != k_ra8_ok) {
        return err;
    }
    if (produced != @as(usize, len)) {
        return k_ra8_err_invalid_size;
    }
    return k_ra8_ok;
}

/// The exported C ABI seam behind book_chunked_read, the
/// ra8_vsource_read_fn implementation: look the chunk covering `offset` up
/// in the resident table, stage its compressed stream and inflate it
/// directly into `buf`. Chunk-aligned by contract.
pub export fn book_chunked_read(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: u32) callconv(.c) ra8_err_t {
    if (ctx == null) {
        return k_ra8_err_null_ptr;
    }
    if (buf == null) {
        return k_ra8_err_null_ptr;
    }
    const rd: *const BookChunked = @ptrCast(@alignCast(ctx.?));
    if (rd.table == null) {
        return k_ra8_err_invalid_state;
    }
    if (offset >= rd.inflated_total) {
        return k_ra8_err_out_of_range;
    }
    if ((offset % @as(u64, rd.chunk_bytes)) != 0) {
        return k_ra8_err_invalid_arg;
    }
    var expected = rd.inflated_total - offset;
    if (expected > @as(u64, rd.chunk_bytes)) {
        expected = rd.chunk_bytes;
    }
    if (@as(u64, len) != expected) {
        return k_ra8_err_invalid_arg;
    }
    return stageAndInflate(rd, @intCast(offset / @as(u64, rd.chunk_bytes)), buf.?, len);
}
