//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/tsn.zig.

const std = @import("std");
const tsn = @import("tsn");

test "Config mirrors ra8_tsn_config_t" {
    try std.testing.expectEqual(@as(usize, 6), @sizeOf(tsn.Config));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(tsn.Config, "stab_us"));
}

test "configOk accepts only the factory references and a 30 us floor" {
    try std.testing.expect(tsn.configOk(.{ .high_ref_degc = 125, .low_ref_degc = -40, .stab_us = 30 }));
    try std.testing.expect(tsn.configOk(.{ .high_ref_degc = 105, .low_ref_degc = -40, .stab_us = 100 }));
    try std.testing.expect(!tsn.configOk(.{ .high_ref_degc = 95, .low_ref_degc = -40, .stab_us = 30 }));
    try std.testing.expect(!tsn.configOk(.{ .high_ref_degc = 125, .low_ref_degc = -20, .stab_us = 30 }));
    try std.testing.expect(!tsn.configOk(.{ .high_ref_degc = 125, .low_ref_degc = -40, .stab_us = 29 }));
}

test "maskCode keeps 12 bits" {
    try std.testing.expectEqual(@as(u16, 0x0ABC), tsn.maskCode(0xFABC));
}

test "convert hits both trim points exactly" {
    try std.testing.expectEqual(@as(i32, 125_000), tsn.convert(3000, 3000, 1000, 125, -40).?);
    try std.testing.expectEqual(@as(i32, -40_000), tsn.convert(1000, 3000, 1000, 125, -40).?);
}

test "convert interpolates and masks the trim words" {
    // Midpoint code 2000 sits halfway between -40 and 125: 42.5 degC.
    try std.testing.expectEqual(@as(i32, 42_500), tsn.convert(2000, 0xF000_0BB8, 0x0000_03E8, 125, -40).?);
}

test "convert truncates toward zero like the C" {
    const want: i64 = @divTrunc((tsnUv(1) - tsnUv(3000)) * (125_000 + 40_000), tsnUv(3000) - tsnUv(1001)) + 125_000;
    try std.testing.expectEqual(@as(i32, @intCast(want)), tsn.convert(1, 3000, 1001, 125, -40).?);
}

fn tsnUv(code: i64) i64 {
    return @divTrunc(3_300_000 * code, 4096);
}

test "convert refuses equal trim codes" {
    try std.testing.expect(tsn.convert(100, 0x1234, 0xF234, 125, -40) == null);
}
