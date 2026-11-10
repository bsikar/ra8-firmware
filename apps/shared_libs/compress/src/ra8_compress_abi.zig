//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the `compress` facade (RA8FW-1155): the C-era
//! src/ra8_compress.c ported verbatim behind the unchanged C header in inc/.
//! Every consumer links miniz; the exports keep the C ABI so the C callers
//! (jof, rabook, book, comic, the demos) and the C tests are untouched.

const std = @import("std");

// ---- miniz surface (apps/shared_libs/third_party/miniz) --------------------
// The vendored miniz.c provides these; the library only declares them.
const TDEFL_STATUS_OKAY: c_int = 0;
const TDEFL_STATUS_DONE: c_int = 1;
const TDEFL_DEFAULT_MAX_PROBES: c_int = 128;
const TDEFL_WRITE_ZLIB_HEADER: c_int = 0x01000;
const TDEFL_FINISH: c_int = 4;
const TINFL_DECOMPRESS_MEM_TO_MEM_FAILED: usize = std.math.maxInt(usize);

const tdefl_compressor = opaque {};
const tdefl_put_buf_func_ptr =
    *const fn (buf: ?*const anyopaque, len: c_int, user: ?*anyopaque) callconv(.c) c_int;

extern fn tdefl_init(
    d: *tdefl_compressor,
    pPut_buf_func: ?tdefl_put_buf_func_ptr,
    pPut_buf_user: ?*anyopaque,
    flags: c_int,
) c_int;
extern fn tdefl_compress_buffer(
    d: *tdefl_compressor,
    pSrc_buf: ?*const anyopaque,
    src_buf_len: usize,
    finish: c_int,
) c_int;
extern fn tinfl_decompress_mem_to_mem(
    pOut_buf: ?*anyopaque,
    out_buf_len: usize,
    pSrc_buf: ?*const anyopaque,
    src_buf_len: usize,
    flags: c_int,
) usize;

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint32_t) --------------
const ra8_err_t = c_uint;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_fail: ra8_err_t = 0x101;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// k_ra8_compress_scratch_bytes in ra8_compress.h is `sizeof(tdefl_compressor)`,
/// pinned Zig-side at the same value jof_abi.zig pins (319,352 bytes). The
/// still-C ra8_vfs_compress.c carries the matching _Static_assert.
const k_ra8_compress_scratch_bytes: u32 = 319_352;

const CompressCtx = struct {
    out: [*]u8,
    cap: u32,
    len: u32,
    overflow: u8,
};

fn internal_compress_put(buf: ?*const anyopaque, len: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const c: *CompressCtx = @ptrCast(@alignCast(user.?));
    const n: u32 = @intCast(len);
    if (n > c.cap - c.len) {
        c.overflow = 1;
        return 0;
    }
    const src: [*]const u8 = @ptrCast(buf.?);
    @memcpy(c.out[c.len .. c.len + n], src[0..n]);
    c.len += n;
    return 1;
}

fn internal_compress_validate(
    src: ?*const anyopaque,
    out: ?*const anyopaque,
    scratch: ?*const anyopaque,
    scratch_len: u32,
    out_len: ?*const u32,
) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    if (out == null) return k_ra8_err_null_ptr;
    if (scratch == null) return k_ra8_err_null_ptr;
    if (out_len == null) return k_ra8_err_null_ptr;
    if (scratch_len < k_ra8_compress_scratch_bytes) return k_ra8_err_invalid_size;
    return k_ra8_ok;
}

fn internal_compress_drive(
    d: *tdefl_compressor,
    ctx: *CompressCtx,
    src: [*]const u8,
    src_len: u32,
    flags: c_int,
    out_len: *u32,
) ra8_err_t {
    if (tdefl_init(d, internal_compress_put, ctx, flags) != TDEFL_STATUS_OKAY) {
        return k_ra8_fail;
    }
    const st = tdefl_compress_buffer(d, @ptrCast(src), src_len, TDEFL_FINISH);
    if (ctx.overflow != 0) {
        return k_ra8_err_no_mem;
    }
    if (st != TDEFL_STATUS_DONE) {
        return k_ra8_fail;
    }
    out_len.* = ctx.len;
    return k_ra8_ok;
}

pub export fn ra8_compress(
    src: ?[*]const u8,
    src_len: u32,
    out: ?[*]u8,
    out_cap: u32,
    scratch: ?*anyopaque,
    scratch_len: u32,
    out_len: ?*u32,
) callconv(.c) ra8_err_t {
    const vr = internal_compress_validate(src, out, scratch, scratch_len, out_len);
    if (vr != k_ra8_ok) {
        return vr;
    }
    var ctx = CompressCtx{ .out = out.?, .cap = out_cap, .len = 0, .overflow = 0 };
    const d: *tdefl_compressor = @ptrCast(@alignCast(scratch.?));
    return internal_compress_drive(d, &ctx, src.?, src_len, TDEFL_DEFAULT_MAX_PROBES, out_len.?);
}

pub export fn ra8_compress_zlib(
    src: ?[*]const u8,
    src_len: u32,
    out: ?[*]u8,
    out_cap: u32,
    scratch: ?*anyopaque,
    scratch_len: u32,
    out_len: ?*u32,
) callconv(.c) ra8_err_t {
    const vr = internal_compress_validate(src, out, scratch, scratch_len, out_len);
    if (vr != k_ra8_ok) {
        return vr;
    }
    var ctx = CompressCtx{ .out = out.?, .cap = out_cap, .len = 0, .overflow = 0 };
    const d: *tdefl_compressor = @ptrCast(@alignCast(scratch.?));
    const flags: c_int = TDEFL_DEFAULT_MAX_PROBES | TDEFL_WRITE_ZLIB_HEADER;
    return internal_compress_drive(d, &ctx, src.?, src_len, flags, out_len.?);
}

pub export fn ra8_decompress(
    src: ?[*]const u8,
    src_len: u32,
    out: ?[*]u8,
    out_cap: u32,
    out_len: ?*u32,
) callconv(.c) ra8_err_t {
    if (src == null) return k_ra8_err_null_ptr;
    if (out == null) return k_ra8_err_null_ptr;
    if (out_len == null) return k_ra8_err_null_ptr;
    const n = tinfl_decompress_mem_to_mem(@ptrCast(out.?), out_cap, @ptrCast(src.?), src_len, 0);
    if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        return k_ra8_err_no_mem;
    }
    out_len.?.* = @intCast(n);
    return k_ra8_ok;
}
