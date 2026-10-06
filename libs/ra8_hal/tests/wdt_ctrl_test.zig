//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/wdt_ctrl.zig (RA8FW-891).

const std = @import("std");
const ctrl = @import("wdt_ctrl");

const base: ctrl.Cfg = .{ .timeout = 1, .clock_div = 0xF, .window_start = 3, .window_end = 3, .on_expiry = 1, .stop_in_sleep = 1 };

test "cfg mirrors ra8_wdt_cfg_t" {
    try std.testing.expectEqual(@as(usize, 6), @sizeOf(ctrl.Cfg));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(ctrl.Cfg, "on_expiry"));
}

test "validation rejects a bad CKS or TOPS" {
    try std.testing.expect(ctrl.valid(base));
    var c = base;
    c.clock_div = 0x2;
    try std.testing.expect(!ctrl.valid(c));
    c = base;
    c.timeout = 4;
    try std.testing.expect(!ctrl.valid(c));
}

test "WDTCR packs TOPS, CKS, RPES and RPSS" {
    try std.testing.expectEqual(@as(u16, 0x33F1), ctrl.packWdtcr(base));
    var c = base;
    c.timeout = 0;
    c.clock_div = 0x1;
    c.window_start = 0;
    c.window_end = 2;
    try std.testing.expectEqual(@as(u16, 0x0210), ctrl.packWdtcr(c));
}

test "RSTIRQS and SLCSTP follow the cfg" {
    try std.testing.expectEqual(@as(u8, 0x80), ctrl.rcr(base));
    try std.testing.expectEqual(@as(u8, 0x80), ctrl.cstpr(base));
    var c = base;
    c.on_expiry = 0;
    c.stop_in_sleep = 0;
    try std.testing.expectEqual(@as(u8, 0), ctrl.rcr(c));
    try std.testing.expectEqual(@as(u8, 0), ctrl.cstpr(c));
}

test "a blocking clear names only UNDFF and REFEF" {
    try std.testing.expect(ctrl.clearMaskValid(0xC000));
    try std.testing.expect(ctrl.clearMaskValid(0));
    try std.testing.expect(!ctrl.clearMaskValid(0x2000));
}
