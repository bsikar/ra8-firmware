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
    topmost: i32 = std.math.maxInt(i32),
    bottommost: i32 = std.math.minInt(i32),

    fn putPixel(user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void {
        const recorder: *PixelRecorder = @ptrCast(@alignCast(user.?));
        if (color != 0xFFFFFF) {
            recorder.ink_pixels += 1;
            recorder.topmost = @min(recorder.topmost, y);
            recorder.bottommost = @max(recorder.bottommost, y);
        }
        recorder.rightmost = @max(recorder.rightmost, x);
    }
};

test "bold face expands visible bounds and adds ink for sans and serif" {
    const regular_serif = text.measure("Head", .serif);
    const bold_serif = text.measureWeight("Head", .serif, .bold);
    try std.testing.expectEqual(regular_serif.width + 1, bold_serif.width);
    try std.testing.expectEqual(regular_serif.height, bold_serif.height);

    var regular_pixels = PixelRecorder{};
    var bold_pixels = PixelRecorder{};
    text.drawSerif("Head", 3, 4, 0, 0xFFFFFF, &regular_pixels, PixelRecorder.putPixel);
    text.drawSerifWeight("Head", 3, 4, 0, 0xFFFFFF, .bold, &bold_pixels, PixelRecorder.putPixel);
    try std.testing.expect(bold_pixels.ink_pixels > regular_pixels.ink_pixels);
    try std.testing.expectEqual(3 + @as(i32, @intCast(bold_serif.width)) - 1, bold_pixels.rightmost);

    const regular_sans = text.measure("Head", .sans);
    const bold_sans = text.measureWeight("Head", .sans, .bold);
    try std.testing.expectEqual(regular_sans.width + 1, bold_sans.width);
}

const atlas = @import("../src/internal/font_literata.zig");

test "Literata glyph data begins at a packed coverage byte boundary" {
    for (0..atlas.glyph_count) |index| {
        const glyph = atlas.glyphAt(index);
        try std.testing.expectEqual(@as(u32, 0), glyph.offset % 4);
    }
}

test "native display atlases measure and draw sans and serif at all display sizes" {
    const cases = [_]struct { face: text.Face, weight: text.Weight, size: u8, minimum_height: u32 }{
        .{ .face = .serif, .weight = .regular, .size = 6, .minimum_height = 38 },
        .{ .face = .sans, .weight = .bold, .size = 6, .minimum_height = 38 },
        .{ .face = .serif, .weight = .bold, .size = 7, .minimum_height = 68 },
        .{ .face = .sans, .weight = .regular, .size = 7, .minimum_height = 68 },
        .{ .face = .serif, .weight = .regular, .size = 8, .minimum_height = 120 },
        .{ .face = .sans, .weight = .bold, .size = 8, .minimum_height = 120 },
    };
    for (cases) |case| {
        const measured = text.measureStyle("09:41", case.face, case.weight, case.size);
        try std.testing.expect(measured.width > 0);
        try std.testing.expect(measured.height >= case.minimum_height);
        var pixels = PixelRecorder{};
        text.drawStyle("09:41", 10, 12, case.face, case.weight, case.size, 0, 0xFFFFFF, &pixels, PixelRecorder.putPixel);
        try std.testing.expect(pixels.ink_pixels > 0);
    }
}

test "serif and every native display atlas keep ink inside the line box" {
    const line_y: i32 = 200;
    var reading_pixels = PixelRecorder{};
    text.drawSerif("Hj", 10, line_y, 0, 0xFFFFFF, &reading_pixels, PixelRecorder.putPixel);
    const reading_height: i32 = @intCast(text.measure("Hj", .serif).height);
    try std.testing.expect(reading_pixels.ink_pixels > 0);
    try std.testing.expect(reading_pixels.topmost >= line_y);
    try std.testing.expect(reading_pixels.bottommost < line_y + reading_height);

    for (0..2) |face_index| {
        for (0..2) |weight_index| {
            for (6..9) |size| {
                var pixels = PixelRecorder{};
                const face: text.Face = if (face_index == 0) .sans else .serif;
                const weight: text.Weight = if (weight_index == 0) .regular else .bold;
                const sample: [*:0]const u8 = if (size == 8) "09:41" else "Hj";
                text.drawStyle(sample, 10, line_y, face, weight, @intCast(size), 0, 0xFFFFFF, &pixels, PixelRecorder.putPixel);
                const height: i32 = @intCast(text.measureStyle(sample, face, weight, @intCast(size)).height);
                try std.testing.expect(pixels.ink_pixels > 0);
                try std.testing.expect(pixels.topmost >= line_y);
                try std.testing.expect(pixels.bottommost < line_y + height);
            }
        }
    }
    try std.testing.expectEqual(@as(usize, 12), 2 * 2 * 3);
}
