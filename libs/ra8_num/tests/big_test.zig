//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fixed-capacity integer on its own: normalization, bit length, and the
//! three capacity boundaries the conversion relies on to refuse rather than
//! truncate.

const std = @import("std");
const Big = @import("big");

test "a u64 normalizes to one or two words" {
    try std.testing.expectEqual(@as(u8, 1), Big.fromU64(0).used);
    try std.testing.expectEqual(@as(u8, 1), Big.fromU64(0xFFFF_FFFF).used);
    try std.testing.expectEqual(@as(u8, 2), Big.fromU64(0x1_0000_0000).used);
    try std.testing.expect(Big.fromU64(0).isZero());
    try std.testing.expect(!Big.fromU64(1).isZero());
}

test "bit length is the significant bit count, and zero has none" {
    try std.testing.expectEqual(@as(u16, 0), Big.fromU64(0).bitLength());
    try std.testing.expectEqual(@as(u16, 1), Big.fromU64(1).bitLength());
    try std.testing.expectEqual(@as(u16, 32), Big.fromU64(0x8000_0000).bitLength());
    try std.testing.expectEqual(@as(u16, 64), Big.fromU64(std.math.maxInt(u64)).bitLength());
}

test "multiply carries into a new word and refuses past capacity" {
    var value = Big.fromU64(std.math.maxInt(u64));
    try value.mulSmall(5);
    try std.testing.expectEqual(@as(u8, 3), value.used);

    // 48 words of 5s: the next factor has nowhere to carry into.
    var full = Big.fromU64(1);
    var grew: u32 = 0;
    while (full.used < Big.limits.words) : (grew += 1) {
        try full.mulSmall(0xFFFF_FFFF);
    }
    try std.testing.expectError(Big.Overflow.Capacity, full.mulSmall(0xFFFF_FFFF));
}

test "shift is exact across a word boundary and refuses past capacity" {
    const one = Big.fromU64(1);
    const shifted = try one.shiftLeft(33);
    try std.testing.expectEqual(@as(u16, 34), shifted.bitLength());
    try std.testing.expectEqual(@as(u32, 2), shifted.word[1]);

    // A whole-word shift must contribute no carry from the zero bit count.
    const word = try one.shiftLeft(32);
    try std.testing.expectEqual(@as(u8, 2), word.used);
    try std.testing.expectEqual(@as(u32, 0), word.word[0]);
    try std.testing.expectEqual(@as(u32, 1), word.word[1]);

    try std.testing.expectError(Big.Overflow.Capacity, one.shiftLeft(Big.limits.words * 32));
}

test "zero shifts to zero and stays normalized" {
    const shifted = try Big.fromU64(0).shiftLeft(100);
    try std.testing.expect(shifted.isZero());
    try std.testing.expectEqual(@as(u8, 1), shifted.used);
}

test "ordering compares by length first, then from the high word down" {
    const small = Big.fromU64(5);
    const large = Big.fromU64(0x1_0000_0000);
    try std.testing.expectEqual(std.math.Order.lt, small.order(&large));
    try std.testing.expectEqual(std.math.Order.gt, large.order(&small));
    try std.testing.expectEqual(std.math.Order.eq, small.order(&Big.fromU64(5)));
}

test "subtraction borrows across words and renormalizes" {
    var value = Big.fromU64(0x1_0000_0000);
    value.subtract(&Big.fromU64(1));
    try std.testing.expectEqual(@as(u8, 1), value.used);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), value.word[0]);

    var exact = Big.fromU64(42);
    exact.subtract(&Big.fromU64(42));
    try std.testing.expect(exact.isZero());
}
