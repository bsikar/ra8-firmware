//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared positioned-source readers, big-endian decoders, JPEG SOF classifiers,
//! pull cursors, and durable transaction helpers for `libs/ra8_fmt`.

const std = @import("std");
const c_abi = @import("c_abi");

const jpeg_sof0: u8 = 0xC0;
const jpeg_dht: u8 = 0xC4;
const jpeg_jpg: u8 = 0xC8;
const jpeg_dac: u8 = 0xCC;
const jpeg_sof_last: u8 = 0xCF;

/// Sequential pull state wrapping a positioned `ra8_fmt_source_t`.
pub const PullCursor = struct {
    source: *const c_abi.Source,
    offset: u64 = 0,
};

/// C ABI callback adapting `jof_pull_fn` calls to `PullCursor`.
pub fn pullCallback(
    ctx: ?*anyopaque,
    bytes: ?[*]u8,
    cap: usize,
    got: ?*usize,
) callconv(.c) c_abi.Ra8Err {
    const pull: *PullCursor = @ptrCast(@alignCast(ctx orelse return c_abi.k_ra8_err_null_ptr));
    const got_ptr = got orelse return c_abi.k_ra8_err_null_ptr;
    if (cap != 0 and bytes == null) return c_abi.k_ra8_err_null_ptr;
    got_ptr.* = 0;

    if (cap == 0 or pull.offset >= pull.source.size) return c_abi.k_ra8_ok;
    const read_fn = pull.source.read_at orelse return c_abi.k_ra8_err_null_ptr;

    const remain = pull.source.size - pull.offset;
    const req_len: usize = if (@as(u64, cap) > remain) @intCast(remain) else cap;
    var rc = read_fn(pull.source.ctx, pull.offset, bytes, req_len, got_ptr);
    if (rc == c_abi.k_ra8_ok and got_ptr.* <= req_len) {
        pull.offset += got_ptr.*;
    } else if (rc == c_abi.k_ra8_ok) {
        rc = c_abi.k_ra8_err_protocol_error;
    }
    return rc;
}

/// Reads an exact byte slice via one positioned read call (fails on short read).
pub fn readExactSingle(
    source: *const c_abi.Source,
    offset: u64,
    out: []u8,
) c_abi.Ra8Err {
    const read_fn = source.read_at orelse return c_abi.k_ra8_err_null_ptr;
    var got: usize = 0;
    const rc = read_fn(source.ctx, offset, if (out.len > 0) out.ptr else null, out.len, &got);
    if (rc != c_abi.k_ra8_ok) return rc;
    return if (got == out.len) c_abi.k_ra8_ok else c_abi.k_ra8_err_validation_failed;
}

/// Reads an exact byte slice looping over legal positive short reads.
pub fn readExactLoop(
    source: *const c_abi.Source,
    offset: u64,
    out: []u8,
    short_or_range_err: c_abi.Ra8Err,
) c_abi.Ra8Err {
    if (offset > source.size or @as(u64, out.len) > source.size - offset) {
        return short_or_range_err;
    }
    const read_fn = source.read_at orelse return c_abi.k_ra8_err_null_ptr;
    var done: usize = 0;
    while (done < out.len) {
        var got: usize = 0;
        const ask = out.len - done;
        const rc = read_fn(source.ctx, offset + done, out[done..].ptr, ask, &got);
        if (rc != c_abi.k_ra8_ok) return rc;
        if (got == 0 or got > ask) return short_or_range_err;
        done += got;
    }
    return c_abi.k_ra8_ok;
}

/// Decodes two big-endian bytes without unaligned access.
pub fn be16(bytes: *const [2]u8) u16 {
    return std.mem.readInt(u16, bytes, .big);
}

/// Decodes four big-endian bytes without unaligned access.
pub fn be32(bytes: *const [4]u8) u32 {
    return std.mem.readInt(u32, bytes, .big);
}

/// Returns true for JPEG start-of-frame marker codes, excluding DHT/JPG/DAC.
pub fn isJpegSofMarker(marker: u8) bool {
    const in_range = marker >= jpeg_sof0 and marker <= jpeg_sof_last;
    return in_range and marker != jpeg_dht and marker != jpeg_jpg and marker != jpeg_dac;
}

/// Aborts an optional transaction stage if an abort callback is wired.
pub fn abortTransaction(tx: ?*c_abi.Transaction) void {
    const t = tx orelse return;
    const ops = t.ops orelse return;
    const abort_fn = ops.abort orelse return;
    abort_fn(t.ctx);
}

/// Validates a source via its optional stability callback.
pub fn checkStable(source: *const c_abi.Source) c_abi.Ra8Err {
    const validate_fn = source.validate orelse return c_abi.k_ra8_ok;
    return validate_fn(source.ctx, source.size);
}
