//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for face metrics, UTF-8 fallback, and the pure Literata raster path.

const std = @import("std");
const text = @import("implementation");

test "face metrics retain IBM dimensions and use Literata proportional runs" {
    const sans = text.measure("Library", .sans);
    const serif = text.measure("Library", .serif);
    try std.testing.expectEqual(@as(u32, 56), sans.width);
    try std.testing.expectEqual(@as(u32, 16), sans.height);
    try std.testing.expect(serif.width != sans.width);
    try std.testing.expect(serif.height > sans.height);
}

test "Latin-1 and typographic glyphs decode and draw their own atlas pixels" {
    const plain = text.measure("Cafe", .serif);
    const accented = text.measure("Caf\xC3\xA9", .serif);
    const quotes = text.measure("\xE2\x80\x9Cbook\xE2\x80\x9D", .serif);
    try std.testing.expectEqual(plain.width, accented.width);
    try std.testing.expect(quotes.width > 0);

    var accent_pixels = PixelRecorder{};
    var replacement_pixels = PixelRecorder{};
    text.drawSerif("\xC3\xA9", 5, 7, 0, 0xFFFFFF, &accent_pixels, PixelRecorder.putPixel);
    text.drawSerif("?", 5, 7, 0, 0xFFFFFF, &replacement_pixels, PixelRecorder.putPixel);
    try std.testing.expect(accent_pixels.ink_pixels != replacement_pixels.ink_pixels);
}

test "draw and measure use the same advance for a mixed UTF-8 run" {
    const sample = "A\xC3\xA9\xE2\x80\x94Z";
    const extent = text.measure(sample, .serif);
    var pixels = PixelRecorder{};
    text.drawSerif(sample, 11, 4, 0, 0xFFFFFF, &pixels, PixelRecorder.putPixel);
    try std.testing.expectEqual(@as(i32, 11) + @as(i32, @intCast(extent.width)) - 1, pixels.rightmost);
    try std.testing.expect(pixels.ink_pixels > 0);
}

test "invalid UTF-8 and valid unsupported scalars use replacement metrics" {
    const replacement = text.measure("?", .serif);
    try std.testing.expectEqual(replacement.width, text.measure("\x80", .serif).width);
    try std.testing.expectEqual(replacement.width, text.measure("\xF0\x9F", .serif).width / 2);
    try std.testing.expectEqual(replacement.width, text.measure("\xE2\x98\x83", .serif).width);
}

const PixelRecorder = struct {
    ink_pixels: usize = 0,
    rightmost: i32 = -1,

    fn putPixel(user: ?*anyopaque, x: i32, _: i32, color: u32) callconv(.c) void {
        const recorder: *PixelRecorder = @ptrCast(@alignCast(user.?));
        if (color != 0xFFFFFF) recorder.ink_pixels += 1;
        recorder.rightmost = @max(recorder.rightmost, x);
    }
};
