//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ev = @import("lvd_events");

const Fake = struct {
    mem: [16]u8 = [_]u8{0} ** 16,
    pvdlr: ?u8 = null,
    pvdsar: ?u32 = null,
    rmw_cr0: [4]usize = .{ 0, 0, 0, 0 },
    rmw_clr: [4]u8 = .{ 0, 0, 0, 0 },
    rmw_set: [4]u8 = .{ 0, 0, 0, 0 },
    rmw_calls: u8 = 0,
    errs: u8 = 0,

    pub fn read8(f: *Fake, addr: usize) u8 {
        return f.mem[addr];
    }
    pub fn write8(f: *Fake, addr: usize, value: u8) void {
        if (addr == ev.pvdlr_off) f.pvdlr = value else f.mem[addr] = value;
    }
    pub fn write32(f: *Fake, addr: usize, value: u32) void {
        std.debug.assert(addr == ev.pvdsar_off);
        f.pvdsar = value;
    }
    pub fn channelToIdx(_: *Fake, channel: u8, idx: *u8) u16 {
        idx.* = switch (channel) {
            1 => 0,
            2 => 1,
            4 => 2,
            5 => 3,
            else => return ev.invalid_arg,
        };
        return ev.ok;
    }
    pub fn map(_: *Fake, idx: u8) ev.Map {
        return .{ .cmpcr = 0, .cr0 = 8 + idx, .cr1 = 0, .sr = idx, .fcr = 0, .has_irq = idx < 2 };
    }
    pub fn cr0Rmw(f: *Fake, m: *const ev.Map, clr: u8, set: u8) void {
        f.rmw_cr0[f.rmw_calls] = m.cr0;
        f.rmw_clr[f.rmw_calls] = clr;
        f.rmw_set[f.rmw_calls] = set;
        f.rmw_calls += 1;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, _: u16) void {
        f.errs += 1;
    }
};

var hits: u8 = 0;
var last_ctx: ?*anyopaque = null;
var last_channel: u8 = 0;

fn onEvent(ctx: ?*anyopaque, channel: u8) callconv(.c) void {
    hits += 1;
    last_ctx = ctx;
    last_channel = channel;
}

test "set_security rejects bits outside PVD1 and PVD2" {
    var f = Fake{};
    try std.testing.expectEqual(ev.invalid_arg, ev.setSecurity(&f, 0x4));
    try std.testing.expectEqual(@as(?u32, null), f.pvdsar);
    try std.testing.expectEqual(ev.ok, ev.setSecurity(&f, 0x3));
    try std.testing.expectEqual(@as(?u32, 0x3), f.pvdsar);
}

test "unlock writes 0 and relock writes 1 to PVDLR" {
    var f = Fake{};
    try std.testing.expectEqual(ev.ok, ev.unlockN(&f));
    try std.testing.expectEqual(@as(?u8, 0), f.pvdlr);
    try std.testing.expectEqual(ev.ok, ev.relockN(&f));
    try std.testing.expectEqual(@as(?u8, 1), f.pvdlr);
}

test "enable_elc_event clears DET then sets CMPE on an m channel" {
    var f = Fake{};
    f.mem[1] = 0x03;
    try std.testing.expectEqual(ev.ok, ev.enableElcEvent(&f, 2));
    try std.testing.expectEqual(@as(u8, 0x02), f.mem[1]);
    try std.testing.expectEqual(@as(usize, 9), f.rmw_cr0[0]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_clr[0]);
    try std.testing.expectEqual(ev.cr0_cmpe, f.rmw_set[0]);
}

test "event calls refuse n channels and log bad channels" {
    var f = Fake{};
    var s = ev.State{};
    try std.testing.expectEqual(ev.not_supported, ev.enableElcEvent(&f, 4));
    try std.testing.expectEqual(ev.not_supported, ev.disableElcEvent(&f, 5));
    try std.testing.expectEqual(ev.not_supported, ev.attachChannelHandler(&s, &f, 4, onEvent, null));
    try std.testing.expectEqual(@as(u8, 0), f.errs);
    try std.testing.expectEqual(ev.invalid_arg, ev.disableElcEvent(&f, 3));
    try std.testing.expectEqual(ev.invalid_arg, ev.configureForStandby(&f, 0));
    try std.testing.expectEqual(@as(u8, 2), f.errs);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_calls);
}

test "disable_elc_event clears CMPE" {
    var f = Fake{};
    try std.testing.expectEqual(ev.ok, ev.disableElcEvent(&f, 1));
    try std.testing.expectEqual(ev.cr0_cmpe, f.rmw_clr[0]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_set[0]);
}

test "configure_for_standby clears RI and RN only on m channels" {
    var f = Fake{};
    try std.testing.expectEqual(ev.ok, ev.configureForStandby(&f, 1));
    try std.testing.expectEqual(ev.ok, ev.configureForStandby(&f, 5));
    try std.testing.expectEqual(ev.cr0_ri | ev.cr0_rn, f.rmw_clr[0]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_clr[1]);
    try std.testing.expectEqual(ev.cr0_dfdis, f.rmw_set[0]);
    try std.testing.expectEqual(ev.cr0_dfdis, f.rmw_set[1]);
}

test "cancel_deep_standby_path clears RI on both m channels" {
    var f = Fake{};
    try std.testing.expectEqual(ev.ok, ev.cancelDeepStandbyPath(&f));
    try std.testing.expectEqual(@as(u8, 2), f.rmw_calls);
    try std.testing.expectEqual(@as(usize, 8), f.rmw_cr0[0]);
    try std.testing.expectEqual(@as(usize, 9), f.rmw_cr0[1]);
    try std.testing.expectEqual(ev.cr0_ri, f.rmw_clr[1]);
}

test "filter_delay_us follows the HUM formula and clamps the divider" {
    // div 0: (2*2+3)=7 cycles at 32768 Hz -> 213 us, +1.
    try std.testing.expectEqual(@as(u32, 214), ev.filterDelayUs(0, 0));
    // div 3: (2*16+3)=35 cycles -> 1068 us, +1; div 9 clamps to 3.
    try std.testing.expectEqual(@as(u32, 1069), ev.filterDelayUs(3, 32768));
    try std.testing.expectEqual(@as(u32, 1069), ev.filterDelayUs(9, 0));
    try std.testing.expectEqual(@as(u32, 8), ev.filterDelayUs(0, 1_000_000));
}

test "dispatch prefers the channel handler and clears DET" {
    var f = Fake{};
    var s = ev.State{};
    var a: u8 = 0;
    var b: u8 = 0;
    hits = 0;
    _ = ev.attachHandler(&s, onEvent, &a);
    try std.testing.expectEqual(ev.ok, ev.attachChannelHandler(&s, &f, 1, onEvent, &b));
    f.mem[0] = 0x81;
    ev.dispatch(&s, &f, 1);
    try std.testing.expectEqual(@as(u8, 1), hits);
    try std.testing.expectEqual(@as(?*anyopaque, &b), last_ctx);
    try std.testing.expectEqual(@as(u8, 0x80), f.mem[0]);
    f.mem[1] = 0x01;
    ev.dispatch(&s, &f, 2);
    try std.testing.expectEqual(@as(?*anyopaque, &a), last_ctx);
    try std.testing.expectEqual(@as(u8, 2), last_channel);
}

test "dispatch ignores bad channels, n channels and an unlatched DET" {
    var f = Fake{};
    var s = ev.State{};
    hits = 0;
    _ = ev.attachHandler(&s, onEvent, null);
    ev.dispatch(&s, &f, 7);
    ev.dispatch(&s, &f, 4);
    ev.dispatch(&s, &f, 1);
    try std.testing.expectEqual(@as(u8, 0), hits);
    _ = ev.attachHandler(&s, null, null);
    f.mem[0] = 0x01;
    ev.dispatch(&s, &f, 1);
    try std.testing.expectEqual(@as(u8, 0), hits);
    try std.testing.expectEqual(@as(u8, 0), f.mem[0]);
}
