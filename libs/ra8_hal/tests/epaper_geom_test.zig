//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const geom = @import("epaper_geom");

fn goodCfg() geom.Cfg {
    return .{
        .bus = .{ .xfer8 = @ptrFromInt(0x1000), .ctx = null },
        .waveform = .{ .init = 0, .du = 1, .gc16 = 2, .a2 = 4 },
        .reset_pin = 0,
        .busy_pin = 0,
        .panel_width = 1448,
        .panel_height = 1072,
    };
}

test "bitsPerPixel maps each format and defaults out-of-range to 8" {
    try std.testing.expectEqual(@as(u8, 1), geom.bitsPerPixel(geom.pf_1bpp));
    try std.testing.expectEqual(@as(u8, 2), geom.bitsPerPixel(geom.pf_2bpp));
    try std.testing.expectEqual(@as(u8, 4), geom.bitsPerPixel(geom.pf_4bpp));
    try std.testing.expectEqual(@as(u8, 8), geom.bitsPerPixel(geom.pf_8bpp));
    try std.testing.expectEqual(@as(u8, 8), geom.bitsPerPixel(0xFF));
}

test "imageBytes packs each row to a byte boundary and rejects empty areas" {
    const a = geom.Area{ .x = 0, .y = 0, .width = 3, .height = 5 };
    try std.testing.expectEqual(@as(usize, 5), try geom.imageBytes(a, geom.pf_1bpp));
    try std.testing.expectEqual(@as(usize, 10), try geom.imageBytes(a, geom.pf_4bpp));
    try std.testing.expectEqual(@as(usize, 15), try geom.imageBytes(a, geom.pf_8bpp));
    const empty = geom.Area{ .x = 0, .y = 0, .width = 0, .height = 5 };
    try std.testing.expectError(error.ZeroSize, geom.imageBytes(empty, geom.pf_4bpp));
}

test "alignArea grows 1 bpp windows onto the 32 px grid and clamps to the panel" {
    var a = geom.Area{ .x = 40, .y = 0, .width = 10, .height = 1 };
    try std.testing.expect(!geom.isAligned(a, geom.pf_1bpp));
    try std.testing.expect(geom.isAligned(a, geom.pf_4bpp));
    try geom.alignArea(&a, geom.pf_1bpp, 1448);
    try std.testing.expectEqual(@as(u16, 32), a.x);
    try std.testing.expectEqual(@as(u16, 32), a.width);
    try std.testing.expect(geom.isAligned(a, geom.pf_1bpp));
    var edge = geom.Area{ .x = 1440, .y = 0, .width = 8, .height = 1 };
    try std.testing.expectError(error.OffGrid, geom.alignArea(&edge, geom.pf_1bpp, 1448));
    var past = geom.Area{ .x = 1440, .y = 0, .width = 9, .height = 1 };
    try std.testing.expectError(error.OutOfPanel, geom.alignArea(&past, geom.pf_4bpp, 1448));
    try std.testing.expectError(error.OutOfPanel, geom.alignArea(&past, geom.pf_4bpp, 0));
}

test "waveformForLut picks A2 mode 4 for M641 and 6 otherwise" {
    try std.testing.expectEqual(geom.Waveform{ .init = 0, .du = 1, .gc16 = 2, .a2 = 4 }, geom.waveformForLut("M641_V1"));
    try std.testing.expectEqual(@as(u8, 6), geom.waveformForLut("M64").a2);
    try std.testing.expectEqual(@as(u8, 6), geom.waveformForLut("M841").a2);
}

test "validateCfg accepts a sane panel and rejects bad bus, size or waveform" {
    var c = goodCfg();
    try geom.validateCfg(&c);
    c.bus.xfer8 = null;
    try std.testing.expectError(error.Invalid, geom.validateCfg(&c));
    c = goodCfg();
    c.panel_width = 4097;
    try std.testing.expectError(error.Invalid, geom.validateCfg(&c));
    c = goodCfg();
    c.waveform.du = 0;
    try std.testing.expectError(error.Invalid, geom.validateCfg(&c));
    c = goodCfg();
    c.waveform.init = 8;
    try std.testing.expectError(error.Invalid, geom.validateCfg(&c));
}
