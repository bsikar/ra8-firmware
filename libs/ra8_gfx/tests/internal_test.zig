//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure core of the `ra8_gfx` rasteriser: colour
//! packing round trips, format classification, the clip algebra, the pixel
//! codecs and both walk steppers.

const std = @import("std");
const impl = @import("implementation");

test "state layout matches the C ra8_gfx_state_t" {
    const ptr = @sizeOf(usize);
    const clip_at = std.mem.alignForward(usize, ptr + 7, 4);
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(impl.State, "fb"));
    try std.testing.expectEqual(ptr, @offsetOf(impl.State, "width"));
    try std.testing.expectEqual(ptr + 4, @offsetOf(impl.State, "format"));
    try std.testing.expectEqual(ptr + 6, @offsetOf(impl.State, "initialized"));
    try std.testing.expectEqual(clip_at + 12, @offsetOf(impl.State, "clip_y1"));
    try std.testing.expectEqual(clip_at + 16, @sizeOf(impl.State));
}

test "default state is an unbound RGB565 binding" {
    const s: impl.State = .{ .format = impl.format.rgb565 };
    try std.testing.expect(s.fb == null);
    try std.testing.expect(!s.initialized);
    try std.testing.expectEqual(@as(u8, 2), s.format);
    try std.testing.expectEqual(@as(u8, 0), s.bpp);
}

test "colour channels split 0xAARRGGBB" {
    const c: u32 = 0x8412_3456;
    try std.testing.expectEqual(@as(u8, 0x84), impl.colorA(c));
    try std.testing.expectEqual(@as(u8, 0x12), impl.colorR(c));
    try std.testing.expectEqual(@as(u8, 0x34), impl.colorG(c));
    try std.testing.expectEqual(@as(u8, 0x56), impl.colorB(c));
}

test "pack565 pins black, white and a mid colour" {
    try std.testing.expectEqual(@as(u16, 0x0000), impl.pack565(0xFF00_0000));
    try std.testing.expectEqual(@as(u16, 0xFFFF), impl.pack565(0x00FF_FFFF));
    // 0x18 >> 3 = 3, 0x40 >> 2 = 16, 0x80 >> 3 = 16.
    try std.testing.expectEqual(
        @as(u16, (3 << 11) | (16 << 5) | 16),
        impl.pack565(0x0018_4080),
    );
}

test "unpack565 recovers the quantised channels" {
    const word = impl.pack565(0x00FF_FFFF);
    try std.testing.expectEqual(@as(u32, 0x00F8_FCF8), impl.unpack565(word));
    try std.testing.expectEqual(@as(u32, 0), impl.unpack565(0));
}

test "pack565 is idempotent through unpack565" {
    var color: u32 = 0;
    while (color <= 0xFF) : (color += 17) {
        const gray = impl.grayToColor(color);
        const once = impl.pack565(gray);
        const twice = impl.pack565(impl.unpack565(once));
        try std.testing.expectEqual(once, twice);
    }
}

test "bpp of a format is its enumerator" {
    try std.testing.expectEqual(@as(u8, 2), impl.bppOf(impl.format.rgb565));
    try std.testing.expectEqual(@as(u8, 3), impl.bppOf(impl.format.rgb888));
    try std.testing.expectEqual(@as(u8, 4), impl.bppOf(impl.format.argb8888));
}

test "formatOk accepts exactly the three addressable formats" {
    try std.testing.expect(impl.formatOk(2));
    try std.testing.expect(impl.formatOk(3));
    try std.testing.expect(impl.formatOk(4));
    try std.testing.expect(!impl.formatOk(0));
    try std.testing.expect(!impl.formatOk(1));
    try std.testing.expect(!impl.formatOk(5));
    try std.testing.expect(!impl.formatOk(255));
}

test "grayToColor replicates the level across three channels" {
    try std.testing.expectEqual(@as(u32, 0x0080_8080), impl.grayToColor(0x80));
    try std.testing.expectEqual(@as(u32, 0), impl.grayToColor(0));
}

test "initStatus judges framebuffer, then width, then height, then format" {
    try std.testing.expectEqual(impl.err.null_ptr, impl.initStatus(false, 0, 0, 0));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.initStatus(true, 0, 8, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.initStatus(true, 4097, 8, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.initStatus(true, 8, 0, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.initStatus(true, 8, 4097, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.initStatus(true, 8, 8, 7));
    try std.testing.expectEqual(impl.err.ok, impl.initStatus(true, 1, 1, 2));
    try std.testing.expectEqual(impl.err.ok, impl.initStatus(true, 4096, 4096, 4));
}

test "clampClip clamps into the surface" {
    const box = impl.clampClip(2, 3, 4, 5, 10, 10);
    try std.testing.expectEqual(@as(i32, 2), box.x0);
    try std.testing.expectEqual(@as(i32, 3), box.y0);
    try std.testing.expectEqual(@as(i32, 6), box.x1);
    try std.testing.expectEqual(@as(i32, 8), box.y1);
}

test "clampClip pulls negative origins to zero and clips the far edge" {
    const box = impl.clampClip(-5, -5, 100, 100, 10, 20);
    try std.testing.expectEqual(@as(i32, 0), box.x0);
    try std.testing.expectEqual(@as(i32, 0), box.y0);
    try std.testing.expectEqual(@as(i32, 10), box.x1);
    try std.testing.expectEqual(@as(i32, 20), box.y1);
}

test "clampClip collapses an inverted or off-surface box onto its origin" {
    const inverted = impl.clampClip(8, 8, -4, -4, 10, 10);
    try std.testing.expectEqual(inverted.x0, inverted.x1);
    try std.testing.expectEqual(inverted.y0, inverted.y1);
    try std.testing.expect(inverted.isEmpty());

    const beyond = impl.clampClip(50, 60, 4, 4, 10, 20);
    try std.testing.expectEqual(@as(i32, 10), beyond.x0);
    try std.testing.expectEqual(@as(i32, 20), beyond.y0);
    try std.testing.expectEqual(@as(i32, 10), beyond.x1);
    try std.testing.expectEqual(@as(i32, 20), beyond.y1);
}

test "clampClip widens the far edge in 64-bit so INT32_MAX cannot wrap" {
    const box = impl.clampClip(1, 1, std.math.maxInt(i32), std.math.maxInt(i32), 16, 16);
    try std.testing.expectEqual(@as(i32, 16), box.x1);
    try std.testing.expectEqual(@as(i32, 16), box.y1);
}

test "fillSpan intersects a rectangle with the clip box" {
    const c = impl.Box{ .x0 = 2, .y0 = 2, .x1 = 8, .y1 = 8 };
    const box = impl.fillSpan(0, 0, 5, 5, c).?;
    try std.testing.expectEqual(@as(i32, 2), box.x0);
    try std.testing.expectEqual(@as(i32, 2), box.y0);
    try std.testing.expectEqual(@as(i32, 5), box.x1);
    try std.testing.expectEqual(@as(i32, 5), box.y1);
}

test "fillSpan refuses a rectangle wholly outside the clip box" {
    const c = impl.Box{ .x0 = 2, .y0 = 2, .x1 = 8, .y1 = 8 };
    try std.testing.expect(impl.fillSpan(20, 0, 5, 5, c) == null);
    try std.testing.expect(impl.fillSpan(0, 20, 5, 5, c) == null);
    try std.testing.expect(impl.fillSpan(2, 2, 0, 4, c) == null);
    try std.testing.expect(impl.fillSpan(2, 2, 4, 0, c) == null);
}

test "fillSpan keeps a degenerate width from wrapping the far edge" {
    const c = impl.Box{ .x0 = 0, .y0 = 0, .x1 = 16, .y1 = 16 };
    const box = impl.fillSpan(1, 1, std.math.maxInt(i32), 4, c).?;
    try std.testing.expectEqual(@as(i32, 16), box.x1);
    try std.testing.expectEqual(@as(i32, 5), box.y1);
}

test "blitWindow intersects the destination with the clip box" {
    const c = impl.Box{ .x0 = 0, .y0 = 0, .x1 = 10, .y1 = 10 };
    const box = impl.blitWindow(8, 8, 5, 5, c).?;
    try std.testing.expectEqual(@as(i32, 8), box.x0);
    try std.testing.expectEqual(@as(i32, 8), box.y0);
    try std.testing.expectEqual(@as(i32, 10), box.x1);
    try std.testing.expectEqual(@as(i32, 10), box.y1);
}

test "blitWindow is empty when the destination misses the clip box" {
    const c = impl.Box{ .x0 = 4, .y0 = 4, .x1 = 6, .y1 = 6 };
    try std.testing.expect(impl.blitWindow(10, 10, 2, 2, c) == null);
    try std.testing.expect(impl.blitWindow(0, 0, 2, 2, c) == null);
}

test "pixelInBounds is judged against the surface, not the clip" {
    try std.testing.expect(impl.pixelInBounds(0, 0, 4, 4));
    try std.testing.expect(impl.pixelInBounds(3, 3, 4, 4));
    try std.testing.expect(!impl.pixelInBounds(-1, 0, 4, 4));
    try std.testing.expect(!impl.pixelInBounds(0, -1, 4, 4));
    try std.testing.expect(!impl.pixelInBounds(4, 0, 4, 4));
    try std.testing.expect(!impl.pixelInBounds(0, 4, 4, 4));
}

test "plotInClip rejects on either edge of either axis" {
    const c = impl.Box{ .x0 = 2, .y0 = 2, .x1 = 6, .y1 = 6 };
    try std.testing.expect(impl.plotInClip(2, 2, c));
    try std.testing.expect(impl.plotInClip(5, 5, c));
    try std.testing.expect(!impl.plotInClip(1, 3, c));
    try std.testing.expect(!impl.plotInClip(3, 1, c));
    try std.testing.expect(!impl.plotInClip(6, 3, c));
    try std.testing.expect(!impl.plotInClip(3, 6, c));
}

test "fillIsFlat picks the memset path only for identical halves" {
    try std.testing.expect(impl.fillIsFlat(0, 0));
    try std.testing.expect(impl.fillIsFlat(0xFF, 0xFF));
    try std.testing.expect(!impl.fillIsFlat(0x1F, 0x00));
}

test "pixelOffset walks rows by stride and columns by depth" {
    try std.testing.expectEqual(@as(usize, 0), impl.pixelOffset(32, 2, 0, 0));
    try std.testing.expectEqual(@as(usize, 6), impl.pixelOffset(32, 2, 3, 0));
    try std.testing.expectEqual(@as(usize, 34), impl.pixelOffset(32, 2, 1, 1));
    try std.testing.expectEqual(@as(usize, 99), impl.pixelOffset(48, 3, 1, 2));
}

test "putPixel writes RGB565 little-endian" {
    var fb = [_]u8{0} ** 16;
    impl.putPixel(&fb, 8, impl.format.rgb565, 1, 1, 0x00FF_FFFF);
    try std.testing.expectEqual(@as(u8, 0xFF), fb[10]);
    try std.testing.expectEqual(@as(u8, 0xFF), fb[11]);
    try std.testing.expectEqual(@as(u8, 0), fb[0]);
}

test "putPixel writes RGB888 in R, G, B order" {
    var fb = [_]u8{0} ** 32;
    impl.putPixel(&fb, 12, impl.format.rgb888, 2, 1, 0x0011_2233);
    try std.testing.expectEqual(@as(u8, 0x11), fb[18]);
    try std.testing.expectEqual(@as(u8, 0x22), fb[19]);
    try std.testing.expectEqual(@as(u8, 0x33), fb[20]);
}

test "putPixel writes ARGB8888 in B, G, R, A order" {
    var fb = [_]u8{0} ** 32;
    impl.putPixel(&fb, 16, impl.format.argb8888, 1, 1, 0x8811_2233);
    try std.testing.expectEqual(@as(u8, 0x33), fb[20]);
    try std.testing.expectEqual(@as(u8, 0x22), fb[21]);
    try std.testing.expectEqual(@as(u8, 0x11), fb[22]);
    try std.testing.expectEqual(@as(u8, 0x88), fb[23]);
}

test "putPixel writes nothing for an unrecognised format" {
    var fb = [_]u8{0xAA} ** 16;
    impl.putPixel(&fb, 8, 9, 0, 0, 0x00FF_FFFF);
    for (fb) |byte| try std.testing.expectEqual(@as(u8, 0xAA), byte);
}

test "getPixel round trips every addressable format through putPixel" {
    const cases = [_]struct { fmt: u8, color: u32, want: u32 }{
        .{ .fmt = impl.format.rgb565, .color = 0x00FF_FFFF, .want = 0x00F8_FCF8 },
        .{ .fmt = impl.format.rgb888, .color = 0x0011_2233, .want = 0x0011_2233 },
        .{ .fmt = impl.format.argb8888, .color = 0x8811_2233, .want = 0x8811_2233 },
    };
    for (cases) |c| {
        var buf = [_]u8{0} ** 64;
        const stride = 4 * @as(usize, impl.bppOf(c.fmt));
        impl.putPixel(&buf, stride, c.fmt, 2, 1, c.color);
        try std.testing.expectEqual(c.want, impl.getPixel(&buf, stride, c.fmt, 2, 1));
    }
}

test "getPixel reads zero for an unrecognised format" {
    const buf = [_]u8{0xFF} ** 16;
    try std.testing.expectEqual(@as(u32, 0), impl.getPixel(&buf, 8, 0, 0, 0));
}

test "lineStart seeds absolute spans and per-axis direction" {
    const walk = impl.lineStart(2, 2, 8, 5);
    try std.testing.expectEqual(@as(i32, 6), walk.dx);
    try std.testing.expectEqual(@as(i32, -3), walk.dy);
    try std.testing.expectEqual(@as(i32, 1), walk.sx);
    try std.testing.expectEqual(@as(i32, 1), walk.sy);
    try std.testing.expectEqual(@as(i32, 3), walk.e);

    const back = impl.lineStart(8, 5, 2, 2);
    try std.testing.expectEqual(@as(i32, 6), back.dx);
    try std.testing.expectEqual(@as(i32, -3), back.dy);
    try std.testing.expectEqual(@as(i32, -1), back.sx);
    try std.testing.expectEqual(@as(i32, -1), back.sy);
}

test "lineStep walks a horizontal run one column at a time" {
    var walk = impl.lineStart(0, 0, 4, 0);
    var steps: u32 = 0;
    while (!((walk.x == 4) and (walk.y == 0))) {
        walk = impl.lineStep(walk);
        steps += 1;
        try std.testing.expectEqual(@as(i32, 0), walk.y);
        try std.testing.expect(steps <= 8);
    }
    try std.testing.expectEqual(@as(u32, 4), steps);
}

test "lineStep advances both axes on a perfect diagonal" {
    var walk = impl.lineStart(0, 0, 3, 3);
    walk = impl.lineStep(walk);
    try std.testing.expectEqual(@as(i32, 1), walk.x);
    try std.testing.expectEqual(@as(i32, 1), walk.y);
}

test "lineStep reaches a steep endpoint within the iteration ceiling" {
    var walk = impl.lineStart(0, 0, 2, 9);
    var i: i32 = 0;
    while (i < impl.Line.max_iterations) : (i += 1) {
        if ((walk.x == 2) and (walk.y == 9)) break;
        walk = impl.lineStep(walk);
    }
    try std.testing.expectEqual(@as(i32, 2), walk.x);
    try std.testing.expectEqual(@as(i32, 9), walk.y);
    try std.testing.expect(i < impl.Line.max_iterations);
}

test "lineStart survives endpoints that would overflow a plain subtraction" {
    const walk = impl.lineStart(std.math.minInt(i32), 0, std.math.maxInt(i32), 0);
    try std.testing.expectEqual(@as(i32, -1), walk.dx);
    try std.testing.expectEqual(@as(i32, 1), walk.sx);
}

test "circleStart seeds the first octant" {
    const walk = impl.circleStart(5);
    try std.testing.expectEqual(@as(i32, 5), walk.x);
    try std.testing.expectEqual(@as(i32, 0), walk.y);
    try std.testing.expectEqual(@as(i32, -4), walk.e);
    try std.testing.expect(!walk.done());
}

test "a zero radius closes the octant immediately" {
    const walk = impl.circleStart(0);
    try std.testing.expect(walk.done());
    try std.testing.expectEqual(@as(i32, 1), walk.e);
}

test "circleStep stays on the radius and terminates" {
    var walk = impl.circleStart(10);
    var i: i32 = 0;
    while (i < impl.Circle.max_iterations) : (i += 1) {
        if (walk.done()) break;
        const before = walk;
        walk = impl.circleStep(walk);
        try std.testing.expectEqual(before.y + 1, walk.y);
        try std.testing.expect(walk.x <= before.x);
        const radius2 = (walk.x * walk.x) + (walk.y * walk.y);
        try std.testing.expect(radius2 >= 81);
        try std.testing.expect(radius2 <= 121);
    }
    try std.testing.expect(walk.done());
    try std.testing.expect(i < impl.Circle.max_iterations);
}

test "circleStep takes the negative-error branch without shrinking x" {
    const seeded = impl.Circle{ .x = 9, .y = 2, .e = -5 };
    const next = impl.circleStep(seeded);
    try std.testing.expectEqual(@as(i32, 9), next.x);
    try std.testing.expectEqual(@as(i32, 3), next.y);
    try std.testing.expectEqual(@as(i32, 2), next.e);
}

test "circleStep takes the non-negative-error branch and shrinks x" {
    const seeded = impl.Circle{ .x = 9, .y = 2, .e = 1 };
    const next = impl.circleStep(seeded);
    try std.testing.expectEqual(@as(i32, 8), next.x);
    try std.testing.expectEqual(@as(i32, 3), next.y);
    try std.testing.expectEqual(@as(i32, -8), next.e);
}

test "blitGray8ArgsOk demands a buffer and a positive extent" {
    try std.testing.expect(impl.blitGray8ArgsOk(true, 1, 1));
    try std.testing.expect(!impl.blitGray8ArgsOk(false, 1, 1));
    try std.testing.expect(!impl.blitGray8ArgsOk(true, 0, 1));
    try std.testing.expect(!impl.blitGray8ArgsOk(true, 1, 0));
    try std.testing.expect(!impl.blitGray8ArgsOk(true, -1, 1));
    try std.testing.expect(!impl.blitGray8ArgsOk(true, 1, -1));
}

test "blitArgsOk demands a non-empty source in a known format" {
    try std.testing.expect(impl.blitArgsOk(1, 1, impl.format.rgb565));
    try std.testing.expect(!impl.blitArgsOk(0, 1, impl.format.rgb565));
    try std.testing.expect(!impl.blitArgsOk(1, 0, impl.format.rgb565));
    try std.testing.expect(!impl.blitArgsOk(1, 1, 9));
}

test "gray4FlatIndex walks rows by the nibble stride" {
    try std.testing.expectEqual(@as(usize, 0), impl.gray4FlatIndex(4, 0, 0));
    try std.testing.expectEqual(@as(usize, 3), impl.gray4FlatIndex(4, 3, 0));
    try std.testing.expectEqual(@as(usize, 4), impl.gray4FlatIndex(4, 0, 1));
    try std.testing.expectEqual(@as(usize, 11), impl.gray4FlatIndex(3, 2, 3));
}

test "gray4Nibble picks the high half on an even index and the low half on an odd one" {
    try std.testing.expectEqual(@as(u8, 0x0A), impl.gray4Nibble(0xAB, 0));
    try std.testing.expectEqual(@as(u8, 0x0B), impl.gray4Nibble(0xAB, 1));
    try std.testing.expectEqual(@as(u8, 0x0A), impl.gray4Nibble(0xAB, 2));
    try std.testing.expectEqual(@as(u8, 0x0B), impl.gray4Nibble(0xAB, 3));
}

test "gray4ToGray8 replicates the level into both halves" {
    try std.testing.expectEqual(@as(u8, 0x00), impl.gray4ToGray8(0x0));
    try std.testing.expectEqual(@as(u8, 0x11), impl.gray4ToGray8(0x1));
    try std.testing.expectEqual(@as(u8, 0x77), impl.gray4ToGray8(0x7));
    try std.testing.expectEqual(@as(u8, 0xFF), impl.gray4ToGray8(0xF));
}

test "gray4ToGray8 masks a stray high half rather than overflowing the shift" {
    try std.testing.expectEqual(@as(u8, 0x22), impl.gray4ToGray8(0xF2));
}

test "gray4Window keeps a wholly in-image request untouched" {
    const w = impl.gray4Window(1, 1, 2, 2, 8, 8);
    try std.testing.expectEqual(@as(i32, 1), w.x0);
    try std.testing.expectEqual(@as(i32, 1), w.y0);
    try std.testing.expectEqual(@as(i32, 3), w.x1);
    try std.testing.expectEqual(@as(i32, 3), w.y1);
    try std.testing.expect(!w.isEmpty());
}

test "gray4Window clamps a negative origin up and a far edge down" {
    const w = impl.gray4Window(-2, -3, 10, 10, 4, 4);
    try std.testing.expectEqual(@as(i32, 0), w.x0);
    try std.testing.expectEqual(@as(i32, 0), w.y0);
    try std.testing.expectEqual(@as(i32, 4), w.x1);
    try std.testing.expectEqual(@as(i32, 4), w.y1);
}

test "gray4Window collapses a non-positive extent so nothing is drawn" {
    try std.testing.expect(impl.gray4Window(0, 0, 0, 4, 4, 4).isEmpty());
    try std.testing.expect(impl.gray4Window(0, 0, 4, 0, 4, 4).isEmpty());
    try std.testing.expect(impl.gray4Window(0, 0, -1, -1, 4, 4).isEmpty());
}

test "gray4Window collapses a sub-rectangle that starts past the image" {
    const w = impl.gray4Window(9, 9, 2, 2, 4, 4);
    try std.testing.expectEqual(@as(i32, 9), w.x0);
    try std.testing.expectEqual(@as(i32, 4), w.x1);
    try std.testing.expect(w.isEmpty());
}

test "gray4Window wraps the far edge the way the C int32 addition did" {
    const w = impl.gray4Window(std.math.maxInt(i32), 0, 1, 4, 4, 4);
    try std.testing.expectEqual(std.math.minInt(i32), w.x1);
    try std.testing.expect(w.isEmpty());
}

test "gray4ZoomArgsOk demands a buffer, a positive zoom, then a non-empty image" {
    try std.testing.expect(impl.gray4ZoomArgsOk(true, 1, 4, 4));
    try std.testing.expect(!impl.gray4ZoomArgsOk(false, 1, 4, 4));
    try std.testing.expect(!impl.gray4ZoomArgsOk(true, 0, 4, 4));
    try std.testing.expect(!impl.gray4ZoomArgsOk(true, -1, 4, 4));
    try std.testing.expect(!impl.gray4ZoomArgsOk(true, 1, 0, 4));
    try std.testing.expect(!impl.gray4ZoomArgsOk(true, 1, 4, 0));
    try std.testing.expect(!impl.gray4ZoomArgsOk(true, 1, -1, -1));
}

test "font layout matches the C ra8_gfx_font_t" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(impl.Font, "glyph_data"));
    try std.testing.expectEqual(ptr, @offsetOf(impl.Font, "glyph_width"));
    try std.testing.expectEqual(ptr + 1, @offsetOf(impl.Font, "glyph_height"));
    try std.testing.expectEqual(ptr + 2, @offsetOf(impl.Font, "bytes_per_glyph"));
    try std.testing.expectEqual(ptr + 3, @offsetOf(impl.Font, "first_codepoint"));
    try std.testing.expectEqual(ptr + 4, @offsetOf(impl.Font, "last_codepoint"));
    try std.testing.expectEqual(std.mem.alignForward(usize, ptr + 5, ptr), @sizeOf(impl.Font));
}

test "glyphRowBytes rounds a cell width up to whole bytes" {
    try std.testing.expectEqual(@as(u32, 1), impl.glyphRowBytes(1));
    try std.testing.expectEqual(@as(u32, 1), impl.glyphRowBytes(8));
    try std.testing.expectEqual(@as(u32, 2), impl.glyphRowBytes(9));
    try std.testing.expectEqual(@as(u32, 2), impl.glyphRowBytes(16));
    try std.testing.expectEqual(@as(u32, 32), impl.glyphRowBytes(255));
    try std.testing.expectEqual(@as(u32, 0), impl.glyphRowBytes(0));
}

test "glyphIndex maps a codepoint to its slot and folds the rest onto space" {
    try std.testing.expectEqual(@as(u8, 0), impl.glyphIndex(0x20, 0x20, 0x7E));
    try std.testing.expectEqual(@as(u8, 1), impl.glyphIndex(0x21, 0x20, 0x7E));
    try std.testing.expectEqual(@as(u8, 94), impl.glyphIndex(0x7E, 0x20, 0x7E));
    try std.testing.expectEqual(@as(u8, 0), impl.glyphIndex(0x1F, 0x20, 0x7E));
    try std.testing.expectEqual(@as(u8, 0), impl.glyphIndex(0x7F, 0x20, 0x7E));
    try std.testing.expectEqual(@as(u8, 0), impl.glyphIndex(0, 0x20, 0x7E));
}

test "glyphDataOffset walks the packed table in whole glyphs" {
    try std.testing.expectEqual(@as(usize, 0), impl.glyphDataOffset(0, 16));
    try std.testing.expectEqual(@as(usize, 16), impl.glyphDataOffset(1, 16));
    try std.testing.expectEqual(@as(usize, 1504), impl.glyphDataOffset(94, 16));
    try std.testing.expectEqual(@as(usize, 0), impl.glyphDataOffset(94, 0));
}

test "glyph bits are MSB-first within each row byte" {
    const cell = [_]u8{ 0b1000_0001, 0b0100_0010 };
    try std.testing.expect(impl.glyphBitSet(&cell, 1, 0, 0));
    try std.testing.expect(!impl.glyphBitSet(&cell, 1, 0, 1));
    try std.testing.expect(impl.glyphBitSet(&cell, 1, 0, 7));
    try std.testing.expect(!impl.glyphBitSet(&cell, 1, 1, 0));
    try std.testing.expect(impl.glyphBitSet(&cell, 1, 1, 1));
    try std.testing.expect(impl.glyphBitSet(&cell, 1, 1, 6));
}

test "a wide glyph row spans several bytes" {
    const cell = [_]u8{ 0x00, 0x80, 0x01, 0x00 };
    try std.testing.expect(!impl.glyphBitSet(&cell, 2, 0, 0));
    try std.testing.expect(impl.glyphBitSet(&cell, 2, 0, 8));
    try std.testing.expect(impl.glyphBitSet(&cell, 2, 1, 7));
    try std.testing.expect(!impl.glyphBitSet(&cell, 2, 1, 8));
}

test "glyphWindow intersects a cell with the clip box" {
    const clip = impl.Box{ .x0 = 0, .y0 = 0, .x1 = 16, .y1 = 8 };
    const w = impl.glyphWindow(4, 2, 8, 2, clip) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(@as(i32, 4), w.x0);
    try std.testing.expectEqual(@as(i32, 2), w.y0);
    try std.testing.expectEqual(@as(i32, 12), w.x1);
    try std.testing.expectEqual(@as(i32, 4), w.y1);
}

test "glyphWindow clamps a cell straddling the clip edges" {
    const clip = impl.Box{ .x0 = 2, .y0 = 2, .x1 = 10, .y1 = 6 };
    const w = impl.glyphWindow(-3, 5, 8, 16, clip) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(@as(i32, 2), w.x0);
    try std.testing.expectEqual(@as(i32, 5), w.y0);
    try std.testing.expectEqual(@as(i32, 5), w.x1);
    try std.testing.expectEqual(@as(i32, 6), w.y1);
}

test "glyphWindow refuses a cell fully outside the clip" {
    const clip = impl.Box{ .x0 = 0, .y0 = 0, .x1 = 16, .y1 = 8 };
    try std.testing.expect(impl.glyphWindow(16, 0, 8, 2, clip) == null);
    try std.testing.expect(impl.glyphWindow(0, -2, 8, 2, clip) == null);
    try std.testing.expect(impl.glyphWindow(0, 0, 0, 2, clip) == null);
    try std.testing.expect(impl.glyphWindow(0, 0, 8, 0, clip) == null);
}

test "glyphWindow widens the far edge so a cell cannot wrap into view" {
    const clip = impl.Box{ .x0 = 0, .y0 = 0, .x1 = 16, .y1 = 8 };
    try std.testing.expect(impl.glyphWindow(std.math.maxInt(i32), 0, 8, 2, clip) == null);
    try std.testing.expect(impl.glyphWindow(0, std.math.maxInt(i32), 8, 2, clip) == null);
}

test "textLength stops at the NUL" {
    try std.testing.expectEqual(@as(u32, 0), impl.textLength(""));
    try std.testing.expectEqual(@as(u32, 3), impl.textLength("abc"));
    try std.testing.expectEqual(@as(u32, 1), impl.textLength("a\x00bc"));
}

test "textLength caps at the glyph ceiling" {
    var buf: [impl.max_chars + 8]u8 = undefined;
    @memset(&buf, 'x');
    try std.testing.expectEqual(impl.max_chars, impl.textLength(&buf));
}

test "textExtent is one row of n cells" {
    const measured = impl.textExtent(3, 8, 16);
    try std.testing.expectEqual(@as(u32, 24), measured.w);
    try std.testing.expectEqual(@as(u32, 16), measured.h);
    const empty = impl.textExtent(0, 8, 16);
    try std.testing.expectEqual(@as(u32, 0), empty.w);
    try std.testing.expectEqual(@as(u32, 16), empty.h);
}

test "textOutStatus judges the null pair before the binding" {
    try std.testing.expectEqual(impl.err.null_ptr, impl.textOutStatus(false, true, false));
    try std.testing.expectEqual(impl.err.null_ptr, impl.textOutStatus(true, false, false));
    try std.testing.expectEqual(impl.err.null_ptr, impl.textOutStatus(false, false, true));
    try std.testing.expectEqual(impl.err.not_initialized, impl.textOutStatus(true, true, false));
    try std.testing.expectEqual(impl.err.ok, impl.textOutStatus(true, true, true));
}

test "textSizeStatus wants all four pointers and never the binding" {
    try std.testing.expectEqual(impl.err.ok, impl.textSizeStatus(true, true, true, true));
    try std.testing.expectEqual(impl.err.null_ptr, impl.textSizeStatus(false, true, true, true));
    try std.testing.expectEqual(impl.err.null_ptr, impl.textSizeStatus(true, false, true, true));
    try std.testing.expectEqual(impl.err.null_ptr, impl.textSizeStatus(true, true, false, true));
    try std.testing.expectEqual(impl.err.null_ptr, impl.textSizeStatus(true, true, true, false));
}

test "max_chars is the dimension ceiling both text loops were bounded by" {
    try std.testing.expectEqual(@as(u32, impl.dim.max), impl.max_chars);
}

// --- blue-noise dither (#477) ----------------------------------------------

test "the dither constants carry the C palette geometry" {
    try std.testing.expectEqual(@as(u8, 16), impl.dither.levels);
    try std.testing.expectEqual(@as(u8, 17), impl.dither.step);
    try std.testing.expectEqual(@as(u8, 15), impl.dither.max_level);
    try std.testing.expectEqual(@as(u32, 4096), impl.dither.mask_len);
    try std.testing.expectEqual(@as(u32, 64), impl.dither.mask_dim);
    try std.testing.expectEqual(impl.dither.mask_dim - 1, impl.dither.mask_index_mask);
}

test "maskIndex reduces onto the mask edge toroidally" {
    try std.testing.expectEqual(@as(u32, 0), impl.maskIndex(0, 0));
    try std.testing.expectEqual(@as(u32, 63), impl.maskIndex(63, 0));
    try std.testing.expectEqual(@as(u32, 64), impl.maskIndex(0, 1));
    try std.testing.expectEqual(@as(u32, 0), impl.maskIndex(64, 64));
    try std.testing.expectEqual(impl.maskIndex(0, 0), impl.maskIndex(-64, -64));
    try std.testing.expectEqual(impl.maskIndex(63, 63), impl.maskIndex(-1, -1));
}

test "every mask index is a valid subscript, negative coordinates included" {
    var y: i32 = -70;
    while (y <= 70) : (y += 7) {
        var x: i32 = -70;
        while (x <= 70) : (x += 5) {
            try std.testing.expect(impl.maskIndex(x, y) < impl.dither.mask_len);
        }
    }
}

test "quantise keeps the exact integer round-up rule" {
    // A zero remainder can never round up, whatever the threshold.
    try std.testing.expectEqual(@as(u8, 0), impl.quantise(0, 0));
    try std.testing.expectEqual(@as(u8, 1), impl.quantise(17, 0));
    try std.testing.expectEqual(@as(u8, 15), impl.quantise(255, 0));
    try std.testing.expectEqual(@as(u8, 15), impl.quantise(255, 255));
    // One step of 17 above a level: thr * 17 < 1 * 256 holds only below 16.
    try std.testing.expectEqual(@as(u8, 2), impl.quantise(18, 15));
    try std.testing.expectEqual(@as(u8, 1), impl.quantise(18, 16));
    // Eight steps up: the cut sits at 8 * 256 / 17, i.e. thresholds 0..120.
    try std.testing.expectEqual(@as(u8, 2), impl.quantise(25, 120));
    try std.testing.expectEqual(@as(u8, 1), impl.quantise(25, 121));
}

test "quantise never leaves the 4-bit range, so no clamp is needed" {
    var g: u32 = 0;
    while (g <= 255) : (g += 1) {
        var t: u32 = 0;
        while (t <= 255) : (t += 1) {
            const level = impl.quantise(@intCast(g), @intCast(t));
            try std.testing.expect(level <= impl.dither.max_level);
        }
    }
}

test "the round-up frequency is exactly the fractional distance" {
    // 25 sits 8/17 of the way from level 1 to level 2, so exactly the
    // thresholds below 8 * 256 / 17 round up: an unbiased mask, no banding.
    var ups: u32 = 0;
    var t: u32 = 0;
    while (t <= 255) : (t += 1) {
        if (impl.quantise(25, @intCast(t)) == 2) ups += 1;
    }
    try std.testing.expectEqual(@as(u32, 121), ups);
}

test "ditherLevel is quantise against the mask threshold at that coordinate" {
    try std.testing.expectEqual(
        impl.quantise(129, impl.maskThreshold(5, 9)),
        impl.ditherLevel(129, 5, 9),
    );
    try std.testing.expectEqual(impl.ditherLevel(129, 5, 9), impl.ditherLevel(129, 69, -55));
}

test "levelToColor matches the shared gray4 expansion" {
    try std.testing.expectEqual(@as(u32, 0x00000000), impl.levelToColor(0));
    try std.testing.expectEqual(@as(u32, 0x00FFFFFF), impl.levelToColor(15));
    var n: u8 = 0;
    while (n <= 15) : (n += 1) {
        try std.testing.expectEqual(impl.grayToColor(impl.gray4ToGray8(n)), impl.levelToColor(n));
    }
}

test "packedBytes rounds an odd pixel count up" {
    try std.testing.expectEqual(@as(u32, 1), impl.packedBytes(1, 1));
    try std.testing.expectEqual(@as(u32, 1), impl.packedBytes(2, 1));
    try std.testing.expectEqual(@as(u32, 2), impl.packedBytes(3, 1));
    try std.testing.expectEqual(@as(u32, 5), impl.packedBytes(3, 3));
    try std.testing.expectEqual(@as(u32, 8), impl.packedBytes(4, 4));
}

test "nibble placement alternates by flat index" {
    try std.testing.expectEqual(@as(u32, 0), impl.packByteIndex(0));
    try std.testing.expectEqual(@as(u32, 0), impl.packByteIndex(1));
    try std.testing.expectEqual(@as(u32, 1), impl.packByteIndex(2));
    try std.testing.expect(impl.packIsHighNibble(0));
    try std.testing.expect(!impl.packIsHighNibble(1));
    try std.testing.expect(impl.packIsHighNibble(4));
}

test "an even index assigns the byte and an odd index ORs into it" {
    try std.testing.expectEqual(@as(u8, 0xA0), impl.packNibble(0xFF, 0xA, 0));
    try std.testing.expectEqual(@as(u8, 0xA5), impl.packNibble(0xA0, 0x5, 1));
    // The assign on the even index is what lets the caller skip pre-zeroing.
    try std.testing.expectEqual(@as(u8, 0x30), impl.packNibble(0x77, 0x3, 4));
}

test "packGuard judges the three pointers before the dimensions" {
    try std.testing.expectEqual(impl.PackGuard.no_src, impl.packGuard(false, false, false, 0, 0, 0));
    try std.testing.expectEqual(impl.PackGuard.no_out, impl.packGuard(true, false, false, 0, 0, 0));
    try std.testing.expectEqual(impl.PackGuard.no_out_size, impl.packGuard(true, true, false, 0, 0, 0));
    try std.testing.expectEqual(impl.PackGuard.bad_dims, impl.packGuard(true, true, true, 0, 4, 64));
    try std.testing.expectEqual(impl.PackGuard.bad_dims, impl.packGuard(true, true, true, 4, -1, 64));
    try std.testing.expectEqual(impl.PackGuard.too_small, impl.packGuard(true, true, true, 4, 4, 7));
    try std.testing.expectEqual(impl.PackGuard.ok, impl.packGuard(true, true, true, 4, 4, 8));
}

test "packStatus answers each verdict with the C's code" {
    try std.testing.expectEqual(impl.err.ok, impl.packStatus(.ok));
    try std.testing.expectEqual(impl.err.null_ptr, impl.packStatus(.no_src));
    try std.testing.expectEqual(impl.err.null_ptr, impl.packStatus(.no_out));
    try std.testing.expectEqual(impl.err.null_ptr, impl.packStatus(.no_out_size));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.packStatus(.bad_dims));
    try std.testing.expectEqual(impl.err.no_mem, impl.packStatus(.too_small));
    try std.testing.expectEqual(@as(u16, 0x102), impl.err.no_mem);
}

test "ditherBlitStatus checks the binding before its arguments" {
    try std.testing.expectEqual(impl.err.not_initialized, impl.ditherBlitStatus(false, true, 4, 4));
    try std.testing.expectEqual(impl.err.not_initialized, impl.ditherBlitStatus(false, false, 0, 0));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.ditherBlitStatus(true, false, 4, 4));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.ditherBlitStatus(true, true, 0, 4));
    try std.testing.expectEqual(impl.err.invalid_arg, impl.ditherBlitStatus(true, true, 4, -3));
    try std.testing.expectEqual(impl.err.ok, impl.ditherBlitStatus(true, true, 1, 1));
}
