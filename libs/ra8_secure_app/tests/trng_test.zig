//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The xorshift64* stand-in is deterministic on purpose, so the byte order it
//! emits is testable and must not drift: the C fanned each 64-bit word out
//! low byte first.

const std = @import("std");
const trng = @import("trng");

test "the first bytes are the seeded word, low byte first" {
    try std.testing.expectEqual(trng.Err.ok, trng.reset());

    var out: [8]u8 = undefined;
    try std.testing.expectEqual(trng.Err.ok, trng.read(&out));

    // Recompute the C's first word: xorshift64*(golden-ratio seed).
    var x: u64 = 0x9E3779B97F4A7C15;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    const word = x *% 0x2545F4914F6CDD1D;

    var expected: [8]u8 = undefined;
    std.mem.writeInt(u64, &expected, word, .little);
    try std.testing.expectEqualSlices(u8, &expected, &out);
}

test "reset makes the stream reproducible" {
    var first: [64]u8 = undefined;
    var second: [64]u8 = undefined;

    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    try std.testing.expectEqual(trng.Err.ok, trng.read(&first));
    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    try std.testing.expectEqual(trng.Err.ok, trng.read(&second));

    try std.testing.expectEqualSlices(u8, &first, &second);
}

test "successive reads advance the state" {
    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    var first: [32]u8 = undefined;
    var second: [32]u8 = undefined;
    try std.testing.expectEqual(trng.Err.ok, trng.read(&first));
    try std.testing.expectEqual(trng.Err.ok, trng.read(&second));
    try std.testing.expect(!std.mem.eql(u8, &first, &second));
}

test "a partial word is the prefix of the whole word" {
    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    var whole: [8]u8 = undefined;
    try std.testing.expectEqual(trng.Err.ok, trng.read(&whole));

    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    var partial: [3]u8 = undefined;
    try std.testing.expectEqual(trng.Err.ok, trng.read(&partial));

    try std.testing.expectEqualSlices(u8, whole[0..3], &partial);
}

test "a length that is not a multiple of eight still fills exactly" {
    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    var out: [13]u8 = .{0} ** 13;
    try std.testing.expectEqual(trng.Err.ok, trng.read(&out));

    var nonzero: usize = 0;
    for (out) |b| {
        if (b != 0) nonzero += 1;
    }
    try std.testing.expect(nonzero > 0);
}

test "zero length and over-cap length are rejected" {
    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    var empty: [0]u8 = undefined;
    try std.testing.expectEqual(trng.Err.invalid_arg, trng.read(&empty));

    var over: [trng.Limits.max_bytes + 1]u8 = undefined;
    try std.testing.expectEqual(trng.Err.invalid_arg, trng.read(&over));
}

test "the cap itself is accepted" {
    try std.testing.expectEqual(trng.Err.ok, trng.reset());
    var at_cap: [trng.Limits.max_bytes]u8 = undefined;
    try std.testing.expectEqual(trng.Err.ok, trng.read(&at_cap));
}
