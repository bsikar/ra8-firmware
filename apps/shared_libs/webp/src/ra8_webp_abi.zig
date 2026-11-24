//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zero-heap WebP (VP8 / VP8L) decode facade over the vendored libwebp,
//! ported from src/ra8_webp.c (RA8FW-1209, slice 1). This module is the only
//! first-party Zig that names libwebp's decode entry points; every consumer
//! reaches WebP through the ra8_err_t facade here, never through libwebp
//! directly. C source status: ra8_webp.c and ra8_webp_arena.c stay on dev
//! until the package flip slice (CMake still globs them for consumers).

const std = @import("std");
pub const arena = @import("ra8_webp_arena_abi.zig");
pub const fixture = @import("webp_fixture.zig");
const imgdec = @import("ra8_webp_imgdec_abi.zig");

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_not_supported: u16 = 0x107;
pub const err_validation_failed: u16 = 0x501;
pub const err_range_check_failed: u16 = 0x503;
pub const err_null_ptr: u16 = 0x504;

/// Max accepted width/height, pixels/axis (k_ra8_webp_max_dim).
const max_dim: u32 = 8192;
/// Output bytes per pixel, RGBA8888 (k_ra8_webp_bytes_per_px).
pub const bytes_per_px: u32 = 4;

const webp_tag: [*:0]const u8 = "WEBP";

extern fn WebPGetInfo(data: [*]const u8, data_size: usize, width: ?*c_int, height: ?*c_int) c_int;
extern fn WebPDecodeRGBAInto(data: [*]const u8, data_size: usize, output_buffer: [*]u8, output_buffer_size: usize, output_stride: c_int) ?[*]u8;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn logError(message: [*:0]const u8) void {
    ra8_log_emit_error(webp_tag, message);
}

/// Read a WebP header's pixel dimensions without decoding the body.
pub export fn ra8_webp_get_info(data: ?[*]const u8, size: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) u16 {
    if (data == null or out_w == null or out_h == null) {
        logError("null pointer");
        return err_null_ptr;
    }
    if (size == 0) {
        logError("empty WebP buffer");
        return err_invalid_arg;
    }

    // WebPGetInfo returns 0 for any invalid container or a zero/oversized
    // dimension, so on success both are already in [1, WEBP_MAX_DIMENSION];
    // no separate non-positive check is needed (it would be dead code).
    var w: c_int = 0;
    var h: c_int = 0;
    if (WebPGetInfo(data.?, size, &w, &h) == 0) {
        return err_validation_failed;
    }
    if (@as(u32, @intCast(w)) > max_dim or @as(u32, @intCast(h)) > max_dim) {
        logError("WebP dimension exceeds cap");
        return err_not_supported;
    }

    out_w.?.* = @intCast(w);
    out_h.?.* = @intCast(h);
    return ok;
}

/// Validate the caller's output geometry against the decoded dimensions.
fn checkOutput(data: [*]const u8, size: usize, out_stride: usize, out_capacity: usize, w: *u32, h: *u32) u16 {
    const info = ra8_webp_get_info(data, size, w, h);
    if (info != ok) return info;

    const min_stride: usize = @as(usize, w.*) * bytes_per_px;
    if (out_stride < min_stride or @as(usize, h.*) * out_stride > out_capacity) {
        logError("output buffer too small");
        return err_range_check_failed;
    }
    if (out_stride > std.math.maxInt(c_int)) {
        logError("output stride exceeds INT_MAX");
        return err_range_check_failed;
    }
    return ok;
}

/// Decode a WebP into a caller-owned RGBA8888 buffer, heap-free.
pub export fn ra8_webp_decode_rgba(
    data: ?[*]const u8,
    size: usize,
    arena_ptr: ?*arena.Scratch,
    out_rgba: ?[*]u8,
    out_stride: usize,
    out_capacity: usize,
    out_w: ?*u32,
    out_h: ?*u32,
) callconv(.c) u16 {
    if (data == null or arena_ptr == null or out_rgba == null) {
        logError("null pointer");
        return err_null_ptr;
    }

    var w: u32 = 0;
    var h: u32 = 0;
    const validated = checkOutput(data.?, size, out_stride, out_capacity, &w, &h);
    if (validated != ok) return validated;

    arena.ra8_webp_arena_bind(arena_ptr);
    const result = WebPDecodeRGBAInto(data.?, size, out_rgba.?, out_capacity, @intCast(out_stride));
    arena.ra8_webp_arena_unbind();
    if (result == null) {
        return err_validation_failed;
    }

    if (out_w) |p| p.* = w;
    if (out_h) |p| p.* = h;
    return ok;
}

comptime {
    // Pull the imgdec adapter's exports into the library archive.
    _ = imgdec.ra8_webp_imgdec_bind;
}
