//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the NVIC + ICU IELSR slot allocator (RA8FW-760).

const std = @import("std");
const isr = @import("isr");

const Op = enum { enable, disable, clear, prio, ielsr };

const Fake = struct {
    ielsr: [isr.slot_count]u32 = @splat(0xFFFF_FFFF),
    enabled: [isr.slot_count]bool = @splat(true),
    prio: [isr.slot_count]u8 = @splat(0),
    log: [8]Op = undefined,
    n: usize = 0,

    fn rec(self: *Fake, op: Op) void {
        if (self.n < self.log.len) self.log[self.n] = op;
        self.n += 1;
    }
    pub fn nvicEnable(self: *Fake, n: u16) void {
        self.enabled[n] = true;
        self.rec(.enable);
    }
    pub fn nvicDisable(self: *Fake, n: u16) void {
        self.enabled[n] = false;
        self.rec(.disable);
    }
    pub fn nvicClearPending(self: *Fake, _: u16) void {
        self.rec(.clear);
    }
    pub fn nvicSetPriority(self: *Fake, n: u16, p: u8) void {
        self.prio[n] = p;
        self.rec(.prio);
    }
    pub fn ielsrRead(self: *Fake, slot: u16) u32 {
        return self.ielsr[slot];
    }
    pub fn ielsrWrite(self: *Fake, slot: u16, v: u32) void {
        self.ielsr[slot] = v;
        self.rec(.ielsr);
    }
};

var hits: u32 = 0;
var last_ctx: ?*anyopaque = null;
fn handler(ctx: ?*anyopaque) callconv(.c) void {
    hits += 1;
    last_ctx = ctx;
}

fn fresh(f: *Fake) isr.Pool {
    var pool: isr.Pool = undefined;
    isr.init(&pool, f);
    f.n = 0;
    return pool;
}

test "init clears every slot, NVIC line and IELSR" {
    var f = Fake{};
    var pool: isr.Pool = undefined;
    isr.init(&pool, &f);
    for (0..isr.slot_count) |i| {
        try std.testing.expect(!pool[i].in_use);
        try std.testing.expect(!f.enabled[i]);
        try std.testing.expectEqual(@as(u32, 0), f.ielsr[i]);
    }
    try std.testing.expectEqual(@as(usize, 3 * isr.slot_count), f.n);
}

test "register binds the first free slot in hardware order" {
    var f = Fake{};
    var pool = fresh(&f);
    var slot: u16 = 0xAAAA;
    try std.testing.expectEqual(isr.ok, isr.register(&pool, &f, 0x1405, handler, null, 5, &slot));
    try std.testing.expectEqual(@as(u16, 0), slot);
    try std.testing.expectEqual(@as(u32, 0x005), f.ielsr[0]); // IELS masked to 10 bits
    try std.testing.expectEqual(@as(u8, 5), f.prio[0]);
    try std.testing.expect(f.enabled[0]);
    try std.testing.expectEqualSlices(Op, &.{ .ielsr, .clear, .prio, .enable }, f.log[0..4]);
    try std.testing.expectEqual(isr.ok, isr.register(&pool, &f, 0x20, handler, null, 0, null));
    try std.testing.expectEqual(@as(u16, 1), isr.findEvent(&pool, 0x20));
}

test "register rejects bad priority, duplicates and exhaustion" {
    var f = Fake{};
    var pool = fresh(&f);
    try std.testing.expectEqual(isr.err_invalid_arg, isr.register(&pool, &f, 1, handler, null, 16, null));
    try std.testing.expectEqual(isr.ok, isr.register(&pool, &f, 1, handler, null, 15, null));
    try std.testing.expectEqual(isr.err_exists, isr.register(&pool, &f, 1, handler, null, 1, null));
    for (2..isr.slot_count + 1) |e| {
        try std.testing.expectEqual(isr.ok, isr.register(&pool, &f, @intCast(e), handler, null, 1, null));
    }
    var slot: u16 = 7;
    try std.testing.expectEqual(isr.err_no_mem, isr.register(&pool, &f, 500, handler, null, 1, &slot));
    try std.testing.expectEqual(@as(u16, 7), slot);
}

test "unregister frees the slot and quiesces it" {
    var f = Fake{};
    var pool = fresh(&f);
    _ = isr.register(&pool, &f, 9, handler, null, 3, null);
    f.n = 0;
    try std.testing.expectEqual(isr.ok, isr.unregister(&pool, &f, 9));
    try std.testing.expectEqualSlices(Op, &.{ .disable, .ielsr, .clear }, f.log[0..3]);
    try std.testing.expect(!pool[0].in_use);
    try std.testing.expectEqual(@as(u32, 0), f.ielsr[0]);
    try std.testing.expectEqual(isr.err_not_found, isr.unregister(&pool, &f, 9));
    try std.testing.expectEqual(isr.slot_none, isr.findEvent(&pool, 9));
}

test "dispatch clears IR then calls the handler with its context" {
    var f = Fake{};
    var pool = fresh(&f);
    var token: u8 = 0;
    _ = isr.register(&pool, &f, 0x33, handler, &token, 2, null);
    f.ielsr[0] = 0x0101_0033;
    hits = 0;
    isr.dispatch(&pool, &f, 0);
    try std.testing.expectEqual(@as(u32, 0x0100_0033), f.ielsr[0]);
    try std.testing.expectEqual(@as(u32, 1), hits);
    try std.testing.expectEqual(@as(?*anyopaque, &token), last_ctx);
}

test "dispatch ignores out-of-range slots and empty handlers" {
    var f = Fake{};
    var pool = fresh(&f);
    hits = 0;
    isr.dispatch(&pool, &f, isr.slot_count);
    try std.testing.expectEqual(@as(usize, 0), f.n);
    isr.dispatch(&pool, &f, 4);
    try std.testing.expectEqual(@as(u32, 0), hits);
    try std.testing.expectEqual(@as(usize, 1), f.n);
}

test "setPriority updates a bound event only" {
    var f = Fake{};
    var pool = fresh(&f);
    try std.testing.expectEqual(isr.err_invalid_arg, isr.setPriority(&pool, &f, 1, 16));
    try std.testing.expectEqual(isr.err_not_found, isr.setPriority(&pool, &f, 1, 3));
    _ = isr.register(&pool, &f, 1, handler, null, 3, null);
    try std.testing.expectEqual(isr.ok, isr.setPriority(&pool, &f, 1, 12));
    try std.testing.expectEqual(@as(u8, 12), pool[0].priority);
    try std.testing.expectEqual(@as(u8, 12), f.prio[0]);
}

test "setDtc toggles DTCE on bound slots" {
    var f = Fake{};
    var pool = fresh(&f);
    try std.testing.expectEqual(isr.err_invalid_arg, isr.setDtc(&pool, &f, isr.slot_count, true));
    try std.testing.expectEqual(isr.err_not_found, isr.setDtc(&pool, &f, 0, true));
    _ = isr.register(&pool, &f, 0x44, handler, null, 1, null);
    try std.testing.expectEqual(isr.ok, isr.setDtc(&pool, &f, 0, true));
    try std.testing.expectEqual(@as(u32, 0x0100_0044), f.ielsr[0]);
    try std.testing.expectEqual(isr.ok, isr.setDtc(&pool, &f, 0, false));
    try std.testing.expectEqual(@as(u32, 0x044), f.ielsr[0]);
}
