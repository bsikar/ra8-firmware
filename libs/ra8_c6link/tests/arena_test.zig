//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bump, rollback and exhaustion behaviour of the decode arena.

const std = @import("std");
const implementation = @import("implementation");
const Arena = implementation.arena.Arena;

fn fresh(buf: []u8) Arena {
    return .{ .base = buf.ptr, .bytes = @intCast(buf.len), .used = 0, .last = 0 };
}

test "blocks are handed out eight-byte aligned from the front" {
    var buf: [64]u8 align(8) = undefined;
    var a = fresh(&buf);
    const p = a.alloc(3).?;
    const q = a.alloc(8).?;
    try std.testing.expectEqual(@intFromPtr(&buf), @intFromPtr(p));
    try std.testing.expectEqual(@intFromPtr(&buf) + 8, @intFromPtr(q));
    try std.testing.expectEqual(@as(u32, 16), a.used);
    try std.testing.expectEqual(@as(u32, 9), a.last);
}

test "freeing the newest block rolls the offset back" {
    var buf: [64]u8 align(8) = undefined;
    var a = fresh(&buf);
    _ = a.alloc(8).?;
    const q = a.alloc(5).?;
    a.free(q);
    try std.testing.expectEqual(@as(u32, 8), a.used);
    try std.testing.expectEqual(@as(u32, 0), a.last);
}

test "freeing an older block keeps it until reset" {
    var buf: [64]u8 align(8) = undefined;
    var a = fresh(&buf);
    const p = a.alloc(8).?;
    _ = a.alloc(8).?;
    a.free(p);
    try std.testing.expectEqual(@as(u32, 16), a.used);
    a.reset();
    try std.testing.expectEqual(@as(u32, 0), a.used);
    try std.testing.expectEqual(@as(u32, 0), a.last);
}

test "a second free after a rollback, or a null free, is ignored" {
    var buf: [64]u8 align(8) = undefined;
    var a = fresh(&buf);
    _ = a.alloc(8).?;
    const q = a.alloc(8).?;
    a.free(q);
    try std.testing.expectEqual(@as(u32, 8), a.used);
    a.free(q);
    try std.testing.expectEqual(@as(u32, 8), a.used);
    _ = a.alloc(8).?;
    a.free(null);
    try std.testing.expectEqual(@as(u32, 16), a.used);
}

test "a request that fits only without its trailing padding is refused" {
    var buf: [16]u8 align(8) = undefined;
    var a = fresh(&buf);
    _ = a.alloc(9).?;
    try std.testing.expectEqual(@as(u32, 16), a.used);
    try std.testing.expect(a.alloc(1) == null);

    var b = fresh(buf[0..12]);
    _ = b.alloc(1).?;
    try std.testing.expectEqual(@as(u32, 8), b.used);
    try std.testing.expect(b.alloc(4) == null);
    try std.testing.expectEqual(@as(u32, 8), b.used);
}

test "an oversized or unbacked request returns null and changes nothing" {
    var buf: [16]u8 align(8) = undefined;
    var a = fresh(&buf);
    try std.testing.expect(a.alloc(17) == null);
    try std.testing.expect(a.alloc(std.math.maxInt(usize)) == null);
    try std.testing.expectEqual(@as(u32, 0), a.used);

    var none: Arena = .{ .base = null, .bytes = 16, .used = 0, .last = 0 };
    try std.testing.expect(none.alloc(1) == null);
}

test "an exact fill succeeds and the next byte does not" {
    var buf: [16]u8 align(8) = undefined;
    var a = fresh(&buf);
    try std.testing.expect(a.alloc(16) != null);
    try std.testing.expect(a.alloc(0) != null);
    try std.testing.expect(a.alloc(1) == null);
}
