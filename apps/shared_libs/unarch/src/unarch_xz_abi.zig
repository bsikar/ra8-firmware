//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bounded XZ/LZMA2 decode wrapper over the vendored xz-embedded. Ported
//! from src/unarch_xz.c (RA8FW-1199) behind the unchanged declarations in
//! inc/unarch_xz.h -- the C ABI callers and the host tests drive stays
//! exactly as declared. The XZ container work (stream header, block
//! header, index/footer, CRC32 verification, budget-charged streaming over
//! the unarch read seam) moves to Zig; the LZMA2 bitstream stays with the
//! vendored xz_embedded C (SOUP) behind the seam, pattern per jof_abi.zig.
//! Clean-room: no code from XZ Utils.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, C23 enum : uint16_t) ---------

const k_ra8_ok: u16 = 0;
const k_ra8_err_no_mem: u16 = 0x102;
const k_ra8_err_invalid_state: u16 = 0x104;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_not_supported: u16 = 0x107;
const k_ra8_err_busy: u16 = 0x109;
const k_ra8_err_validation_failed: u16 = 0x501;
const k_ra8_err_null_ptr: u16 = 0x504;

// ---- unarch_xz_dims_t (inc/unarch_xz.h, C23 enum : uint32_t) --------------

const k_unarch_xz_state_reserve: u32 = 64 * 1024;
const k_unarch_xz_sig_len: usize = 6;

/// xz_wrap_dims_t (src/unarch_xz.c, C23 enum : uint16_t): the unwrap loop's
/// fixed input refill window and output window per decode pass.
const k_xz_chunk: usize = 512;
const k_xz_out_window: usize = 4096;

// ---- The decompression-policy seam (libs/ra8_core) ------------------------

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

// ---- The vendored xz-embedded decoder seam (SOUP, decode-only) ------------

/// enum xz_ret (third_party/xz_embedded/xz.h).
const XzRet = enum(c_int) {
    ok,
    stream_end,
    unsupported_check,
    mem_error,
    memlimit_error,
    format_error,
    options_error,
    data_error,
    buf_error,
};

/// enum xz_mode (third_party/xz_embedded/xz.h): the session drives the
/// decoder in prealloc mode -- state and dictionary allocated once at init.
const XzMode = enum(c_int) { single, prealloc, dynalloc };

/// struct xz_buf (third_party/xz_embedded/xz.h): the in/out window one
/// xz_dec_run pass consumes and fills.
const XzBuf = extern struct {
    in: ?[*]const u8 = null,
    in_pos: usize = 0,
    in_size: usize = 0,
    out: ?[*]u8 = null,
    out_pos: usize = 0,
    out_size: usize = 0,
};

extern fn xz_crc32_init() callconv(.c) void;
extern fn xz_crc64_init() callconv(.c) void;
extern fn xz_dec_init(mode: XzMode, dict_max: u32) callconv(.c) ?*anyopaque;
extern fn xz_dec_run(s: ?*anyopaque, b: *XzBuf) callconv(.c) XzRet;
extern fn xz_dec_end(s: ?*anyopaque) callconv(.c) void;

// ---- The zero-heap allocation pool (src/unarch_xz_pool.c, stays C) --------

extern fn unarch_xz_pool_install(base: ?*anyopaque, len: usize) callconv(.c) u16;
extern fn unarch_xz_pool_reset() callconv(.c) void;

// ---- The public types (inc/unarch_xz.h, inc/unarch_io.h) ------------------

/// unarch_read_fn: the positioned byte reader the archive backs onto.
pub const unarch_read_fn = *const fn (ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize;

/// unarch_xz_stream_t: one multi-call XZ decode session (opaque decoder +
/// liveness flag), bound by stream_begin over a caller scratch.
pub const unarch_xz_stream_t = extern struct {
    dec: ?*anyopaque = null,
    live: bool = false,
};

// ---- Session internals ----------------------------------------------------

/// Map a failing xz-embedded return code onto the wrapper's error space.
/// The success codes are caller-handled; memlimit is the declared-dictionary
/// breach, format/options are non-XZ or unsupported-feature streams, and
/// data/buf (and the impossible codes) are corruption or a stuck stream.
fn mapXzErr(ret: XzRet) u16 {
    return switch (ret) {
        .memlimit_error => k_ra8_err_no_mem,
        .format_error, .options_error => k_ra8_err_not_supported,
        else => k_ra8_err_validation_failed,
    };
}

// ---- Public ABI -----------------------------------------------------------

/// unarch_xz_magic: pure probe over the six magic bytes
/// `FD 37 7A 58 5A 00` so open paths can route a wrapped archive.
pub export fn unarch_xz_magic(sig: ?[*]const u8, sig_len: usize) callconv(.c) bool {
    if (sig == null) {
        return false;
    }
    if (sig_len < k_unarch_xz_sig_len) {
        return false;
    }
    const want = [_]u8{ 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00 };
    return std.mem.eql(u8, sig.?[0..k_unarch_xz_sig_len], &want);
}

/// unarch_xz_stream_begin: install the caller scratch as the XZ allocation
/// arena and create a prealloc decoder whose dictionary budget is
/// `scratch_len - state_reserve`. A stream declaring a larger dictionary is
/// rejected during run (no growth, no fallback).
pub export fn unarch_xz_stream_begin(xs: ?*unarch_xz_stream_t, scratch: ?*anyopaque, scratch_len: u32) callconv(.c) u16 {
    if (xs == null) {
        return k_ra8_err_null_ptr;
    }
    xs.?.* = .{};
    if (scratch == null) {
        return k_ra8_err_null_ptr;
    }
    if (scratch_len <= k_unarch_xz_state_reserve) {
        return k_ra8_err_invalid_size;
    }
    const perr = unarch_xz_pool_install(scratch, scratch_len);
    if (perr != k_ra8_ok) {
        return perr;
    }
    // The CRC lookup tables must exist before any other xz_* call; both
    // initialisers are idempotent, so calling per-session is safe.
    xz_crc32_init();
    xz_crc64_init();
    const dict_max = scratch_len - k_unarch_xz_state_reserve;
    xs.?.dec = xz_dec_init(.prealloc, dict_max);
    if (xs.?.dec == null) {
        unarch_xz_pool_reset();
        return k_ra8_err_no_mem;
    }
    xs.?.live = true;
    return k_ra8_ok;
}

/// unarch_xz_stream_run: one xz_dec_run pass; consumes up to in_len input
/// bytes, writes up to out_cap output bytes, reports both counts and
/// whether the stream verified and ended. The caller loops, charging its
/// own budget per pass, until *end or an error.
pub export fn unarch_xz_stream_run(
    xs: ?*unarch_xz_stream_t,
    in: ?[*]const u8,
    in_len: usize,
    in_used: ?*usize,
    out: ?[*]u8,
    out_cap: usize,
    out_used: ?*usize,
    end: ?*bool,
) callconv(.c) u16 {
    if (xs == null) {
        return k_ra8_err_null_ptr;
    }
    if (in_used == null) {
        return k_ra8_err_null_ptr;
    }
    if (out == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_used == null) {
        return k_ra8_err_null_ptr;
    }
    if (end == null) {
        return k_ra8_err_null_ptr;
    }
    in_used.?.* = 0;
    out_used.?.* = 0;
    end.?.* = false;
    if (in_len > 0 and in == null) {
        return k_ra8_err_null_ptr;
    }
    if (!xs.?.live) {
        return k_ra8_err_invalid_state;
    }
    var b = XzBuf{ .in = in, .in_size = in_len, .out = out, .out_size = out_cap };
    const ret = xz_dec_run(xs.?.dec.?, &b);
    in_used.?.* = b.in_pos;
    out_used.?.* = b.out_pos;
    if (ret == .stream_end) {
        end.?.* = true;
        return k_ra8_ok;
    }
    if (ret == .ok) {
        return k_ra8_ok;
    }
    return mapXzErr(ret);
}

/// unarch_xz_stream_end: free the decoder and release the allocation pool.
/// Safe on a null or never-begun session (teardown paths call it
/// unconditionally).
pub export fn unarch_xz_stream_end(xs: ?*unarch_xz_stream_t) callconv(.c) void {
    if (xs == null) {
        return;
    }
    if (!xs.?.live) {
        return;
    }
    xz_dec_end(xs.?.dec);
    unarch_xz_pool_reset();
    xs.?.dec = null;
    xs.?.live = false;
}

// ---- Unwrap internals -----------------------------------------------------

/// Loop-carried state of one unwrap: cursors, session, and budget.
const UnwrapState = struct {
    read: unarch_read_fn,
    ctx: ?*anyopaque,
    size: u64,
    out: [*]u8,
    out_cap: usize,
    in_off: u64 = 0,
    total_out: usize = 0,
    xs: unarch_xz_stream_t = .{},
    budget: Budget = .{},
    chunk: [k_xz_chunk]u8 = undefined,
};

/// Run one refill + decode pass of the unwrap loop: charge one iteration,
/// refill the input window from the read seam at the current cursor, run
/// the session, advance both cursors, and charge the produced output. A
/// zero-byte refill before the stream ended is a truncated backing; a full
/// arena with the stream still open is an undersized arena.
fn unwrapPass(st: *UnwrapState, end: *bool) u16 {
    const ierr = ra8_decomp_budget_charge_iter(&st.budget);
    if (ierr != 0) {
        return @intCast(ierr);
    }
    const remain = st.size - st.in_off;
    var want: usize = @intCast(@min(remain, @as(u64, k_xz_chunk)));
    if (want > 0) {
        var got = st.read(st.ctx, st.in_off, &st.chunk, want);
        if (got > want) {
            got = want; // clamp a misbehaving backing to the request
        }
        if (got == 0) {
            return k_ra8_err_validation_failed; // backing truncated under the stream
        }
        want = got;
    }
    const space = st.out_cap - st.total_out;
    const owin = @min(space, k_xz_out_window);
    var used: usize = 0;
    var produced: usize = 0;
    const rerr = unarch_xz_stream_run(&st.xs, &st.chunk, want, &used, st.out + st.total_out, owin, &produced, end);
    st.in_off += used;
    st.total_out += produced;
    if (rerr != k_ra8_ok) {
        if (rerr == k_ra8_err_validation_failed and st.total_out == st.out_cap) {
            return k_ra8_err_no_mem; // stuck only because the arena is full
        }
        return rerr;
    }
    const oerr = ra8_decomp_budget_charge_output(&st.budget, st.in_off, @intCast(produced));
    if (oerr != 0) {
        return @intCast(oerr);
    }
    return k_ra8_ok;
}

/// unarch_xz_unwrap: decode one whole XZ stream from a read seam into a
/// caller arena. Runs a full streaming session over [0, size) of the
/// backing: input fetched in fixed chunks, each pass charged against the
/// limits, the integrity check verified before success, and trailing bytes
/// after the footer rejected.
pub export fn unarch_xz_unwrap(
    read: ?unarch_read_fn,
    ctx: ?*anyopaque,
    size: u64,
    out: ?[*]u8,
    out_cap: usize,
    scratch: ?*anyopaque,
    scratch_len: u32,
    limits: ?*const Limits,
    out_len: ?*usize,
) callconv(.c) u16 {
    if (read == null) {
        return k_ra8_err_null_ptr;
    }
    if (out == null) {
        return k_ra8_err_null_ptr;
    }
    if (scratch == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_len == null) {
        return k_ra8_err_null_ptr;
    }
    out_len.?.* = 0;
    if (size == 0) {
        return k_ra8_err_invalid_size;
    }
    if (out_cap == 0) {
        return k_ra8_err_invalid_size;
    }
    var st = UnwrapState{ .read = read.?, .ctx = ctx, .size = size, .out = out.?, .out_cap = out_cap };
    const berr = ra8_decomp_budget_init(&st.budget, limits);
    if (berr != 0) {
        return @intCast(berr);
    }
    const serr = unarch_xz_stream_begin(&st.xs, scratch, scratch_len);
    if (serr != k_ra8_ok) {
        return serr;
    }
    var end = false;
    var perr: u16 = k_ra8_ok;
    while (!end) {
        perr = unwrapPass(&st, &end);
        if (perr != k_ra8_ok) {
            break;
        }
    }
    unarch_xz_stream_end(&st.xs);
    if (perr != k_ra8_ok) {
        return perr;
    }
    if (st.in_off < st.size) {
        return k_ra8_err_validation_failed; // trailing bytes after the footer
    }
    out_len.?.* = st.total_out;
    return k_ra8_ok;
}

// ---- Package tests --------------------------------------------------------
// Compiled against the real vendored xz_embedded (decode-only), the real
// zero-heap pool, and the committed .xz fixtures
// (tests/inc/unarch_xz_fixture.h, generated by
// scripts/gen/gen_unarch_xz_fixture.py) so the container work runs end to
// end. The budget seam comes from src/unarch_xz_teststub.zig.

const fx = struct {
    extern fn unarch_xz_fx_crc64_4k(len: *usize) ?[*]const u8;
    extern fn unarch_xz_fx_crc32(len: *usize) ?[*]const u8;
    extern fn unarch_xz_fx_sha256(len: *usize) ?[*]const u8;
    extern fn unarch_xz_fx_bigdict(len: *usize) ?[*]const u8;
    extern fn unarch_xz_fx_bomb_1m(len: *usize) ?[*]const u8;

    fn get(comptime f: fn (*usize) callconv(.c) ?[*]const u8) []const u8 {
        var len: usize = 0;
        return f(&len).?[0..len];
    }
};

/// The flat-memory read seam: serves [0, len) of one backing buffer.
const Mem = struct { base: [*]const u8, len: usize };

fn memRead(ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize {
    const m: *Mem = @ptrCast(@alignCast(ctx.?));
    const off: usize = @intCast(offset);
    if (off >= m.len) {
        return 0;
    }
    const n = @min(m.len - off, len);
    const dst: [*]u8 = @ptrCast(buf.?);
    var i: usize = 0;
    while (i < n) : (i += 1) {
        dst[i] = m.base[off + i];
    }
    return n;
}

/// Regenerate the 4 KiB LCG payload exactly as the fixture generator.
fn lcgFill(dst: []u8) void {
    var s: u32 = 0x12345678;
    for (dst) |*b| {
        s = s *% 1103515245 +% 12345;
        b.* = @truncate((s >> 16) & 0xFF);
    }
}

/// Session scratch shared by the tests (8-aligned for the pool): 64k state
/// reserve + 128k dictionary budget, holding the committed fixtures.
var s_scratch: [192 * 1024]u8 align(8) = undefined;
var s_arena: [512 * 1024]u8 align(8) = undefined;
var s_expect: [4096]u8 = undefined;
var s_mut: [4156 + 8]u8 = undefined;

fn unwrapMem(data: []const u8, limits: ?*const Limits, out: []u8) u16 {
    var got: usize = 0;
    var mem = Mem{ .base = data.ptr, .len = data.len };
    return unarch_xz_unwrap(memRead, &mem, data.len, out.ptr, out.len, &s_scratch, s_scratch.len, limits, &got);
}

test "magic probe" {
    const crc32 = fx.get(fx.unarch_xz_fx_crc32);
    try std.testing.expect(unarch_xz_magic(crc32.ptr, crc32.len));
    try std.testing.expect(!unarch_xz_magic(crc32.ptr, 5)); // too short
    try std.testing.expect(!unarch_xz_magic(null, 6));
}

test "unwrap crc64 4k matches the LCG payload" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    var got: usize = 0;
    var mem = Mem{ .base = data.ptr, .len = data.len };
    const err = unarch_xz_unwrap(memRead, &mem, data.len, &s_arena, s_arena.len, &s_scratch, s_scratch.len, null, &got);
    try std.testing.expectEqual(@as(u16, k_ra8_ok), err);
    try std.testing.expectEqual(@as(usize, 4096), got);
    lcgFill(s_expect[0..4096]);
    try std.testing.expectEqualSlices(u8, s_expect[0..4096], s_arena[0..4096]);
}

test "unwrap crc32 matches the repeated-text payload" {
    const data = fx.get(fx.unarch_xz_fx_crc32);
    const line = "hello xz stream, bounded and fail-closed\n";
    var got: usize = 0;
    var mem = Mem{ .base = data.ptr, .len = data.len };
    const err = unarch_xz_unwrap(memRead, &mem, data.len, &s_arena, s_arena.len, &s_scratch, s_scratch.len, null, &got);
    try std.testing.expectEqual(@as(u16, k_ra8_ok), err);
    try std.testing.expectEqual(@as(usize, line.len * 8), got);
    for (0..8) |i| {
        try std.testing.expectEqualSlices(u8, line, s_arena[i * line.len .. (i + 1) * line.len]);
    }
}

test "corrupt mid-stream byte rejects fail-closed" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    @memcpy(s_mut[0..data.len], data);
    s_mut[data.len / 2] ^= 0xFF;
    const err = unwrapMem(s_mut[0..data.len], null, &s_arena);
    try std.testing.expectEqual(@as(u16, k_ra8_err_validation_failed), err);
}

test "truncated backing rejects fail-closed" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    const err = unwrapMem(data[0 .. data.len - 8], null, &s_arena);
    try std.testing.expectEqual(@as(u16, k_ra8_err_validation_failed), err);
}

test "trailing bytes after the footer reject" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    @memcpy(s_mut[0..data.len], data);
    @memset(s_mut[data.len..][0..4], 0xA5);
    const err = unwrapMem(s_mut[0 .. data.len + 4], null, &s_arena);
    try std.testing.expectEqual(@as(u16, k_ra8_err_validation_failed), err);
}

test "sha256 check type is unsupported" {
    const data = fx.get(fx.unarch_xz_fx_sha256);
    const err = unwrapMem(data, null, &s_arena);
    try std.testing.expectEqual(@as(u16, k_ra8_err_not_supported), err);
}

test "declared dictionary over the scratch budget is no_mem" {
    const data = fx.get(fx.unarch_xz_fx_bigdict);
    const err = unwrapMem(data, null, &s_arena);
    try std.testing.expectEqual(@as(u16, k_ra8_err_no_mem), err);
}

test "zeros bomb breaches the default ratio bound" {
    const data = fx.get(fx.unarch_xz_fx_bomb_1m);
    const err = unwrapMem(data, null, &s_arena);
    try std.testing.expectEqual(@as(u16, 0x506), err); // decomp_ratio
}

test "tight output cap fires the output bound" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    const lim = Limits{ .max_output_bytes = 1024, .max_ratio = 1024, .ratio_grace_bytes = 4096, .max_iterations = 1 << 20, .max_depth = 8 };
    const err = unwrapMem(data, &lim, &s_arena);
    try std.testing.expectEqual(@as(u16, 0x505), err); // decomp_output_cap
}

test "tight iteration budget fires the iteration bound" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    const lim = Limits{ .max_output_bytes = 1 << 30, .max_ratio = 1024, .ratio_grace_bytes = 4096, .max_iterations = 2, .max_depth = 8 };
    const err = unwrapMem(data, &lim, &s_arena);
    try std.testing.expectEqual(@as(u16, 0x509), err); // decomp_iterations
}

test "undersized arena is no_mem" {
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    var small: [1024]u8 align(8) = undefined;
    const err = unwrapMem(data, null, &small);
    try std.testing.expectEqual(@as(u16, k_ra8_err_no_mem), err);
}

test "session begin/run/end happy path and guards" {
    var xs: unarch_xz_stream_t = .{};
    var out: [8192]u8 align(8) = undefined;
    // Nulls and size guards.
    try std.testing.expectEqual(@as(u16, k_ra8_err_null_ptr), unarch_xz_stream_begin(null, &s_scratch, s_scratch.len));
    try std.testing.expectEqual(@as(u16, k_ra8_err_null_ptr), unarch_xz_stream_begin(&xs, null, s_scratch.len));
    try std.testing.expectEqual(@as(u16, k_ra8_err_invalid_size), unarch_xz_stream_begin(&xs, &s_scratch, k_unarch_xz_state_reserve));
    // Begin, run the whole 4k fixture to the end, end, re-begin.
    const data = fx.get(fx.unarch_xz_fx_crc64_4k);
    try std.testing.expectEqual(@as(u16, k_ra8_ok), unarch_xz_stream_begin(&xs, &s_scratch, s_scratch.len));
    var used: usize = 0;
    var produced: usize = 0;
    var end = false;
    var total: usize = 0;
    var off: usize = 0;
    var passes: usize = 0;
    while (!end and passes < 64) : (passes += 1) {
        const feed = data[off..];
        const err = unarch_xz_stream_run(&xs, feed.ptr, feed.len, &used, &out, out.len, &produced, &end);
        try std.testing.expectEqual(@as(u16, k_ra8_ok), err);
        off += used;
        total += produced;
    }
    try std.testing.expect(end);
    try std.testing.expectEqual(data.len, off);
    try std.testing.expectEqual(@as(usize, 4096), total);
    lcgFill(s_expect[0..4096]);
    try std.testing.expectEqualSlices(u8, s_expect[0..4096], out[0..4096]);
    unarch_xz_stream_end(&xs);
    try std.testing.expectEqual(@as(u16, k_ra8_ok), unarch_xz_stream_begin(&xs, &s_scratch, s_scratch.len));
    // Run on a dead session is invalid_state; a second live begin is busy.
    var dead: unarch_xz_stream_t = .{};
    try std.testing.expectEqual(@as(u16, k_ra8_err_invalid_state), unarch_xz_stream_run(&dead, data.ptr, data.len, &used, &out, out.len, &produced, &end));
    var second: unarch_xz_stream_t = .{};
    try std.testing.expectEqual(@as(u16, k_ra8_err_busy), unarch_xz_stream_begin(&second, &s_scratch, s_scratch.len));
    unarch_xz_stream_end(&xs);
    // End is safe on null and on a never-begun session.
    unarch_xz_stream_end(null);
    unarch_xz_stream_end(&second);
}
