//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Port of apps/shared_libs/unarch/src/unarch_gzip.c to Zig behind the
//! unchanged declarations in inc/unarch_gzip.h (RA8FW-1198).
//!
//! The RFC 1952 container work moves here: the exact-read cursor with the
//! running header CRC, the fail-closed header parser (magic, method,
//! reserved bits, bounded FEXTRA/FNAME/FCOMMENT, FHCRC), the windowed
//! inflate loop charged against the unified decompression budget, and the
//! CRC32+ISIZE trailer verifier. The DEFLATE bitstream itself stays with
//! the vendored miniz tinfl core (SOUP, stays C): tinfl_decompress and
//! mz_crc32 are externs resolved against the miniz the consumers already
//! link, the same seam pattern the jof port established. The inflator is
//! held as a byte-exact block pinned to miniz's tinfl_decompressor size
//! (8376 bytes, the size the jof seam asserts); tinfl_init is an inline
//! that only clears m_state, so a zeroed block is the fresh init.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, C23 enum : uint16_t) ---------
const k_ra8_ok: u16 = 0;
const k_ra8_err_no_mem: u16 = 0x102;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_not_supported: u16 = 0x107;
const k_ra8_err_validation_failed: u16 = 0x501;
const k_ra8_err_checksum_mismatch: u16 = 0x502;
const k_ra8_err_null_ptr: u16 = 0x504;

/// ra8_decomp_limits_t mirror (libs/ra8_core/src/internal/decomp/policy.zig).
pub const Limits = extern struct {
    max_output_bytes: u64 = 0,
    max_ratio: u32 = 0,
    ratio_grace_bytes: u32 = 0,
    max_entries: u32 = 0,
    max_iterations: u32 = 0,
    max_depth: u8 = 0,
};

/// ra8_decomp_budget_t mirror (libs/ra8_core/src/internal/decomp/budget.zig).
pub const Budget = extern struct {
    limits: Limits = .{},
    out_bytes: u64 = 0,
    in_bytes: u64 = 0,
    entries: u32 = 0,
    iters: u32 = 0,
    depth: u8 = 0,
};

extern fn ra8_decomp_budget_init(b: ?*Budget, limits: ?*const Limits) callconv(.c) c_int;
extern fn ra8_decomp_budget_charge_iter(b: ?*Budget) callconv(.c) c_int;
extern fn ra8_decomp_budget_charge_output(b: ?*Budget, in_pos: u64, out_bytes: u64) callconv(.c) c_int;

// ---- miniz seam (vendored miniz; the consumers link it) -------------------
//
// tinfl_decompressor is held as an aligned byte block pinned to miniz's
// size (8376, the jof seam's assert); the C side owns the real layout and
// this file only feeds it whole to tinfl_decompress. miniz flags/statuses
// pinned from the vendored miniz.h enum: TINFL_FLAG_HAS_MORE_INPUT 2,
// TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4, TINFL_STATUS_FAILED.. -4,
// NEEDS_MORE_INPUT 1, HAS_MORE_OUTPUT 2, DONE 0. MZ_CRC32_INIT is 0.
const k_gz_tinfl_size: usize = 8376;

var s_gz_inflator: [k_gz_tinfl_size]u8 align(8) = std.mem.zeroes([k_gz_tinfl_size]u8);

extern fn tinfl_decompress(
    r: [*c]u8,
    in_buf_next: [*c]const u8,
    in_buf_size: *usize,
    out_buf_start: [*c]u8,
    out_buf_next: [*c]u8,
    out_buf_size: *usize,
    decomp_flags: c_uint,
) callconv(.c) c_int;
extern fn mz_crc32(crc: c_ulong, ptr: [*c]const u8, buf_len: usize) callconv(.c) c_ulong;

const k_tinfl_status_done: c_int = 0;
const k_tinfl_status_needs_more_input: c_int = 1;
const k_tinfl_status_has_more_output: c_int = 2;
const k_tinfl_flag_has_more_input: c_uint = 2;
const k_tinfl_flag_non_wrapping: c_uint = 4;

// ---- unarch_read_fn (inc/unarch_io.h) -------------------------------------
pub const unarch_read_fn = *const fn (ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize;

// ---- RFC 1952 grammar constants (inc/unarch_gzip.h) -----------------------
const k_gz_sig_len: usize = 2;
const k_gz_str_max: u32 = 2048;
const k_gz_id1: u8 = 0x1F;
const k_gz_id2: u8 = 0x8B;
const k_gz_cm_deflate: u8 = 8;
const k_gz_flg_fhcrc: u8 = 0x02;
const k_gz_flg_fextra: u8 = 0x04;
const k_gz_flg_fname: u8 = 0x08;
const k_gz_flg_fcomment: u8 = 0x10;
const k_gz_flg_reserved: u8 = 0xE0;
const k_gz_hdr_fixed: usize = 10;
const k_gz_xlen_bytes: usize = 2;
const k_gz_fhcrc_bytes: usize = 2;
const k_gz_trailer_len: usize = 8;
const k_gz_in_chunk: usize = 512;
const k_gz_out_window: usize = 4096;

/// Cursor-tracked exact-read byte source with the running header CRC.
const GzSrc = struct {
    read: unarch_read_fn,
    ctx: ?*anyopaque,
    size: u64,
    pos: u64 = 0,
    crc: u32 = 0,
    track_crc: bool = true,

    /// Consume exactly dst.len bytes; a short read fails closed.
    fn take(s: *GzSrc, dst: []u8) u16 {
        if (dst.len > s.size - s.pos) {
            return k_ra8_err_validation_failed; // runs past the member end
        }
        const got = s.read(s.ctx, s.pos, dst.ptr, dst.len);
        if (got != dst.len) {
            return k_ra8_err_validation_failed; // backing truncated
        }
        if (s.track_crc) {
            s.crc = @truncate(mz_crc32(s.crc, dst.ptr, dst.len));
        }
        s.pos += dst.len;
        return k_ra8_ok;
    }
};

/// Skip one NUL-terminated header string, bounded by k_gz_str_max.
fn skipString(s: *GzSrc) u16 {
    var i: u32 = 0;
    while (i < k_gz_str_max) : (i += 1) {
        var b: [1]u8 = undefined;
        const terr = s.take(&b);
        if (terr != k_ra8_ok) {
            return terr;
        }
        if (b[0] == 0) {
            return k_ra8_ok;
        }
    }
    return k_ra8_err_validation_failed; // string exceeds the fail-closed cap
}

/// Skip the FEXTRA block (2-byte LE XLEN then XLEN bytes, chunked).
fn skipFextra(s: *GzSrc) u16 {
    var xl: [k_gz_xlen_bytes]u8 = undefined;
    const xerr = s.take(&xl);
    if (xerr != k_ra8_ok) {
        return xerr;
    }
    var xlen: u32 = @as(u32, xl[0]) | (@as(u32, xl[1]) << 8);
    var skip: [k_gz_in_chunk]u8 = undefined;
    while (xlen > 0) {
        const n: usize = @min(@as(usize, xlen), k_gz_in_chunk);
        const serr = s.take(skip[0..n]);
        if (serr != k_ra8_ok) {
            return serr;
        }
        xlen -= @intCast(n);
    }
    return k_ra8_ok;
}

/// Parse the whole RFC 1952 header, leaving the cursor at the DEFLATE data.
fn parseHeader(s: *GzSrc) u16 {
    var fixed: [k_gz_hdr_fixed]u8 = undefined;
    const ferr = s.take(&fixed);
    if (ferr != k_ra8_ok) {
        return ferr;
    }
    if (fixed[0] != k_gz_id1 or fixed[1] != k_gz_id2 or fixed[2] != k_gz_cm_deflate) {
        return k_ra8_err_not_supported; // bad magic or non-DEFLATE method
    }
    const flg = fixed[3];
    if ((flg & k_gz_flg_reserved) != 0) {
        return k_ra8_err_not_supported; // reserved bits: unknown format
    }
    if ((flg & k_gz_flg_fextra) != 0) {
        const xerr = skipFextra(s);
        if (xerr != k_ra8_ok) {
            return xerr;
        }
    }
    if ((flg & k_gz_flg_fname) != 0) {
        const nerr = skipString(s);
        if (nerr != k_ra8_ok) {
            return nerr;
        }
    }
    if ((flg & k_gz_flg_fcomment) != 0) {
        const cerr = skipString(s);
        if (cerr != k_ra8_ok) {
            return cerr;
        }
    }
    if ((flg & k_gz_flg_fhcrc) != 0) {
        const want = s.crc & 0xFFFF;
        var hc: [k_gz_fhcrc_bytes]u8 = undefined;
        const herr = s.take(&hc);
        if (herr != k_ra8_ok) {
            return herr;
        }
        const stored = @as(u32, hc[0]) | (@as(u32, hc[1]) << 8);
        if (stored != want) {
            return k_ra8_err_checksum_mismatch;
        }
    }
    s.track_crc = false; // header ends; the payload CRC is separate
    return k_ra8_ok;
}

/// Loop-carried state of the windowed DEFLATE inflation.
const GzInflate = struct {
    src: *GzSrc,
    out: [*]u8,
    out_cap: usize,
    total: usize = 0,
    crc: u32 = 0,
    budget: Budget = .{},
    win: [k_gz_in_chunk]u8 = undefined,
    win_len: usize = 0,
    win_ofs: usize = 0,
    done: bool = false,
};

/// One refill + inflate pass: charge, refill, tinfl, fold CRC, charge output.
fn inflatePass(st: *GzInflate) u16 {
    const ierr = ra8_decomp_budget_charge_iter(&st.budget);
    if (ierr != 0) {
        return @intCast(ierr);
    }
    if (st.win_ofs == st.win_len) {
        const remain = st.src.size - st.src.pos;
        const want: usize = @intCast(@min(remain, k_gz_in_chunk));
        if (want == 0) {
            return k_ra8_err_validation_failed; // input exhausted mid-stream
        }
        const rerr = st.src.take(st.win[0..want]);
        if (rerr != k_ra8_ok) {
            return rerr;
        }
        st.win_len = want;
        st.win_ofs = 0;
    }
    const more_input = st.src.pos < st.src.size;
    var in_bytes: usize = st.win_len - st.win_ofs;
    const space = st.out_cap - st.total;
    var out_bytes: usize = @min(space, k_gz_out_window);
    var flags: c_uint = k_tinfl_flag_non_wrapping;
    if (more_input) {
        flags |= k_tinfl_flag_has_more_input;
    }
    const ts = tinfl_decompress(
        &s_gz_inflator,
        st.win[st.win_ofs..].ptr,
        &in_bytes,
        st.out,
        st.out + st.total,
        &out_bytes,
        flags,
    );
    st.win_ofs += in_bytes;
    if (out_bytes != 0) {
        st.crc = @truncate(mz_crc32(st.crc, st.out + st.total, out_bytes));
    }
    st.total += out_bytes;
    const oerr = ra8_decomp_budget_charge_output(&st.budget, st.src.pos, out_bytes);
    if (oerr != 0) {
        return @intCast(oerr);
    }
    if (ts == k_tinfl_status_done) {
        st.done = true;
        return k_ra8_ok;
    }
    if (ts == k_tinfl_status_needs_more_input) {
        return k_ra8_ok; // the next pass refills
    }
    if (ts == k_tinfl_status_has_more_output) {
        if (st.total == st.out_cap) {
            return k_ra8_err_no_mem; // arena full with the stream open
        }
        return k_ra8_ok;
    }
    return k_ra8_err_validation_failed; // corrupt DEFLATE stream
}

/// Verify the trailer: CRC32 and ISIZE must match, nothing may follow.
fn verifyTrailer(st: *const GzInflate) u16 {
    const leftover: u64 = st.win_len - st.win_ofs;
    const trailer_pos = st.src.pos - leftover;
    const remain = st.src.size - trailer_pos;
    if (remain != k_gz_trailer_len) {
        return k_ra8_err_validation_failed; // truncated or trailing bytes
    }
    var tr: [k_gz_trailer_len]u8 = undefined;
    const got = st.src.read(st.src.ctx, trailer_pos, &tr, k_gz_trailer_len);
    if (got != k_gz_trailer_len) {
        return k_ra8_err_validation_failed; // backing truncated
    }
    var crc_stored: u32 = 0;
    var isize_stored: u32 = 0;
    var i: usize = 0;
    while (i < 4) : (i += 1) {
        crc_stored |= @as(u32, tr[i]) << @intCast(8 * i);
        isize_stored |= @as(u32, tr[i + 4]) << @intCast(8 * i);
    }
    if (crc_stored != st.crc or isize_stored != @as(u32, @truncate(st.total))) {
        return k_ra8_err_checksum_mismatch;
    }
    return k_ra8_ok;
}

/// unarch_gzip_magic: whether the bytes begin a gzip member.
pub export fn unarch_gzip_magic(sig: ?[*]const u8, sig_len: usize) callconv(.c) bool {
    if (sig == null or sig_len < k_gz_sig_len) {
        return false;
    }
    return sig.?[0] == k_gz_id1 and sig.?[1] == k_gz_id2;
}

/// unarch_gzip_unwrap: decode one whole gzip member into the caller arena.
pub export fn unarch_gzip_unwrap(
    read: ?unarch_read_fn,
    ctx: ?*anyopaque,
    size: u64,
    out: ?[*]u8,
    out_cap: usize,
    limits: ?*const Limits,
    out_len: ?*usize,
) callconv(.c) u16 {
    if (read == null or out == null or out_len == null) {
        return k_ra8_err_null_ptr;
    }
    out_len.?.* = 0;
    if (size == 0 or out_cap == 0) {
        return k_ra8_err_invalid_size;
    }
    var src = GzSrc{ .read = read.?, .ctx = ctx, .size = size };
    const herr = parseHeader(&src);
    if (herr != k_ra8_ok) {
        return herr;
    }
    var st = GzInflate{ .src = &src, .out = out.?, .out_cap = out_cap };
    const berr = ra8_decomp_budget_init(&st.budget, limits);
    if (berr != 0) {
        return @intCast(berr);
    }
    s_gz_inflator = std.mem.zeroes([k_gz_tinfl_size]u8); // tinfl_init
    while (!st.done) {
        const perr = inflatePass(&st);
        if (perr != k_ra8_ok) {
            return perr;
        }
    }
    const terr = verifyTrailer(&st);
    if (terr != k_ra8_ok) {
        return terr;
    }
    out_len.?.* = st.total;
    return k_ra8_ok;
}

// ---- Package tests ---------------------------------------------------------
// The gzip package test links the real vendored miniz (build.zig compiles
// miniz.c in) so the tinfl seam is exercised end to end, and the gzip
// teststub stands in for the ra8_core budget seam, same idiom as tar.

const testing = std.testing;

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
