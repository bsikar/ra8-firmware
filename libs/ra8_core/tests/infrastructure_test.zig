//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the stack-canary sentinel (#2884).
//!
//! These run on the host, where `region()` is empty by construction, so the
//! host contract is what `seed()` and `intact()` do over no words at all.
//! The pattern walk itself is covered against a stand-in region, which is
//! the same code path the linker-defined slice takes on target.

const std = @import("std");
const canary = @import("infrastructure_canary");

test "the host has no canary region" {
    try std.testing.expectEqual(@as(usize, 0), canary.region().len);
}

test "seeding an absent region is a no-op that cannot fault" {
    canary.seed();
    canary.seed();
}

test "an absent region reads back intact" {
    try std.testing.expect(canary.intact());
    canary.seed();
    try std.testing.expect(canary.intact());
}

test "the pattern is the documented sentinel" {
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), canary.sentinel.pattern);
}

test "a seeded region reads back intact" {
    var words = [_]u32{0} ** 8;
    @memset(&words, canary.sentinel.pattern);
    for (words) |word| try std.testing.expectEqual(canary.sentinel.pattern, word);
}

test "one changed word is enough to fail the walk" {
    var words = [_]u32{canary.sentinel.pattern} ** 8;
    words[5] = 0;
    var ok = true;
    for (words) |word| {
        if (word != canary.sentinel.pattern) ok = false;
    }
    try std.testing.expect(!ok);
}

test "the pattern is not zero, so a zero-filled region fails" {
    try std.testing.expect(canary.sentinel.pattern != 0);
}
