//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/etha_irq.zig.

const std = @import("std");
const irq = @import("etha_irq");

/// One port's ETHA block; writes are logged in order.
const Regs = struct {
    mem: [0x180]u32 = [_]u32{0} ** 0x180,
    log: [16][2]u32 = undefined,
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

test "block n maps to EAEISn, EAEIEn, EAEIDn" {
    try std.testing.expectEqual(@as(usize, 0x500), irq.eis(0));
    try std.testing.expectEqual(@as(usize, 0x514), irq.eie(1));
    try std.testing.expectEqual(@as(usize, 0x528), irq.eid(2));
    try std.testing.expect(irq.blockOk(2));
    try std.testing.expect(!irq.blockOk(3));
}

test "status reads OPS, the three status words and the TAS monitor" {
    var r = Regs{};
    r.mem[irq.off_eams / 4] = 0xFFFF_FFFE;
    r.mem[0x500 / 4] = 1;
    r.mem[0x510 / 4] = 2;
    r.mem[0x520 / 4] = 3;
    r.mem[irq.off_eatasctm / 4] = 0x1234;
    const s = irq.status(&r);
    try std.testing.expectEqual(@as(u8, 2), s.ops);
    try std.testing.expectEqual([4]u32{ 1, 2, 3, 0x1234 }, [4]u32{ s.eaeis0, s.eaeis1, s.eaeis2, s.tas_cycle });
}

test "clear disables the mask, then clears only those status bits" {
    var r = Regs{};
    r.mem[0x510 / 4] = 0xFF;
    irq.clear(&r, 1, 0x0F);
    try std.testing.expectEqual([2]u32{ 0x518, 0x0F }, r.log[0]);
    try std.testing.expectEqual(@as(u32, 0xF0), r.mem[0x510 / 4]);
}

test "enable ORs and disable clears only the mask" {
    var r = Regs{};
    r.mem[0x524 / 4] = 0x100;
    irq.enable(&r, 2, 0x3);
    try std.testing.expectEqual(@as(u32, 0x103), r.mem[0x524 / 4]);
    irq.disable(&r, 2, 0x101);
    try std.testing.expectEqual(@as(u32, 0x2), r.mem[0x524 / 4]);
}

test "dispatch snapshots, disables what fired, then zeroes status" {
    var r = Regs{};
    r.mem[0x500 / 4] = 0xA;
    r.mem[0x510 / 4] = 0xB;
    r.mem[0x520 / 4] = 0xC;
    try std.testing.expectEqual([3]u32{ 0xA, 0xB, 0xC }, irq.dispatch(&r));
    const want = [_][2]u32{ .{ 0x508, 0xA }, .{ 0x518, 0xB }, .{ 0x528, 0xC }, .{ 0x500, 0 }, .{ 0x510, 0 }, .{ 0x520, 0 } };
    try std.testing.expectEqual(want.len, r.n);
    for (want, 0..) |w, i| try std.testing.expectEqual(w, r.log[i]);
}
