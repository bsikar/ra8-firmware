//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the argb8888 surface (RA8FW-950): solid and blended fills,
//! lines with both ends, the clip stack, blits from every source format and
//! the PPM it writes for goldens.

const std = @import("std");
const gfx = @import("ra8_gfx");

const Surface = gfx.Surface;
const white: u32 = 0xFFFFFF;

fn surface(w: u32, h: u32) !Surface {
    return Surface.init(std.testing.allocator, w, h, white);
}

test "a new surface is the opaque background and refuses a zero size" {
    var s = try surface(3, 2);
    defer s.deinit(std.testing.allocator);
    for (s.pixels) |pixel| try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), pixel);
    try std.testing.expectError(error.InvalidDimensions, Surface.init(std.testing.allocator, 0, 4, white));
}

test "a fill stays inside the surface and stores opaque pixels" {
    var s = try surface(4, 4);
    defer s.deinit(std.testing.allocator);
    s.fill(.{ .x = -2, .y = 2, .w = 4, .h = 9 }, 0xAB112233);
    try std.testing.expectEqual(@as(u32, 0xFF11_2233), s.at(1, 3));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(2, 3));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(1, 1));
}

test "a blended fill composites source-over" {
    var s = try surface(2, 1);
    defer s.deinit(std.testing.allocator);
    s.fill(s.bounds(), 0x000000);
    s.fillAlpha(.{ .x = 0, .y = 0, .w = 1, .h = 1 }, 0xFF8000, 128);
    try std.testing.expectEqual(@as(u32, 0xFF80_4000), s.at(0, 0));
    s.fillAlpha(.{ .x = 1, .y = 0, .w = 1, .h = 1 }, 0xFFFFFF, 0);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(1, 0));
    try std.testing.expectEqual(@as(u32, 0xFF12_3456), gfx.surface.over(0xFF00_0000, 0x123456, 255));
}

test "a line draws both ends and a diagonal one pixel a step" {
    var s = try surface(5, 5);
    defer s.deinit(std.testing.allocator);
    s.line(4, 4, 0, 0, 0x000000);
    var y: u32 = 0;
    while (y < 5) : (y += 1) try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(y, y));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(1, 0));
}

test "clips nest, narrow, and pop back" {
    var s = try surface(8, 8);
    defer s.deinit(std.testing.allocator);
    s.pushClip(.{ .x = 2, .y = 2, .w = 4, .h = 4 });
    s.pushClip(.{ .x = 4, .y = 0, .w = 8, .h = 8 });
    try std.testing.expectEqual(gfx.Rect{ .x = 4, .y = 2, .w = 2, .h = 4 }, s.clip());
    s.fill(s.bounds(), 0x000000);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(5, 5));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(3, 3));
    s.popClip();
    s.plot(3, 3, 0x000000);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(3, 3));
    s.popClip();
    try std.testing.expectEqual(s.bounds(), s.clip());
    s.popClip();
    try std.testing.expectEqual(s.bounds(), s.clip());
}

test "pushes past the clip depth stay balanced" {
    var s = try surface(8, 8);
    defer s.deinit(std.testing.allocator);
    var i: usize = 0;
    while (i < gfx.surface.clip_depth + 3) : (i += 1) s.pushClip(.{ .x = 1, .y = 1, .w = 6, .h = 6 });
    i = 0;
    while (i < gfx.surface.clip_depth + 3) : (i += 1) s.popClip();
    try std.testing.expectEqual(s.bounds(), s.clip());
}

test "glyph coverage blends and skips zero coverage" {
    var s = try surface(2, 1);
    defer s.deinit(std.testing.allocator);
    s.blend(0, 0, 0x000000, 255);
    s.blend(1, 0, 0x000000, 0);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(0, 0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(1, 0));
    s.blend(1, 0, 0x000000, 85);
    try std.testing.expectEqual(@as(u32, 0xFFAA_AAAA), s.at(1, 0));
}

test "blits read every source format and argb composites by its alpha" {
    var s = try surface(4, 1);
    defer s.deinit(std.testing.allocator);
    s.blit(.{ .x = 0, .y = 0, .w = 1, .h = 1 }, &.{0x40}, 1, .gray8);
    s.blit(.{ .x = 1, .y = 0, .w = 1, .h = 1 }, &.{ 0x00, 0xF8 }, 2, .rgb565);
    s.blit(.{ .x = 2, .y = 0, .w = 1, .h = 1 }, &.{ 0x03, 0x02, 0x01 }, 3, .rgb888);
    s.blit(.{ .x = 3, .y = 0, .w = 1, .h = 1 }, &.{ 0x00, 0x00, 0x00, 0x00 }, 4, .argb8888);
    try std.testing.expectEqual(@as(u32, 0xFF40_4040), s.at(0, 0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_0000), s.at(1, 0));
    try std.testing.expectEqual(@as(u32, 0xFF01_0203), s.at(2, 0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(3, 0));
}

test "a blit is clipped and a short source stops instead of overrunning" {
    var s = try surface(3, 2);
    defer s.deinit(std.testing.allocator);
    s.pushClip(.{ .x = 0, .y = 0, .w = 3, .h = 1 });
    const source = [_]u8{ 0, 0, 0, 0, 0, 0 };
    s.blit(.{ .x = 0, .y = 0, .w = 3, .h = 2 }, &source, 3, .gray8);
    s.popClip();
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(2, 0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(0, 1));
    s.blit(.{ .x = 0, .y = 1, .w = 3, .h = 1 }, source[0..2], 3, .gray8);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), s.at(1, 1));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.at(2, 1));
}

test "the PPM is a P6 header and RGB triples" {
    var s = try surface(2, 1);
    defer s.deinit(std.testing.allocator);
    s.plot(1, 0, 0x102030);
    const out = try s.ppm(std.testing.allocator);
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualSlices(u8, "P6\n2 1\n255\n\xff\xff\xff\x10\x20\x30", out);
}
