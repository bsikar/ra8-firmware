//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The bump scratch. The cursor only rewinds when the last block is released,
//! which is the property a decoder's alloc/free pattern depends on.

const std = @import("std");
const scratch = @import("scratch");

const Scratch = extern struct {
    base: ?[*]u8 = null,
    cap: usize = 0,
    offset: usize = 0,
    live: usize = 0,
    high_water: usize = 0,
};

fn store() [256]u8 {
    return @as([256]u8, @splat(0));
}

test "init points the cursor at the caller's store" {
    var backing = store();
    var s: Scratch = .{};
    try std.testing.expectEqual(@as(u16, 0), scratch.init(@ptrCast(&s), &backing));
    try std.testing.expectEqual(@as(usize, 256), s.cap);
    try std.testing.expectEqual(@as(usize, 0), s.offset);
}

test "init of an empty store is a size fault" {
    var s: Scratch = .{};
    try std.testing.expectEqual(@as(u16, 0x105), scratch.init(@ptrCast(&s), &[_]u8{}));
}

test "every block is rounded up to the alignment" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    _ = scratch.alloc(@ptrCast(&s), 1);
    try std.testing.expectEqual(@as(usize, 16), s.offset);
    _ = scratch.alloc(@ptrCast(&s), 17);
    try std.testing.expectEqual(@as(usize, 48), s.offset);
}

test "a request past the remaining capacity is refused" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    try std.testing.expect(scratch.alloc(@ptrCast(&s), 256) != null);
    try std.testing.expect(scratch.alloc(@ptrCast(&s), 1) == null);
}

test "a zero-byte alloc is refused and an unusable scratch allocates nothing" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    try std.testing.expect(scratch.alloc(@ptrCast(&s), 0) == null);

    var empty: Scratch = .{};
    try std.testing.expect(scratch.alloc(@ptrCast(&empty), 8) == null);
}

test "calloc zeroes what it hands back" {
    var backing = store();
    @memset(&backing, 0xAA);
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    const block = scratch.calloc(@ptrCast(&s), 4, 8).?;
    for (block[0..32]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}

test "calloc refuses a count times size that would overflow" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    const half = (~@as(usize, 0) / 2) + 1;
    try std.testing.expect(scratch.calloc(@ptrCast(&s), half, 4) == null);
}

test "realloc carries the shorter of the two lengths across" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    const first = scratch.alloc(@ptrCast(&s), 8).?;
    @memcpy(first[0..8], "abcdefgh");
    const grown = scratch.realloc(@ptrCast(&s), first, 8, 32).?;
    try std.testing.expectEqualStrings("abcdefgh", grown[0..8]);

    const shrunk = scratch.realloc(@ptrCast(&s), grown, 32, 4).?;
    try std.testing.expectEqualStrings("abcd", shrunk[0..4]);
}

test "realloc of a null pointer is a plain alloc" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    try std.testing.expect(scratch.realloc(@ptrCast(&s), null, 0, 16) != null);
    try std.testing.expectEqual(@as(usize, 1), s.live);
}

test "the cursor rewinds only once the last block is freed" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    const a = scratch.alloc(@ptrCast(&s), 16).?;
    const b = scratch.alloc(@ptrCast(&s), 16).?;
    try std.testing.expectEqual(@as(usize, 32), s.offset);

    scratch.free(@ptrCast(&s), a);
    try std.testing.expectEqual(@as(usize, 32), s.offset);
    scratch.free(@ptrCast(&s), b);
    try std.testing.expectEqual(@as(usize, 0), s.offset);
}

test "freeing a null pointer or an empty scratch does nothing" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    scratch.free(@ptrCast(&s), null);
    scratch.free(@ptrCast(&s), backing[0..].ptr);
    try std.testing.expectEqual(@as(usize, 0), s.live);
}

test "high water survives a reset" {
    var backing = store();
    var s: Scratch = .{};
    _ = scratch.init(@ptrCast(&s), &backing);
    _ = scratch.alloc(@ptrCast(&s), 64);
    scratch.reset(@ptrCast(&s));
    try std.testing.expectEqual(@as(usize, 0), s.offset);
    try std.testing.expectEqual(@as(usize, 64), scratch.highWater(@ptrCast(&s)));
}

test "a carve of zero bytes is a size fault" {
    try std.testing.expectEqual(@as(u16, 0x105), scratch.carveAlign(0, 16).fault);
}

test "a zero alignment means the default" {
    try std.testing.expectEqual(@as(u32, 16), scratch.carveAlign(64, 0).ok);
}

test "a non-power-of-two alignment is an argument fault" {
    try std.testing.expectEqual(@as(u16, 0x103), scratch.carveAlign(64, 12).fault);
}

test "an alignment stricter than the default is unsupported" {
    try std.testing.expectEqual(@as(u16, 0x107), scratch.carveAlign(64, 32).fault);
}

test "a looser power-of-two alignment is honoured as asked" {
    try std.testing.expectEqual(@as(u32, 8), scratch.carveAlign(64, 8).ok);
}
