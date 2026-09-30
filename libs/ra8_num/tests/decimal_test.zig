//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The conversion itself, checked against values whose binary64 neighbours are
//! known exactly: round-trip decimals, the two halfway cases that prove ties
//! go to even, the subnormal floor, and every refusal.

const std = @import("std");
const decimal = @import("decimal");

fn expectExact(expected: f64, mantissa: u64, scale: i32, negative: bool) !void {
    const actual = try decimal.toBinary64(mantissa, scale, negative);
    try std.testing.expectEqual(@as(u64, @bitCast(expected)), @as(u64, @bitCast(actual)));
}

test "integers convert exactly" {
    try expectExact(1.0, 1, 0, false);
    try expectExact(-1.0, 1, 0, true);
    try expectExact(10.0, 1, 1, false);
    try expectExact(1000000.0, 1, 6, false);
    try expectExact(9007199254740992.0, 9007199254740992, 0, false);
}

test "a zero mantissa is a signed zero at any accepted scale" {
    try expectExact(0.0, 0, 0, false);
    try expectExact(-0.0, 0, 0, true);
    try expectExact(0.0, 0, 400, false);
    try expectExact(-0.0, 0, -400, true);
}

test "negative scales round correctly to the nearest binary64" {
    try expectExact(0.1, 1, -1, false);
    try expectExact(0.5, 5, -1, false);
    try expectExact(1.5, 15, -1, false);
    try expectExact(3.1415926535897932, 31415926535897932, -16, false);
    try expectExact(2.2250738585072014e-308, 22250738585072014, -324, false);
}

test "ties round to even in both directions" {
    // Both are exactly halfway between two representable binary64 values.
    // 2^53 + 1 rounds down to 2^53; 2^53 + 3 rounds up to 2^53 + 4.
    try expectExact(9007199254740992.0, 9007199254740993, 0, false);
    try expectExact(9007199254740996.0, 9007199254740995, 0, false);
}

test "the value just under a power of two picks the lower exponent" {
    try expectExact(0.9999999999999999, 9999999999999999, -16, false);
    try expectExact(1.0000000000000002, 10000000000000002, -16, false);
}

test "subnormals convert, including the smallest one" {
    try expectExact(std.math.floatTrueMin(f64), 5, -324, false);
    try expectExact(-std.math.floatTrueMin(f64), 5, -324, true);
    try expectExact(std.math.floatMin(f64), 22250738585072014, -324, false);
}

test "a scale past the bound is refused before any work happens" {
    try std.testing.expectError(decimal.Refusal.ScaleOutOfRange, decimal.toBinary64(1, 401, false));
    try std.testing.expectError(decimal.Refusal.ScaleOutOfRange, decimal.toBinary64(1, -401, false));
    try std.testing.expectError(decimal.Refusal.ScaleOutOfRange, decimal.toBinary64(0, 401, false));
}

test "overflow and underflow are refusals, never infinity or zero" {
    try std.testing.expect(decimal.toBinary64(1, 309, false) catch null == null);
    try std.testing.expect(decimal.toBinary64(1, -400, false) catch null == null);
    try expectExact(std.math.floatMax(f64), 17976931348623157, 292, false);
}

test "the bounds are the ones the public header publishes" {
    try std.testing.expectEqual(@as(i32, 400), decimal.bounds.scale_max);
    try std.testing.expectEqual(@as(i32, 17), decimal.bounds.digits_max);
}
