//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i3c_ctl.zig.

const std = @import("std");
const ctl = @import("i3c_ctl");

/// The I3C block up to INST; writes are logged in order.
const Regs = struct {
    mem: [16]u32 = @splat(0),
    log: [4][2]u32 = undefined,
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

test "MSDVAD packs the address at bit 16 with MDYADV and rejects 8-bit values" {
    try std.testing.expectEqual(@as(?u32, 0x8042_0000), ctl.msdvadWord(0x42));
    try std.testing.expectEqual(@as(?u32, 0x807F_0000), ctl.msdvadWord(0x7F));
    try std.testing.expectEqual(@as(?u32, 0x8000_0000), ctl.msdvadWord(0));
    try std.testing.expectEqual(@as(?u32, null), ctl.msdvadWord(0x80));
}

test "bus enable toggles only BCTL bit 31" {
    var r = Regs{};
    r.mem[ctl.off_bctl / 4] = 0x0000_0101;
    ctl.busEnable(&r, true);
    try std.testing.expectEqual(@as(u32, 0x8000_0101), r.mem[ctl.off_bctl / 4]);
    ctl.busEnable(&r, false);
    try std.testing.expectEqual(@as(u32, 0x0000_0101), r.mem[ctl.off_bctl / 4]);
}

test "clear status writes 0 to the masked INST bits only" {
    var r = Regs{};
    r.mem[ctl.off_inst / 4] = 0xFFFF_FFFF;
    ctl.clearStatus(&r, 0xF0F0_F0F0);
    try std.testing.expectEqual(@as(u32, 0x0F0F_0F0F), r.mem[ctl.off_inst / 4]);
}

test "stop clears BCTL then CECTL" {
    var r = Regs{};
    ctl.stop(&r);
    try std.testing.expectEqual(@as(usize, 2), r.n);
    try std.testing.expectEqual([2]u32{ 0x14, 0 }, r.log[0]);
    try std.testing.expectEqual([2]u32{ 0x10, 0 }, r.log[1]);
}
