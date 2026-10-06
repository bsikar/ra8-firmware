//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure network-PAL core: the frame ring, the event
//! translation and the two argument predicates. No C ABI is involved, so
//! nothing here needs a link-time stub.

const std = @import("std");
const implementation = @import("implementation");

test "constants match the header contract" {
    try std.testing.expectEqual(@as(u16, 6), implementation.mac_addr_len);
    try std.testing.expectEqual(@as(u16, 1500), implementation.mtu);
    try std.testing.expectEqual(@as(u16, 1518), implementation.frame_max);
    try std.testing.expectEqual(@as(u16, 4), implementation.ring_slots);
}

test "link state enumerators keep their numbers" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(implementation.LinkState.down));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(implementation.LinkState.up));
}

test "event bits keep their numbers" {
    try std.testing.expectEqual(@as(u32, 0x00), implementation.event_none);
    try std.testing.expectEqual(@as(u32, 0x01), implementation.event_link_up);
    try std.testing.expectEqual(@as(u32, 0x02), implementation.event_link_down);
    try std.testing.expectEqual(@as(u32, 0x04), implementation.event_rx_ready);
    try std.testing.expectEqual(@as(u32, 0x08), implementation.event_tx_done);
    try std.testing.expectEqual(@as(u32, 0x10), implementation.event_error);
}

test "zero mac is all zero bytes" {
    for (implementation.Mac.zero.bytes) |byte| {
        try std.testing.expectEqual(@as(u8, 0), byte);
    }
}

test "translateEvent: clear status is no event" {
    try std.testing.expectEqual(implementation.event_none, implementation.translateEvent(0));
}

test "translateEvent: any single bit becomes an error event" {
    var bit: u5 = 0;
    while (true) {
        const mask = @as(u32, 1) << bit;
        try std.testing.expectEqual(implementation.event_error, implementation.translateEvent(mask));
        if (bit == 31) break;
        bit += 1;
    }
}

test "translateEvent: all bits set is still one error event" {
    try std.testing.expectEqual(
        implementation.event_error,
        implementation.translateEvent(0xFFFFFFFF),
    );
}

test "sendLenValid: zero is rejected" {
    try std.testing.expect(!implementation.sendLenValid(0));
}

test "sendLenValid: one byte is accepted" {
    try std.testing.expect(implementation.sendLenValid(1));
}

test "sendLenValid: exactly frame_max is accepted" {
    try std.testing.expect(implementation.sendLenValid(implementation.frame_max));
}

test "sendLenValid: one past frame_max is rejected" {
    try std.testing.expect(!implementation.sendLenValid(implementation.frame_max + 1));
}

test "sendLenValid: the whole u16 range agrees with the contract" {
    var len: u32 = 0;
    while (len <= 0xFFFF) : (len += 1) {
        const expected = (len != 0) and (len <= implementation.frame_max);
        try std.testing.expectEqual(expected, implementation.sendLenValid(@intCast(len)));
    }
}

test "recvCapacityValid: full frame capacity is accepted" {
    try std.testing.expect(implementation.recvCapacityValid(implementation.frame_max));
}

test "recvCapacityValid: one byte short is rejected" {
    try std.testing.expect(!implementation.recvCapacityValid(implementation.frame_max - 1));
}

test "recvCapacityValid: a short buffer is rejected even for a short frame" {
    try std.testing.expect(!implementation.recvCapacityValid(64));
}

test "recvCapacityValid: an oversized buffer is accepted" {
    try std.testing.expect(implementation.recvCapacityValid(0xFFFF));
}

test "ring starts empty with both cursors at zero" {
    var ring: implementation.Ring = .{};
    try std.testing.expect(ring.isEmpty());
    try std.testing.expect(!ring.isFull());
    try std.testing.expectEqual(@as(u16, 0), ring.head);
    try std.testing.expectEqual(@as(u16, 0), ring.tail);
    try std.testing.expectEqual(@as(u16, 0), ring.count);
}

test "pop on an empty ring reports nothing queued" {
    var ring: implementation.Ring = .{};
    var out: [implementation.frame_max]u8 = undefined;
    try std.testing.expect(ring.pop(&out) == null);
}

test "push then pop round-trips the payload and its length" {
    var ring: implementation.Ring = .{};
    var frame: [64]u8 = undefined;
    for (&frame, 0..) |*byte, i| byte.* = @intCast(0xA0 +% i);

    try std.testing.expect(ring.push(&frame));
    try std.testing.expectEqual(@as(u16, 1), ring.count);

    var out: [implementation.frame_max]u8 = undefined;
    const written = ring.pop(&out);
    try std.testing.expectEqual(@as(?u16, 64), written);
    try std.testing.expectEqualSlices(u8, &frame, out[0..64]);
    try std.testing.expect(ring.isEmpty());
}

test "push fills every slot and then reports the ring full" {
    var ring: implementation.Ring = .{};
    const frame: [64]u8 = @splat(0x11);
    var i: u16 = 0;
    while (i < implementation.ring_slots) : (i += 1) {
        try std.testing.expect(ring.push(&frame));
    }
    try std.testing.expect(ring.isFull());
    try std.testing.expect(!ring.push(&frame));
    try std.testing.expectEqual(implementation.ring_slots, ring.count);
}

test "a refused push leaves the cursors untouched" {
    var ring: implementation.Ring = .{};
    const frame: [32]u8 = @splat(0x22);
    var i: u16 = 0;
    while (i < implementation.ring_slots) : (i += 1) {
        try std.testing.expect(ring.push(&frame));
    }
    const tail_before = ring.tail;
    const count_before = ring.count;
    try std.testing.expect(!ring.push(&frame));
    try std.testing.expectEqual(tail_before, ring.tail);
    try std.testing.expectEqual(count_before, ring.count);
}

test "frames come back in the order they went in" {
    var ring: implementation.Ring = .{};
    var slot: u8 = 0;
    while (slot < 4) : (slot += 1) {
        const frame: [16]u8 = @splat(slot);
        try std.testing.expect(ring.push(&frame));
    }
    var out: [implementation.frame_max]u8 = undefined;
    var expected: u8 = 0;
    while (expected < 4) : (expected += 1) {
        const written = ring.pop(&out) orelse return error.TestUnexpectedResult;
        try std.testing.expectEqual(@as(u16, 16), written);
        try std.testing.expectEqualSlices(u8, &@as([16]u8, @splat(expected)), out[0..16]);
    }
    try std.testing.expect(ring.isEmpty());
}

test "cursors wrap without losing order across many cycles" {
    var ring: implementation.Ring = .{};
    var out: [implementation.frame_max]u8 = undefined;
    var round: u8 = 0;
    while (round < 20) : (round += 1) {
        const frame: [8]u8 = @splat(round);
        try std.testing.expect(ring.push(&frame));
        const written = ring.pop(&out) orelse return error.TestUnexpectedResult;
        try std.testing.expectEqual(@as(u16, 8), written);
        try std.testing.expectEqualSlices(u8, &frame, out[0..8]);
        try std.testing.expect(ring.head < implementation.ring_slots);
        try std.testing.expect(ring.tail < implementation.ring_slots);
    }
}

test "pop frees the slot it drained" {
    var ring: implementation.Ring = .{};
    const frame: [20]u8 = @splat(0x33);
    try std.testing.expect(ring.push(&frame));
    const drained_head = ring.head;
    var out: [implementation.frame_max]u8 = undefined;
    _ = ring.pop(&out);
    try std.testing.expectEqual(@as(u16, 0), ring.slots[drained_head].len);
}

test "reset clears the cursors and every slot length" {
    var ring: implementation.Ring = .{};
    const frame: [128]u8 = @splat(0x44);
    var i: u16 = 0;
    while (i < 3) : (i += 1) {
        try std.testing.expect(ring.push(&frame));
    }
    ring.reset();
    try std.testing.expectEqual(@as(u16, 0), ring.head);
    try std.testing.expectEqual(@as(u16, 0), ring.tail);
    try std.testing.expectEqual(@as(u16, 0), ring.count);
    for (ring.slots) |slot| {
        try std.testing.expectEqual(@as(u16, 0), slot.len);
    }
}

test "reset mid-ring makes the next push land in slot zero" {
    var ring: implementation.Ring = .{};
    const frame: [12]u8 = @splat(0x55);
    try std.testing.expect(ring.push(&frame));
    try std.testing.expect(ring.push(&frame));
    ring.reset();
    try std.testing.expect(ring.push(&frame));
    try std.testing.expectEqual(@as(u16, 12), ring.slots[0].len);
    try std.testing.expectEqual(@as(u16, 1), ring.tail);
}

test "a one-byte frame survives the ring" {
    var ring: implementation.Ring = .{};
    const frame = [_]u8{0xC7};
    try std.testing.expect(ring.push(&frame));
    var out: [implementation.frame_max]u8 = undefined;
    try std.testing.expectEqual(@as(?u16, 1), ring.pop(&out));
    try std.testing.expectEqual(@as(u8, 0xC7), out[0]);
}

test "a frame_max frame survives the ring" {
    var ring: implementation.Ring = .{};
    var frame: [implementation.frame_max]u8 = undefined;
    for (&frame, 0..) |*byte, i| byte.* = @intCast(i % 251);
    try std.testing.expect(ring.push(&frame));
    var out: [implementation.frame_max]u8 = undefined;
    try std.testing.expectEqual(@as(?u16, implementation.frame_max), ring.pop(&out));
    try std.testing.expectEqualSlices(u8, &frame, &out);
}

test "count tracks pushes and pops one for one" {
    var ring: implementation.Ring = .{};
    const frame: [24]u8 = @splat(0x66);
    var out: [implementation.frame_max]u8 = undefined;
    try std.testing.expect(ring.push(&frame));
    try std.testing.expect(ring.push(&frame));
    try std.testing.expectEqual(@as(u16, 2), ring.count);
    _ = ring.pop(&out);
    try std.testing.expectEqual(@as(u16, 1), ring.count);
    _ = ring.pop(&out);
    try std.testing.expectEqual(@as(u16, 0), ring.count);
}

test "interleaved push and pop keeps distinct payloads matched" {
    var ring: implementation.Ring = .{};
    var out: [implementation.frame_max]u8 = undefined;
    const first: [10]u8 = @splat(0xA1);
    const second: [20]u8 = @splat(0xB2);
    try std.testing.expect(ring.push(&first));
    try std.testing.expect(ring.push(&second));
    try std.testing.expectEqual(@as(?u16, 10), ring.pop(&out));
    try std.testing.expectEqualSlices(u8, &first, out[0..10]);
    const third: [30]u8 = @splat(0xC3);
    try std.testing.expect(ring.push(&third));
    try std.testing.expectEqual(@as(?u16, 20), ring.pop(&out));
    try std.testing.expectEqualSlices(u8, &second, out[0..20]);
    try std.testing.expectEqual(@as(?u16, 30), ring.pop(&out));
    try std.testing.expectEqualSlices(u8, &third, out[0..30]);
}

test "pop reports the frame length, not the buffer capacity" {
    var ring: implementation.Ring = .{};
    const frame: [100]u8 = @splat(0x77);
    try std.testing.expect(ring.push(&frame));
    var out: [implementation.frame_max]u8 = undefined;
    try std.testing.expectEqual(@as(?u16, 100), ring.pop(&out));
}

test "a full ring drains completely and accepts a fresh round" {
    var ring: implementation.Ring = .{};
    const frame: [40]u8 = @splat(0x88);
    var out: [implementation.frame_max]u8 = undefined;
    var round: u8 = 0;
    while (round < 3) : (round += 1) {
        var i: u16 = 0;
        while (i < implementation.ring_slots) : (i += 1) {
            try std.testing.expect(ring.push(&frame));
        }
        try std.testing.expect(!ring.push(&frame));
        i = 0;
        while (i < implementation.ring_slots) : (i += 1) {
            try std.testing.expectEqual(@as(?u16, 40), ring.pop(&out));
        }
        try std.testing.expect(ring.pop(&out) == null);
    }
}

test "ringEvent: an empty ring reports no event" {
    try std.testing.expectEqual(implementation.event_none, implementation.ringEvent(0));
}

test "ringEvent: one queued frame reports rx_ready" {
    try std.testing.expectEqual(implementation.event_rx_ready, implementation.ringEvent(1));
}

test "ringEvent: a full ring still reports the single rx_ready bit" {
    try std.testing.expectEqual(
        implementation.event_rx_ready,
        implementation.ringEvent(implementation.ring_slots),
    );
}

test "linkEdge: agreement in the down state raises nothing" {
    try std.testing.expectEqual(implementation.event_none, implementation.linkEdge(.down, .down));
}

test "linkEdge: agreement in the up state raises nothing" {
    try std.testing.expectEqual(implementation.event_none, implementation.linkEdge(.up, .up));
}

test "linkEdge: down to up is the link_up edge" {
    try std.testing.expectEqual(implementation.event_link_up, implementation.linkEdge(.up, .down));
}

test "linkEdge: up to down is the link_down edge" {
    try std.testing.expectEqual(implementation.event_link_down, implementation.linkEdge(.down, .up));
}

test "linkEdge: an edge is one bit, never both" {
    const both = implementation.event_link_up | implementation.event_link_down;
    try std.testing.expect(implementation.linkEdge(.up, .down) != both);
    try std.testing.expect(implementation.linkEdge(.down, .up) != both);
}

test "the ring half and the controller half occupy different bits" {
    const controller = implementation.translateEvent(0x0000_0002);
    const ring = implementation.ringEvent(1);
    try std.testing.expectEqual(@as(u32, 0), controller & ring);
    try std.testing.expectEqual(
        implementation.event_error | implementation.event_rx_ready,
        controller | ring,
    );
}
