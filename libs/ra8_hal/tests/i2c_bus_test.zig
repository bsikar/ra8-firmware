//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i2c_bus.zig (RA8FW-886).

const std = @import("std");
const bus = @import("i2c_bus");

/// Byte register file. `rs_spins` keeps ICCR2.RS set for that many polls;
/// `stuck` makes every poll fail.
const Regs = struct {
    mem: [0x16]u8 = [_]u8{0} ** 0x16,
    polls: u32 = 0,
    rs_spins: u32 = 0,
    stuck: bool = false,

    pub fn read8(self: *Regs, off: usize) u8 {
        return self.mem[off];
    }
    pub fn write8(self: *Regs, off: usize, v: u8) void {
        self.mem[off] = v;
    }
    pub fn poll(self: *Regs, off: usize, _: u32, cond: bool) bool {
        self.polls += 1;
        if (self.stuck) return false;
        if (off == bus.off_iccr2 and self.rs_spins > 0) {
            self.rs_spins -= 1;
            if (self.rs_spins == 0) self.mem[off] &= ~bus.iccr2_rs;
            return false;
        }
        return cond;
    }
};

test "status prefers NACKF over AL" {
    try std.testing.expectEqual(@as(u16, 0), bus.status(0x80));
    try std.testing.expectEqual(bus.nack, bus.status(bus.icsr2_nackf | bus.icsr2_al));
    try std.testing.expectEqual(bus.hw_error, bus.status(bus.icsr2_al));
}

test "clearStatus drops only the W0C flags" {
    var r = Regs{};
    r.mem[bus.off_icsr2] = 0xFF;
    bus.clearStatus(&r);
    try std.testing.expectEqual(@as(u8, 0xE1), r.mem[bus.off_icsr2]);
}

test "open issues ST on an idle bus and waits out RS when held" {
    var r = Regs{};
    bus.open(&r, false);
    try std.testing.expectEqual(bus.iccr2_st, r.mem[bus.off_iccr2]);
    try std.testing.expectEqual(@as(u32, 0), r.polls);
    var h = Regs{ .rs_spins = 3 };
    bus.open(&h, true);
    try std.testing.expectEqual(@as(u8, 0), h.mem[bus.off_iccr2] & bus.iccr2_rs);
    try std.testing.expectEqual(@as(u32, 4), h.polls);
}

test "stop clears STOP, sets SP, and setNack leaves ACKWP clear" {
    var r = Regs{};
    r.mem[bus.off_icsr2] = bus.icsr2_stop;
    bus.stop(&r);
    try std.testing.expectEqual(@as(u8, 0), r.mem[bus.off_icsr2]);
    try std.testing.expectEqual(bus.iccr2_sp, r.mem[bus.off_iccr2]);
    bus.setNack(&r);
    try std.testing.expectEqual(bus.icmr3_ackbt, r.mem[bus.off_icmr3]);
}

test "busyGate and sendAddress" {
    var r = Regs{};
    r.mem[bus.off_iccr2] = bus.iccr2_bbsy;
    try std.testing.expectEqual(bus.busy, bus.busyGate(&r, false));
    try std.testing.expectEqual(@as(u16, 0), bus.busyGate(&r, true));
    r.mem[bus.off_icsr2] = bus.icsr2_tdre | bus.icsr2_nackf;
    try std.testing.expectEqual(bus.nack, bus.sendAddress(&r, 0xA0));
    try std.testing.expectEqual(@as(u8, 0xA0), r.mem[bus.off_icdrt]);
    var s = Regs{ .stuck = true };
    try std.testing.expectEqual(bus.hw_timeout, bus.sendAddress(&s, 0xA0));
    try std.testing.expectEqual(bus.poll_limit, s.polls);
    try std.testing.expectEqual(@as(u8, 0), s.mem[bus.off_icdrt]);
}
