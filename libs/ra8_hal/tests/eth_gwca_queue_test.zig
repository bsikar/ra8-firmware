//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const q = @import("eth_gwca_queue");

const Fake = struct {
    gwdcc_regs: [q.max_queues]u32 = [_]u32{0} ** q.max_queues,
    gwtrc_regs: [2]u32 = .{ 0, 0 },
    nulls: u8 = 0,
    errors: u8 = 0,
    polls: u32 = 0,
    clear_at: ?u32 = null,

    pub fn gwdcc(f: *Fake, queue: u32) ?*volatile u32 {
        if (queue >= q.max_queues) return null;
        return &f.gwdcc_regs[queue];
    }
    pub fn gwtrc(f: *Fake, idx: u1) *volatile u32 {
        return &f.gwtrc_regs[idx];
    }
    pub fn nullPtr(f: *Fake, _: [*:0]const u8) u16 {
        f.nulls += 1;
        return q.null_ptr;
    }
    pub fn balrClear(f: *Fake, reg: *volatile u32, iter: u32) bool {
        f.polls += 1;
        if (f.clear_at == iter) reg.* = reg.* & ~q.gwdcc_balr;
        return (reg.* & q.gwdcc_balr) == 0;
    }
    pub fn logError(f: *Fake, _: [*:0]const u8) void {
        f.errors += 1;
    }
};

// The descriptor holds a 40-bit pointer (PTR[39:32] + PTR[31:0]), so the
// buffers and chains these tests point descriptors at live in static storage,
// which sits below 2^40 on the host, unlike the stack.
var g_chain: [4]q.Desc = undefined;
var g_table: [4]q.Desc = undefined;
var g_pool: [600]u8 = undefined;

fn ptrOf(d: *const q.Desc) usize {
    return (@as(usize, d.ptr_h) << 32) | d.ptr_l;
}

test "descriptor bitfield bytes match the C layout" {
    var d = q.Desc{ .b1 = 0xA0, .b2 = 0x0F };
    q.setDt(&d, q.dt_fsingle);
    try std.testing.expectEqual(@as(u8, 0x8F), d.b2);
    try std.testing.expectEqual(q.dt_fsingle, q.getDt(&d));
    try std.testing.expectEqual(@as(u8, 0xA0), d.b1);
}

test "composeGwdcc packs DQT SL EDE and DCP" {
    const cfg = q.QueueCfg{ .priority = 5, .is_tx = true, .stop_on_last = true, .extended = true };
    try std.testing.expectEqual(q.gwdcc_dqt | q.gwdcc_sl | q.gwdcc_ede | (5 << 16), q.composeGwdcc(&cfg));
    try std.testing.expectEqual(@as(u32, 0), q.composeGwdcc(&q.QueueCfg{}));
}

test "configureQueue writes GWDCC and the LINKFIX entry" {
    var f = Fake{};
    const table = &g_table;
    table.* = [_]q.Desc{.{ .b2 = 0xF3 }} ** 4;
    const chain = &g_chain;
    const cfg = q.QueueCfg{ .priority = 3, .is_tx = true, .chain_head = &chain[0] };
    try std.testing.expectEqual(q.ok, q.configureQueue(&f, table, 2, &cfg));
    try std.testing.expectEqual(q.gwdcc_dqt | (3 << 16), f.gwdcc_regs[2]);
    try std.testing.expectEqual(q.dt_linkfix, q.getDt(&table[2]));
    try std.testing.expectEqual(@as(u8, 0x03), table[2].b2);
    try std.testing.expectEqual(@intFromPtr(&chain[0]), ptrOf(&table[2]));
}

test "configureQueue rejects nulls, priority and queue range" {
    var f = Fake{};
    var table = [_]q.Desc{.{}} ** 1;
    var head = q.Desc{};
    try std.testing.expectEqual(q.null_ptr, q.configureQueue(&f, null, 0, &q.QueueCfg{ .chain_head = &head }));
    try std.testing.expectEqual(q.null_ptr, q.configureQueue(&f, &table, 0, null));
    try std.testing.expectEqual(q.null_ptr, q.configureQueue(&f, &table, 0, &q.QueueCfg{}));
    try std.testing.expectEqual(@as(u8, 3), f.nulls);
    try std.testing.expectEqual(q.invalid_arg, q.configureQueue(&f, &table, 0, &q.QueueCfg{ .priority = 8, .chain_head = &head }));
    try std.testing.expectEqual(q.invalid_arg, q.configureQueue(&f, &table, 32, &q.QueueCfg{ .chain_head = &head }));
}

test "initRing builds FEMPTY slots and a closing LINK" {
    var f = Fake{};
    const chain = &g_chain;
    chain.* = [_]q.Desc{.{ .b1 = 0xFF, .b2 = 0xFF, .ptr_l = 9 }} ** 4;
    try std.testing.expectEqual(q.ok, q.initRing(&f, chain, 4, 0x5EE));
    for (chain[0..3]) |*d| {
        try std.testing.expectEqual(q.dt_fempty, q.getDt(d));
        try std.testing.expectEqual(@as(u8, 0xEE), d.ds_l);
        try std.testing.expectEqual(@as(u8, 0x05), d.b1);
        try std.testing.expectEqual(@as(u32, 0), d.ptr_l);
    }
    try std.testing.expectEqual(q.dt_link, q.getDt(&chain[3]));
    try std.testing.expectEqual(@intFromPtr(&chain[0]), ptrOf(&chain[3]));
}

test "initRing rejects null, short rings and oversized slots" {
    var f = Fake{};
    var chain = [_]q.Desc{.{}} ** 2;
    try std.testing.expectEqual(q.null_ptr, q.initRing(&f, null, 2, 64));
    try std.testing.expectEqual(q.invalid_arg, q.initRing(&f, &chain, 1, 64));
    try std.testing.expectEqual(q.invalid_arg, q.initRing(&f, &chain, 2, 2049));
}

test "attachBuffers points each data slot at its pool offset" {
    var f = Fake{};
    var chain = [_]q.Desc{.{}} ** 3;
    const pool = &g_pool;
    try std.testing.expectEqual(q.ok, q.attachBuffers(&f, &chain, 3, 64, pool));
    try std.testing.expectEqual(@intFromPtr(&pool[0]), ptrOf(&chain[0]));
    try std.testing.expectEqual(@intFromPtr(&pool[64]), ptrOf(&chain[1]));
    try std.testing.expectEqual(@as(u32, 0), chain[2].ptr_l);
    try std.testing.expectEqual(q.null_ptr, q.attachBuffers(&f, null, 3, 64, pool));
    try std.testing.expectEqual(q.null_ptr, q.attachBuffers(&f, &chain, 3, 64, null));
    try std.testing.expectEqual(q.invalid_arg, q.attachBuffers(&f, &chain, 1, 64, pool));
    try std.testing.expectEqual(q.invalid_arg, q.attachBuffers(&f, &chain, 3, 0, pool));
}

test "setDescriptorBuffer and decodePtr round-trip" {
    var f = Fake{};
    var d = q.Desc{};
    const buf = &g_pool;
    try std.testing.expectEqual(q.ok, q.setDescriptorBuffer(&f, &d, buf));
    try std.testing.expectEqual(@intFromPtr(buf), @intFromPtr(q.decodePtr(&d).?));
    try std.testing.expectEqual(q.null_ptr, q.setDescriptorBuffer(&f, null, buf));
    try std.testing.expect(q.decodePtr(&q.Desc{}) == null);
}

test "kickTx sets the queue bit in GWTRC0 or GWTRC1" {
    var f = Fake{ .gwtrc_regs = .{ 1, 0 } };
    try std.testing.expectEqual(q.ok, q.kickTx(&f, 3));
    try std.testing.expectEqual(@as(u32, 0x9), f.gwtrc_regs[0]);
    try std.testing.expectEqual(q.ok, q.kickTx(&f, 33));
    try std.testing.expectEqual(@as(u32, 0x2), f.gwtrc_regs[1]);
    try std.testing.expectEqual(q.invalid_arg, q.kickTx(&f, 64));
}

test "findSlot wraps from start and reports no_data" {
    var f = Fake{};
    var chain = [_]q.Desc{.{}} ** 4;
    q.setDt(&chain[0], q.dt_fempty);
    var out: u32 = 99;
    try std.testing.expectEqual(q.ok, q.findSlot(&f, &chain, 4, q.dt_fempty, 1, &out));
    try std.testing.expectEqual(@as(u32, 0), out);
    try std.testing.expectEqual(q.no_data, q.findSlot(&f, &chain, 4, q.dt_fsingle, 0, &out));
    try std.testing.expectEqual(q.invalid_arg, q.findSlot(&f, &chain, 4, q.dt_fempty, 3, &out));
    try std.testing.expectEqual(q.invalid_arg, q.findSlot(&f, &chain, 1, q.dt_fempty, 0, &out));
    try std.testing.expectEqual(q.null_ptr, q.findSlot(&f, null, 4, q.dt_fempty, 0, &out));
    try std.testing.expectEqual(q.null_ptr, q.findSlot(&f, &chain, 4, q.dt_fempty, 0, null));
}

test "txFrame copies into the next FEMPTY slot and advances tail" {
    var f = Fake{};
    var chain = [_]q.Desc{.{}} ** 3;
    const pool = &g_pool;
    pool.* = [_]u8{0} ** 600;
    try std.testing.expectEqual(q.ok, q.initRing(&f, &chain, 3, 300));
    try std.testing.expectEqual(q.ok, q.attachBuffers(&f, &chain, 3, 300, pool));
    var frame: [258]u8 = undefined;
    for (&frame, 0..) |*b, i| b.* = @truncate(i);
    var tail: u32 = 1;
    try std.testing.expectEqual(q.ok, q.txFrame(&f, &chain, 3, &tail, &frame, 258, 300));
    try std.testing.expectEqual(q.dt_fsingle, q.getDt(&chain[1]));
    try std.testing.expectEqual(@as(u8, 0x02), chain[1].ds_l);
    try std.testing.expectEqual(@as(u8, 0x01), chain[1].b1 & 0xF);
    try std.testing.expectEqualSlices(u8, &frame, pool[300..558]);
    try std.testing.expectEqual(@as(u32, 0), tail);
}

test "txFrame rejects bad args and a full ring" {
    var f = Fake{};
    var chain = [_]q.Desc{.{}} ** 3;
    var tail: u32 = 0;
    const frame = [_]u8{1};
    try std.testing.expectEqual(q.null_ptr, q.txFrame(&f, null, 3, &tail, &frame, 1, 64));
    try std.testing.expectEqual(q.null_ptr, q.txFrame(&f, &chain, 3, null, &frame, 1, 64));
    try std.testing.expectEqual(q.null_ptr, q.txFrame(&f, &chain, 3, &tail, null, 1, 64));
    try std.testing.expectEqual(q.invalid_arg, q.txFrame(&f, &chain, 3, &tail, &frame, 0, 64));
    try std.testing.expectEqual(q.invalid_arg, q.txFrame(&f, &chain, 3, &tail, &frame, 65, 64));
    try std.testing.expectEqual(q.invalid_arg, q.txFrame(&f, &chain, 1, &tail, &frame, 1, 64));
    try std.testing.expectEqual(q.no_data, q.txFrame(&f, &chain, 3, &tail, &frame, 1, 64));
    q.setDt(&chain[0], q.dt_fempty);
    try std.testing.expectEqual(q.invalid_arg, q.txFrame(&f, &chain, 3, &tail, &frame, 1, 64));
}

test "reloadQueue rejects a queue with no GWDCC register" {
    var f = Fake{};
    try std.testing.expectEqual(q.invalid_arg, q.reloadQueue(&f, q.max_queues));
    try std.testing.expectEqual(@as(u32, 0), f.polls);
    try std.testing.expectEqual(@as(u8, 0), f.errors);
}

test "reloadQueue pulses BALR and returns once it self-clears" {
    var f = Fake{ .clear_at = 3 };
    f.gwdcc_regs[5] = q.gwdcc_dqt | (2 << 16);
    try std.testing.expectEqual(q.ok, q.reloadQueue(&f, 5));
    try std.testing.expectEqual(@as(u32, 4), f.polls);
    try std.testing.expectEqual(q.gwdcc_dqt | (2 << 16), f.gwdcc_regs[5]);
    try std.testing.expectEqual(@as(u8, 0), f.errors);
}

test "reloadQueue times out and logs when BALR never clears" {
    var f = Fake{};
    try std.testing.expectEqual(q.hw_timeout, q.reloadQueue(&f, 0));
    try std.testing.expectEqual(q.balr_spin, f.polls);
    try std.testing.expectEqual(q.gwdcc_balr, f.gwdcc_regs[0]);
    try std.testing.expectEqual(@as(u8, 1), f.errors);
}
