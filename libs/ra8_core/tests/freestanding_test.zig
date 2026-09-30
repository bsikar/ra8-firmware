//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The freestanding primitives against the answers the C standard fixes.
//!
//! These check the edges the naive implementations are easy to get wrong:
//! zero lengths, overlap in both directions, the terminator being a valid
//! search target, `strncpy` leaving no terminator, and the two functions
//! whose return values differ from each other on purpose.

const std = @import("std");
const testing = std.testing;

const math = @import("freestanding_math");
const mem = @import("freestanding_mem");
const rng = @import("freestanding_rand");
const str = @import("freestanding_str");

test "set fills the whole span and nothing past it" {
    var buffer = [_]u8{ 1, 2, 3, 4, 5, 6 };
    mem.set(buffer[1..4], 0xAB);
    try testing.expectEqualSlices(u8, &[_]u8{ 1, 0xAB, 0xAB, 0xAB, 5, 6 }, &buffer);
}

test "set of an empty span touches nothing" {
    var buffer = [_]u8{ 7, 7 };
    mem.set(buffer[0..0], 0);
    try testing.expectEqualSlices(u8, &[_]u8{ 7, 7 }, &buffer);
}

test "copy moves every byte" {
    var dst = [_]u8{0} ** 5;
    mem.copy(&dst, "hello");
    try testing.expectEqualSlices(u8, "hello", &dst);
}

test "move handles a forward overlap" {
    var buffer = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    mem.move(buffer[0..5], buffer[3..8]);
    try testing.expectEqualSlices(u8, &[_]u8{ 4, 5, 6, 7, 8, 6, 7, 8 }, &buffer);
}

test "move handles a backward overlap" {
    var buffer = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    mem.move(buffer[3..8], buffer[0..5]);
    try testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3, 1, 2, 3, 4, 5 }, &buffer);
}

test "move onto itself is a no-op" {
    var buffer = [_]u8{ 9, 8, 7 };
    mem.move(&buffer, &buffer);
    try testing.expectEqualSlices(u8, &[_]u8{ 9, 8, 7 }, &buffer);
}

test "compare returns the sign only, never the byte difference" {
    try testing.expectEqual(@as(i32, 0), mem.compare("abc", "abc"));
    try testing.expectEqual(@as(i32, -1), mem.compare("abc", "abd"));
    try testing.expectEqual(@as(i32, 1), mem.compare("abd", "abc"));
    // 'z' - 'a' is 25, and memcmp must still answer 1.
    try testing.expectEqual(@as(i32, 1), mem.compare("z", "a"));
    try testing.expectEqual(@as(i32, 0), mem.compare("", ""));
}

test "indexOf finds the first match, including a zero byte" {
    try testing.expectEqual(@as(?usize, 2), mem.indexOf("abcabc", 'c'));
    try testing.expectEqual(@as(?usize, null), mem.indexOf("abc", 'z'));
    try testing.expectEqual(@as(?usize, 1), mem.indexOf(&[_]u8{ 'a', 0, 'b' }, 0));
    try testing.expectEqual(@as(?usize, null), mem.indexOf("", 'a'));
}

test "length stops at the terminator" {
    try testing.expectEqual(@as(usize, 0), str.length(""));
    try testing.expectEqual(@as(usize, 5), str.length("hello"));
}

test "lengthBounded never reads past max" {
    try testing.expectEqual(@as(usize, 3), str.lengthBounded("abc", 10));
    try testing.expectEqual(@as(usize, 2), str.lengthBounded("abc", 2));
    try testing.expectEqual(@as(usize, 0), str.lengthBounded("abc", 0));
}

test "compare returns the byte difference, unlike memcmp" {
    try testing.expectEqual(@as(i32, 0), str.compare("abc", "abc"));
    try testing.expectEqual(@as(i32, 25), str.compare("z", "a"));
    try testing.expectEqual(@as(i32, -25), str.compare("a", "z"));
    // A shorter prefix loses by the whole value of the next byte.
    try testing.expectEqual(@as(i32, -'c'), str.compare("ab", "abc"));
}

test "compare treats bytes as unsigned" {
    // 0x80 as a signed char would be negative and invert the comparison.
    const high = [_:0]u8{0x80};
    const low = [_:0]u8{0x01};
    try testing.expect(str.compare(&high, &low) > 0);
}

test "compareBounded stops at n or at a shared terminator" {
    try testing.expectEqual(@as(i32, 0), str.compareBounded("abc", "abd", 2));
    try testing.expectEqual(@as(i32, -1), str.compareBounded("abc", "abd", 3));
    try testing.expectEqual(@as(i32, 0), str.compareBounded("ab", "ab", 100));
}

test "indexOfChar finds the terminator when asked for it" {
    try testing.expectEqual(@as(?usize, 1), str.indexOfChar("abc", 'b'));
    try testing.expectEqual(@as(?usize, null), str.indexOfChar("abc", 'z'));
    try testing.expectEqual(@as(?usize, 3), str.indexOfChar("abc", 0));
    try testing.expectEqual(@as(?usize, 0), str.indexOfChar("", 0));
}

test "lastIndexOfChar finds the final match" {
    try testing.expectEqual(@as(?usize, 3), str.lastIndexOfChar("abab", 'b'));
    try testing.expectEqual(@as(?usize, null), str.lastIndexOfChar("abab", 'z'));
    try testing.expectEqual(@as(?usize, 4), str.lastIndexOfChar("abab", 0));
}

test "indexOfString matches an empty needle at the start" {
    try testing.expectEqual(@as(?usize, 0), str.indexOfString("abc", ""));
    try testing.expectEqual(@as(?usize, 0), str.indexOfString("", ""));
    try testing.expectEqual(@as(?usize, null), str.indexOfString("", "a"));
    try testing.expectEqual(@as(?usize, 2), str.indexOfString("abcd", "cd"));
    try testing.expectEqual(@as(?usize, null), str.indexOfString("abcd", "cde"));
    try testing.expectEqual(@as(?usize, 3), str.indexOfString("aaab", "b"));
}

test "indexOfString does not run off the end on a partial tail match" {
    try testing.expectEqual(@as(?usize, null), str.indexOfString("abc", "abcd"));
}

test "copy writes the terminator" {
    var dst = [_]u8{0xFF} ** 8;
    str.copy(&dst, "abc");
    try testing.expectEqualSlices(u8, &[_]u8{ 'a', 'b', 'c', 0, 0xFF, 0xFF, 0xFF, 0xFF }, &dst);
}

test "copyBounded pads short sources and truncates long ones" {
    var dst = [_]u8{0xFF} ** 6;
    str.copyBounded(&dst, "ab", 6);
    try testing.expectEqualSlices(u8, &[_]u8{ 'a', 'b', 0, 0, 0, 0 }, &dst);

    var tight = [_]u8{0xFF} ** 3;
    str.copyBounded(&tight, "abcdef", 3);
    // No terminator: exactly what strncpy does when it runs out of room.
    try testing.expectEqualSlices(u8, &[_]u8{ 'a', 'b', 'c' }, &tight);
}

test "copyBounded with n of zero writes nothing" {
    var dst = [_]u8{0xFF} ** 2;
    str.copyBounded(&dst, "abc", 0);
    try testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xFF }, &dst);
}

test "absolute folds negatives and leaves the minimum alone" {
    try testing.expectEqual(@as(i32, 5), math.absolute(5));
    try testing.expectEqual(@as(i32, 5), math.absolute(-5));
    try testing.expectEqual(@as(i32, 0), math.absolute(0));
    // No representable answer; the C wrapped and so does this.
    try testing.expectEqual(std.math.minInt(i32), math.absolute(std.math.minInt(i32)));
}

test "the errno slot is one stable location" {
    const first = &math.errno_slot;
    math.errno_slot = 7;
    try testing.expectEqual(@as(i32, 7), first.*);
    math.errno_slot = 0;
}

test "a zero seed is remapped so the generator still advances" {
    rng.seed(0);
    try testing.expect(rng.next() != 0);
}

test "the same seed replays the same sequence" {
    rng.seed(42);
    const first = rng.next();
    rng.seed(42);
    try testing.expectEqual(first, rng.next());
}

test "different seeds diverge" {
    rng.seed(1);
    const one = rng.next();
    rng.seed(2);
    try testing.expect(one != rng.next());
}

test "each draw advances the state" {
    rng.seed(1);
    const a = rng.next();
    const b = rng.next();
    const c = rng.next();
    try testing.expect(a != b or b != c);
}

test "every draw stays inside RAND_MAX" {
    rng.seed(7);
    for (0..64) |_| {
        try testing.expect(rng.next() <= rng.rand_max);
    }
}
