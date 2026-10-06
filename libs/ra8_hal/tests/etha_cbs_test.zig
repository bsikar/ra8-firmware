//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/etha_cbs.zig.

const std = @import("std");
const cbs = @import("etha_cbs");

/// One port's ETHA block; writes are logged in order.
const Regs = struct {
    mem: [0x110]u32 = @splat(0),
    log: [8][2]u32 = undefined,
    n: usize = 0,
    pub fn read32(self: *Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
        self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
    }
};

test "enable writes admin increment and limit, then the class bit" {
    var r = Regs{};
    r.mem[cbs.off_eacaec / 4] = 0x01;
    const p = cbs.Param{ .increment = 0x12345, .upper_lim = 0x7000_0001 };
    cbs.configure(&r, 3, &p);
    const want = [_][2]u32{ .{ 0x22C, 0x12345 }, .{ 0x24C, 0x7000_0001 }, .{ 0x200, 0x09 }, .{ 0x204, 0x08 } };
    try std.testing.expectEqual(want.len, r.n);
    for (want, 0..) |w, i| try std.testing.expectEqual(w, r.log[i]);
}

test "disable clears only the class bit and leaves the admin values" {
    var r = Regs{};
    r.mem[cbs.off_eacaec / 4] = 0xFF;
    r.mem[cbs.off_eacc / 4] = 0x81;
    r.mem[0x23C / 4] = 0x55;
    cbs.configure(&r, 7, null);
    try std.testing.expectEqual(@as(u32, 0x7F), r.mem[cbs.off_eacaec / 4]);
    try std.testing.expectEqual(@as(u32, 0x01), r.mem[cbs.off_eacc / 4]);
    try std.testing.expectEqual(@as(u32, 0x55), r.mem[0x23C / 4]);
}

test "param range is 20-bit increment and 31-bit limit" {
    try std.testing.expect(cbs.paramOk(&.{ .increment = 0xF_FFFF, .upper_lim = 0x7FFF_FFFF }));
    try std.testing.expect(!cbs.paramOk(&.{ .increment = 0x10_0000, .upper_lim = 0 }));
    try std.testing.expect(!cbs.paramOk(&.{ .increment = 0, .upper_lim = 0x8000_0000 }));
    try std.testing.expect(cbs.tcOk(7));
    try std.testing.expect(!cbs.tcOk(8));
}

test "state reads oper enable, gate and masked oper values" {
    var r = Regs{};
    r.mem[cbs.off_eacoem / 4] = 0x04;
    r.mem[cbs.off_eacgsm / 4] = 0xFB;
    r.mem[0x288 / 4] = 0xFFFF_FFFF;
    r.mem[0x2A8 / 4] = 0xFFFF_FFFF;
    const s = cbs.state(&r, 2);
    try std.testing.expectEqual(@as(u8, 1), s.enabled);
    try std.testing.expectEqual(@as(u8, 0), s.gate_open);
    try std.testing.expectEqual(cbs.Param{ .increment = 0xF_FFFF, .upper_lim = 0x7FFF_FFFF }, s.oper);
}

test "counters read as 16 bits in register order and clear to zero" {
    var r = Regs{};
    for (0..cbs.counter_count) |i| r.mem[0x100 + i] = 0xABCD_0000 + @as(u32, @intCast(i + 1));
    const c = cbs.readCounters(&r);
    try std.testing.expectEqual([5]u16{ 1, 2, 3, 4, 5 }, c.values);
    cbs.clearCounters(&r);
    for (0..cbs.counter_count) |i| try std.testing.expectEqual(@as(u32, 0), r.mem[0x100 + i]);
}
