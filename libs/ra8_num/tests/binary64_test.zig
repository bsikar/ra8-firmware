//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bit assembly on its own: the carry out of a full significand, the two
//! range refusals, and the subnormal boundary where a rounded-up subnormal
//! becomes the smallest normal.

const std = @import("std");
const binary64 = @import("binary64");

const format = binary64.format;

test "signed zero carries only the sign" {
    try std.testing.expectEqual(@as(u64, 0), @as(u64, @bitCast(binary64.signedZero(false))));
    try std.testing.expectEqual(
        @as(u64, 1) << format.sign_shift,
        @as(u64, @bitCast(binary64.signedZero(true))),
    );
}

test "a normal significand encodes with the hidden bit removed" {
    const one = try binary64.encode(format.hidden_bit, 0, false, .normal);
    try std.testing.expectEqual(@as(f64, 1.0), one);

    const negative = try binary64.encode(format.hidden_bit, 0, true, .normal);
    try std.testing.expectEqual(@as(f64, -1.0), negative);

    const two = try binary64.encode(format.hidden_bit, 1, false, .normal);
    try std.testing.expectEqual(@as(f64, 2.0), two);
}

test "a carry out of 53 bits normalizes once and bumps the exponent" {
    const carried = try binary64.encode(format.carry_bit, 0, false, .normal);
    try std.testing.expectEqual(@as(f64, 2.0), carried);
}

test "a carry at the top of the range overflows rather than wrapping" {
    try std.testing.expectError(
        binary64.Range.OutOfRange,
        binary64.encode(format.carry_bit, format.exponent_max, false, .normal),
    );
}

test "a normal significand below the hidden bit is refused" {
    try std.testing.expectError(
        binary64.Range.OutOfRange,
        binary64.encode(format.hidden_bit - 1, 0, false, .normal),
    );
}

test "the smallest subnormal and the largest one both encode" {
    const smallest = try binary64.encode(1, 0, false, .subnormal);
    try std.testing.expectEqual(std.math.floatTrueMin(f64), smallest);

    // Rounding a subnormal all the way up lands exactly on the smallest normal.
    const boundary = try binary64.encode(format.hidden_bit, 0, false, .subnormal);
    try std.testing.expectEqual(std.math.floatMin(f64), boundary);
}

test "a zero or oversized subnormal significand is refused" {
    try std.testing.expectError(
        binary64.Range.OutOfRange,
        binary64.encode(0, 0, false, .subnormal),
    );
    try std.testing.expectError(
        binary64.Range.OutOfRange,
        binary64.encode(format.hidden_bit + 1, 0, false, .subnormal),
    );
}
