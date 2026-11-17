//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Strict streaming-validation adapter for an open RBKC chunk reader
//! (RA8FW-1247). Ported 1:1 from
//! apps/shared_libs/book/src/book_chunked_validate.c, deleted in the same
//! commit: it rechecks the reader geometry, rejects overlapping caller
//! workspaces, inflates one chunk at a time through a one-chunk cache, and
//! serves the exact flat reads the strict RABOOK1 validator requires. It
//! retains no storage beyond the caller-owned reader, chunk, and scratch
//! buffers.
//!
//! The strict validator itself (book_validate_stream_strict, book_stream.c)
//! stays C and is reached through its unchanged C ABI. The
//! book_chunked_validate_strict C ABI is kept for the callers that remain C
//! (the book test suite); the archive (libra8_book.a) provides it wherever
//! the C TU used to. One delta from the C: span overlap is decided with
//! @intFromPtr instead of the C memcpy-into-uintptr_t trick (the same
//! integer-address comparison, well-defined by construction in Zig); the
//! returned error codes are identical.

const std = @import("std");
const wire = @import("book_stream_wire.zig");
const chunked = @import("book_chunked.zig");

const BookChunked = chunked.BookChunked;
const BookHeader = wire.BookHeader;

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_state: ra8_err_t = 0x104;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_out_of_range: ra8_err_t = 0x208;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// `ra8_vsource_read_fn` (libs/ra8_mem/inc/ra8_vsource.h): the flat-source
/// read seam book_validate_stream_strict streams through.
const FlatReadFn = *const fn (?*anyopaque, u64, [*]u8, u32) callconv(.c) ra8_err_t;

/// The strict RABOOK1 validator over a flat blob (book_stream.zig,
/// RA8FW-1248; this extern resolves against the archive now that the C TU
/// is deleted -- the adapter's test module keeps stubbing the seam, so a
/// direct import would collide with that stub).
extern fn book_validate_stream_strict(
    read: ?FlatReadFn,
    read_ctx: ?*anyopaque,
    source_size: u64,
    scratch: ?[*]u8,
    scratch_cap: u32,
    out_header: ?*BookHeader,
) ra8_err_t;

/// Fixed span count used by strict alias validation: reader, chunk, scratch,
/// staging, and table.
const k_span_count = 5;

/// Cached inflated-chunk view used by arbitrary flat-source reads.
const ChunkValidate = struct {
    rd: *BookChunked,
    chunk: [*]u8,
    loaded_idx: u32,
    loaded_len: u32,
    loaded: bool,
};

/// Address of an optional many-item pointer, 0 when null (the C code passes
/// the raw pointer and compares against NULL).
fn addrOf(p: ?[*]u8) usize {
    return if (p) |q| @intFromPtr(q) else 0;
}

fn addrOfConst(p: ?[*]const u64) usize {
    return if (p) |q| @intFromPtr(q) else 0;
}

/// Decide whether two non-empty caller-memory spans overlap. Converts bases
/// to integer addresses, proves each exclusive end is representable, then
/// applies the standard half-open overlap test. `invalid` receives true when
/// either end address would overflow, independently of the overlap result.
/// Pure and thread-safe.
fn spansOverlap(a: usize, a_len: u64, b: usize, b_len: u64, invalid: *bool) bool {
    invalid.* = true;
    if (a_len > @as(u64, std.math.maxInt(usize))) {
        return false;
    }
    if (b_len > @as(u64, std.math.maxInt(usize))) {
        return false;
    }
    const a_len_usize: usize = @intCast(a_len);
    const b_len_usize: usize = @intCast(b_len);
    if (a > std.math.maxInt(usize) - a_len_usize) {
        return false;
    }
    if (b > std.math.maxInt(usize) - b_len_usize) {
        return false;
    }
    invalid.* = false;
    const a_end = a + a_len_usize;
    const b_end = b + b_len_usize;
    if (a >= b_end) {
        return false;
    }
    return b < a_end;
}

/// Require every reader callback and geometry field to be present: a null
/// file-read or inflate callback, a null table or staging buffer, or a zero
/// chunk size, chunk count, inflated total, or staging capacity is rejected
/// before any arithmetic on those fields is attempted. Pure and thread-safe
/// for an immutable reader.
fn validateReaderFields(rd: *const BookChunked) ra8_err_t {
    // The struct mirror stores the callbacks as raw function pointers; a
    // zeroed (never-opened) reader reads as the null pointer the C checks
    // for, compared by address.
    if (@intFromPtr(rd.file_read) == 0) {
        return k_ra8_err_invalid_state;
    }
    if (@intFromPtr(rd.inflate_cb) == 0) {
        return k_ra8_err_invalid_state;
    }
    if (rd.table == null) {
        return k_ra8_err_invalid_state;
    }
    if (rd.staging == null) {
        return k_ra8_err_invalid_state;
    }
    if (rd.chunk_bytes == 0) {
        return k_ra8_err_invalid_state;
    }
    if (rd.chunk_count == 0) {
        return k_ra8_err_invalid_state;
    }
    if (rd.inflated_total == 0) {
        return k_ra8_err_invalid_state;
    }
    if (rd.staging_cap == 0) {
        return k_ra8_err_invalid_state;
    }
    return k_ra8_ok;
}

/// Revalidate the derived chunk count and the offset table's shape: the
/// chunk count implied by `inflated_total` and `chunk_bytes` must match
/// `chunk_count` exactly, the table must fit its declared capacity and start
/// at zero, every entry must strictly increase bounded by the staging
/// capacity, and the final payload offset addition must not overflow. Pure
/// and thread-safe for an immutable reader.
fn validateReaderTable(rd: *const BookChunked) ra8_err_t {
    const table = rd.table.?;
    const expected_count: u64 = (@as(u64, rd.inflated_total) / @as(u64, rd.chunk_bytes)) +
        @intFromBool((rd.inflated_total % @as(u64, rd.chunk_bytes)) != 0);
    const table_entries: u64 = @as(u64, rd.chunk_count) + 1;
    if (expected_count > @as(u64, std.math.maxInt(u32))) {
        return k_ra8_err_invalid_state;
    }
    if (@as(u64, rd.chunk_count) != expected_count) {
        return k_ra8_err_invalid_state;
    }
    if (table_entries > @as(u64, rd.table_cap_entries)) {
        return k_ra8_err_invalid_state;
    }
    if (table[0] != 0) {
        return k_ra8_err_invalid_state;
    }
    var i: u32 = 0;
    while (i < rd.chunk_count) : (i += 1) {
        if (table[i + 1] <= table[i]) {
            return k_ra8_err_invalid_state;
        }
        if ((table[i + 1] - table[i]) > @as(u64, rd.staging_cap)) {
            return k_ra8_err_invalid_state;
        }
    }
    if (table[rd.chunk_count] > (std.math.maxInt(u64) - rd.payload_off)) {
        return k_ra8_err_invalid_state;
    }
    return k_ra8_ok;
}

/// Revalidate the open-reader invariants needed by strict validation:
/// field presence, then chunk geometry, table bounds, monotonic compressed
/// extents, and payload addition before any cache read is attempted. Pure
/// and thread-safe for an immutable reader.
fn validateReader(rd: *const BookChunked) ra8_err_t {
    const fields = validateReaderFields(rd);
    if (fields != k_ra8_ok) {
        return fields;
    }
    return validateReaderTable(rd);
}

/// Reject output aliases before the output is zeroed or published: compare
/// the result object with the reader, inflated chunk, strict scratch,
/// compressed staging, and offset-table spans fail-closed. The opaque file
/// callback context cannot be sized here and remains a documented caller
/// no-alias precondition.
fn outputIsAliased(
    rd: *const BookChunked,
    chunk: ?[*]u8,
    chunk_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
    out_header: *const BookHeader,
) bool {
    const table_len: u64 = @as(u64, rd.table_cap_entries) * @sizeOf(u64);
    const bases = [k_span_count]usize{
        @intFromPtr(rd),
        addrOf(chunk),
        addrOf(scratch),
        addrOf(rd.staging),
        addrOfConst(rd.table),
    };
    const lens = [k_span_count]u64{
        @sizeOf(BookChunked),
        chunk_cap,
        scratch_cap,
        rd.staging_cap,
        table_len,
    };
    var i: usize = 0;
    while (i < k_span_count) : (i += 1) {
        if (bases[i] != 0) {
            if (lens[i] == 0) {
                continue;
            }
            var invalid = false;
            if (spansOverlap(@intFromPtr(out_header), @sizeOf(BookHeader), bases[i], lens[i], &invalid)) {
                return true;
            }
            if (invalid) {
                return true;
            }
        }
    }
    return false;
}

/// Require strict-validation workspaces to be pairwise disjoint: every pair
/// among the reader, inflated chunk, semantic scratch, compressed staging,
/// and exact validated table spans. Pure and thread-safe for immutable span
/// descriptors.
fn validateWorkspaces(
    rd: *const BookChunked,
    chunk: [*]u8,
    chunk_len: u32,
    scratch: [*]u8,
    scratch_cap: u32,
) ra8_err_t {
    const table_len: u64 = (@as(u64, rd.chunk_count) + 1) * @sizeOf(u64);
    const bases = [k_span_count]usize{
        @intFromPtr(rd),
        @intFromPtr(chunk),
        @intFromPtr(scratch),
        addrOf(rd.staging),
        addrOfConst(rd.table),
    };
    const lens = [k_span_count]u64{
        @sizeOf(BookChunked),
        chunk_len,
        scratch_cap,
        rd.staging_cap,
        table_len,
    };
    var i: usize = 0;
    while (i < k_span_count) : (i += 1) {
        var j: usize = i + 1;
        while (j < k_span_count) : (j += 1) {
            var invalid = false;
            if (spansOverlap(bases[i], lens[i], bases[j], lens[j], &invalid)) {
                return k_ra8_err_invalid_arg;
            }
            if (invalid) {
                return k_ra8_err_invalid_arg;
            }
        }
    }
    return k_ra8_ok;
}

/// Inflate one requested chunk into the caller cache when not resident:
/// reuse an already resident matching chunk; otherwise derive the exact
/// final-chunk length and delegate decompression to the reader. Not
/// thread-safe; reuses both reader staging and caller chunk storage.
fn loadChunk(ctx: *ChunkValidate, idx: u32) ra8_err_t {
    if (ctx.loaded and (ctx.loaded_idx == idx)) {
        return k_ra8_ok;
    }
    const offset: u64 = @as(u64, idx) * @as(u64, ctx.rd.chunk_bytes);
    const remain: u64 = ctx.rd.inflated_total - offset;
    var span: u32 = ctx.rd.chunk_bytes;
    if (remain < @as(u64, span)) {
        span = @intCast(remain);
    }
    const err = chunked.book_chunked_read(ctx.rd, offset, ctx.chunk, span);
    if (err == k_ra8_ok) {
        ctx.loaded = true;
        ctx.loaded_idx = idx;
        ctx.loaded_len = span;
    }
    return err;
}

/// Serve an exact arbitrary flat-blob read through the one-chunk cache:
/// split a flat range at chunk boundaries, inflate each chunk at most once
/// while resident, and copy until the exact request is met. Reads spanning
/// chunk boundaries are copied iteratively without recursion.
fn flatRead(opaque_ctx: ?*anyopaque, offset: u64, dst: ?[*]u8, len: u32) callconv(.c) ra8_err_t {
    const ctx: *ChunkValidate = @ptrCast(@alignCast(opaque_ctx.?));
    var cursor = offset;
    // mcdc-deactivated: flatRead range guard; this callback is reachable only
    // from book_validate_stream_strict, whose priv_book_stream_read applies
    // the identical `(off > source_size) || (len > source_size - off)` test
    // against the same rd->inflated_total before invoking it, so neither
    // condition can be true here -- it is a defensive duplicate of an
    // upstream bound.
    if ((cursor > ctx.rd.inflated_total) or (@as(u64, len) > (ctx.rd.inflated_total - cursor))) {
        return k_ra8_err_out_of_range;
    }
    var copied: u32 = 0;
    while (copied < len) {
        const idx: u32 = @intCast(cursor / @as(u64, ctx.rd.chunk_bytes));
        const err = loadChunk(ctx, idx);
        if (err != k_ra8_ok) {
            return err;
        }
        const in_chunk: u32 = @intCast(cursor % @as(u64, ctx.rd.chunk_bytes));
        var span: u32 = ctx.loaded_len - in_chunk;
        if (span > (len - copied)) {
            span = len - copied;
        }
        @memcpy(dst.?[copied .. copied + span], ctx.chunk[in_chunk .. in_chunk + span]);
        copied += span;
        cursor += span;
    }
    return k_ra8_ok;
}

/// Strictly validate the complete flat blob behind an open RBKC reader:
/// adapt the chunk-aligned book_chunked_read interface to the random-read
/// strict validator. `chunk` receives one complete inflated chunk at a time;
/// `scratch` is the independent bounded CRC workspace. No heap allocation or
/// recursion is used; the reader is not thread-safe.
pub export fn book_chunked_validate_strict(
    rd: ?*BookChunked,
    chunk: ?[*]u8,
    chunk_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
    out_header: ?*BookHeader,
) callconv(.c) ra8_err_t {
    if (out_header == null) {
        return k_ra8_err_null_ptr;
    }
    const oh = out_header.?;
    if (rd == null) {
        oh.* = std.mem.zeroes(BookHeader);
        return k_ra8_err_null_ptr;
    }
    const reader = rd.?;
    if (outputIsAliased(reader, chunk, chunk_cap, scratch, scratch_cap, oh)) {
        return k_ra8_err_invalid_arg;
    }
    oh.* = std.mem.zeroes(BookHeader);
    if (chunk == null or scratch == null) {
        return k_ra8_err_null_ptr;
    }
    if (chunk_cap == 0 or scratch_cap == 0) {
        return k_ra8_err_invalid_size;
    }
    var err = validateReader(reader);
    if (err != k_ra8_ok) {
        return err;
    }
    if (chunk_cap < reader.chunk_bytes) {
        return k_ra8_err_invalid_size;
    }
    err = validateWorkspaces(reader, chunk.?, reader.chunk_bytes, scratch.?, scratch_cap);
    if (err != k_ra8_ok) {
        return err;
    }
    var ctx = ChunkValidate{
        .rd = reader,
        .chunk = chunk.?,
        .loaded_idx = 0,
        .loaded_len = 0,
        .loaded = false,
    };
    var validated = std.mem.zeroes(BookHeader);
    err = book_validate_stream_strict(flatRead, &ctx, reader.inflated_total, scratch.?, scratch_cap, &validated);
    if (err == k_ra8_ok) {
        oh.* = validated;
    }
    return err;
}
