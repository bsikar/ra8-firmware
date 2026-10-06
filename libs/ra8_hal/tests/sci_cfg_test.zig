//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/sci_cfg.zig (RA8FW-905).

const std = @import("std");
const sci = @import("sci_cfg");

fn cfgOf(data_bits: u8, parity: u8, stop_bits: u8) sci.Cfg {
    return .{ .baud = 115200, .data_bits = data_bits, .parity = parity, .stop_bits = stop_bits, .pclk_hz = 60_000_000 };
}

test "brr divides by 32 and saturates" {
    try std.testing.expectEqual(@as(u8, 15), sci.brr(60_000_000, 115200));
    try std.testing.expectEqual(@as(u8, 0), sci.brr(0, 115200));
    try std.testing.expectEqual(@as(u8, 0), sci.brr(60_000_000, 0));
    try std.testing.expectEqual(@as(u8, 0), sci.brr(1000, 115200));
}

test "ccr1 parity bits" {
    try std.testing.expectEqual(@as(u32, 0x30), sci.ccr1(cfgOf(8, 0, 0)));
    try std.testing.expectEqual(@as(u32, 0x130), sci.ccr1(cfgOf(8, 1, 0)));
    try std.testing.expectEqual(@as(u32, 0x330), sci.ccr1(cfgOf(8, 2, 0)));
}

test "ccr2 packs brr and mddr" {
    try std.testing.expectEqual(@as(u32, 0xFF00_0F00), sci.ccr2(cfgOf(8, 0, 0)));
}

test "ccr3 chr and stop" {
    try std.testing.expectEqual(@as(u32, 0x1280), sci.ccr3(cfgOf(8, 0, 0)));
    try std.testing.expectEqual(@as(u32, 0x1380), sci.ccr3(cfgOf(7, 0, 0)));
    try std.testing.expectEqual(@as(u32, 0x5280), sci.ccr3(cfgOf(8, 0, 1)));
}

test "baudCalculate walks cks" {
    const fast = sci.baudCalculate(115200, 60_000_000).?;
    try std.testing.expectEqual(@as(u16, 15), fast.brr);
    try std.testing.expectEqual(@as(u8, 0), fast.cks);
    const slow = sci.baudCalculate(1200, 60_000_000).?;
    try std.testing.expectEqual(@as(u8, 2), slow.cks);
    try std.testing.expectEqual(@as(u16, 96), slow.brr);
    try std.testing.expect(sci.baudCalculate(0xFFFF_FFFF, 1000) == null);
    try std.testing.expect(sci.baudCalculate(0, 60_000_000) == null);
}
