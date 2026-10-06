//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/etha_stats.zig.

const std = @import("std");
const st = @import("etha_stats");

/// One port's ETHA block. EAMS reads OPERATION after `settle` reads; a
/// negative count never settles.
const Regs = struct {
    mem: [0x40]u32 = @splat(0),
    settle: i32 = 2,
    eams_reads: u32 = 0,

    pub fn read32(self: *Regs, off: usize) u32 {
        if (off == st.off_eams) {
            self.eams_reads += 1;
            if (self.settle < 0 or self.eams_reads <= @as(u32, @intCast(self.settle))) return 1;
            return 0x7; // OPS = 3, other bits set
        }
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
    }
};

const Ops = struct {
    err: ?[]const u8 = null,
    fail_step: u8 = 0,
    calls: [4]u8 = undefined,
    n: usize = 0,
    seen: [3]u32 = undefined,

    fn step(self: *Ops, id: u8) u16 {
        self.calls[self.n] = id;
        self.n += 1;
        return if (self.fail_step == id) 0x30A else st.ok;
    }
    pub fn logError(self: *Ops, msg: [*:0]const u8) void {
        self.err = std.mem.span(msg);
    }
    pub fn phyReset(self: *Ops, port: u8, addr: u8) u16 {
        self.seen[0] = (@as(u32, port) << 8) | addr;
        return self.step(1);
    }
    pub fn phySetAdvertise(self: *Ops, _: u8, _: u8, caps: u16) u16 {
        self.seen[1] = caps;
        return self.step(2);
    }
    pub fn phyAutoNegStart(self: *Ops, _: u8, _: u8) u16 {
        return self.step(3);
    }
    pub fn phyAutoNegWait(self: *Ops, _: u8, _: u8, ms: u32, _: *anyopaque) u16 {
        self.seen[2] = ms;
        return self.step(4);
    }
};

test "the ABI structs match ra8_etha_types.h" {
    try std.testing.expectEqual(@as(usize, 28), @sizeOf(st.Stats));
    try std.testing.expectEqual(@as(usize, 0x14), @offsetOf(st.Stats, "ring_tx"));
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(st.PhyOpen));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(st.PhyOpen, "advertise"));
}

test "ring args are range-checked on all three values" {
    try std.testing.expect(st.ringArgsOk(1, 4096, 64));
    try std.testing.expect(st.ringArgsOk(4096, 1, 16383));
    try std.testing.expect(!st.ringArgsOk(0, 1, 64));
    try std.testing.expect(!st.ringArgsOk(1, 4097, 64));
    try std.testing.expect(!st.ringArgsOk(1, 1, 63));
    try std.testing.expect(!st.ringArgsOk(1, 1, 16384));
}

test "ring init records geometry and clamps EATDQDC to 11 bits" {
    var s = st.Stats{};
    var r = Regs{};
    var o = Ops{};
    try std.testing.expectEqual(st.ok, st.ringInit(&s, &r, &o, 64, 32, 1536));
    try std.testing.expectEqual(@as(u16, 64), s.ring_tx);
    try std.testing.expectEqual(@as(u16, 32), s.ring_rx);
    try std.testing.expectEqual(@as(u16, 1536), s.ring_buf);
    try std.testing.expectEqual(@as(u32, 64), r.mem[0x60 / 4]);
    try std.testing.expectEqual(@as(u32, 64), r.mem[0x7C / 4]);
    try std.testing.expectEqual(st.ok, st.ringInit(&s, &r, &o, 4096, 1, 64));
    try std.testing.expectEqual(@as(u32, 0x7FF), r.mem[0x6C / 4]);
}

test "bad ring args log and leave stats and registers alone" {
    var s = st.Stats{};
    var r = Regs{};
    var o = Ops{};
    try std.testing.expectEqual(st.invalid_arg, st.ringInit(&s, &r, &o, 0, 1, 64));
    try std.testing.expectEqualStrings("etha_descriptor_ring_init: ring args out of range", o.err.?);
    try std.testing.expectEqual(@as(u16, 0), s.ring_tx);
    try std.testing.expectEqual(@as(u32, 0), r.mem[0x60 / 4]);
}

test "account adds and saturates each counter" {
    var s = st.Stats{ .tx_ok = 0xFFFF_FFF0, .rx_drop = 7 };
    st.account(&s, 0x20, 1, 2, 3, 4);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), s.tx_ok);
    try std.testing.expectEqual(@as(u32, 1), s.tx_err);
    try std.testing.expectEqual(@as(u32, 2), s.rx_ok);
    try std.testing.expectEqual(@as(u32, 3), s.rx_err);
    try std.testing.expectEqual(@as(u32, 11), s.rx_drop);
}

test "to_operation writes EAMC and polls EAMS.OPS" {
    var r = Regs{};
    var o = Ops{};
    try std.testing.expectEqual(st.ok, st.toOperation(&r, &o));
    try std.testing.expectEqual(@as(u32, 3), r.mem[0]);
    try std.testing.expectEqual(@as(u32, 3), r.eams_reads);
    r = .{ .settle = -1 };
    try std.testing.expectEqual(st.hw_timeout, st.toOperation(&r, &o));
    try std.testing.expectEqual(st.mode_spin, r.eams_reads);
    try std.testing.expectEqualStrings("etha_to_operation: EAMS never reached OPERATION", o.err.?);
}

var link: u32 = 0;

test "open runs the PHY steps in order with the phy fields" {
    var r = Regs{};
    var o = Ops{};
    const phy = st.PhyOpen{ .phy_addr = 5, .advertise = 0x1E1, .timeout_ms = 3000 };
    try std.testing.expectEqual(st.ok, st.open(&r, &o, 1, &phy, &link));
    try std.testing.expectEqual([4]u8{ 1, 2, 3, 4 }, o.calls);
    try std.testing.expectEqual([3]u32{ 0x105, 0x1E1, 3000 }, o.seen);
}

test "open stops at the first failing step and names it" {
    const phy = st.PhyOpen{ .phy_addr = 1, .advertise = 0, .timeout_ms = 0 };
    const names = [_][]const u8{ "etha_open: phy_reset", "etha_open: set_advertise", "etha_open: auto_neg_start" };
    for (names, 1..) |name, i| {
        var r = Regs{};
        var o = Ops{ .fail_step = @intCast(i) };
        try std.testing.expectEqual(@as(u16, 0x30A), st.open(&r, &o, 0, &phy, &link));
        try std.testing.expectEqual(i, o.n);
        try std.testing.expectEqualStrings(name, o.err.?);
    }
    var r = Regs{};
    var o = Ops{ .fail_step = 4 };
    try std.testing.expectEqual(@as(u16, 0x30A), st.open(&r, &o, 0, &phy, &link));
    try std.testing.expect(o.err == null);
}

test "open does no PHY work when OPERATION is never reached" {
    var r = Regs{ .settle = -1 };
    var o = Ops{};
    const phy = st.PhyOpen{ .phy_addr = 1, .advertise = 0, .timeout_ms = 0 };
    try std.testing.expectEqual(st.hw_timeout, st.open(&r, &o, 0, &phy, &link));
    try std.testing.expectEqual(@as(usize, 0), o.n);
}
