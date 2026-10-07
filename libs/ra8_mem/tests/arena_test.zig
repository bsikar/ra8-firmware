//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the init-time bump arena. Every case works over a real aligned
//! buffer, because the arena's arithmetic is about addresses rather than
//! offsets and a fake base would not exercise it.

const std = @import("std");
const testing = std.testing;

const arena = @import("arena");

const Arena = arena.Arena;
const Slot = arena.Slot;

/// A 4 KiB region aligned hard enough that a test can reason about where a
/// carve must land.
const Region = struct {
    storage: []align(4096) u8,

    fn init(bytes: u32) !Region {
        const buf = try testing.allocator.alignedAlloc(u8, .fromByteUnits(4096), bytes);
        return .{ .storage = buf };
    }

    fn deinit(self: Region) void {
        testing.allocator.free(self.storage);
    }

    fn bind(self: Region) Arena {
        var a: Arena = .{};
        try_bind(&a, self);
        return a;
    }

    fn try_bind(a: *Arena, self: Region) void {
        std.debug.assert(arena.init(a, self.storage.ptr, @intCast(self.storage.len)) == .ok);
    }
};

test "init rejects a zero-length region and leaves the arena unbound" {
    var a: Arena = .{};
    var byte: u8 = 0;
    try testing.expectEqual(arena.Err.invalid_size, arena.init(&a, @ptrCast(&byte), 0));
    try testing.expectEqual(@as(?[*]u8, null), a.base);
}

test "init zeroes the cursor and the peak" {
    const region = try Region.init(256);
    defer region.deinit();
    var a = region.bind();
    try testing.expectEqual(@as(u32, 256), a.size);
    try testing.expectEqual(@as(u32, 0), a.used);
    try testing.expectEqual(@as(u32, 0), a.high_water);
    try testing.expectEqual(@as(u32, 256), arena.remaining(&a));
}

test "carve hands back an aligned block and charges the padding" {
    const region = try Region.init(256);
    defer region.deinit();
    var a = region.bind();

    var first: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 1, 1, &first));
    try testing.expectEqual(@as(u32, 1), a.used);

    var second: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 4, 64, &second));
    try testing.expectEqual(@as(usize, 0), @intFromPtr(second) % 64);
    // The 63 bytes of padding are charged, so the cursor is past the block.
    try testing.expectEqual(@as(u32, 68), a.used);
}

test "carve rejects zero bytes and a non-power-of-two alignment" {
    const region = try Region.init(256);
    defer region.deinit();
    var a = region.bind();
    var out: ?*anyopaque = null;

    try testing.expectEqual(arena.Err.invalid_size, arena.carve(&a, 0, 1, &out));
    try testing.expectEqual(arena.Err.invalid_arg, arena.carve(&a, 1, 0, &out));
    try testing.expectEqual(arena.Err.invalid_arg, arena.carve(&a, 1, 3, &out));
    try testing.expectEqual(arena.Err.invalid_arg, arena.carve(&a, 1, 24, &out));
    // Every rejection leaves the arena untouched.
    try testing.expectEqual(@as(u32, 0), a.used);
}

test "carve refuses a block that does not fit the remainder" {
    const region = try Region.init(64);
    defer region.deinit();
    var a = region.bind();
    var out: ?*anyopaque = null;

    try testing.expectEqual(arena.Err.no_mem, arena.carve(&a, 65, 1, &out));
    try testing.expectEqual(@as(u32, 0), a.used);
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 64, 1, &out));
    try testing.expectEqual(@as(u32, 0), arena.remaining(&a));
}

test "carve refuses when the alignment padding itself runs past the end" {
    const region = try Region.init(96);
    defer region.deinit();
    var a = region.bind();

    var head: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 80, 1, &head));
    // The cursor is at base+80; rounding to 128 leaves the region entirely.
    var tail: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.no_mem, arena.carve(&a, 1, 128, &tail));
    try testing.expectEqual(@as(u32, 80), a.used);
}

test "the peak survives a reset and reset keeps the region" {
    const region = try Region.init(256);
    defer region.deinit();
    var a = region.bind();

    var out: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 100, 1, &out));
    try testing.expectEqual(@as(u32, 100), arena.highWater(&a));

    arena.reset(&a);
    try testing.expectEqual(@as(u32, 0), a.used);
    try testing.expectEqual(@as(u32, 100), arena.highWater(&a));
    try testing.expectEqual(@as(u32, 256), arena.remaining(&a));

    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 10, 1, &out));
    // A smaller second run does not lower the peak.
    try testing.expectEqual(@as(u32, 100), arena.highWater(&a));
}

test "carveAll fills every slot in array order" {
    const region = try Region.init(512);
    defer region.deinit();
    var a = region.bind();

    var p0: ?*anyopaque = null;
    var p1: ?*anyopaque = null;
    var p2: ?*anyopaque = null;
    const slots = [_]Slot{
        .{ .bytes = 16, .alignment = 4, .out_ptr = &p0 },
        .{ .bytes = 32, .alignment = 16, .out_ptr = &p1 },
        .{ .bytes = 8, .alignment = 1, .out_ptr = &p2 },
    };

    try testing.expectEqual(arena.Err.ok, arena.carveAll(&a, &slots));
    try testing.expect(@intFromPtr(p0) < @intFromPtr(p1));
    try testing.expect(@intFromPtr(p1) < @intFromPtr(p2));
    try testing.expectEqual(@as(usize, 0), @intFromPtr(p1) % 16);
    // No two blocks overlap: p1 starts at or past the end of p0.
    try testing.expect(@intFromPtr(p1) >= @intFromPtr(p0) + 16);
}

test "carveAll publishes nothing when one slot does not fit" {
    const region = try Region.init(64);
    defer region.deinit();
    var a = region.bind();

    var p0: ?*anyopaque = null;
    var p1: ?*anyopaque = null;
    const slots = [_]Slot{
        .{ .bytes = 32, .alignment = 1, .out_ptr = &p0 },
        .{ .bytes = 64, .alignment = 1, .out_ptr = &p1 },
    };

    try testing.expectEqual(arena.Err.no_mem, arena.carveAll(&a, &slots));
    // The atomicity guarantee: no pointer written, no cursor moved.
    try testing.expectEqual(@as(?*anyopaque, null), p0);
    try testing.expectEqual(@as(?*anyopaque, null), p1);
    try testing.expectEqual(@as(u32, 0), a.used);
}

test "carveAll validates the whole table before carving any of it" {
    const region = try Region.init(256);
    defer region.deinit();
    var a = region.bind();

    var p0: ?*anyopaque = null;
    var p1: ?*anyopaque = null;

    const bad_align = [_]Slot{
        .{ .bytes = 8, .alignment = 1, .out_ptr = &p0 },
        .{ .bytes = 8, .alignment = 3, .out_ptr = &p1 },
    };
    try testing.expectEqual(arena.Err.invalid_arg, arena.carveAll(&a, &bad_align));

    const bad_bytes = [_]Slot{
        .{ .bytes = 8, .alignment = 1, .out_ptr = &p0 },
        .{ .bytes = 0, .alignment = 1, .out_ptr = &p1 },
    };
    try testing.expectEqual(arena.Err.invalid_size, arena.carveAll(&a, &bad_bytes));

    const null_out = [_]Slot{
        .{ .bytes = 8, .alignment = 1, .out_ptr = &p0 },
        .{ .bytes = 8, .alignment = 1, .out_ptr = null },
    };
    try testing.expectEqual(arena.Err.null_ptr, arena.carveAll(&a, &null_out));

    try testing.expectEqual(@as(?*anyopaque, null), p0);
    try testing.expectEqual(@as(u32, 0), a.used);
}

test "carveAll bounds the slot count" {
    const region = try Region.init(4096);
    defer region.deinit();
    var a = region.bind();

    var sink: ?*anyopaque = null;
    const empty: []const Slot = &.{};
    try testing.expectEqual(arena.Err.invalid_arg, arena.carveAll(&a, empty));

    var over: [arena.Limits.slot_cap + 1]Slot = undefined;
    for (&over) |*slot| slot.* = .{ .bytes = 1, .alignment = 1, .out_ptr = &sink };
    try testing.expectEqual(arena.Err.invalid_arg, arena.carveAll(&a, &over));

    var at_cap: [arena.Limits.slot_cap]Slot = undefined;
    for (&at_cap) |*slot| slot.* = .{ .bytes = 1, .alignment = 1, .out_ptr = &sink };
    try testing.expectEqual(arena.Err.ok, arena.carveAll(&a, &at_cap));
    try testing.expectEqual(@as(u32, arena.Limits.slot_cap), a.used);
}

test "carveRemaining reports the usable extent, not the raw remainder" {
    const region = try Region.init(256);
    defer region.deinit();
    var a = region.bind();

    var head: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 1, 1, &head));

    var tail: ?*anyopaque = null;
    var bytes: u32 = 0;
    try testing.expectEqual(arena.Err.ok, arena.carveRemaining(&a, 64, &tail, &bytes));
    try testing.expectEqual(@as(usize, 0), @intFromPtr(tail) % 64);
    // 63 bytes of padding were charged, so the block is short of the 255 left.
    try testing.expectEqual(@as(u32, 192), bytes);
    try testing.expectEqual(@as(u32, 0), arena.remaining(&a));
}

test "carveRemaining calls an empty tail no_mem rather than a zero-length block" {
    const region = try Region.init(64);
    defer region.deinit();
    var a = region.bind();

    var all: ?*anyopaque = null;
    try testing.expectEqual(arena.Err.ok, arena.carve(&a, 64, 1, &all));

    var tail: ?*anyopaque = null;
    var bytes: u32 = 0;
    try testing.expectEqual(arena.Err.no_mem, arena.carveRemaining(&a, 1, &tail, &bytes));
    try testing.expectEqual(@as(?*anyopaque, null), tail);
    try testing.expectEqual(@as(u32, 0), bytes);
}

test "carveRemaining rejects a bad alignment before touching the arena" {
    const region = try Region.init(64);
    defer region.deinit();
    var a = region.bind();

    var tail: ?*anyopaque = null;
    var bytes: u32 = 0;
    try testing.expectEqual(arena.Err.invalid_arg, arena.carveRemaining(&a, 0, &tail, &bytes));
    try testing.expectEqual(arena.Err.invalid_arg, arena.carveRemaining(&a, 6, &tail, &bytes));
    try testing.expectEqual(@as(u32, 0), a.used);
}

test "the layout matches ra8_arena.h" {
    // Field for field with the C struct: a pointer, then three u32. Asserting
    // the offsets rather than @sizeOf is what makes this hold on both the
    // 64-bit host (where the tail pads to the pointer's alignment) and the
    // 32-bit target (where it does not).
    try testing.expectEqual(0, @offsetOf(Arena, "base"));
    try testing.expectEqual(@sizeOf(usize), @offsetOf(Arena, "size"));
    try testing.expectEqual(@sizeOf(usize) + 4, @offsetOf(Arena, "used"));
    try testing.expectEqual(@sizeOf(usize) + 8, @offsetOf(Arena, "high_water"));
    try testing.expectEqual(@as(u32, 16), arena.Limits.slot_cap);
}
