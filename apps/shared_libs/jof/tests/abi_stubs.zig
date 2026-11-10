//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Test-link stubs for the jof archive's sibling C symbols. The streaming
//! trio calls the ra8_webp decode seam and
//! the ra8_webp decode seam, whose real definitions live in the C TUs jof's
//! consumers compile beside the archive. The standalone `zig build test`
//! link carries none of those, so these no-ops satisfy the linker. They are
//! never called by the unit tests, and they never enter libjof.a.
const jof_h = @import("jof_h");

pub export fn ra8_webp_get_info(data: [*c]const u8, size: usize, out_w: [*c]u32, out_h: [*c]u32) callconv(.c) jof_h.ra8_err_t {
    _ = .{ data, size, out_w, out_h };
    return jof_h.k_ra8_err_invalid_size;
}

pub export fn ra8_webp_decode_rgba(data: [*c]const u8, size: usize, arena: [*c]jof_h.ra8_webp_arena_t, out_rgba: [*c]u8, out_stride: usize, out_capacity: usize, out_w: [*c]u32, out_h: [*c]u32) callconv(.c) jof_h.ra8_err_t {
    _ = .{ data, size, arena, out_rgba, out_stride, out_capacity, out_w, out_h };
    return jof_h.k_ra8_err_invalid_size;
}

pub export fn tinfl_decompress(r: [*c]jof_h.struct_tinfl_decompressor_tag, pIn_buf_next: [*c]const u8, pIn_buf_size: [*c]usize, pOut_buf_start: [*c]u8, pOut_buf_next: [*c]u8, pOut_buf_size: [*c]usize, decomp_flags: c_uint) callconv(.c) c_int {
    _ = .{ r, pIn_buf_next, pIn_buf_size, pOut_buf_start, pOut_buf_next, pOut_buf_size, decomp_flags };
    return 0;
}

pub export fn ra8_log_emit_error(tag: [*c]const u8, message: [*c]const u8) callconv(.c) void {
    _ = .{ tag, message };
}

pub export fn ra8_decompress(src: [*c]const u8, src_len: u32, out: [*c]u8, out_cap: u32, out_len: [*c]u32) callconv(.c) jof_h.ra8_err_t {
    _ = .{ src, src_len, out, out_cap, out_len };
    return jof_h.k_ra8_err_invalid_size;
}

/// Deterministic copy "compressor" for the standalone test link: copies
/// min(src_len, out_cap) bytes and reports that length, so the deflate
/// encode arm's bookkeeping (entry offset/length, sink order) is exercised
/// without the real tdefl. The real ra8_compress TU links in consumers.
pub export fn ra8_compress(src: [*c]const u8, src_len: u32, out: [*c]u8, out_cap: u32, scratch: ?*anyopaque, scratch_len: u32, out_len: [*c]u32) callconv(.c) jof_h.ra8_err_t {
    _ = .{ scratch, scratch_len };
    if ((src == null) or (out == null) or (out_len == null)) {
        return jof_h.k_ra8_err_null_ptr;
    }
    const n: u32 = if (src_len < out_cap) src_len else out_cap;
    @memcpy(out[0..n], src[0..n]);
    out_len.* = n;
    return jof_h.k_ra8_ok;
}

/// Fake streaming JPEG decoder for the standalone test link: the real
/// ra8_jpeg_sw_decode_stripes lives in the JPEG C library, which the jof
/// test build does not link. The fake binds the geometry hook, hands back a
/// stripe and streams the configured pixels through the row hook, so the
/// producer state machine runs end to end. Config comes from the exported
/// jpeg_stub_* variables below.
const JpegGeomFn = ?*const fn (ctx: ?*anyopaque, width: u16, height: u16, channels: u8, stripe_rows: u16, out_stripe: [*c][*c]u8, out_stripe_cap: [*c]u32) callconv(.c) jof_h.ra8_err_t;
const JpegRowsFn = ?*const fn (ctx: ?*anyopaque, px: [*c]const u8, width: u16, y0: u16, nrows: u16, channels: u8) callconv(.c) jof_h.ra8_err_t;

export var jpeg_stub_w: u16 = 0;
export var jpeg_stub_h: u16 = 0;
export var jpeg_stub_channels: u8 = 0;
export var jpeg_stub_rows_per_call: u16 = 0;
export var jpeg_stub_px: [4096]u8 = undefined;

pub export fn ra8_jpeg_sw_decode_stripes(pull: ?*const fn (ctx: ?*anyopaque, buf: [*c]u8, cap: usize, got: [*c]usize) callconv(.c) jof_h.ra8_err_t, pull_ctx: ?*anyopaque, window: [*c]u8, window_cap: u32, on_geom: JpegGeomFn, on_rows: JpegRowsFn, cb_ctx: ?*anyopaque) callconv(.c) jof_h.ra8_err_t {
    _ = .{ pull, pull_ctx, window, window_cap };
    var stripe: [*c]u8 = null;
    var cap: u32 = 0;
    const gerr = on_geom.?(cb_ctx, jpeg_stub_w, jpeg_stub_h, jpeg_stub_channels, 8, &stripe, &cap);
    if (gerr != jof_h.k_ra8_ok) {
        return gerr;
    }
    // The fake decoder paints the stripe from the configured pixels, exactly
    // as the real decoder would fill it before each on_rows stripe sink.
    const rowbytes: usize = @as(usize, jpeg_stub_w) * jpeg_stub_channels;
    const total: usize = rowbytes * jpeg_stub_h;
    @memcpy(stripe[0..total], jpeg_stub_px[0..total]);
    var y: u16 = 0;
    while (y < jpeg_stub_h) : (y += jpeg_stub_rows_per_call) {
        const nrows: u16 = @min(jpeg_stub_rows_per_call, jpeg_stub_h - y);
        const off: usize = @as(usize, y) * rowbytes;
        const rerr = on_rows.?(cb_ctx, @ptrCast(&stripe[off]), jpeg_stub_w, y, nrows, jpeg_stub_channels);
        if (rerr != jof_h.k_ra8_ok) {
            return rerr;
        }
    }
    return jof_h.k_ra8_ok;
}

pub export fn ra8_jpeg_sw_get_dimensions(jpeg_buf: [*c]const u8, jpeg_len: u32, out_w: [*c]u16, out_h: [*c]u16) callconv(.c) jof_h.ra8_err_t {
    _ = .{ jpeg_buf, jpeg_len };
    out_w.* = jpeg_stub_w;
    out_h.* = jpeg_stub_h;
    return jof_h.k_ra8_ok;
}

/// Shared container sniff/dims: the real ones live in the image-decoder C
/// library. The stub reports format_none (the JPEG SOI arm never reaches
/// them), so dispatch and probe exercise the not-supported arm.
pub export fn ra8_imgdec_sniff(bytes: [*c]const u8, byte_count: u32, out: [*c]c_int) callconv(.c) jof_h.ra8_err_t {
    _ = .{ bytes, byte_count };
    out.* = 0;
    return jof_h.k_ra8_ok;
}

pub export fn ra8_imgdec_dims(bytes: [*c]const u8, byte_count: u32, out: *anyopaque) callconv(.c) jof_h.ra8_err_t {
    _ = .{ bytes, byte_count, out };
    return jof_h.k_ra8_err_not_supported;
}
