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

test "Literata v2 decodes every glyph to its declared pixel count" {
    try std.testing.expectEqual(@as(u8, 2), atlas.bytes[4]);
    try std.testing.expectEqual(@as(u8, 1), atlas.bytes[5]);
    for (0..atlas.glyph_count) |index| {
        const glyph = atlas.glyphAt(index);
        var decoder = atlas.decoder(glyph.offset);
        for (0..@as(usize, glyph.width) * glyph.height) |_| {
            try std.testing.expect(decoder.next() <= 3);
        }
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

const display_atlas = @import("../src/internal/font_display.zig");

test "reader atlases provide five native regular sizes for both faces" {
    const expected_heights = [_][2]u32{
        .{ 46, 36 },
        .{ 52, 41 },
        .{ 57, 45 },
        .{ 66, 52 },
        .{ 79, 62 },
    };
    for (1..6) |size| {
        for (0..2) |face_index| {
            const face: text.Face = if (face_index == 0) .serif else .sans;
            const measured = text.measureStyle("Hj", face, .regular, @intCast(size));
            try std.testing.expectEqual(expected_heights[size - 1][face_index], measured.height);
            var pixels = PixelRecorder{};
            text.drawStyle("Hj", 10, 20, face, .regular, @intCast(size), 0, 0xFFFFFF, &pixels, PixelRecorder.putPixel);
            try std.testing.expect(pixels.ink_pixels > 0);
            try std.testing.expect(pixels.topmost >= 20);
            try std.testing.expect(pixels.bottommost < 20 + @as(i32, @intCast(measured.height)));
        }
    }
}

test "default reader size, step three and body 38 use the same native atlas" {
    for (0..2) |face_index| {
        const face: text.Face = if (face_index == 0) .serif else .sans;
        const default = text.measureStyle("reader", face, .regular, 0);
        const step_three = text.measureStyle("reader", face, .regular, 3);
        const body_38 = text.measureStyle("reader", face, .regular, 6);
        try std.testing.expectEqual(step_three.width, default.width);
        try std.testing.expectEqual(body_38.width, step_three.width);
        try std.testing.expectEqual(step_three.height, default.height);
        try std.testing.expectEqual(body_38.height, step_three.height);
    }
}

test "all native reader atlases decode every glyph" {
    for (0..2) |face_index| {
        for ([_]u8{ 1, 2, 4, 5 }) |size| {
            const selected = display_atlas.get(@intCast(face_index), 0, size).?;
            try std.testing.expectEqual(@as(u8, 2), selected.bytes[4]);
            try std.testing.expectEqual(@as(u8, 1), selected.bytes[5]);
            for (0..selected.glyph_count) |glyph_index| {
                const glyph = selected.glyphAt(glyph_index);
                var decoder = selected.decoder(glyph.offset);
                for (0..@as(usize, glyph.width) * glyph.height) |_| {
                    try std.testing.expect(decoder.next() <= 3);
                }
            }
        }
    }
}

test "synthetic serif bold display glyphs retain fill and solid H stems" {
    const samples = "Hoen0123456789";
    for (6..9) |size| {
        const regular = display_atlas.get(1, 0, @intCast(size)).?;
        const bold = display_atlas.get(1, 1, @intCast(size)).?;
        for (samples) |character| {
            const codepoint: u32 = character;
            const regular_glyph = findDisplayGlyph(regular, codepoint) orelse continue;
            try std.testing.expect(findDisplayGlyph(bold, codepoint) != null);
            const bold_glyph = findDisplayGlyph(bold, codepoint).?;
            try std.testing.expect(
                displayInk(bold, bold_glyph) >= displayInk(regular, regular_glyph),
            );
        }

        if (findDisplayGlyph(bold, 'H')) |glyph| {
            const first_stem_y = glyph.height / 4;
            var first_x: ?usize = null;
            var last_x: ?usize = null;
            for (0..glyph.width) |x| {
                if (displayCoverage(bold, glyph, x, first_stem_y) != 0) {
                    first_x = first_x orelse x;
                    last_x = x;
                }
            }
            try std.testing.expect(first_x != null and last_x != null);
            const middle_y = glyph.height / 2;
            for (first_x.?..last_x.? + 1) |x| {
                try std.testing.expect(displayCoverage(bold, glyph, x, middle_y) != 0);
            }
        }
    }
}

fn findDisplayGlyph(atlas_value: display_atlas.Atlas, codepoint: u32) ?display_atlas.Glyph {
    for (0..atlas_value.glyph_count) |index| {
        const glyph = atlas_value.glyphAt(index);
        if (glyph.codepoint == codepoint) return glyph;
    }
    return null;
}

fn displayInk(atlas_value: display_atlas.Atlas, glyph: display_atlas.Glyph) u32 {
    var total: u32 = 0;
    var decoder = atlas_value.decoder(glyph.offset);
    for (0..@as(usize, glyph.width) * glyph.height) |_| {
        if (decoder.next() != 0) total += 1;
    }
    return total;
}

fn displayCoverage(
    atlas_value: display_atlas.Atlas,
    glyph: display_atlas.Glyph,
    x: usize,
    y: usize,
) u8 {
    var decoder = atlas_value.decoder(glyph.offset);
    const pixel_index = y * @as(usize, glyph.width) + x;
    for (0..pixel_index) |_| _ = decoder.next();
    return decoder.next();
}

test "every native display atlas decodes each glyph without allocation" {
    for (0..2) |face_index| {
        for (0..2) |weight_index| {
            for (6..9) |size| {
                const selected = display_atlas.get(@intCast(face_index), @intCast(weight_index), @intCast(size)).?;
                try std.testing.expectEqual(@as(u8, 2), selected.bytes[4]);
                try std.testing.expectEqual(@as(u8, 1), selected.bytes[5]);
                for (0..selected.glyph_count) |glyph_index| {
                    const glyph = selected.glyphAt(glyph_index);
                    var decoder = selected.decoder(glyph.offset);
                    for (0..@as(usize, glyph.width) * glyph.height) |_| {
                        try std.testing.expect(decoder.next() <= 3);
                    }
                }
            }
        }
    }
}

test "native UI sans atlases measure and draw at 26 and 30 pixels" {
    for ([_]u8{ 9, 10 }) |size| {
        for ([_]u8{ 0, 1 }) |weight| {
            const measured = text.measureStyle("Settings", .sans, if (weight == 1) .bold else .regular, size);
            try std.testing.expect(measured.width > 0);
            try std.testing.expect(measured.height >= @as(u32, if (size == 9) 26 else 30));
            var canvas = [_]u8{255} ** 4096;
            const sink = struct {
                fn pixel(user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void {
                    const pixels: *[4096]u8 = @ptrCast(@alignCast(user.?));
                    if (x >= 0 and x < 64 and y >= 0 and y < 64) pixels[@as(usize, @intCast(y * 64 + x))] = @truncate(color);
                }
            };
            text.drawStyle("Settings", 0, 0, .sans, if (weight == 1) .bold else .regular, size, 0, 255, &canvas, sink.pixel);
            try std.testing.expect(std.mem.indexOfNone(u8, &canvas, &.{255}) != null);
        }
    }
}
