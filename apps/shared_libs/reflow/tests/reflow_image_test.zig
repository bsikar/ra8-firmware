// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for zero-heap raster image decode and scaling (`src/reflow_image.zig`).

const std = @import("std");
const image = @import("reflow").image;

const testing = std.testing;
const Ra8ImgArena = image.Ra8ImgArena;

test "fit box fills the tighter width axis" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    image.fitBox(100, 50, 40, 100, &fw, &fh);
    try testing.expectEqual(@as(i32, 40), fw);
    try testing.expectEqual(@as(i32, 20), fh);
}

test "fit box fills the tighter height axis" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    image.fitBox(50, 100, 100, 40, &fw, &fh);
    try testing.expectEqual(@as(i32, 20), fw);
    try testing.expectEqual(@as(i32, 40), fh);
}

test "fit box clamps degenerate scales to the minimum edge" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    image.fitBox(1000, 2, 10, 10, &fw, &fh);
    try testing.expectEqual(@as(i32, 10), fw);
    try testing.expectEqual(@as(i32, 1), fh);
}

test "fit box uses i64 products on large dimensions" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    image.fitBox(60000, 60000, 30000, 30000, &fw, &fh);
    try testing.expectEqual(@as(i32, 30000), fw);
    try testing.expectEqual(@as(i32, 30000), fh);
}

const t_max_emit = 256;
var t_xy: [t_max_emit][2]i32 = undefined;
var t_color: [t_max_emit]u32 = undefined;
var t_count: usize = 0;

fn tEmit(x: i32, y: i32, color: u32) void {
    if (t_count < t_max_emit) {
        t_xy[t_count] = .{ x, y };
        t_color[t_count] = color;
    }
    t_count += 1;
}

fn tReset() void {
    t_count = 0;
}

test "blit maps 1:1 for an unscaled image and packs 0x00RRGGBB" {
    const px = [_]u8{
        0x10, 0x20, 0x30,
        0x40, 0x50, 0x60,
        0x70, 0x80, 0x90,
        0xa0, 0xb0, 0xc0,
    };
    tReset();
    image.blitScaled(&px, 2, 2, 3, 2, 2, 5, 7, tEmit);
    try testing.expectEqual(@as(usize, 4), t_count);
    try testing.expectEqual([2]i32{ 5, 7 }, t_xy[0]);
    try testing.expectEqual([2]i32{ 6, 7 }, t_xy[1]);
    try testing.expectEqual([2]i32{ 5, 8 }, t_xy[2]);
    try testing.expectEqual([2]i32{ 6, 8 }, t_xy[3]);
    try testing.expectEqual(@as(u32, 0x102030), t_color[0]);
    try testing.expectEqual(@as(u32, 0x405060), t_color[1]);
    try testing.expectEqual(@as(u32, 0x708090), t_color[2]);
    try testing.expectEqual(@as(u32, 0xa0b0c0), t_color[3]);
}

test "blit picks nearest-neighbour source columns when downscaling" {
    const px = [_]u8{
        1,  2,  3,
        4,  5,  6,
        7,  8,  9,
        10, 11, 12,
    };
    tReset();
    image.blitScaled(&px, 4, 1, 3, 2, 1, 0, 0, tEmit);
    try testing.expectEqual(@as(usize, 2), t_count);
    try testing.expectEqual(@as(u32, 0x010203), t_color[0]);
    try testing.expectEqual(@as(u32, 0x070809), t_color[1]);
}

test "blit strides by src_bpp for RGBA sources" {
    const px = [_]u8{
        0x11, 0x22, 0x33, 0xff,
        0x44, 0x55, 0x66, 0x80,
    };
    tReset();
    image.blitScaled(&px, 1, 2, 4, 1, 2, 0, 0, tEmit);
    try testing.expectEqual(@as(usize, 2), t_count);
    try testing.expectEqual(@as(u32, 0x112233), t_color[0]);
    try testing.expectEqual(@as(u32, 0x445566), t_color[1]);
}

test "webp classification passes the shared statuses through" {
    try testing.expectEqual(image.k_ra8_ok, image.webpClassify(image.k_ra8_ok));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.webpClassify(image.k_ra8_err_null_ptr));
    try testing.expectEqual(image.k_ra8_err_invalid_arg, image.webpClassify(image.k_ra8_err_invalid_arg));
    try testing.expectEqual(image.k_ra8_err_not_supported, image.webpClassify(image.k_ra8_err_not_supported));
    try testing.expectEqual(image.k_ra8_err_not_supported, image.webpClassify(image.k_ra8_err_no_mem));
    try testing.expectEqual(image.k_ra8_err_not_supported, image.webpClassify(0x105));
}

test "decode blit rejects a null arena and null bytes" {
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_blit(null, "x", 1, 0, 0, 1, 1, null, null));
    var scratch = Ra8ImgArena{ .base = null, .cap = 16, .offset = 0, .live = 0, .high_water = 0 };
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_blit(&scratch, null, 1, 0, 0, 1, 1, null, null));
}

test "decode blit rejects an empty input or an empty box" {
    var scratch = Ra8ImgArena{ .base = null, .cap = 16, .offset = 0, .live = 0, .high_water = 0 };
    try testing.expectEqual(image.k_ra8_err_invalid_arg, image.ra8_img_decode_blit(&scratch, "x", 0, 0, 0, 1, 1, null, null));
    try testing.expectEqual(image.k_ra8_err_invalid_arg, image.ra8_img_decode_blit(&scratch, "x", 1, 0, 0, 0, 1, null, null));
    try testing.expectEqual(image.k_ra8_err_invalid_arg, image.ra8_img_decode_blit(&scratch, "x", 1, 0, 0, 1, 0, null, null));
}

test "decode blit drains the arena on the decode-failure path" {
    var backing: [64]u8 = undefined;
    var scratch = Ra8ImgArena{
        .base = &backing,
        .cap = backing.len,
        .offset = 24,
        .live = 3,
        .high_water = 24,
    };
    try testing.expectEqual(image.k_ra8_err_not_supported, image.ra8_img_decode_blit(&scratch, "x", 1, 0, 0, 1, 1, null, null));
    try testing.expectEqual(@as(usize, 0), scratch.offset);
    try testing.expectEqual(@as(usize, 0), scratch.live);
    try testing.expectEqual(@as(usize, 24), scratch.high_water);
}

test "probe rejects the seam's not-supported stack" {
    var w: i32 = 0;
    var h: i32 = 0;
    try testing.expectEqual(image.k_ra8_err_not_supported, image.ra8_img_probe_size("x", 1, &w, &h));
}

test "probe rejects null arguments" {
    var w: i32 = 0;
    var h: i32 = 0;
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_probe_size(null, 1, &w, &h));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_probe_size("x", 1, null, &h));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_probe_size("x", 1, &w, null));
}

test "rgbaToGray8 matches BT.601 integer luma and ignores alpha" {
    const rgba = [_]u8{
        255, 0,   0,   0x11,
        0,   255, 0,   0x22,
        0,   0,   255, 0x33,
        255, 255, 255, 0x00,
    };
    var gray: [4]u8 = undefined;
    image.rgbaToGray8(&rgba, 4, &gray);
    try testing.expectEqual([4]u8{ 76, 149, 28, 255 }, gray);
}

test "decode gray8 rejects null arguments and invalid sizes" {
    var scratch = Ra8ImgArena{ .base = null, .cap = 16, .offset = 0, .live = 0, .high_water = 0 };
    var dst: [4]u8 = undefined;
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_gray8(null, "x", 1, &dst, dst.len, &w, &h));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_gray8(&scratch, null, 1, &dst, dst.len, &w, &h));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_gray8(&scratch, "x", 1, null, dst.len, &w, &h));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_gray8(&scratch, "x", 1, &dst, dst.len, null, &h));
    try testing.expectEqual(image.k_ra8_err_null_ptr, image.ra8_img_decode_gray8(&scratch, "x", 1, &dst, dst.len, &w, null));
    try testing.expectEqual(image.k_ra8_err_invalid_arg, image.ra8_img_decode_gray8(&scratch, "x", 0, &dst, dst.len, &w, &h));
    try testing.expectEqual(image.k_ra8_err_invalid_arg, image.ra8_img_decode_gray8(&scratch, "x", 1, &dst, 0, &w, &h));
}

test "decode gray8 drains the arena on the decode-failure path" {
    var backing: [64]u8 = undefined;
    var scratch = Ra8ImgArena{
        .base = &backing,
        .cap = backing.len,
        .offset = 16,
        .live = 2,
        .high_water = 16,
    };
    var dst: [4]u8 = undefined;
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(image.k_ra8_err_not_supported, image.ra8_img_decode_gray8(&scratch, "x", 1, &dst, dst.len, &w, &h));
    try testing.expectEqual(@as(usize, 0), scratch.offset);
    try testing.expectEqual(@as(usize, 0), scratch.live);
    try testing.expectEqual(@as(usize, 16), scratch.high_water);
}
