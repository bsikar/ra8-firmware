//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host cover for the static key-handle pool: allocation, exhaustion, and
//! the pointer-ownership test the security suite drives with out-of-range
//! handles.

const std = @import("std");
const pool = @import("pool");

const max_keys = 16;

test "a fresh pool hands out every slot exactly once" {
    var p: pool.Pool = .{};
    var seen: [max_keys]*pool.Slot = undefined;
    for (&seen) |*entry| {
        const slot = p.alloc() orelse return error.PoolExhaustedEarly;
        slot.in_use = true;
        entry.* = slot;
    }
    for (seen, 0..) |slot, i| {
        for (seen[i + 1 ..]) |other| try std.testing.expect(slot != other);
    }
}

test "the pool refuses a seventeenth key" {
    var p: pool.Pool = .{};
    for (0..max_keys) |_| {
        const slot = p.alloc() orelse return error.PoolExhaustedEarly;
        slot.in_use = true;
    }
    try std.testing.expect(p.alloc() == null);
}

test "a cleared slot is handed out again" {
    var p: pool.Pool = .{};
    const first = p.alloc().?;
    first.in_use = true;
    first.key_len = 4;
    first.clear();
    try std.testing.expectEqual(first, p.alloc().?);
}

test "clear wipes the key material" {
    var p: pool.Pool = .{};
    const slot = p.alloc().?;
    slot.in_use = true;
    @memset(slot.key[0..8], 0xAA);
    slot.key_len = 8;
    slot.clear();
    try std.testing.expectEqual(@as(usize, 0), slot.key_len);
    try std.testing.expect(!slot.in_use);
    for (slot.key) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}

test "owns accepts a live slot and rejects a freed one" {
    var p: pool.Pool = .{};
    const slot = p.alloc().?;
    slot.in_use = true;
    try std.testing.expect(p.owns(slot));
    slot.clear();
    try std.testing.expect(!p.owns(slot));
}

test "owns rejects null and addresses either side of the pool" {
    var p: pool.Pool = .{};
    const slot = p.alloc().?;
    slot.in_use = true;
    try std.testing.expect(!p.owns(null));

    // Both arms of the C's `(handle < base) || (handle >= end)` decision.
    const base = @intFromPtr(&p.slots[0]);
    const end = base + max_keys * @sizeOf(pool.Slot);
    try std.testing.expect(!p.owns(@ptrFromInt(base - @sizeOf(pool.Slot))));
    try std.testing.expect(!p.owns(@ptrFromInt(end)));
}

test "owns rejects a slot belonging to a different pool" {
    var mine: pool.Pool = .{};
    var theirs: pool.Pool = .{};
    const slot = theirs.alloc().?;
    slot.in_use = true;
    try std.testing.expect(!mine.owns(slot));
}

test "material is exactly the imported bytes" {
    var p: pool.Pool = .{};
    const slot = p.alloc().?;
    @memcpy(slot.key[0..3], "abc");
    slot.key_len = 3;
    try std.testing.expectEqualSlices(u8, "abc", slot.material());
}

test "reset frees every slot without touching key bytes" {
    var p: pool.Pool = .{};
    const slot = p.alloc().?;
    slot.in_use = true;
    slot.key[0] = 0x5A;
    slot.key_len = 1;
    p.reset();
    try std.testing.expect(!slot.in_use);
    try std.testing.expectEqual(@as(usize, 0), slot.key_len);
    try std.testing.expectEqual(@as(u8, 0x5A), slot.key[0]);
}

test "releaseAll runs the callback once per live slot then clears it" {
    var p: pool.Pool = .{};
    const a = p.alloc().?;
    a.in_use = true;
    const b = p.alloc().?;
    b.in_use = true;
    const c = p.alloc().?;
    c.in_use = true;
    c.clear();

    var count: usize = 0;
    p.releaseAll(&count, struct {
        fn hit(counter: *usize, _: *pool.Slot) void {
            counter.* += 1;
        }
    }.hit);

    try std.testing.expectEqual(@as(usize, 2), count);
    try std.testing.expect(!a.in_use);
    try std.testing.expect(!b.in_use);
}
