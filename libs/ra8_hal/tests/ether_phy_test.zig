//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const phy = @import("ether_phy");
const Code = phy.Code;

/// A PHY whose BMCR reset bit self-clears after `reset_reads` reads.
const FakePhy = struct {
    regs: [32]u16 = @splat(0),
    reset_reads: u32 = 2,
    fail_code: u16 = 0,
    reads: u32 = 0,
    last_write: u16 = 0,

    fn read(ctx: ?*anyopaque, _: u8, reg: u8, out: *u16) callconv(.c) u16 {
        const f: *FakePhy = @ptrCast(@alignCast(ctx.?));
        f.reads += 1;
        if (reg == phy.reg_control and f.reads >= f.reset_reads) f.regs[0] &= ~phy.bmcr_reset;
        out.* = f.regs[reg];
        return 0;
    }

    fn write(ctx: ?*anyopaque, _: u8, reg: u8, data: u16) callconv(.c) u16 {
        const f: *FakePhy = @ptrCast(@alignCast(ctx.?));
        if (f.fail_code != 0) return f.fail_code;
        f.regs[reg] = data;
        f.last_write = data;
        return 0;
    }

    fn cfg(f: *FakePhy, addr: u8) phy.Cfg {
        return .{ .io = .{ .read = read, .write = write, .ctx = f }, .phy_address = addr, .mii_type = 1, .reset_wait_us = 0 };
    }
};

test "open resets the PHY, rejects a second open and close tracks state" {
    var f = FakePhy{};
    var s = phy.State{};
    try std.testing.expectEqual(Code.ok, s.open(f.cfg(3)));
    try std.testing.expectEqual(@as(u16, 0), f.regs[0] & phy.bmcr_reset);
    try std.testing.expectEqual(Code.exists, s.open(f.cfg(3)));
    try std.testing.expectEqual(Code.ok, s.close());
    try std.testing.expectEqual(Code.invalid_state, s.close());
}

test "a stuck reset times out and an io error passes through, both leaving it closed" {
    var f = FakePhy{ .reset_reads = 1000 };
    var s = phy.State{};
    try std.testing.expectEqual(Code.hw_timeout, s.open(f.cfg(1)));
    var v: u16 = 0;
    try std.testing.expectEqual(Code.not_initialized, s.mdioRead(0, &v));
    var g = FakePhy{ .fail_code = 0x0E01 };
    try std.testing.expectEqual(@as(u16, 0x0E01), s.open(g.cfg(1)));
    try std.testing.expect(!s.opened);
    try std.testing.expectEqual(Code.invalid_arg, s.open(f.cfg(32)));
}

test "mdio read/write pass through and reject registers past 31" {
    var f = FakePhy{};
    var s = phy.State{};
    try std.testing.expectEqual(Code.not_initialized, s.mdioWrite(4, 1));
    _ = s.open(f.cfg(0));
    try std.testing.expectEqual(Code.ok, s.mdioWrite(4, 0x01E1));
    var v: u16 = 0;
    try std.testing.expectEqual(Code.ok, s.mdioRead(4, &v));
    try std.testing.expectEqual(@as(u16, 0x01E1), v);
    try std.testing.expectEqual(Code.invalid_arg, s.mdioRead(32, &v));
    try std.testing.expectEqual(Code.invalid_arg, s.mdioWrite(32, 0));
    try std.testing.expectEqual(Code.ok, s.autoNegotiateStart());
    try std.testing.expectEqual(@as(u16, 0x1200), f.last_write);
}

test "linkStatus resolves speed only when link and AN are both up" {
    var f = FakePhy{};
    var s = phy.State{};
    _ = s.open(f.cfg(0));
    var link: phy.Link = undefined;
    f.regs[phy.reg_status] = phy.bmsr_link_up;
    f.regs[phy.reg_an_partner] = phy.anar_100full;
    try std.testing.expectEqual(Code.ok, s.linkStatus(&link));
    try std.testing.expectEqual(phy.Speed.no_link, link.speed);
    try std.testing.expectEqual(@as(u8, 1), link.link_up);
    f.regs[phy.reg_status] = phy.bmsr_link_up | phy.bmsr_an_complete;
    try std.testing.expectEqual(Code.ok, s.linkStatus(&link));
    try std.testing.expectEqual(phy.Speed.s100f, link.speed);
    try std.testing.expectEqual(@as(u16, 0x0024), link.bmsr);
    try std.testing.expectEqual(@as(u16, 0x0024), s.last_bmsr);
}

test "speedFromPartner picks the highest advertised mode" {
    try std.testing.expectEqual(phy.Speed.s100f, phy.speedFromPartner(0x01E0));
    try std.testing.expectEqual(phy.Speed.s100h, phy.speedFromPartner(0x00A0));
    try std.testing.expectEqual(phy.Speed.s10f, phy.speedFromPartner(0x0060));
    try std.testing.expectEqual(phy.Speed.s10h, phy.speedFromPartner(0x0020));
    try std.testing.expectEqual(phy.Speed.no_link, phy.speedFromPartner(0));
}
