//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const el = @import("eth_link");

const Fake = struct {
    regs: [16]u16 = @splat(0),
    fail_reg: ?u8 = null,
    set_link_ret: u16 = 0,
    modes: [8]u8 = @splat(0),
    n_modes: usize = 0,
    pis: ?u8 = null,
    delays: u32 = 0,
    bmsr_trace: ?u16 = null,
    speed_trace: ?u8 = null,
    errs: u8 = 0,
    infos: u8 = 0,

    pub fn mdioRead(f: *Fake, _: u8, reg: u8, out: *u16) u16 {
        if (f.fail_reg == reg) return 0x203;
        out.* = f.regs[reg];
        return 0;
    }
    pub fn setLink(f: *Fake, _: u8, pis: u8, _: u8, _: u8) u16 {
        f.pis = pis;
        return f.set_link_ret;
    }
    pub fn ethaSetMode(f: *Fake, _: u8, mode: u8) u16 {
        f.modes[f.n_modes] = mode;
        f.n_modes += 1;
        return 0;
    }
    pub fn delayMs(f: *Fake, _: u32) void {
        f.delays += 1;
    }
    pub fn traceBmsr(f: *Fake, v: u16) void {
        f.bmsr_trace = v;
    }
    pub fn traceAdvert(_: *Fake, _: u16, _: u16) void {}
    pub fn traceResync(f: *Fake, speed: u8, _: u8) void {
        f.speed_trace = speed;
    }
    pub fn info(f: *Fake, _: [*:0]const u8) void {
        f.infos += 1;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, _: u32) void {
        f.errs += 1;
    }
};

var link: el.Link = undefined;

test "pickSpeed keeps the highest advertised mode" {
    try std.testing.expectEqual(el.Neg{ .speed = el.lsc_10, .duplex = el.half }, el.pickSpeed(0, 0));
    try std.testing.expectEqual(el.Neg{ .speed = el.lsc_100, .duplex = el.full }, el.pickSpeed(0x01E0, 0));
    try std.testing.expectEqual(el.Neg{ .speed = el.lsc_1000, .duplex = el.half }, el.pickSpeed(0x0100, 0x0400));
    try std.testing.expectEqual(el.Neg{ .speed = el.lsc_1000, .duplex = el.full }, el.pickSpeed(0, 0x0C00));
}

test "null out and closed NIC are rejected in that order" {
    var f = Fake{};
    var r = false;
    try std.testing.expectEqual(el.null_ptr, el.linkStatus(&f, null, false, 0, &r));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    try std.testing.expectEqual(el.not_initialized, el.linkStatus(&f, &link, false, 0, &r));
}

test "link down reports BMSR/BMCR and skips the resync" {
    var f = Fake{};
    f.regs[el.reg_bmcr] = 0x2100;
    var r = false;
    try std.testing.expectEqual(el.ok, el.linkStatus(&f, &link, true, 1, &r));
    try std.testing.expectEqual(@as(u8, 0), link.link_up);
    try std.testing.expectEqual(@as(u16, 100), link.speed_mbps);
    try std.testing.expectEqual(@as(u8, 1), link.full_duplex);
    try std.testing.expect(!r and f.n_modes == 0);
}

test "a BMCR read failure logs twice per layer and returns the code" {
    var f = Fake{ .fail_reg = el.reg_bmcr };
    var r = false;
    try std.testing.expectEqual(@as(u16, 0x203), el.linkStatus(&f, &link, true, 0, &r));
    try std.testing.expectEqual(@as(u8, 4), f.errs);
}

test "link up resyncs the MAC once, GMII at 1000" {
    var f = Fake{};
    f.regs[el.reg_bmsr] = el.bmsr_link_up | el.bmsr_an_done;
    f.regs[el.reg_gbsr] = 0x0800;
    var r = false;
    try std.testing.expectEqual(el.ok, el.linkStatus(&f, &link, true, 0, &r));
    try std.testing.expect(r);
    try std.testing.expectEqual(@as(?u8, el.pis_gmii), f.pis);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 1, 3 }, f.modes[0..f.n_modes]);
    try std.testing.expectEqual(@as(?u8, el.lsc_1000), f.speed_trace);
    try std.testing.expectEqual(@as(u8, 1), f.infos);
    f.n_modes = 0;
    try std.testing.expectEqual(el.ok, el.linkStatus(&f, &link, true, 0, &r));
    try std.testing.expectEqual(@as(usize, 0), f.n_modes);
}

test "autoneg wait polls 80 times when AN never completes" {
    var f = Fake{};
    f.regs[el.reg_bmsr] = el.bmsr_link_up;
    var r = false;
    try std.testing.expectEqual(el.ok, el.linkStatus(&f, &link, true, 0, &r));
    try std.testing.expectEqual(@as(u32, 80), f.delays);
    try std.testing.expectEqual(@as(?u16, el.bmsr_link_up), f.bmsr_trace);
    try std.testing.expectEqual(@as(?u8, el.pis_mii), f.pis);
}

test "set_link failure still runs both mode switches and wins" {
    var f = Fake{ .set_link_ret = 0x104 };
    f.regs[el.reg_bmsr] = el.bmsr_link_up | el.bmsr_an_done;
    var r = false;
    try std.testing.expectEqual(@as(u16, 0x104), el.linkStatus(&f, &link, true, 0, &r));
    try std.testing.expectEqual(@as(usize, 4), f.n_modes);
    try std.testing.expect(!r);
}

test "an ANLPAR read failure stops before the MAC is touched" {
    var f = Fake{ .fail_reg = el.reg_anlpar };
    f.regs[el.reg_bmsr] = el.bmsr_link_up | el.bmsr_an_done;
    var r = false;
    try std.testing.expectEqual(@as(u16, 0x203), el.linkStatus(&f, &link, true, 0, &r));
    try std.testing.expectEqual(@as(usize, 0), f.n_modes);
    try std.testing.expectEqual(@as(u8, 1), el.channelToPort(7));
}
