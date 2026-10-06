//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/spi_b_xfer.zig (RA8FW-899).

const std = @import("std");
const xfer = @import("spi_b_xfer");

test "only the 8/16/32-bit SPB codes are supported" {
    try std.testing.expectEqual(@as(?u8, 1), xfer.unitBytes(xfer.width_8));
    try std.testing.expectEqual(@as(?u8, 2), xfer.unitBytes(xfer.width_16));
    try std.testing.expectEqual(@as(?u8, 4), xfer.unitBytes(xfer.width_32));
    try std.testing.expectEqual(@as(?u8, null), xfer.unitBytes(8));
}

test "SPB replaces only bits 20:16 of SPCMD0" {
    try std.testing.expectEqual(@as(u32, 0x000F_1003), xfer.withWidth(0x0007_1003, xfer.width_16));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), xfer.withWidth(0xFFE0_FFFF, xfer.width_32));
}

test "dummy fill is all ones at the frame width" {
    try std.testing.expectEqual(@as(u32, 0xFF), xfer.dummy(xfer.width_8));
    try std.testing.expectEqual(@as(u32, 0xFFFF), xfer.dummy(xfer.width_16));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), xfer.dummy(xfer.width_32));
}

test "frames load and store at their index like the C arrays" {
    const words = [_]u32{ 0x1122_3344, 0xAABB_CCDD };
    const halves = [_]u16{ 0x0102, 0xBEEF };
    const bytes: [*]const u8 = @ptrCast(&words);
    try std.testing.expectEqual(@as(u32, 0xAABB_CCDD), xfer.load(bytes, 1, xfer.width_32));
    try std.testing.expectEqual(@as(u32, 0xBEEF), xfer.load(@ptrCast(&halves), 1, xfer.width_16));
    var out16 = [_]u16{ 0, 0 };
    xfer.store(@ptrCast(&out16), 1, xfer.width_16, 0x1234_5678);
    try std.testing.expectEqual(@as(u16, 0x5678), out16[1]);
    var out8 = [_]u8{ 0, 0, 0 };
    xfer.store(&out8, 2, xfer.width_8, 0x1A5);
    try std.testing.expectEqual(@as(u8, 0xA5), out8[2]);
}
