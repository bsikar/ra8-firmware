//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the blue-noise dither core: the toroidal mask lookup, the
//! flat quantise rule, the tone-curve fallback, the gray4 nibble packer and
//! the level-to-color expansion.

const std = @import("std");
const dither = @import("dither");

test "dither constants match the C enums" {
    try std.testing.expectEqual(@as(u8, 16), dither.dither.levels);
    try std.testing.expectEqual(@as(u8, 17), dither.dither.step);
    try std.testing.expectEqual(@as(u8, 15), dither.dither.max_level);
    try std.testing.expectEqual(@as(u8, 2), dither.dither.ppb);
    try std.testing.expectEqual(@as(u32, 64), dither.dither.mask_dim);
    try std.testing.expectEqual(@as(u32, 63), dither.dither.mask_index_mask);
    try std.testing.expectEqual(@as(u16, 256), dither.dither.byte_levels);
    try std.testing.expectEqual(@as(u16, 4096), dither.dither.mask_len);
}

test "the palette step spans the full byte range" {
    try std.testing.expectEqual(
        @as(u32, 255),
        @as(u32, dither.dither.step) * @as(u32, dither.dither.max_level),
    );
}

test "mask is the committed 64x64 texture" {
    try std.testing.expectEqual(@as(usize, 4096), dither.mask.len);
}

test "mask is a permutation of the byte range, 16 of each value" {
    var histogram = [_]u16{0} ** 256;
    for (dither.mask) |value| histogram[value] += 1;
    for (histogram) |count| try std.testing.expectEqual(@as(u16, 16), count);
}

test "mask index is row-major inside the tile" {
    try std.testing.expectEqual(@as(u32, 0), dither.maskIndex(0, 0));
    try std.testing.expectEqual(@as(u32, 1), dither.maskIndex(1, 0));
    try std.testing.expectEqual(@as(u32, 64), dither.maskIndex(0, 1));
    try std.testing.expectEqual(@as(u32, 4095), dither.maskIndex(63, 63));
}

test "mask index wraps toroidally, negatives included" {
    try std.testing.expectEqual(dither.maskIndex(0, 0), dither.maskIndex(64, 64));
    try std.testing.expectEqual(dither.maskIndex(5, 7), dither.maskIndex(69, 71));
    try std.testing.expectEqual(dither.maskIndex(63, 63), dither.maskIndex(-1, -1));
    try std.testing.expectEqual(dither.maskIndex(62, 0), dither.maskIndex(-2, 128));
}

test "quantise pins the palette anchors exactly" {
    var level: u8 = 0;
    while (level <= dither.dither.max_level) : (level += 1) {
        const gray = level * dither.dither.step;
        try std.testing.expectEqual(level, dither.quantise(gray, 0));
        try std.testing.expectEqual(level, dither.quantise(gray, 255));
    }
}

test "quantise never leaves the 4-bit range" {
    var gray: u16 = 0;
    while (gray < 256) : (gray += 1) {
        var thr: u16 = 0;
        while (thr < 256) : (thr += 17) {
            const level = dither.quantise(@intCast(gray), @intCast(thr));
            try std.testing.expect(level <= dither.dither.max_level);
        }
    }
}

test "quantise rounds up exactly when the threshold falls inside the remainder" {
    // gray 8 sits 8/17 above level 0, so thresholds below 8*256/17 = 120.4 round up.
    try std.testing.expectEqual(@as(u8, 1), dither.quantise(8, 0));
    try std.testing.expectEqual(@as(u8, 1), dither.quantise(8, 120));
    try std.testing.expectEqual(@as(u8, 0), dither.quantise(8, 121));
    try std.testing.expectEqual(@as(u8, 0), dither.quantise(8, 255));
}

test "quantise is monotone in gray for a fixed threshold" {
    var thr: u16 = 0;
    while (thr < 256) : (thr += 37) {
        var previous: u8 = 0;
        var gray: u16 = 0;
        while (gray < 256) : (gray += 1) {
            const level = dither.quantise(@intCast(gray), @intCast(thr));
            try std.testing.expect(level >= previous);
            previous = level;
        }
    }
}

test "a null curve falls back byte-for-byte to the flat rule" {
    var gray: u16 = 0;
    while (gray < 256) : (gray += 1) {
        const thr = dither.thresholdAt(@intCast(gray & 63), 3);
        try std.testing.expectEqual(
            dither.quantise(@intCast(gray), thr),
            dither.quantiseAny(null, @intCast(gray), thr),
        );
    }
}

test "the nominal curve reproduces the flat rule" {
    const tone = dither.tone;
    var map: tone.Map = undefined;
    tone.prepareMap(&tone.nominal, &map);
    var gray: u16 = 0;
    while (gray < 256) : (gray += 1) {
        var thr: u16 = 0;
        while (thr < 256) : (thr += 29) {
            try std.testing.expectEqual(
                dither.quantise(@intCast(gray), @intCast(thr)),
                dither.quantiseAny(&map, @intCast(gray), @intCast(thr)),
            );
        }
    }
}

test "level expands to a gray with the nibble replicated" {
    try std.testing.expectEqual(@as(u32, 0x000000), dither.levelToColor(0));
    try std.testing.expectEqual(@as(u32, 0xFFFFFF), dither.levelToColor(15));
    try std.testing.expectEqual(@as(u32, 0x888888), dither.levelToColor(8));
    try std.testing.expectEqual(@as(u32, 0x111111), dither.levelToColor(1));
}

test "packed byte count rounds the odd pixel up" {
    try std.testing.expectEqual(@as(u32, 1), dither.packedBytes(1, 1));
    try std.testing.expectEqual(@as(u32, 1), dither.packedBytes(2, 1));
    try std.testing.expectEqual(@as(u32, 2), dither.packedBytes(3, 1));
    try std.testing.expectEqual(@as(u32, 8), dither.packedBytes(4, 4));
}

test "packTile writes the high nibble first" {
    const src = [_]u8{ 255, 0, 0, 255 };
    var out = [_]u8{ 0xAA, 0xAA };
    dither.packTile(null, &src, 2, 2, 0, 0, &out);
    try std.testing.expectEqual(@as(u8, 0xF0), out[0]);
    try std.testing.expectEqual(@as(u8, 0x0F), out[1]);
}

test "packTile agrees with per-pixel quantise at the tile origin" {
    var src: [64]u8 = undefined;
    for (&src, 0..) |*pixel, i| pixel.* = @intCast((i * 7) & 0xFF);
    var out: [32]u8 = undefined;
    dither.packTile(null, &src, 8, 8, 13, 29, &out);

    for (src, 0..) |pixel, i| {
        const x: i32 = 13 + @as(i32, @intCast(i % 8));
        const y: i32 = 29 + @as(i32, @intCast(i / 8));
        const expected = dither.quantise(pixel, dither.thresholdAt(x, y));
        const byte = out[i / 2];
        const actual = if ((i & 1) == 0) byte >> 4 else byte & 0x0F;
        try std.testing.expectEqual(expected, actual);
    }
}

test "an odd-length tile leaves the trailing low nibble clear" {
    const src = [_]u8{ 255, 255, 255 };
    var out = [_]u8{ 0, 0 };
    dither.packTile(null, &src, 3, 1, 0, 0, &out);
    try std.testing.expectEqual(@as(u8, 0xFF), out[0]);
    try std.testing.expectEqual(@as(u8, 0xF0), out[1]);
}

test "tile phase is continuous across abutting tiles" {
    const flat = [_]u8{128} ** 16;
    var left: [8]u8 = undefined;
    var right: [8]u8 = undefined;
    dither.packTile(null, flat[0..16], 4, 4, 60, 0, &left);
    dither.packTile(null, flat[0..16], 4, 4, 124, 0, &right);
    try std.testing.expectEqualSlices(u8, &left, &right);
}
