//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The exported entry point driven through the C shape: the null destination,
//! the untouched-on-failure promise, and a refusal reported as `false` rather
//! than as a saturated value.

const std = @import("std");
const abi = @import("abi");

test "a converted value is published and reported true" {
    var out: f64 = -1.0;
    try std.testing.expect(abi.ra8_num_decimal_to_binary64(1, -1, false, &out));
    try std.testing.expectEqual(@as(f64, 0.1), out);
}

test "a null destination is refused and calls nothing" {
    try std.testing.expect(!abi.ra8_num_decimal_to_binary64(1, 0, false, null));
}

test "a refusal leaves the caller's destination untouched" {
    const sentinel: f64 = 12345.6789;
    var out: f64 = sentinel;

    try std.testing.expect(!abi.ra8_num_decimal_to_binary64(1, 401, false, &out));
    try std.testing.expectEqual(sentinel, out);

    try std.testing.expect(!abi.ra8_num_decimal_to_binary64(1, 309, false, &out));
    try std.testing.expectEqual(sentinel, out);

    try std.testing.expect(!abi.ra8_num_decimal_to_binary64(1, -400, false, &out));
    try std.testing.expectEqual(sentinel, out);
}

test "a zero mantissa publishes a signed zero and succeeds" {
    var out: f64 = 1.0;
    try std.testing.expect(abi.ra8_num_decimal_to_binary64(0, 0, true, &out));
    try std.testing.expectEqual(@as(u64, 1) << 63, @as(u64, @bitCast(out)));
}

test "no input produces infinity or NaN" {
    var out: f64 = 0.0;
    const scales = [_]i32{ -400, -324, -1, 0, 1, 308, 309, 400 };
    for (scales) |scale| {
        if (abi.ra8_num_decimal_to_binary64(std.math.maxInt(u64), scale, false, &out)) {
            try std.testing.expect(std.math.isFinite(out));
        }
    }
}
