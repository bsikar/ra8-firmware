// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const pool = @import("pool");

/// Two words wide, so an offset can be correctly aligned and still not
/// land on a slot boundary.
const Payload = struct { value: u32, spare: u32 };
const Four = pool.Pool(Payload, 4);

test "acquire hands out every slot once, then reports exhaustion" {
    var slots: Four = .{};
    var seen: [4]*Payload = undefined;
    for (&seen) |*entry| entry.* = slots.acquire().?;
    try std.testing.expect(slots.acquire() == null);
    for (seen, 0..) |entry, index| {
        for (seen[index + 1 ..]) |other| try std.testing.expect(entry != other);
    }
}

test "owns accepts a live slot and rejects null, foreign and released ones" {
    var slots: Four = .{};
    var other: Four = .{};
    var stack: Payload = .{ .value = 0, .spare = 0 };

    const live = slots.acquire().?;
    try std.testing.expect(slots.owns(live));
    try std.testing.expect(!slots.owns(null));
    try std.testing.expect(!slots.owns(&stack));
    try std.testing.expect(!other.owns(live));

    slots.release(live);
    try std.testing.expect(!slots.owns(live));
}

test "an interior pointer is not a slot" {
    var slots: Four = .{};
    _ = slots.acquire().?;
    const half = @sizeOf(Payload) / 2;
    const interior: *const Payload = @ptrFromInt(@intFromPtr(&slots.slots[0]) + half);
    try std.testing.expect(slots.indexOf(interior) == null);
}

test "release wipes the slot it frees" {
    var slots: Four = .{};
    const live = slots.acquire().?;
    live.value = 0x1234;
    slots.release(live);
    try std.testing.expectEqual(@as(u32, 0), slots.slots[0].value);
}

test "reset runs the callback over live slots only, then clears all" {
    var slots: Four = .{};
    const first = slots.acquire().?;
    first.value = 7;
    _ = slots.acquire().?;

    const Counter = struct {
        var seen: usize = 0;
        fn note(_: void, slot: *Payload) void {
            _ = slot;
            seen += 1;
        }
    };
    Counter.seen = 0;
    slots.reset({}, Counter.note);

    try std.testing.expectEqual(@as(usize, 2), Counter.seen);
    try std.testing.expect(!slots.owns(first));
    try std.testing.expectEqual(@as(u32, 0), slots.slots[0].value);
    try std.testing.expect(slots.acquire() != null);
}
