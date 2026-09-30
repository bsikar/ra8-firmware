//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the key-to-cell map: the built-in hash, the injected one,
//! the fold to a bucket, and the chain operations over the cell metadata.

const std = @import("std");

const index = @import("keycache_index");

const Cell = index.Cell;
const Table = index.Table;

const none: i32 = -1;

/// A table over fixed-size keys, with everything caller-owned as the engine
/// has it.
const Fixture = struct {
    buckets: [4]i32 = .{ none, none, none, none },
    meta: [4]Cell = [_]Cell{.{}} ** 4,
    keys: [4 * 2]u8 = @splat(0),

    fn table(self: *Fixture, hash: ?index.HashFn, ctx: ?*anyopaque) Table {
        return .{
            .buckets = &self.buckets,
            .meta = &self.meta,
            .keys = &self.keys,
            .key_bytes = 2,
            .hash = hash,
            .hash_ctx = ctx,
        };
    }

    /// Store `key` as cell `idx`'s key and mark the cell live.
    fn place(self: *Fixture, idx: u32, key: [2]u8) void {
        @memcpy(self.keys[idx * 2 ..][0..2], &key);
        self.meta[idx].valid = 1;
    }
};

/// Everything lands in bucket 1, so the chain is what is under test.
fn collide(_: ?*const anyopaque, _: u32, _: ?*anyopaque) callconv(.c) u32 {
    return 5;
}

/// Hash by the first key byte, so a caller can steer placement.
fn firstByte(key: ?*const anyopaque, _: u32, _: ?*anyopaque) callconv(.c) u32 {
    return @as([*]const u8, @ptrCast(key.?))[0];
}

/// Reports the context it was handed, so the plumbing is observable.
fn viaCtx(_: ?*const anyopaque, _: u32, ctx: ?*anyopaque) callconv(.c) u32 {
    return @as(*const u32, @ptrCast(@alignCast(ctx.?))).*;
}

test "fnv1a matches the published offset basis on an empty key" {
    try std.testing.expectEqual(index.fnv.offset_basis, index.fnv1a(&.{}));
}

test "fnv1a folds the reference vector" {
    // FNV-1a 32-bit of "a" is 0xe40c292c, a published test vector.
    try std.testing.expectEqual(@as(u32, 0xe40c292c), index.fnv1a("a"));
}

test "fnv1a separates keys that differ in one byte" {
    try std.testing.expect(index.fnv1a(&.{ 1, 2 }) != index.fnv1a(&.{ 1, 3 }));
}

test "the built-in hash runs when the config injects none" {
    var fx: Fixture = .{};
    const t = fx.table(null, null);
    const key = [_]u8{ 7, 9 };
    try std.testing.expectEqual(index.fnv1a(&key) % 4, t.bucketOf(&key));
}

test "an injected hash replaces the built-in one" {
    var fx: Fixture = .{};
    const t = fx.table(firstByte, null);
    try std.testing.expectEqual(@as(u32, 2), t.bucketOf(&.{ 6, 0 }));
}

test "the hash context reaches the injected callback" {
    var fx: Fixture = .{};
    var ctx: u32 = 7;
    const t = fx.table(viaCtx, &ctx);
    try std.testing.expectEqual(@as(u32, 3), t.bucketOf(&.{ 0, 0 }));
}

test "the raw hash is folded into the bucket range" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    try std.testing.expectEqual(@as(u32, 1), t.bucketOf(&.{ 0, 0 }));
}

test "insert then lookup finds the cell" {
    var fx: Fixture = .{};
    const t = fx.table(null, null);
    fx.place(2, .{ 4, 4 });
    t.insert(2);

    try std.testing.expectEqual(@as(?u32, 2), t.lookup(&.{ 4, 4 }));
}

test "lookup misses a key nobody stored" {
    var fx: Fixture = .{};
    const t = fx.table(null, null);
    fx.place(0, .{ 1, 1 });
    t.insert(0);

    try std.testing.expectEqual(@as(?u32, null), t.lookup(&.{ 2, 2 }));
}

test "lookup walks a chain of colliding keys" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    fx.place(0, .{ 1, 0 });
    fx.place(1, .{ 2, 0 });
    fx.place(2, .{ 3, 0 });
    t.insert(0);
    t.insert(1);
    t.insert(2);

    try std.testing.expectEqual(@as(?u32, 0), t.lookup(&.{ 1, 0 }));
    try std.testing.expectEqual(@as(?u32, 1), t.lookup(&.{ 2, 0 }));
    try std.testing.expectEqual(@as(?u32, 2), t.lookup(&.{ 3, 0 }));
}

test "lookup skips a cell that is chained but not valid" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    fx.place(0, .{ 1, 0 });
    fx.place(1, .{ 1, 0 });
    t.insert(0);
    t.insert(1);
    // Cell 1 is the chain head and holds the same bytes; invalidate it.
    fx.meta[1].valid = 0;

    try std.testing.expectEqual(@as(?u32, 0), t.lookup(&.{ 1, 0 }));
}

test "remove takes the chain head out" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    fx.place(0, .{ 1, 0 });
    fx.place(1, .{ 2, 0 });
    t.insert(0);
    t.insert(1);

    t.remove(1);

    try std.testing.expectEqual(@as(?u32, null), t.lookup(&.{ 2, 0 }));
    try std.testing.expectEqual(@as(?u32, 0), t.lookup(&.{ 1, 0 }));
    try std.testing.expectEqual(none, fx.meta[1].hash_next);
}

test "remove takes a cell out of the middle of a chain" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    fx.place(0, .{ 1, 0 });
    fx.place(1, .{ 2, 0 });
    fx.place(2, .{ 3, 0 });
    t.insert(0);
    t.insert(1);
    t.insert(2);

    t.remove(1);

    try std.testing.expectEqual(@as(?u32, null), t.lookup(&.{ 2, 0 }));
    try std.testing.expectEqual(@as(?u32, 0), t.lookup(&.{ 1, 0 }));
    try std.testing.expectEqual(@as(?u32, 2), t.lookup(&.{ 3, 0 }));
}

test "remove of a cell that is not chained leaves the chain intact" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    fx.place(0, .{ 1, 0 });
    t.insert(0);
    fx.place(3, .{ 9, 0 });

    t.remove(3);

    try std.testing.expectEqual(@as(?u32, 0), t.lookup(&.{ 1, 0 }));
}

test "a corrupt chain ring terminates instead of spinning" {
    var fx: Fixture = .{};
    const t = fx.table(collide, null);
    fx.place(0, .{ 1, 0 });
    fx.place(1, .{ 2, 0 });
    fx.buckets[1] = 0;
    fx.meta[0].hash_next = 1;
    fx.meta[1].hash_next = 0;

    try std.testing.expectEqual(@as(?u32, null), t.lookup(&.{ 5, 5 }));
}

test "clear empties every bucket" {
    var fx: Fixture = .{};
    const t = fx.table(null, null);
    fx.place(1, .{ 8, 8 });
    t.insert(1);

    t.clear();

    try std.testing.expectEqual(@as(?u32, null), t.lookup(&.{ 8, 8 }));
    for (fx.buckets) |b| try std.testing.expectEqual(none, b);
}

test "keyOf addresses the key storage by stride" {
    var fx: Fixture = .{};
    const t = fx.table(null, null);
    fx.place(3, .{ 0xAB, 0xCD });

    try std.testing.expectEqualSlices(u8, &.{ 0xAB, 0xCD }, t.keyOf(3));
}
