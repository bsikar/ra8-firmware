//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the intrusive recency list: splicing, MRU insertion, and the
//! bounded walk that finds an unpinned victim.

const std = @import("std");

const list = @import("keycache_list");

const Cell = list.Cell;
const Segment = list.Segment;

/// A fresh, fully detached metadata array.
fn cells(comptime n: usize) [n]Cell {
    return @as([n]Cell, @splat(.{}));
}

/// The list from head to tail, as cell indices.
fn order(seg: Segment, meta: []const Cell, buf: []u32) []u32 {
    var cur = seg.head;
    var i: usize = 0;
    while (cur != list.none and i < buf.len) : (i += 1) {
        buf[i] = @intCast(cur);
        cur = meta[@intCast(cur)].next;
    }
    return buf[0..i];
}

test "push head builds an MRU-first order and sets the tail once" {
    var meta = cells(3);
    var seg: Segment = .{};

    seg.pushHead(&meta, 0);
    try std.testing.expectEqual(@as(i32, 0), seg.head);
    try std.testing.expectEqual(@as(i32, 0), seg.tail);

    seg.pushHead(&meta, 1);
    seg.pushHead(&meta, 2);

    var buf: [3]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 2, 1, 0 }, order(seg, &meta, &buf));
    try std.testing.expectEqual(@as(i32, 0), seg.tail);
}

test "unlink from the middle keeps both neighbours and the ends" {
    var meta = cells(3);
    var seg: Segment = .{};
    for (0..3) |i| seg.pushHead(&meta, @intCast(i));

    seg.unlink(&meta, 1);

    var buf: [3]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 2, 0 }, order(seg, &meta, &buf));
    try std.testing.expectEqual(@as(i32, 2), seg.head);
    try std.testing.expectEqual(@as(i32, 0), seg.tail);
}

test "unlink the head moves the head to the next cell" {
    var meta = cells(3);
    var seg: Segment = .{};
    for (0..3) |i| seg.pushHead(&meta, @intCast(i));

    seg.unlink(&meta, 2);

    try std.testing.expectEqual(@as(i32, 1), seg.head);
    try std.testing.expectEqual(@as(i32, 0), seg.tail);
}

test "unlink the tail moves the tail to the previous cell" {
    var meta = cells(3);
    var seg: Segment = .{};
    for (0..3) |i| seg.pushHead(&meta, @intCast(i));

    seg.unlink(&meta, 0);

    try std.testing.expectEqual(@as(i32, 2), seg.head);
    try std.testing.expectEqual(@as(i32, 1), seg.tail);
}

test "unlink the only cell empties the list" {
    var meta = cells(1);
    var seg: Segment = .{};
    seg.pushHead(&meta, 0);

    seg.unlink(&meta, 0);

    try std.testing.expectEqual(list.none, seg.head);
    try std.testing.expectEqual(list.none, seg.tail);
}

test "unlinking an already detached cell leaves the list alone" {
    var meta = cells(3);
    var seg: Segment = .{};
    seg.pushHead(&meta, 0);
    seg.pushHead(&meta, 1);

    // Cell 2 was never linked: no prev, no next, neither end.
    seg.unlink(&meta, 2);

    var buf: [3]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 1, 0 }, order(seg, &meta, &buf));
}

test "first unpinned walks from the tail toward the head" {
    var meta = cells(4);
    var seg: Segment = .{};
    for (0..4) |i| seg.pushHead(&meta, @intCast(i));

    // Tail is 0; pin it and the next two up.
    meta[0].pin_count = 1;
    meta[1].pin_count = 2;

    try std.testing.expectEqual(@as(?u32, 2), seg.firstUnpinned(&meta));
}

test "first unpinned is null when every cell is pinned" {
    var meta = cells(3);
    var seg: Segment = .{};
    for (0..3) |i| {
        seg.pushHead(&meta, @intCast(i));
        meta[i].pin_count = 1;
    }

    try std.testing.expectEqual(@as(?u32, null), seg.firstUnpinned(&meta));
}

test "first unpinned on an empty list is null" {
    var meta = cells(2);
    const seg: Segment = .{};
    try std.testing.expectEqual(@as(?u32, null), seg.firstUnpinned(&meta));
}

test "a corrupt link ring terminates instead of spinning" {
    var meta = cells(2);
    var seg: Segment = .{ .head = 0, .tail = 1 };
    // A ring: 1 -> 0 -> 1, every cell pinned so the walk cannot stop early.
    meta[0].prev = 1;
    meta[1].prev = 0;
    meta[0].pin_count = 1;
    meta[1].pin_count = 1;

    try std.testing.expectEqual(@as(?u32, null), seg.firstUnpinned(&meta));
}

test "relinking after an unlink restores a well formed list" {
    var meta = cells(3);
    var seg: Segment = .{};
    for (0..3) |i| seg.pushHead(&meta, @intCast(i));

    seg.unlink(&meta, 0);
    seg.pushHead(&meta, 0);

    var buf: [3]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 0, 2, 1 }, order(seg, &meta, &buf));
    try std.testing.expectEqual(@as(i32, 1), seg.tail);
}

test "the cell record is the layout the C header publishes" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(Cell));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(Cell, "pin_count"));
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(Segment));
}
