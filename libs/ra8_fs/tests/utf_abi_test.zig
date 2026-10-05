//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! UTF-8 <-> UTF-16LE name codec (RA8FW-747).

const std = @import("std");
const fs = @import("ra8_fs");
const utf = fs.utf;
comptime {
    _ = @import("fs_walker_fake.zig");
}

fn toUtf16(s: [:0]const u8, out: []u16) !u32 {
    var n: u32 = 0;
    const err = utf.priv_utf8_to_utf16(s.ptr, out.ptr, @intCast(out.len), &n);
    if (err != utf.ok) return error.Codec;
    return n;
}

test "ASCII, 2-, 3- and 4-byte forms round-trip, matching std" {
    const names = [_][:0]const u8{ "README.TXT", "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC", "a\xF0\x9F\x98\x80b" };
    for (names) |s| {
        var u16buf: [16]u16 = undefined;
        const n = try toUtf16(s, &u16buf);
        var ref: [16]u16 = undefined;
        const rn = try std.unicode.utf8ToUtf16Le(&ref, s);
        try std.testing.expectEqualSlices(u16, ref[0..rn], u16buf[0..n]);
        var back: [32]u8 = undefined;
        try std.testing.expectEqual(utf.ok, utf.priv_utf16_to_utf8(&u16buf, n, &back, back.len));
        try std.testing.expectEqualStrings(s, std.mem.sliceTo(&back, 0));
    }
}

test "malformed UTF-8 is refused" {
    const bad = [_][:0]const u8{ "\x80", "\xC3", "\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF8\x88\x80\x80\x80", "\xC3(" };
    for (bad) |s| {
        var out: [8]u16 = undefined;
        var n: u32 = 99;
        try std.testing.expectEqual(utf.err_invalid_arg, utf.priv_utf8_to_utf16(s.ptr, &out, out.len, &n));
        try std.testing.expectEqual(@as(u32, 0), n);
    }
}

test "decoder overflow and null arguments" {
    var out: [2]u16 = undefined;
    var n: u32 = 0;
    try std.testing.expectEqual(utf.err_no_mem, utf.priv_utf8_to_utf16("abc", &out, out.len, &n));
    try std.testing.expectEqual(utf.err_no_mem, utf.priv_utf8_to_utf16("a\xF0\x9F\x98\x80", &out, out.len, &n));
    try std.testing.expectEqual(utf.ok, utf.priv_utf8_to_utf16("ab", &out, out.len, &n));
    try std.testing.expectEqual(@as(u32, 2), n);
    try std.testing.expectEqual(utf.err_null_ptr, utf.priv_utf8_to_utf16(null, &out, 2, &n));
    try std.testing.expectEqual(utf.err_null_ptr, utf.priv_utf8_to_utf16("a", null, 2, &n));
    try std.testing.expectEqual(utf.err_null_ptr, utf.priv_utf8_to_utf16("a", &out, 2, null));
}

test "unpaired surrogates are refused by the encoder" {
    const cases = [_][]const u16{ &.{0xDC00}, &.{ 'a', 0xD800 }, &.{ 0xD800, 'b' } };
    for (cases) |u| {
        var out: [16]u8 = .{0xAA} ** 16;
        try std.testing.expectEqual(utf.err_invalid_arg, utf.priv_utf16_to_utf8(u.ptr, @intCast(u.len), &out, out.len));
    }
}

test "encoder keeps room for the NUL and empties out on overflow" {
    const u = [_]u16{ 'a', 0x00E9 };
    var out: [3]u8 = undefined;
    try std.testing.expectEqual(utf.err_no_mem, utf.priv_utf16_to_utf8(&u, 2, &out, out.len));
    try std.testing.expectEqual(@as(u8, 0), out[0]);
    var fit: [4]u8 = undefined;
    try std.testing.expectEqual(utf.ok, utf.priv_utf16_to_utf8(&u, 2, &fit, fit.len));
    try std.testing.expectEqualStrings("a\xC3\xA9", std.mem.sliceTo(&fit, 0));
    try std.testing.expectEqual(utf.err_null_ptr, utf.priv_utf16_to_utf8(&u, 2, &fit, 0));
    try std.testing.expectEqual(utf.ok, utf.priv_utf16_to_utf8(&u, 0, &fit, 1));
    try std.testing.expectEqual(@as(u8, 0), fit[0]);
}

test "case-folded compare and the ASCII predicate" {
    const a = [_]u16{ 'R', 'e', 'A', 'd' };
    const b = [_]u16{ 'r', 'E', 'a', 'D' };
    const d = [_]u16{ 'r', 'E', 'a', 'X' };
    try std.testing.expectEqual(@as(u8, 1), utf.priv_utf16_ieq(&a, 4, &b, 4));
    try std.testing.expectEqual(@as(u8, 0), utf.priv_utf16_ieq(&a, 4, &d, 4));
    try std.testing.expectEqual(@as(u8, 0), utf.priv_utf16_ieq(&a, 4, &b, 3));
    try std.testing.expectEqual(@as(u8, 1), utf.priv_utf16_all_ascii(&a, 4));
    const e = [_]u16{ 'a', 0x80 };
    try std.testing.expectEqual(@as(u8, 0), utf.priv_utf16_all_ascii(&e, 2));
}
