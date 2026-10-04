//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/ipc_sem_ring.zig (RA8FW-606).

const std = @import("std");
const ipc = @import("ipc_sem_ring");

/// Models IPCSEMn read-to-set / W1C and the NMI windows.
const Fake = struct {
    sem: [16]u32 = [_]u32{0} ** 16,
    nmista: [2]u32 = .{ 0, 0 },
    nmiset: [2]u32 = .{ 0, 0 },
    nmiclr: [2]u32 = .{ 0, 0 },
    reads: usize = 0,
    barriers: usize = 0,
    events: [4]struct { ch: u8, ev: u8 } = undefined,
    event_n: usize = 0,
    errs: usize = 0,
    pub fn read32(self: *Fake, addr: usize) u32 {
        self.reads += 1;
        if (addr < ipc.nmi_base) {
            const i = (addr - ipc.base) / 4;
            const prev = self.sem[i];
            self.sem[i] = 1;
            return prev;
        }
        const u = (addr - ipc.nmi_base) / ipc.nmi_stride;
        return self.nmista[u];
    }
    pub fn write32(self: *Fake, addr: usize, value: u32) void {
        if (addr < ipc.nmi_base) {
            const i = (addr - ipc.base) / 4;
            self.sem[i] &= ~value;
            return;
        }
        const u = (addr - ipc.nmi_base) / ipc.nmi_stride;
        switch ((addr - ipc.nmi_base) % ipc.nmi_stride) {
            ipc.off_nmiset => self.nmiset[u] = value,
            ipc.off_nmiclr => self.nmiclr[u] = value,
            else => unreachable,
        }
    }
    pub fn barrier(self: *Fake) void {
        self.barriers += 1;
    }
    pub fn sendEvent(self: *Fake, ch: u8, ev: u8) u16 {
        self.events[self.event_n] = .{ .ch = ch, .ev = ev };
        self.event_n += 1;
        return ipc.ok;
    }
    pub fn err(self: *Fake, _: [*:0]const u8) void {
        self.errs += 1;
    }
};

test "register addresses match the HUM map" {
    try std.testing.expectEqual(@as(usize, 0x4002_003C), ipc.semAddr(15));
    try std.testing.expectEqual(@as(usize, 0x4002_0090), ipc.nmiAddr(1));
}

test "try take acquires once, then reports busy until released" {
    var f = Fake{};
    try std.testing.expectEqual(ipc.ok, ipc.semTryTake(&f, 3));
    try std.testing.expectEqual(ipc.busy, ipc.semTryTake(&f, 3));
    try std.testing.expectEqual(@as(usize, 1), f.barriers);
    try std.testing.expectEqual(ipc.ok, ipc.semRelease(&f, 3));
    try std.testing.expectEqual(@as(u32, 0), f.sem[3]);
    try std.testing.expectEqual(@as(usize, 2), f.barriers);
    try std.testing.expectEqual(ipc.invalid_arg, ipc.semTryTake(&f, 16));
    try std.testing.expectEqual(ipc.invalid_arg, ipc.semRelease(&f, 16));
}

test "take timeout spins at most the capped count" {
    var f = Fake{};
    f.sem[0] = 1;
    try std.testing.expectEqual(ipc.hw_timeout, ipc.semTakeTimeout(&f, 0, 5));
    try std.testing.expectEqual(@as(usize, 5), f.reads);
    f.reads = 0;
    try std.testing.expectEqual(ipc.hw_timeout, ipc.semTakeTimeout(&f, 0, 0xFFFF));
    try std.testing.expectEqual(@as(usize, 1024), f.reads);
    f.sem[1] = 0;
    try std.testing.expectEqual(ipc.ok, ipc.semTakeTimeout(&f, 1, 1));
    try std.testing.expectEqual(ipc.invalid_arg, ipc.semTakeTimeout(&f, 1, 0));
    try std.testing.expectEqual(ipc.invalid_arg, ipc.semTakeTimeout(&f, 16, 1));
}

test "is locked gives a free semaphore back and leaves a held one held" {
    var f = Fake{};
    var locked = true;
    try std.testing.expectEqual(ipc.ok, ipc.semIsLocked(&f, 2, &locked));
    try std.testing.expect(!locked);
    try std.testing.expectEqual(@as(u32, 0), f.sem[2]);
    f.sem[2] = 1;
    try std.testing.expectEqual(ipc.ok, ipc.semIsLocked(&f, 2, &locked));
    try std.testing.expect(locked);
    try std.testing.expectEqual(@as(u32, 1), f.sem[2]);
    try std.testing.expectEqual(ipc.null_ptr, ipc.semIsLocked(&f, 2, null));
    try std.testing.expectEqual(ipc.invalid_arg, ipc.semIsLocked(&f, 16, &locked));
    try std.testing.expectEqual(@as(usize, 1), f.errs);
}

test "nmi send, clear and status hit the unit's window" {
    var f = Fake{};
    try std.testing.expectEqual(ipc.ok, ipc.nmiSend(&f, 1));
    try std.testing.expectEqual(@as(u32, 1), f.nmiset[1]);
    try std.testing.expectEqual(ipc.ok, ipc.nmiClear(&f, 0));
    try std.testing.expectEqual(@as(u32, 1), f.nmiclr[0]);
    var pending = false;
    f.nmista[1] = 1;
    try std.testing.expectEqual(ipc.ok, ipc.nmiGetStatus(&f, 1, &pending));
    try std.testing.expect(pending);
    try std.testing.expectEqual(ipc.invalid_arg, ipc.nmiSend(&f, 2));
    try std.testing.expectEqual(ipc.invalid_arg, ipc.nmiClear(&f, 2));
    try std.testing.expectEqual(ipc.invalid_arg, ipc.nmiGetStatus(&f, 2, &pending));
    try std.testing.expectEqual(ipc.null_ptr, ipc.nmiGetStatus(&f, 0, null));
}

const Seen = struct {
    var unit: ?u8 = null;
    var seen_ctx: ?*anyopaque = null;
    fn handler(ctx: ?*anyopaque, unit_arg: u8) callconv(.c) void {
        unit = unit_arg;
        seen_ctx = ctx;
    }
};

test "dispatch runs the handler only when pending, then acks" {
    var f = Fake{};
    var token: u8 = 0;
    const slot = ipc.NmiSlot{ .func = Seen.handler, .ctx = &token };
    ipc.dispatchNmi(&f, &slot, 0);
    try std.testing.expectEqual(@as(?u8, null), Seen.unit);
    try std.testing.expectEqual(@as(u32, 0), f.nmiclr[0]);
    f.nmista[1] = 1;
    ipc.dispatchNmi(&f, &slot, 1);
    try std.testing.expectEqual(@as(?u8, 1), Seen.unit);
    try std.testing.expectEqual(@as(?*anyopaque, &token), Seen.seen_ctx);
    try std.testing.expectEqual(@as(u32, 1), f.nmiclr[1]);
    f.nmista[0] = 1;
    ipc.dispatchNmi(&f, &ipc.NmiSlot{}, 0);
    try std.testing.expectEqual(@as(u32, 1), f.nmiclr[0]);
    ipc.dispatchNmi(&f, &slot, 2);
}

var slots = [_]u32{0} ** 4;
var head: u32 = 7;
var tail: u32 = 7;

fn ring() ipc.Ring {
    return .{ .slots = &slots, .head = &head, .tail = &tail, .capacity = 4, .channel = 1, .sem_id = 5, .notify_id = 2 };
}

test "ring layout matches ra8_ipc_ring_t" {
    try std.testing.expectEqual(3 * @sizeOf(usize), @offsetOf(ipc.Ring, "capacity"));
    try std.testing.expectEqual(3 * @sizeOf(usize) + 4, @offsetOf(ipc.Ring, "channel"));
    try std.testing.expectEqual(3 * @sizeOf(usize) + 6, @offsetOf(ipc.Ring, "notify_id"));
}

test "ring init zeroes indices and validates its config" {
    var f = Fake{};
    var r = ring();
    try std.testing.expectEqual(ipc.ok, ipc.ringInit(&f, &r));
    try std.testing.expectEqual(@as(u32, 0), head);
    try std.testing.expectEqual(@as(u32, 0), tail);
    inline for (.{ .{ "capacity", 0 }, .{ "capacity", 3 }, .{ "channel", 4 }, .{ "sem_id", 16 }, .{ "notify_id", 8 } }) |case| {
        var bad = ring();
        @field(bad, case[0]) = case[1];
        try std.testing.expectEqual(ipc.invalid_arg, ipc.ringInit(&f, &bad));
    }
    var no_slots = ring();
    no_slots.slots = null;
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringInit(&f, &no_slots));
    var no_tail = ring();
    no_tail.tail = null;
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringInit(&f, &no_tail));
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringInit(&f, null));
    try std.testing.expectEqual(@as(usize, 3), f.errs);
}

test "ring produce and consume round trip, full and empty report" {
    var f = Fake{};
    var r = ring();
    head = 0xFFFF_FFFE;
    tail = 0xFFFF_FFFE;
    for (0..4) |i| try std.testing.expectEqual(ipc.ok, ipc.ringProduce(&f, &r, @intCast(10 + i)));
    var full = false;
    try std.testing.expectEqual(ipc.ok, ipc.ringIsFull(&f, &r, &full));
    try std.testing.expect(full);
    try std.testing.expectEqual(ipc.busy, ipc.ringProduce(&f, &r, 99));
    try std.testing.expectEqual(@as(u32, 0), f.sem[5]);
    try std.testing.expectEqual(@as(usize, 4), f.event_n);
    try std.testing.expectEqual(@as(u8, 2), f.events[0].ev);
    var got: u32 = 0;
    for (0..4) |i| {
        try std.testing.expectEqual(ipc.ok, ipc.ringConsume(&f, &r, &got));
        try std.testing.expectEqual(@as(u32, @intCast(10 + i)), got);
    }
    var empty = false;
    try std.testing.expectEqual(ipc.ok, ipc.ringIsEmpty(&f, &r, &empty));
    try std.testing.expect(empty);
    try std.testing.expectEqual(ipc.no_data, ipc.ringConsume(&f, &r, &got));
    try std.testing.expectEqual(@as(u32, 0), f.sem[5]);
}

test "ring ops report busy when the semaphore is held and reject nulls" {
    var f = Fake{};
    var r = ring();
    f.sem[5] = 1;
    var got: u32 = 0;
    try std.testing.expectEqual(ipc.busy, ipc.ringProduce(&f, &r, 1));
    try std.testing.expectEqual(ipc.busy, ipc.ringConsume(&f, &r, &got));
    try std.testing.expectEqual(@as(usize, 0), f.event_n);
    var b = false;
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringProduce(&f, null, 1));
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringConsume(&f, &r, null));
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringIsEmpty(&f, null, &b));
    try std.testing.expectEqual(ipc.null_ptr, ipc.ringIsFull(&f, &r, null));
    try std.testing.expectEqual(@as(usize, 4), f.errs);
}
