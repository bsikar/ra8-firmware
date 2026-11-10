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
