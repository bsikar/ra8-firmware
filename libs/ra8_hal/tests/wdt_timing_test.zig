//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/wdt_timing.zig (RA8FW-889).

const std = @import("std");
const timing = @import("wdt_timing");

test "TOPS maps to counter cycles" {
    const want = [_]u16{ 1024, 4096, 8192, 16384 };
    for (want, 0..) |c, i| try std.testing.expectEqual(@as(?u16, c), timing.cycles(@intCast(i)));
    try std.testing.expectEqual(@as(?u16, null), timing.cycles(4));
}

test "only the six CKS encodings are legal" {
    const pairs = [_][2]u16{ .{ 0x1, 4 }, .{ 0x4, 64 }, .{ 0xF, 128 }, .{ 0x6, 512 }, .{ 0x7, 2048 }, .{ 0x8, 8192 } };
    for (pairs) |p| try std.testing.expectEqual(@as(?u16, p[1]), timing.divisor(@intCast(p[0])));
    var legal: u8 = 0;
    for (0..256) |v| {
        if (timing.divisor(@intCast(v)) != null) legal += 1;
    }
    try std.testing.expectEqual(@as(u8, 6), legal);
}

test "total multiplies and rejects either bad input" {
    try std.testing.expectEqual(@as(?u32, 134_217_728), timing.total(3, 0x8));
    try std.testing.expectEqual(@as(?u32, 4096), timing.total(0, 0x1));
    try std.testing.expectEqual(@as(?u32, null), timing.total(9, 0x1));
    try std.testing.expectEqual(@as(?u32, null), timing.total(0, 0x2));
}
