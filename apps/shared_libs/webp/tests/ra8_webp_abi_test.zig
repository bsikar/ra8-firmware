//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `ra8_webp_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("ra8_webp_abi");

const arena = sut.arena;
const bytes_per_px = sut.bytes_per_px;
const err_invalid_arg = sut.err_invalid_arg;
const err_not_supported = sut.err_not_supported;
const err_null_ptr = sut.err_null_ptr;
const err_range_check_failed = sut.err_range_check_failed;
const err_validation_failed = sut.err_validation_failed;
const fixture = sut.fixture;
const ok = sut.ok;
const ra8_webp_decode_rgba = sut.ra8_webp_decode_rgba;
const ra8_webp_get_info = sut.ra8_webp_get_info;

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
