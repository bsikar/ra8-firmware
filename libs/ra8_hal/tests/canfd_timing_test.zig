//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_timing.zig.

const std = @import("std");
const timing = @import("canfd_timing");

test "channelBase maps channels 0 and 1 only" {
    try std.testing.expectEqual(@as(?usize, 0x4038_0000), timing.channelBase(0));
    try std.testing.expectEqual(@as(?usize, 0x4038_2000), timing.channelBase(1));
    try std.testing.expectEqual(@as(?usize, null), timing.channelBase(2));
}

test "solve prefers the largest exact time-quanta count" {
    // 120 MHz / (500 kbit * 24) = 10 (25 does not divide) -> tq 24.
    const t = timing.solve(120_000_000, 500_000, timing.prescaler_max).?;
    try std.testing.expectEqual(timing.Timing{ .prescaler = 10, .tseg1 = 17, .tseg2 = 6, .sjw = 6 }, t);
}

test "solve caps SJW at 16 and the prescaler at the ceiling" {
    try std.testing.expectEqual(@as(u32, 6), timing.solve(100_000_000, 1_000_000, 1024).?.sjw);
    try std.testing.expectEqual(@as(?timing.Timing, null), timing.solve(120_000_000, 1_000, 256));
}

test "solve rejects zero inputs and inexact clocks" {
    try std.testing.expectEqual(@as(?timing.Timing, null), timing.solve(0, 500_000, 1024));
    try std.testing.expectEqual(@as(?timing.Timing, null), timing.solve(120_000_000, 0, 1024));
    try std.testing.expectEqual(@as(?timing.Timing, null), timing.solve(1_000_003, 1_000, 1024));
}

test "packNcfg places BRP, SJW, TSEG1 and TSEG2" {
    const t = timing.Timing{ .prescaler = 10, .tseg1 = 17, .tseg2 = 6, .sjw = 6 };
    try std.testing.expectEqual(@as(u32, 9 | (5 << 10) | (17 << 17) | (6 << 25)), timing.packNcfg(t));
}

test "packDcfg masks to the narrower data-phase fields" {
    const t = timing.Timing{ .prescaler = 2, .tseg1 = 0x3F, .tseg2 = 0x1F, .sjw = 0x11 };
    try std.testing.expectEqual(@as(u32, 1 | (0x1F << 8) | (0xF << 16) | (0 << 24)), timing.packDcfg(t));
}
