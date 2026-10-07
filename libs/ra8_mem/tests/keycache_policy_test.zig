//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the eviction policy: what a re-reference does under LRU and
//! under SLRU, where the protected segment overflows to, and which cell the
//! victim scan picks.

const std = @import("std");

const policy = @import("keycache_policy");

const Cell = policy.Cell;
const Sets = policy.Sets;
const Seg = policy.Seg;

fn cells(comptime n: usize) [n]Cell {
    return @as([n]Cell, @splat(.{}));
}

/// One segment's cells from MRU to LRU.
fn order(seg: policy.Segment, meta: []const Cell, buf: []u32) []u32 {
    var cur = seg.head;
    var i: usize = 0;
    while (cur != -1 and i < buf.len) : (i += 1) {
        buf[i] = @intCast(cur);
        cur = meta[@intCast(cur)].next;
    }
    return buf[0..i];
}

test "protected cap takes the default share when the caller asks for none" {
    try std.testing.expectEqual(@as(u32, 3), policy.protectedCap(4, 0));
    try std.testing.expectEqual(@as(u32, 75), policy.protectedCap(100, 0));
}

test "protected cap honours an explicit share" {
    try std.testing.expectEqual(@as(u32, 5), policy.protectedCap(10, 50));
    try std.testing.expectEqual(@as(u32, 10), policy.protectedCap(10, 100));
}

test "a small cache can round its protected segment down to nothing" {
    try std.testing.expectEqual(@as(u32, 0), policy.protectedCap(1, 50));
}

test "seed puts every cell cold in probation, tail first by index" {
    var meta = cells(3);
    var sets: Sets = .{};

    sets.seed(&meta);

    try std.testing.expectEqual(@as(i32, 0), sets.pb.tail);
    try std.testing.expectEqual(@as(i32, 2), sets.pb.head);
    try std.testing.expectEqual(@as(i32, -1), sets.pt.head);
    for (meta) |c| {
        try std.testing.expectEqual(@as(u8, 0), c.valid);
        try std.testing.expectEqual(@as(u16, 0), c.pin_count);
        try std.testing.expectEqual(@backingInt(Seg.probation), c.seg);
    }
}

test "the first victim of a fresh cache is cell 0" {
    var meta = cells(4);
    var sets: Sets = .{};
    sets.seed(&meta);

    try std.testing.expectEqual(@as(?u32, 0), sets.pickVictim(&meta));
}

test "LRU access moves the cell to the MRU and touches no segment tag" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);

    sets.access(&meta, .lru, 0);

    var buf: [3]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 0, 2, 1 }, order(sets.pb, &meta, &buf));
    try std.testing.expectEqual(@backingInt(Seg.probation), meta[0].seg);
    try std.testing.expectEqual(@as(u32, 0), sets.protected_count);
}

test "LRU never moves a cell into the protected segment" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;

    for (0..3) |_| sets.access(&meta, .lru, 1);

    try std.testing.expectEqual(@as(i32, -1), sets.pt.head);
    try std.testing.expectEqual(@as(u32, 0), sets.protected_count);
}

test "SLRU promotes a probationary cell into the protected segment" {
    var meta = cells(4);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;

    sets.access(&meta, .slru, 1);

    try std.testing.expectEqual(@backingInt(Seg.protected), meta[1].seg);
    try std.testing.expectEqual(@as(i32, 1), sets.pt.head);
    try std.testing.expectEqual(@as(u32, 1), sets.protected_count);
}

test "SLRU re-touching a protected cell keeps it protected at the MRU" {
    var meta = cells(4);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;

    sets.access(&meta, .slru, 1);
    sets.access(&meta, .slru, 2);
    sets.access(&meta, .slru, 1);

    var buf: [4]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 1, 2 }, order(sets.pt, &meta, &buf));
    try std.testing.expectEqual(@as(u32, 2), sets.protected_count);
}

test "a full protected segment demotes its LRU back to probation" {
    var meta = cells(4);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;

    sets.access(&meta, .slru, 0);
    sets.access(&meta, .slru, 1);
    sets.access(&meta, .slru, 2);

    // 0 was the protected LRU, so it is the one that went back.
    try std.testing.expectEqual(@backingInt(Seg.probation), meta[0].seg);
    try std.testing.expectEqual(@as(u32, 2), sets.protected_count);
    var buf: [4]u32 = undefined;
    try std.testing.expectEqualSlices(u32, &.{ 2, 1 }, order(sets.pt, &meta, &buf));
    try std.testing.expectEqual(@as(i32, 0), sets.pb.head);
}

test "a zero-capacity protected segment promotes without demoting anything" {
    var meta = cells(2);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 0;

    sets.access(&meta, .slru, 0);

    try std.testing.expectEqual(@backingInt(Seg.protected), meta[0].seg);
    try std.testing.expectEqual(@as(u32, 1), sets.protected_count);
}

test "the victim comes from probation before protected" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;
    sets.access(&meta, .slru, 0);

    // 0 is protected now; 1 is the probationary LRU.
    try std.testing.expectEqual(@as(?u32, 1), sets.pickVictim(&meta));
}

test "a pinned probationary cell is skipped, not evicted" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);
    meta[0].pin_count = 1;

    try std.testing.expectEqual(@as(?u32, 1), sets.pickVictim(&meta));
}

test "the protected LRU is the victim once probation is empty or pinned" {
    var meta = cells(2);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;
    sets.access(&meta, .slru, 0);
    sets.access(&meta, .slru, 1);

    try std.testing.expectEqual(@as(?u32, 0), sets.pickVictim(&meta));
}

test "no victim when every cell is pinned" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);
    for (&meta) |*c| c.pin_count = 1;

    try std.testing.expectEqual(@as(?u32, null), sets.pickVictim(&meta));
}

test "detaching a protected cell decrements the protected count" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;
    sets.access(&meta, .slru, 0);

    sets.detach(&meta, 0);

    try std.testing.expectEqual(@as(u32, 0), sets.protected_count);
    try std.testing.expectEqual(@as(i32, -1), sets.pt.head);
}

test "detaching a probationary cell leaves the protected count alone" {
    var meta = cells(3);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;
    sets.access(&meta, .slru, 0);

    sets.detach(&meta, 1);

    try std.testing.expectEqual(@as(u32, 1), sets.protected_count);
}

test "a scan of cold keys cannot displace the protected set" {
    var meta = cells(4);
    var sets: Sets = .{};
    sets.seed(&meta);
    sets.protected_cap = 2;
    // Two cells earn protection.
    sets.access(&meta, .slru, 0);
    sets.access(&meta, .slru, 1);

    // A flood of one-shot inserts recycles only the probationary pair.
    for (0..8) |_| {
        const v = sets.pickVictim(&meta).?;
        try std.testing.expect(v == 2 or v == 3);
        sets.detach(&meta, v);
        meta[v].seg = @backingInt(Seg.probation);
        sets.pb.pushHead(&meta, v);
    }

    try std.testing.expectEqual(@backingInt(Seg.protected), meta[0].seg);
    try std.testing.expectEqual(@backingInt(Seg.protected), meta[1].seg);
}

test "the grouped recency words are the six the C state spelled loose" {
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(Sets));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(Sets, "protected_count"));
}
