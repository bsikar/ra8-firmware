// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const cstr = @import("cstr");

test "a short name is copied and terminated" {
    var out: [8]u8 = [_]u8{0xAA} ** 8;
    cstr.write(&out, "abc");
    try std.testing.expectEqualStrings("abc", out[0..3]);
    try std.testing.expectEqual(@as(u8, 0), out[3]);
}

test "a long name is truncated and still terminated" {
    var out: [4]u8 = [_]u8{0xAA} ** 4;
    cstr.write(&out, "abcdefgh");
    try std.testing.expectEqualStrings("abc", out[0..3]);
    try std.testing.expectEqual(@as(u8, 0), out[3]);
}

test "a one byte buffer holds only the terminator" {
    var out: [1]u8 = [_]u8{0xAA} ** 1;
    cstr.write(&out, "abc");
    try std.testing.expectEqual(@as(u8, 0), out[0]);
}

test "an empty buffer is left alone" {
    var out: [0]u8 = undefined;
    cstr.write(&out, "abc");
}
