//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `unarch_gzip_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("unarch_gzip_abi");

const Limits = sut.Limits;
const k_gz_cm_deflate = sut.k_gz_cm_deflate;
const k_gz_flg_fcomment = sut.k_gz_flg_fcomment;
const k_gz_flg_fextra = sut.k_gz_flg_fextra;
const k_gz_flg_fhcrc = sut.k_gz_flg_fhcrc;
const k_gz_flg_fname = sut.k_gz_flg_fname;
const k_gz_hdr_fixed = sut.k_gz_hdr_fixed;
const k_gz_id1 = sut.k_gz_id1;
const k_gz_id2 = sut.k_gz_id2;
const k_ra8_err_checksum_mismatch = sut.k_ra8_err_checksum_mismatch;
const k_ra8_err_invalid_arg = sut.k_ra8_err_invalid_arg;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_no_mem = sut.k_ra8_err_no_mem;
const k_ra8_err_not_supported = sut.k_ra8_err_not_supported;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;
const mz_crc32 = sut.mz_crc32;
const unarch_gzip_magic = sut.unarch_gzip_magic;
const unarch_gzip_unwrap = sut.unarch_gzip_unwrap;

// ---- Package tests ---------------------------------------------------------
// The gzip package test links the real vendored miniz (build.zig compiles
// miniz.c in) so the tinfl seam is exercised end to end, and the gzip
// teststub stands in for the ra8_core budget seam, same idiom as tar.

fn le16(v: u16) [2]u8 {
    return .{ @truncate(v), @truncate(v >> 8) };
}

fn le32(v: u32) [4]u8 {
    return .{ @truncate(v), @truncate(v >> 8), @truncate(v >> 16), @truncate(v >> 24) };
}

const MemberOpts = struct {
    corrupt_fhcrc: bool = false,
    corrupt_trailer_crc: bool = false,
};

/// Assemble one whole gzip member: fixed header, the requested optional
/// fields, FHCRC over the header prefix, one final stored DEFLATE block,
/// then the CRC32+ISIZE trailer.
fn buildGzip(payload: []const u8, flg: u8, extra: []const u8, name: []const u8, comment: []const u8, opts: MemberOpts) MemberBuf {
    var mb: [1024]u8 = undefined;
    var n: usize = 0;
    mb[0] = k_gz_id1;
    mb[1] = k_gz_id2;
    mb[2] = k_gz_cm_deflate;
    mb[3] = flg;
    @memset(mb[4..10], 0);
    n = k_gz_hdr_fixed;
    if ((flg & k_gz_flg_fextra) != 0) {
        const xl = le16(@intCast(extra.len));
        @memcpy(mb[n..][0..2], &xl);
        n += 2;
        @memcpy(mb[n..][0..extra.len], extra);
        n += extra.len;
    }
    if ((flg & k_gz_flg_fname) != 0) {
        @memcpy(mb[n..][0..name.len], name);
        n += name.len;
        mb[n] = 0;
        n += 1;
    }
    if ((flg & k_gz_flg_fcomment) != 0) {
        @memcpy(mb[n..][0..comment.len], comment);
        n += comment.len;
        mb[n] = 0;
        n += 1;
    }
    if ((flg & k_gz_flg_fhcrc) != 0) {
        var crc: u32 = @truncate(mz_crc32(0, mb[0..n].ptr, n));
        if (opts.corrupt_fhcrc) {
            crc ^= 1;
        }
        const hc = le16(@truncate(crc));
        @memcpy(mb[n..][0..2], &hc);
        n += 2;
    }
    const dlen = le16(@intCast(payload.len));
    const dnlen = le16(@intCast(~@as(u16, @intCast(payload.len))));
    mb[n] = 0x01; // BFINAL=1, BTYPE=00 stored block
    @memcpy(mb[n + 1 ..][0..2], &dlen);
    @memcpy(mb[n + 3 ..][0..2], &dnlen);
    n += 5;
    @memcpy(mb[n..][0..payload.len], payload);
    n += payload.len;
    var tcrc: u32 = @truncate(mz_crc32(0, payload.ptr, payload.len));
    if (opts.corrupt_trailer_crc) {
        tcrc ^= 1;
    }
    @memcpy(mb[n..][0..4], &le32(tcrc));
    @memcpy(mb[n + 4 ..][0..4], &le32(@intCast(payload.len)));
    n += 8;
    return .{ .bytes = mb, .len = n };
}

const MemberBuf = struct {
    bytes: [1024]u8,
    len: usize,
};

const TestBuf = struct {
    data: []const u8,
    pos: u64 = 0,

    fn read(ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize {
        const self: *TestBuf = @ptrCast(@alignCast(ctx.?));
        if (offset >= self.data.len) {
            return 0;
        }
        const n = @min(len, self.data.len - @as(usize, @intCast(offset)));
        const dst: [*]u8 = @ptrCast(buf.?);
        @memcpy(dst[0..n], self.data[@intCast(offset)..][0..n]);
        return n;
    }
};

test "gzip magic accepts and rejects" {
    const sig = [_]u8{ 0x1F, 0x8B };
    try testing.expect(unarch_gzip_magic(&sig, 2));
    try testing.expect(!unarch_gzip_magic(&sig, 1));
    const bad = [_]u8{ 0x1F, 0x8C };
    try testing.expect(!unarch_gzip_magic(&bad, 2));
    try testing.expect(!unarch_gzip_magic(null, 2));
}

test "unwrap round trips a fully flagged member" {
    const payload = "the quick brown fox jumps over the lazy dog";
    const mb = buildGzip(payload, k_gz_flg_fhcrc | k_gz_flg_fextra | k_gz_flg_fname | k_gz_flg_fcomment, "XA", "n.txt", "a comment", .{});
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [4096]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_ok, err);
    try testing.expectEqual(payload.len, out_len);
    try testing.expectEqualSlices(u8, payload, out[0..out_len]);
}

test "unwrap rejects reserved flag bits" {
    const payload = "x";
    const mb = buildGzip(payload, 0x20, "", "", "", .{});
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_not_supported, err);
}

test "unwrap rejects a non-DEFLATE method" {
    const payload = "x";
    var mb = buildGzip(payload, 0, "", "", "", .{});
    mb.bytes[2] = 9;
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_not_supported, err);
}

test "unwrap rejects a corrupt FHCRC" {
    const payload = "the payload";
    const mb = buildGzip(payload, k_gz_flg_fhcrc, "", "", "", .{ .corrupt_fhcrc = true });
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_checksum_mismatch, err);
}

test "unwrap rejects a bad trailer CRC" {
    const payload = "the payload";
    const mb = buildGzip(payload, 0, "", "", "", .{ .corrupt_trailer_crc = true });
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_checksum_mismatch, err);
}

test "unwrap refuses trailing bytes after the member" {
    const payload = "the payload";
    var mb = buildGzip(payload, 0, "", "", "", .{});
    mb.bytes[mb.len] = 0;
    mb.len += 1;
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_validation_failed, err);
}

test "unwrap rejects a truncated stream" {
    const payload = "the payload";
    const mb = buildGzip(payload, 0, "", "", "", .{});
    var buf = TestBuf{ .data = mb.bytes[0 .. mb.len - 4] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len - 4, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_validation_failed, err);
}

test "unwrap rejects an arena too small for the member" {
    const payload = "the payload";
    const mb = buildGzip(payload, 0, "", "", "", .{});
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [4]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_no_mem, err);
}

test "unwrap rejects null and zero arguments" {
    const payload = "x";
    const mb = buildGzip(payload, 0, "", "", "", .{});
    var buf = TestBuf{ .data = mb.bytes[0..mb.len] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    try testing.expectEqual(k_ra8_err_null_ptr, unarch_gzip_unwrap(null, &buf, mb.len, &out, out.len, null, &out_len));
    try testing.expectEqual(k_ra8_err_null_ptr, unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, null, out.len, null, &out_len));
    try testing.expectEqual(k_ra8_err_null_ptr, unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, null, null));
    try testing.expectEqual(k_ra8_err_invalid_size, unarch_gzip_unwrap(TestBuf.read, &buf, 0, &out, out.len, null, &out_len));
    try testing.expectEqual(k_ra8_err_invalid_size, unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, 0, null, &out_len));
    var zero_limits = Limits{};
    try testing.expectEqual(k_ra8_err_invalid_arg, unarch_gzip_unwrap(TestBuf.read, &buf, mb.len, &out, out.len, &zero_limits, &out_len));
}

test "unwrap rejects a truncated header" {
    const payload = "x";
    const mb = buildGzip(payload, 0, "", "", "", .{});
    var buf = TestBuf{ .data = mb.bytes[0..3] };
    var out: [64]u8 = undefined;
    var out_len: usize = 0;
    const err = unarch_gzip_unwrap(TestBuf.read, &buf, 3, &out, out.len, null, &out_len);
    try testing.expectEqual(k_ra8_err_validation_failed, err);
}
