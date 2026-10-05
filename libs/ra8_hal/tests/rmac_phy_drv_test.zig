//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Callback-driven PHY driver logic (RA8FW-765).

const std = @import("std");
const drv = @import("rmac_phy_drv");

const Fake = struct {
    regs: [32]u16 = [_]u16{0} ** 32,
    fail_read: ?u8 = null,
    fail_write: ?u8 = null,
    reset_reads: u16 = 0,
    reads: u16 = 0,
    writes: [8]u8 = undefined,
    nw: usize = 0,

    pub fn read(self: *Fake, reg: u8, out: *u16) u16 {
        self.reads += 1;
        if (self.fail_read == reg) return 0x201;
        if (reg == drv.reg_control and self.regs[0] & drv.bmcr_reset != 0) {
            if (self.reset_reads == 0) self.regs[0] &= ~drv.bmcr_reset else self.reset_reads -= 1;
            out.* = self.regs[0];
            return 0;
        }
        out.* = self.regs[reg];
        return 0;
    }
    pub fn write(self: *Fake, reg: u8, value: u16) u16 {
        if (self.fail_write == reg) return 0x202;
        self.writes[self.nw] = reg;
        self.nw += 1;
        self.regs[reg] = value;
        return 0;
    }
};

test "reset clears on the first poll" {
    var f = Fake{};
    try std.testing.expectEqual(drv.ok, drv.resetAndWait(&f, 32));
    try std.testing.expectEqual(@as(u16, 1), f.reads);
}

test "reset that never clears times out after poll_max reads" {
    var f = Fake{ .reset_reads = 100 };
    try std.testing.expectEqual(drv.err_hw_timeout, drv.resetAndWait(&f, 5));
    try std.testing.expectEqual(@as(u16, 5), f.reads);
}

test "reset propagates write and read errors" {
    var w = Fake{ .fail_write = drv.reg_control };
    try std.testing.expectEqual(@as(u16, 0x202), drv.resetAndWait(&w, 4));
    var r = Fake{ .fail_read = drv.reg_control };
    try std.testing.expectEqual(@as(u16, 0x201), drv.resetAndWait(&r, 4));
}

test "advertise writes reg 9 only with gigabit" {
    var f = Fake{};
    try std.testing.expectEqual(drv.ok, drv.programAdvertise(&f, 0x01E1, 0));
    try std.testing.expectEqualSlices(u8, &.{4}, f.writes[0..f.nw]);
    var g = Fake{};
    try std.testing.expectEqual(drv.ok, drv.programAdvertise(&g, 0x01E1, 0x0300));
    try std.testing.expectEqualSlices(u8, &.{ 4, 9 }, g.writes[0..g.nw]);
    try std.testing.expectEqual(@as(u16, 0x0300), g.regs[9]);
}

test "speed resolution prefers 1000T, falls back to the partner" {
    var f = Fake{};
    f.regs[drv.reg_1000t_status] = drv.msr_1000half;
    var l = drv.Link{};
    drv.resolve(&f, 0x0300, &l);
    try std.testing.expectEqual(drv.speed_1000h, l.speed);
    try std.testing.expectEqual(@as(u16, 0), l.partner_ability);

    var e = Fake{ .fail_read = drv.reg_1000t_status };
    e.regs[drv.reg_an_partner] = drv.lpa_100half | drv.lpa_10full;
    var m = drv.Link{};
    drv.resolve(&e, 0x0300, &m);
    try std.testing.expectEqual(drv.speed_100h, m.speed);
    try std.testing.expectEqual(@as(u16, 0x00C0), m.partner_ability);
}

test "partner read failure leaves no link and no ability" {
    var f = Fake{ .fail_read = drv.reg_an_partner };
    var l = drv.Link{};
    drv.resolve(&f, 0, &l);
    try std.testing.expectEqual(drv.speed_no_link, l.speed);
    try std.testing.expectEqual(@as(?u8, null), drv.speedFromLpa(0x0001));
    try std.testing.expectEqual(@as(?u8, drv.speed_10h), drv.speedFromLpa(drv.lpa_10half));
}

test "fromBmsr flags and speedOk" {
    var l = drv.Link{ .speed = 4, .partner_ability = 9 };
    try std.testing.expect(!drv.fromBmsr(0x0004, &l));
    try std.testing.expectEqual(@as(u8, 1), l.link_up);
    try std.testing.expectEqual(@as(u8, 0), l.auto_neg_done);
    try std.testing.expectEqual(drv.speed_no_link, l.speed);
    try std.testing.expectEqual(@as(u16, 0), l.partner_ability);
    try std.testing.expect(drv.fromBmsr(0x0024, &l));
    try std.testing.expect(drv.speedOk(0, 0x0800, 0x0800));
    try std.testing.expect(!drv.speedOk(0, 0, 0x0800));
    try std.testing.expect(!drv.speedOk(0x103, 0x0800, 0x0800));
}
