//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host and freestanding unit checks for `EinkSurface` (RA8FW-1012):
//! packed gray4 storage, ordered blue-noise dithering, tone calibration,
//! clipping, blits, text rendering, and IT8951 packed-row streaming.

const std = @import("std");
const gfx = @import("ra8_gfx");

test "eink surface validates dimensions, stride, and caller buffer length" {
    var small: [3]u8 = @splat(0);
    try std.testing.expectError(error.InvalidDimensions, gfx.EinkSurface.init(&small, 0, 2, 0, 0, null));
    try std.testing.expectError(error.InvalidDimensions, gfx.EinkSurface.init(&small, 2, 0, 0, 0, null));
    try std.testing.expectError(error.InvalidStride, gfx.EinkSurface.init(&small, 5, 2, 2, 0, null));
    try std.testing.expectError(error.BufferTooSmall, gfx.EinkSurface.init(&small, 4, 2, 0, 0, null));

    var buf: [6]u8 = @splat(0);
    const panel = try gfx.EinkSurface.init(&buf, 5, 2, 0, 0xFF_FFFF, null);
    try std.testing.expectEqual(@as(u32, 3), panel.stride_bytes);
    try std.testing.expectEqual(@as(u4, 15), panel.nibbleAt(0, 0));
    try std.testing.expectEqual(@as(u4, 15), panel.nibbleAt(4, 1));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), panel.at(4, 1));
}

test "eink surface advertises gray4 + dithering capabilities without color or alpha" {
    var buf: [8]u8 = @splat(0);
    const panel = try gfx.EinkSurface.init(&buf, 4, 4, 0, 0x000000, null);
    const caps = panel.capabilities();
    try std.testing.expectEqual(gfx.PixelFormat.gray4, panel.pixelFormat());
    try std.testing.expectEqual(gfx.PixelFormat.gray4, caps.pixel_format);
    try std.testing.expect(caps.gray4);
    try std.testing.expect(caps.dither);
    try std.testing.expect(caps.line);
    try std.testing.expect(caps.blit);
    try std.testing.expect(caps.clip);
    try std.testing.expect(!caps.color);
    try std.testing.expect(!caps.alpha);
}

test "eink surface dithers mid-gray fills using the committed blue-noise mask and tone curve" {
    var buf: [16 * 8]u8 = @splat(0);
    var panel = try gfx.EinkSurface.init(&buf, 16, 16, 0, 0xFFFFFF, null);
    panel.fill(.{ .x = 0, .y = 0, .w = 16, .h = 16 }, 0x808080);

    var lows: usize = 0;
    var highs: usize = 0;
    var y: u32 = 0;
    while (y < 16) : (y += 1) {
        var x: u32 = 0;
        while (x < 16) : (x += 1) {
            const lvl = panel.nibbleAt(x, y);
            const expected: u4 = @intCast(gfx.dither.quantise(0x80, gfx.dither.thresholdAt(@intCast(x), @intCast(y))));
            try std.testing.expectEqual(expected, lvl);
            if (lvl == 7) lows += 1 else if (lvl == 8) highs += 1 else return error.UnexpectedLevel;
        }
    }
    try std.testing.expect(lows > 0 and highs > 0);

    var lut = gfx.tone.nominal;
    lut.level_gray8[1] = 200;
    var i: usize = 2;
    while (i < 15) : (i += 1) lut.level_gray8[i] = @intCast(200 + i);
    lut.level_gray8[15] = 255;
    var map: gfx.tone.Map = undefined;
    gfx.tone.prepareMap(&lut, &map);

    var cal_buf: [32]u8 = @splat(0);
    var cal = try gfx.EinkSurface.init(&cal_buf, 8, 8, 0, 0xFFFFFF, &map);
    cal.fill(.{ .x = 0, .y = 0, .w = 8, .h = 8 }, 0x808080);
    try std.testing.expect(cal.nibbleAt(0, 0) <= 1);
}

test "eink surface supports clip stack, lines, alpha blend, blits, and text" {
    var buf: [24 * 32]u8 = @splat(0);
    var panel = try gfx.EinkSurface.init(&buf, 48, 32, 0, 0xFFFFFF, null);

    panel.pushClip(.{ .x = 4, .y = 2, .w = 10, .h = 8 });
    panel.line(0, 4, 20, 4, 0x000000);
    panel.popClip();
    try std.testing.expectEqual(@as(u4, 15), panel.nibbleAt(2, 4));
    try std.testing.expectEqual(@as(u4, 0), panel.nibbleAt(6, 4));
    try std.testing.expectEqual(@as(u4, 15), panel.nibbleAt(16, 4));

    const gray4_src = [_]u8{ 0x1A, 0x3F };
    panel.blitGray4(.{ .x = 1, .y = 10, .w = 3, .h = 1 }, &gray4_src, 2);
    try std.testing.expectEqual(@as(u4, 1), panel.nibbleAt(1, 10));
    try std.testing.expectEqual(@as(u4, 10), panel.nibbleAt(2, 10));
    try std.testing.expectEqual(@as(u4, 3), panel.nibbleAt(3, 10));
    try std.testing.expectEqual(@as(u4, 15), panel.nibbleAt(4, 10));

    panel.blit(.{ .x = 0, .y = 12, .w = 1, .h = 1 }, &.{ 0x00, 0x00, 0xFF, 0x00 }, 4, .argb8888);
    try std.testing.expectEqual(@as(u4, 15), panel.nibbleAt(0, 12));
    panel.fillAlpha(.{ .x = 20, .y = 10, .w = 4, .h = 4 }, 0x000000, 255);
    try std.testing.expectEqual(@as(u4, 0), panel.nibbleAt(21, 11));

    panel.drawTextStyle("Hi", 2, 14, .serif, .regular, 0, 0x000000, 0xFFFFFF);
    const ext = gfx.EinkSurface.measureTextStyle("Hi", .serif, .regular, 0);
    try std.testing.expect(ext.width > 0 and ext.height > 0);
    var lit: usize = 0;
    var ty: u32 = 14;
    while (ty < 14 + ext.height and ty < 32) : (ty += 1) {
        var tx: u32 = 2;
        while (tx < 2 + ext.width and tx < 48) : (tx += 1) {
            if (panel.nibbleAt(tx, ty) < 12) lit += 1;
        }
    }
    try std.testing.expect(lit > 0);
}

const RowLog = struct {
    count: usize = 0,
    first_x: u16 = 0,
    first_y: u16 = 0,
    first_w: u16 = 0,
    row0: [8]u8 = undefined,
    row0_len: usize = 0,
};

fn recordRow(user: ?*anyopaque, x: u16, y: u16, w: u16, packed_bytes: []const u8) bool {
    const log: *RowLog = @ptrCast(@alignCast(user orelse return false));
    if (log.count == 0) {
        log.first_x = x;
        log.first_y = y;
        log.first_w = w;
        log.row0_len = packed_bytes.len;
        @memcpy(log.row0[0..packed_bytes.len], packed_bytes);
    }
    log.count += 1;
    return true;
}

test "flushPackedRows streams even-aligned spans zero-copy and pads odd spans cleanly" {
    var buf: [4 * 6]u8 = @splat(0);
    var panel = try gfx.EinkSurface.init(&buf, 8, 6, 0, 0xFFFFFF, null);
    panel.fill(.{ .x = 1, .y = 2, .w = 3, .h = 2 }, 0x000000);

    var scratch: [4]u8 = undefined;
    var even_log = RowLog{};
    try std.testing.expect(panel.flushPackedRows(.{ .x = 0, .y = 2, .w = 4, .h = 2 }, &.{}, &even_log, recordRow));
    try std.testing.expectEqual(@as(usize, 2), even_log.count);
    try std.testing.expectEqualSlices(u8, &.{ 0xF0, 0x00 }, even_log.row0[0..even_log.row0_len]);

    var odd_log = RowLog{};
    try std.testing.expect(panel.flushPackedRows(.{ .x = 1, .y = 2, .w = 3, .h = 2 }, &scratch, &odd_log, recordRow));
    try std.testing.expectEqual(@as(usize, 2), odd_log.count);
    try std.testing.expectEqual(@as(u16, 1), odd_log.first_x);
    try std.testing.expectEqual(@as(u16, 3), odd_log.first_w);
    try std.testing.expectEqualSlices(u8, &.{ 0x00, 0x0F }, odd_log.row0[0..odd_log.row0_len]);

    try std.testing.expect(!panel.flushPackedRows(.{ .x = 1, .y = 2, .w = 3, .h = 1 }, scratch[0..1], &odd_log, recordRow));
}
