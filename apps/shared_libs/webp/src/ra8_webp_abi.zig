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
const arena = @import("ra8_webp_arena_abi.zig");
const fixture = @import("webp_fixture.zig");
const imgdec = @import("ra8_webp_imgdec_abi.zig");

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_supported: u16 = 0x107;
const err_validation_failed: u16 = 0x501;
const err_range_check_failed: u16 = 0x503;
const err_null_ptr: u16 = 0x504;

/// Max accepted width/height, pixels/axis (k_ra8_webp_max_dim).
const max_dim: u32 = 8192;
/// Output bytes per pixel, RGBA8888 (k_ra8_webp_bytes_per_px).
const bytes_per_px: u32 = 4;

const webp_tag: [*:0]const u8 = "WEBP";

extern fn WebPGetInfo(data: [*]const u8, data_size: usize, width: ?*c_int, height: ?*c_int) c_int;
extern fn WebPDecodeRGBAInto(data: [*]const u8, data_size: usize, output_buffer: [*]u8, output_buffer_size: usize, output_stride: c_int) ?[*]u8;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn logError(message: [*:0]const u8) void {
    ra8_log_emit_error(webp_tag, message);
}

/// Read a WebP header's pixel dimensions without decoding the body.
export fn ra8_webp_get_info(data: ?[*]const u8, size: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) u16 {
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
export fn ra8_webp_decode_rgba(
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

const testing = std.testing;

test "getInfo reads lossless dimensions" {
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(ok, ra8_webp_get_info(&fixture.lossless, fixture.lossless.len, &w, &h));
    try testing.expectEqual(@as(u32, fixture.lossless_w), w);
    try testing.expectEqual(@as(u32, fixture.lossless_h), h);
}

test "getInfo reads lossy dimensions" {
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(ok, ra8_webp_get_info(&fixture.lossy, fixture.lossy.len, &w, &h));
    try testing.expectEqual(@as(u32, fixture.lossless_w), w);
    try testing.expectEqual(@as(u32, fixture.lossless_h), h);
}

test "getInfo rejects null and empty" {
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(err_null_ptr, ra8_webp_get_info(null, 1, &w, &h));
    try testing.expectEqual(err_null_ptr, ra8_webp_get_info(&fixture.lossless, fixture.lossless.len, null, &h));
    try testing.expectEqual(err_invalid_arg, ra8_webp_get_info(&fixture.lossless, 0, &w, &h));
}

test "getInfo rejects a non-WebP buffer" {
    var w: u32 = 0;
    var h: u32 = 0;
    const not_webp = [_]u8{ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B };
    try testing.expectEqual(err_validation_failed, ra8_webp_get_info(&not_webp, not_webp.len, &w, &h));
}

test "getInfo refuses a dimension past the facade cap" {
    var w: u32 = 0;
    var h: u32 = 0;
    const oversized = oversizedHeader(8193, 2);
    try testing.expectEqual(err_not_supported, ra8_webp_get_info(&oversized, oversized.len, &w, &h));
}

test "decode fills the buffer exactly on a lossless image" {
    const stride: usize = fixture.lossless_w * bytes_per_px;
    const cap: usize = fixture.lossless_h * stride;
    var pixels: [cap]u8 = undefined;
    @memset(&pixels, 0xAA);
    var w: u32 = 0;
    var h: u32 = 0;
    var scratch = arenaScratch();
    try testing.expectEqual(ok, ra8_webp_decode_rgba(
        &fixture.lossless,
        fixture.lossless.len,
        &scratch,
        &pixels,
        stride,
        cap,
        &w,
        &h,
    ));
    try testing.expectEqual(@as(u32, fixture.lossless_w), w);
    try testing.expectEqual(@as(u32, fixture.lossless_h), h);
    try testing.expectEqualSlices(u8, &fixture.lossless_expected_rgba, &pixels);
    try testing.expect(scratch.offset == 0 and scratch.live == 0);
    try testing.expect(scratch.high_water > 0);
}

test "decode reports out_w and out_h as optional" {
    const stride: usize = fixture.lossless_w * bytes_per_px;
    const cap: usize = fixture.lossless_h * stride;
    var pixels: [cap]u8 = undefined;
    var scratch = arenaScratch();
    try testing.expectEqual(ok, ra8_webp_decode_rgba(
        &fixture.lossy,
        fixture.lossy.len,
        &scratch,
        &pixels,
        stride,
        cap,
        null,
        null,
    ));
}

test "decode refuses a too-small stride or capacity" {
    const stride: usize = fixture.lossless_w * bytes_per_px;
    var pixels: [fixture.lossless_h * stride]u8 = undefined;
    var scratch = arenaScratch();
    try testing.expectEqual(err_range_check_failed, ra8_webp_decode_rgba(
        &fixture.lossless,
        fixture.lossless.len,
        &scratch,
        &pixels,
        stride - 1,
        pixels.len,
        null,
        null,
    ));
    try testing.expectEqual(err_range_check_failed, ra8_webp_decode_rgba(
        &fixture.lossless,
        fixture.lossless.len,
        &scratch,
        &pixels,
        stride,
        pixels.len - 1,
        null,
        null,
    ));
}

test "decode refuses null arguments" {
    var pixels: [64]u8 = undefined;
    var scratch = arenaScratch();
    try testing.expectEqual(err_null_ptr, ra8_webp_decode_rgba(null, 1, &scratch, &pixels, 16, 64, null, null));
    try testing.expectEqual(err_null_ptr, ra8_webp_decode_rgba(&fixture.lossless, fixture.lossless.len, null, &pixels, 16, 64, null, null));
    try testing.expectEqual(err_null_ptr, ra8_webp_decode_rgba(&fixture.lossless, fixture.lossless.len, &scratch, null, 16, 64, null, null));
}

test "decode refuses a corrupt body" {
    var corrupt: [fixture.lossless.len]u8 = fixture.lossless;
    corrupt[corrupt.len - 4] ^= 0xFF;
    const stride: usize = fixture.lossless_w * bytes_per_px;
    var pixels: [fixture.lossless_h * stride]u8 = undefined;
    var scratch = arenaScratch();
    try testing.expectEqual(err_validation_failed, ra8_webp_decode_rgba(
        &corrupt,
        corrupt.len,
        &scratch,
        &pixels,
        stride,
        pixels.len,
        null,
        null,
    ));
}

test "arena hooks refuse when unbound and drain when freed" {
    var storage: [256]u8 align(16) = undefined;
    var scratch = arena.Scratch{ .base = &storage, .cap = storage.len };
    arena.ra8_webp_arena_unbind();
    try testing.expect(arena.ra8_webp_arena_malloc(16) == null);

    arena.ra8_webp_arena_bind(&scratch);
    const block = arena.ra8_webp_arena_malloc(16) orelse return error.TestUnexpectedResult;
    try testing.expect(@intFromPtr(block) % 16 == 0);
    const zeroed = arena.ra8_webp_arena_calloc(4, 8) orelse return error.TestUnexpectedResult;
    const zero_bytes: [*]const u8 = @ptrCast(zeroed);
    for (0..32) |i| try testing.expectEqual(@as(u8, 0), zero_bytes[i]);
    arena.ra8_webp_arena_free(block);
    arena.ra8_webp_arena_free(zeroed);
    try testing.expectEqual(@as(usize, 0), scratch.live);
    try testing.expectEqual(@as(usize, 0), scratch.offset);
    arena.ra8_webp_arena_unbind();
}

test "arena bind resets a dirty arena and refuses an unusable descriptor" {
    var storage: [256]u8 align(16) = undefined;
    var dirty = arena.Scratch{ .base = &storage, .cap = storage.len };
    dirty.offset = 128;
    dirty.live = 1;
    arena.ra8_webp_arena_bind(&dirty);
    try testing.expectEqual(@as(usize, 0), dirty.offset);
    try testing.expectEqual(@as(usize, 0), dirty.live);
    arena.ra8_webp_arena_unbind();

    var unusable = arena.Scratch{ .base = null, .cap = 0 };
    arena.ra8_webp_arena_bind(&unusable);
    try testing.expect(arena.ra8_webp_arena_malloc(16) == null);
    arena.ra8_webp_arena_unbind();
}

/// Caller-shaped arena over a file-scope 64 KiB store (never a local's
/// address; the tests drain it after every decode).
var decode_storage: [64 * 1024]u8 align(16) = undefined;

fn arenaScratch() arena.Scratch {
    return .{ .base = &decode_storage, .cap = decode_storage.len };
}

/// A minimal RIFF/WEBP/VP8L container whose declared dimensions are `w` x `h`
/// (encoded as width-1 / height-1), enough to parse but never decode: the
/// facade's cap sits below libwebp's WEBP_MAX_DIMENSION, so get_info returns
/// not_supported instead of validation_failed for it.
fn oversizedHeader(w: u32, h: u32) [25]u8 {
    var out: [25]u8 = undefined;
    @memcpy(out[0..4], "RIFF");
    std.mem.writeInt(u32, out[4..8], 17, .little);
    @memcpy(out[8..12], "WEBP");
    @memcpy(out[12..16], "VP8L");
    std.mem.writeInt(u32, out[16..20], 5, .little);
    const dims: u32 = (w - 1) | ((h - 1) << 14);
    out[20] = 0x2F;
    std.mem.writeInt(u32, out[21..25], dims, .little);
    return out;
}

comptime {
    // Pull the imgdec adapter's exports into the library archive.
    _ = imgdec.ra8_webp_imgdec_bind;
}

test {
    _ = @import("ra8_webp_imgdec_abi.zig");
}
