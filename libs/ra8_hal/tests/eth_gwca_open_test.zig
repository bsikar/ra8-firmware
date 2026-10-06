//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_open.zig (RA8FW-859).

const std = @import("std");
const o = @import("eth_gwca_open");
const q = o.q;

/// Each call appends a letter; `fail_on` makes that letter's call fail.
const Hw = struct {
    log: *std.ArrayList(u8),
    fail_on: u8 = 0,
    pre: *u32,
    opn: *u32,
    fn step(self: Hw, c: u8) u16 {
        self.log.append(c) catch unreachable;
        return if (c == self.fail_on) q.invalid_arg else q.ok;
    }
    pub fn init(self: Hw) u16 {
        return self.step('i');
    }
    pub fn initRing(self: Hw, _: ?[*]volatile q.Desc, _: u32, _: u32) u16 {
        return self.step('n');
    }
    pub fn attachBuffers(self: Hw, _: ?[*]volatile q.Desc, _: u32, _: u32, _: ?[*]u8) u16 {
        return self.step('a');
    }
    pub fn bringUp(self: Hw, _: ?[*]volatile q.Desc, _: u32) u16 {
        return self.step('b');
    }
    pub fn setMode(self: Hw, opc: u32) u16 {
        return self.step(if (opc == o.opc_config) 'c' else 'o');
    }
    pub fn configureQueue(self: Hw, _: ?[*]volatile q.Desc, _: u32, cfg: *const q.QueueCfg) u16 {
        return self.step(if (cfg.is_tx and cfg.extended) 'T' else 'R');
    }
    pub fn reloadQueue(self: Hw, qi: u32) u16 {
        return self.step(if (qi == 1) 'x' else 'y');
    }
    pub fn setPreStep(self: Hw, v: u32) void {
        self.pre.* = v;
    }
    pub fn setOpenStep(self: Hw, v: u32) void {
        self.opn.* = v;
    }
    pub fn nullPtr(_: Hw, _: [*:0]const u8) u16 {
        return q.null_ptr;
    }
    pub fn fail(self: Hw, _: [*:0]const u8, code: u16) u16 {
        self.log.append('!') catch unreachable;
        return code;
    }
};

/// Descriptor pointers are 40-bit, so the TX ring is a file-scope global.
var tx: [3]o.ExtDesc = undefined;
var pool: [3 * 64]u8 = undefined;

fn freshState() !o.DefaultState {
    if (@intFromPtr(&tx) >= (@as(usize, 1) << 40)) return error.SkipZigTest;
    if (@intFromPtr(&pool) >= (@as(usize, 1) << 40)) return error.SkipZigTest;
    tx = [_]o.ExtDesc{.{ .info1_lo = 0xAA, .info1_hi = 0xBB }} ** 3;
    var s = std.mem.zeroes(o.DefaultState);
    s.tx_chain = &tx;
    s.tx_depth = 3;
    s.tx_pool = &pool;
    s.tx_slot_bytes = 64;
    s.rx_queue_index = 1;
    s.tx_queue_index = 2;
    s.rx_head = 5;
    s.tx_tail = 6;
    return s;
}

test "open runs every step in order and primes the TX chain" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var pre: u32 = 99;
    var opn: u32 = 99;
    var s = try freshState();
    try std.testing.expectEqual(q.ok, o.open(Hw{ .log = &log, .pre = &pre, .opn = &opn }, &s));
    try std.testing.expectEqualStrings("inabcRToxy", log.items);
    try std.testing.expectEqual(@as(u32, 4), pre);
    try std.testing.expectEqual(@as(u32, 3), opn);
    try std.testing.expectEqual(@as(u32, 0), s.rx_head);
    try std.testing.expectEqual(@as(u32, 0), s.tx_tail);
    for (0..2) |i| {
        try std.testing.expectEqual(o.dt_fempty, q.getDt(&tx[i].base));
        try std.testing.expectEqual(@as(u32, 64), q.getDs(&tx[i].base));
        try std.testing.expectEqual(@intFromPtr(&pool) + i * 64, @intFromPtr(q.decodePtr(&tx[i].base).?));
        try std.testing.expectEqual(@as(u32, 0), tx[i].info1_lo);
    }
    try std.testing.expectEqual(@as(u8, 14), q.getDt(&tx[2].base));
    try std.testing.expectEqual(@intFromPtr(&tx[0]), @intFromPtr(q.decodePtr(&tx[2].base).?));
}

test "open records the failing step on both trails" {
    const Case = struct { fail_on: u8, log: []const u8, pre: u32, opn: u32 };
    const cases = [_]Case{
        .{ .fail_on = 'i', .log = "i", .pre = 0x11, .opn = 0x11 },
        .{ .fail_on = 'a', .log = "na!", .pre = 0x12, .opn = 0x11 },
        .{ .fail_on = 'b', .log = "inab", .pre = 0x13, .opn = 0x11 },
        .{ .fail_on = 'c', .log = "inabc", .pre = 0x14, .opn = 0x11 },
        .{ .fail_on = 'R', .log = "inabcR!", .pre = 4, .opn = 0x12 },
        .{ .fail_on = 'o', .log = "inabcRTo", .pre = 4, .opn = 0x13 },
        .{ .fail_on = 'y', .log = "inabcRToxy", .pre = 4, .opn = 0x13 },
    };
    for (cases) |c| {
        var log = std.ArrayList(u8).init(std.testing.allocator);
        defer log.deinit();
        var pre: u32 = 99;
        var opn: u32 = 99;
        var s = try freshState();
        try std.testing.expectEqual(q.invalid_arg, o.open(Hw{ .log = &log, .fail_on = c.fail_on, .pre = &pre, .opn = &opn }, &s));
        try std.testing.expect(std.mem.endsWith(u8, log.items, c.log));
        try std.testing.expectEqual(c.pre, pre);
        try std.testing.expectEqual(c.opn, opn);
    }
}

test "txExtInit guards" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var pre: u32 = 0;
    var opn: u32 = 0;
    const hw = Hw{ .log = &log, .pre = &pre, .opn = &opn };
    _ = try freshState();
    try std.testing.expectEqual(q.null_ptr, o.open(hw, null));
    try std.testing.expectEqual(q.null_ptr, o.txExtInit(hw, null, 3, 64, &pool));
    try std.testing.expectEqual(q.null_ptr, o.txExtInit(hw, &tx, 3, 64, null));
    try std.testing.expectEqual(q.invalid_arg, o.txExtInit(hw, &tx, 1, 64, &pool));
    try std.testing.expectEqual(q.invalid_arg, o.txExtInit(hw, &tx, 3, 2049, &pool));
    try std.testing.expectEqual(q.ok, o.txExtInit(hw, &tx, 2, 2048, &pool));
}
