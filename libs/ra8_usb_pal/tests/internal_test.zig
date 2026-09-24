//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Core tests: the two MC/DC-promoted predicates, the status translation, the
//! address mask, the open-parameter rules, and the per-endpoint packet ring.

const std = @import("std");
const core = @import("implementation");

test "shouldDispatchEvent: both conditions true dispatches" {
    var sink: u8 = 0;
    try std.testing.expect(core.shouldDispatchEvent(&sink, 0x0001, 0x0000));
}

test "shouldDispatchEvent: null callback blocks (MC/DC condition 1)" {
    try std.testing.expect(!core.shouldDispatchEvent(null, 0x0001, 0x0000));
}

test "shouldDispatchEvent: mask equal to none blocks (MC/DC condition 2)" {
    var sink: u8 = 0;
    try std.testing.expect(!core.shouldDispatchEvent(&sink, 0x0000, 0x0000));
}

test "shouldDispatchEvent: none_value is a parameter, not a constant" {
    var sink: u8 = 0;
    try std.testing.expect(!core.shouldDispatchEvent(&sink, 0x8000, 0x8000));
    try std.testing.expect(core.shouldDispatchEvent(&sink, 0x0000, 0x8000));
}

test "epOutOfRange: in-range endpoint accepted" {
    try std.testing.expect(!core.epOutOfRange(1, 10));
    try std.testing.expect(!core.epOutOfRange(10, 10));
}

test "epOutOfRange: endpoint zero rejected (MC/DC condition 1)" {
    try std.testing.expect(core.epOutOfRange(0, 10));
}

test "epOutOfRange: above limit rejected (MC/DC condition 2)" {
    try std.testing.expect(core.epOutOfRange(11, 10));
}

test "epOutOfRange: every 8-bit address against the PAL limit" {
    var addr: u16 = 0;
    while (addr <= 255) : (addr += 1) {
        const byte: u8 = @intCast(addr);
        const expected = (byte == 0) or (byte > core.ep_max);
        try std.testing.expectEqual(expected, core.epOutOfRange(byte, core.ep_max));
    }
}

test "translate: zero status is no event" {
    try std.testing.expectEqual(core.event_none, core.translate(0));
}

test "translate: any raised bit becomes a controller error" {
    try std.testing.expectEqual(core.event_error, core.translate(0x0001));
    try std.testing.expectEqual(core.event_error, core.translate(0x8000));
    try std.testing.expectEqual(core.event_error, core.translate(0xFFFF));
}

test "translate: sweep of single-bit masks" {
    var bit: u4 = 0;
    while (true) {
        const mask: u16 = @as(u16, 1) << bit;
        try std.testing.expectEqual(core.event_error, core.translate(mask));
        if (bit == 15) break;
        bit += 1;
    }
}

test "maskEpAddr: descriptor form and bare number collapse" {
    try std.testing.expectEqual(@as(u8, 3), core.maskEpAddr(0x83));
    try std.testing.expectEqual(@as(u8, 3), core.maskEpAddr(3));
    try std.testing.expectEqual(@as(u8, 0), core.maskEpAddr(0x80));
    try std.testing.expectEqual(@as(u8, 0x7F), core.maskEpAddr(0xFF));
}

test "speedValid: only the two controller selectors" {
    try std.testing.expect(core.speedValid(0));
    try std.testing.expect(core.speedValid(1));
    try std.testing.expect(!core.speedValid(2));
    try std.testing.expect(!core.speedValid(255));
}

test "dirValid: only OUT and IN" {
    try std.testing.expect(core.dirValid(0));
    try std.testing.expect(core.dirValid(1));
    try std.testing.expect(!core.dirValid(2));
    try std.testing.expect(!core.dirValid(200));
}

test "typeAndPacketValid: accepts the four transfer types at a legal size" {
    var raw: u8 = 0;
    while (raw <= 3) : (raw += 1) {
        try std.testing.expect(core.typeAndPacketValid(raw, 64));
    }
}

test "typeAndPacketValid: rejects a type above intr" {
    try std.testing.expect(!core.typeAndPacketValid(4, 64));
    try std.testing.expect(!core.typeAndPacketValid(255, 64));
}

test "typeAndPacketValid: rejects zero and oversize packets" {
    try std.testing.expect(!core.typeAndPacketValid(2, 0));
    try std.testing.expect(core.typeAndPacketValid(2, core.xfer_max));
    try std.testing.expect(!core.typeAndPacketValid(2, core.xfer_max + 1));
}

test "slot defaults are unopened OUT/control" {
    const slot = core.EpSlot{};
    try std.testing.expect(!slot.opened);
    try std.testing.expectEqual(core.EpDir.out, slot.dir);
    try std.testing.expectEqual(core.EpType.control, slot.type);
    try std.testing.expectEqual(@as(u16, 0), slot.max_packet);
    try std.testing.expect(slot.isEmpty());
}

test "open records the configuration and empties the ring" {
    var slot = core.EpSlot{};
    try slot.push(&[_]u8{ 1, 2, 3 });
    slot.open(.in, .bulk, 512);
    try std.testing.expect(slot.opened);
    try std.testing.expectEqual(core.EpDir.in, slot.dir);
    try std.testing.expectEqual(core.EpType.bulk, slot.type);
    try std.testing.expectEqual(@as(u16, 512), slot.max_packet);
    try std.testing.expect(slot.isEmpty());
    try std.testing.expectEqual(@as(u16, 0), slot.head);
    try std.testing.expectEqual(@as(u16, 0), slot.tail);
}

test "push then pop returns the same bytes" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, 64);
    try slot.push(&[_]u8{ 0xDE, 0xAD, 0xBE, 0xEF });
    var out: [8]u8 = undefined;
    const n = try slot.pop(out[0..]);
    try std.testing.expectEqual(@as(u16, 4), n);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xDE, 0xAD, 0xBE, 0xEF }, out[0..4]);
    try std.testing.expect(slot.isEmpty());
}

test "pop truncates to the caller capacity" {
    var slot = core.EpSlot{};
    slot.open(.out, .bulk, 64);
    try slot.push(&[_]u8{ 1, 2, 3, 4, 5, 6 });
    var out: [3]u8 = undefined;
    const n = try slot.pop(out[0..]);
    try std.testing.expectEqual(@as(u16, 3), n);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3 }, out[0..]);
    try std.testing.expect(slot.isEmpty());
}

test "zero-length packet round-trips" {
    var slot = core.EpSlot{};
    slot.open(.in, .intr, 8);
    try slot.push(&[_]u8{});
    var out: [4]u8 = undefined;
    const n = try slot.pop(out[0..]);
    try std.testing.expectEqual(@as(u16, 0), n);
}

test "ring holds exactly ring_slots packets" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, 64);
    var i: u8 = 0;
    while (i < core.ring_slots) : (i += 1) {
        try slot.push(&[_]u8{i});
    }
    try std.testing.expect(slot.isFull());
    try std.testing.expectError(core.PushFault.Full, slot.push(&[_]u8{0xFF}));
}

test "pop on an empty ring faults" {
    var slot = core.EpSlot{};
    slot.open(.out, .bulk, 64);
    var out: [4]u8 = undefined;
    try std.testing.expectError(core.PopFault.Empty, slot.pop(out[0..]));
}

test "cursors wrap modulo ring_slots" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, 64);
    var out: [4]u8 = undefined;
    var round: u8 = 0;
    while (round < 10) : (round += 1) {
        try slot.push(&[_]u8{round});
        const n = try slot.pop(out[0..]);
        try std.testing.expectEqual(@as(u16, 1), n);
        try std.testing.expectEqual(round, out[0]);
    }
    try std.testing.expect(slot.head < core.ring_slots);
    try std.testing.expect(slot.tail < core.ring_slots);
}

test "FIFO order is preserved across a full ring" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, 64);
    try slot.push(&[_]u8{10});
    try slot.push(&[_]u8{20});
    try slot.push(&[_]u8{30});
    var out: [2]u8 = undefined;
    _ = try slot.pop(out[0..]);
    try std.testing.expectEqual(@as(u8, 10), out[0]);
    _ = try slot.pop(out[0..]);
    try std.testing.expectEqual(@as(u8, 20), out[0]);
    _ = try slot.pop(out[0..]);
    try std.testing.expectEqual(@as(u8, 30), out[0]);
}

test "a full-size packet fits the slot" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, core.xfer_max);
    var payload: [core.pkt_max]u8 = undefined;
    for (&payload, 0..) |*byte, i| byte.* = @intCast(i & 0xFF);
    try slot.push(payload[0..]);
    var out: [core.pkt_max]u8 = undefined;
    const n = try slot.pop(out[0..]);
    try std.testing.expectEqual(core.pkt_max, n);
    try std.testing.expectEqualSlices(u8, payload[0..], out[0..]);
}

test "resetRing clears depth without unopening" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, 64);
    try slot.push(&[_]u8{1});
    slot.resetRing();
    try std.testing.expect(slot.isEmpty());
    try std.testing.expect(slot.opened);
}

test "reset returns a slot to the unopened defaults" {
    var slot = core.EpSlot{};
    slot.open(.in, .bulk, 512);
    try slot.push(&[_]u8{1});
    slot.reset();
    try std.testing.expect(!slot.opened);
    try std.testing.expectEqual(core.EpDir.out, slot.dir);
    try std.testing.expectEqual(core.EpType.control, slot.type);
    try std.testing.expectEqual(@as(u16, 0), slot.max_packet);
    try std.testing.expect(slot.isEmpty());
}

test "table indexes every endpoint slot and resets them all" {
    var table = core.Table{};
    var ep: u8 = 1;
    while (ep <= core.ep_max) : (ep += 1) {
        table.at(ep).open(.in, .bulk, 64);
        try table.at(ep).push(&[_]u8{ep});
    }
    table.resetAll();
    ep = 0;
    while (ep < core.ep_table_len) : (ep += 1) {
        try std.testing.expect(!table.at(ep).opened);
        try std.testing.expect(table.at(ep).isEmpty());
    }
}

test "slots are independent" {
    var table = core.Table{};
    table.at(1).open(.in, .bulk, 64);
    table.at(2).open(.out, .intr, 8);
    try table.at(1).push(&[_]u8{0xA1});
    try std.testing.expect(table.at(2).isEmpty());
    try std.testing.expectEqual(@as(u16, 1), table.at(1).count);
}

test "event bit values match the public header" {
    try std.testing.expectEqual(@as(u16, 0x0000), core.event_none);
    try std.testing.expectEqual(@as(u16, 0x0001), core.event_reset);
    try std.testing.expectEqual(@as(u16, 0x0002), core.event_suspend);
    try std.testing.expectEqual(@as(u16, 0x0004), core.event_resume);
    try std.testing.expectEqual(@as(u16, 0x0008), core.event_setup);
    try std.testing.expectEqual(@as(u16, 0x0010), core.event_ep_in);
    try std.testing.expectEqual(@as(u16, 0x0020), core.event_ep_out);
    try std.testing.expectEqual(@as(u16, 0x0040), core.event_sof);
    try std.testing.expectEqual(@as(u16, 0x0080), core.event_attach);
    try std.testing.expectEqual(@as(u16, 0x0100), core.event_detach);
    try std.testing.expectEqual(@as(u16, 0x8000), core.event_error);
}

test "limit constants match the public header" {
    try std.testing.expectEqual(@as(u8, 10), core.ep_max);
    try std.testing.expectEqual(@as(u16, 64), core.ep0_max_packet);
    try std.testing.expectEqual(@as(u16, 64), core.bulk_max_fs);
    try std.testing.expectEqual(@as(u16, 512), core.bulk_max_hs);
    try std.testing.expectEqual(@as(u16, 1024), core.xfer_max);
    try std.testing.expectEqual(@as(u8, 0x7F), core.ep_addr_mask);
}

test "pal state enumerators match the public header" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(core.PalState.detached));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(core.PalState.attached));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(core.PalState.default));
    try std.testing.expectEqual(@as(u8, 3), @intFromEnum(core.PalState.addressed));
    try std.testing.expectEqual(@as(u8, 4), @intFromEnum(core.PalState.configd));
    try std.testing.expectEqual(@as(u8, 5), @intFromEnum(core.PalState.suspended));
}
