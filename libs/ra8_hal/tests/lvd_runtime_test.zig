//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const rt = @import("lvd_runtime");

/// Registers live in `mem`: channel idx i has sr=i, cmpcr=4+i, fcr=8+i,
/// cr0=12+i. Channels 1/2 (idx 0/1) are m channels, 4/5 (idx 2/3) are n.
const Fake = struct {
    mem: [16]u8 = @splat(0),
    writes: [8]usize = @splat(0),
    write_vals: [8]u8 = @splat(0),
    write_calls: u8 = 0,
    rmw_clr: [4]u8 = .{ 0, 0, 0, 0 },
    rmw_set: [4]u8 = .{ 0, 0, 0, 0 },
    rmw_cr0: [4]usize = .{ 0, 0, 0, 0 },
    rmw_calls: u8 = 0,
    ri: u8 = 1,
    errs: u8 = 0,
    last_err: u16 = 0,

    pub fn read8(f: *Fake, addr: usize) u8 {
        return f.mem[addr];
    }
    pub fn write8(f: *Fake, addr: usize, value: u8) void {
        f.mem[addr] = value;
        f.writes[f.write_calls] = addr;
        f.write_vals[f.write_calls] = value;
        f.write_calls += 1;
    }
    pub fn channelToIdx(_: *Fake, channel: u8, idx: *u8) u16 {
        idx.* = switch (channel) {
            1 => 0,
            2 => 1,
            4 => 2,
            5 => 3,
            else => return rt.invalid_arg,
        };
        return rt.ok;
    }
    pub fn map(_: *Fake, idx: u8) rt.Map {
        return .{ .cmpcr = 4 + idx, .cr0 = 12 + idx, .cr1 = 0, .sr = idx, .fcr = 8 + idx, .has_irq = idx < 2 };
    }
    pub fn cr0Rmw(f: *Fake, m: *const rt.Map, clr: u8, set: u8) void {
        f.rmw_cr0[f.rmw_calls] = m.cr0;
        f.rmw_clr[f.rmw_calls] = clr;
        f.rmw_set[f.rmw_calls] = set;
        f.rmw_calls += 1;
    }
    pub fn validateDiv(_: *Fake, div: u8) u16 {
        return if (div > 3) rt.invalid_arg else rt.ok;
    }
    pub fn readRi(f: *Fake, _: *const rt.Map) u8 {
        return f.ri;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, value: u16) void {
        f.errs += 1;
        f.last_err = value;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
        f.last_err = rt.null_ptr;
    }
};

const Fn = fn (*Fake, u8) u16;

fn bad(comptime f: anytype) !void {
    var fake = Fake{};
    try std.testing.expectEqual(rt.invalid_arg, f(&fake, 9));
    try std.testing.expectEqual(@as(u8, 1), fake.errs);
    try std.testing.expectEqual(rt.invalid_arg, fake.last_err);
    try std.testing.expectEqual(@as(u8, 0), fake.rmw_calls);
}

test "every channel-taking control logs and returns invalid_arg on a bad channel" {
    try bad(rt.enableIrq);
    try bad(rt.disableIrq);
    try bad(rt.enableReset);
    try bad(rt.disableReset);
    try bad(rt.enableCmpe);
    try bad(rt.disableCmpe);
    try bad(rt.clearStatus);
}

test "irq enable/disable toggle RIE on m channels, not_supported on n" {
    var f = Fake{};
    try std.testing.expectEqual(rt.ok, rt.enableIrq(&f, 1));
    try std.testing.expectEqual(rt.ok, rt.disableIrq(&f, 2));
    try std.testing.expectEqual(@as(usize, 12), f.rmw_cr0[0]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_clr[0]);
    try std.testing.expectEqual(rt.cr0_rie, f.rmw_set[0]);
    try std.testing.expectEqual(@as(usize, 13), f.rmw_cr0[1]);
    try std.testing.expectEqual(rt.cr0_rie, f.rmw_clr[1]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_set[1]);
    try std.testing.expectEqual(rt.not_supported, rt.enableIrq(&f, 4));
    try std.testing.expectEqual(rt.not_supported, rt.disableIrq(&f, 5));
    try std.testing.expectEqual(@as(u8, 2), f.rmw_calls);
}

test "reset enable sets RI|RIE on m and RE on n; disable clears the enable bit" {
    var f = Fake{};
    try std.testing.expectEqual(rt.ok, rt.enableReset(&f, 1));
    try std.testing.expectEqual(rt.ok, rt.enableReset(&f, 4));
    try std.testing.expectEqual(rt.ok, rt.disableReset(&f, 2));
    try std.testing.expectEqual(rt.ok, rt.disableReset(&f, 5));
    try std.testing.expectEqual(rt.cr0_ri | rt.cr0_rie, f.rmw_set[0]);
    try std.testing.expectEqual(rt.cr0_re, f.rmw_set[1]);
    try std.testing.expectEqual(rt.cr0_rie, f.rmw_clr[2]);
    try std.testing.expectEqual(rt.cr0_re, f.rmw_clr[3]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_set[2] | f.rmw_set[3]);
}

test "cmpe enable/disable work on both channel kinds" {
    var f = Fake{};
    try std.testing.expectEqual(rt.ok, rt.enableCmpe(&f, 4));
    try std.testing.expectEqual(rt.ok, rt.disableCmpe(&f, 1));
    try std.testing.expectEqual(rt.cr0_cmpe, f.rmw_set[0]);
    try std.testing.expectEqual(@as(usize, 14), f.rmw_cr0[0]);
    try std.testing.expectEqual(rt.cr0_cmpe, f.rmw_clr[1]);
}

test "set_filter disables, programs FSAMP, then re-enables only when asked" {
    var f = Fake{};
    try std.testing.expectEqual(rt.ok, rt.setFilter(&f, 1, 2, true));
    try std.testing.expectEqual(@as(u8, 3), f.rmw_calls);
    try std.testing.expectEqual(rt.cr0_dfdis, f.rmw_set[0]);
    try std.testing.expectEqual(rt.cr0_fsamp, f.rmw_clr[1]);
    try std.testing.expectEqual(@as(u8, 0x20), f.rmw_set[1]);
    try std.testing.expectEqual(rt.cr0_dfdis, f.rmw_clr[2]);
    var g = Fake{};
    try std.testing.expectEqual(rt.ok, rt.setFilter(&g, 4, 3, false));
    try std.testing.expectEqual(@as(u8, 2), g.rmw_calls);
    try std.testing.expectEqual(@as(u8, 0x30), g.rmw_set[1]);
}

test "set_filter rejects a bad channel or divider before touching CR0" {
    var f = Fake{};
    try std.testing.expectEqual(rt.invalid_arg, rt.setFilter(&f, 7, 0, true));
    try std.testing.expectEqual(rt.invalid_arg, rt.setFilter(&f, 1, 4, true));
    try std.testing.expectEqual(@as(u8, 2), f.errs);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_calls);
}

test "set_hysteresis_mode clears PVDE, writes RHSEL, restores PVDE" {
    var f = Fake{};
    f.mem[4] = rt.cmpcr_pvde | 0x05;
    try std.testing.expectEqual(rt.ok, rt.setHysteresisMode(&f, 1, 1));
    try std.testing.expectEqual(@as(u8, 3), f.write_calls);
    try std.testing.expectEqual(@as(usize, 4), f.writes[0]);
    try std.testing.expectEqual(@as(u8, 0x05), f.write_vals[0]);
    try std.testing.expectEqual(@as(usize, 8), f.writes[1]);
    try std.testing.expectEqual(@as(u8, 1), f.write_vals[1]);
    try std.testing.expectEqual(rt.cmpcr_pvde | 0x05, f.mem[4]);
    var g = Fake{};
    g.mem[6] = 0x03;
    try std.testing.expectEqual(rt.ok, rt.setHysteresisMode(&g, 4, 0));
    try std.testing.expectEqual(@as(u8, 0x03), g.mem[6]);
    try std.testing.expectEqual(@as(u8, 0), g.mem[10]);
}

test "set_hysteresis_mode guards channel, range and the RI-before-HVD rule" {
    var f = Fake{};
    try std.testing.expectEqual(rt.invalid_arg, rt.setHysteresisMode(&f, 3, 0));
    try std.testing.expectEqual(rt.invalid_arg, rt.setHysteresisMode(&f, 1, 2));
    f.ri = 0;
    try std.testing.expectEqual(rt.invalid_state, rt.setHysteresisMode(&f, 1, 1));
    try std.testing.expectEqual(@as(u8, 0), f.write_calls);
    // n channels have no RI, so HVD is allowed without it.
    try std.testing.expectEqual(rt.ok, rt.setHysteresisMode(&f, 5, 1));
}

test "set_negate_mode programs RN and enforces its guards" {
    var f = Fake{};
    try std.testing.expectEqual(rt.ok, rt.setNegateMode(&f, 1, 1));
    try std.testing.expectEqual(rt.ok, rt.setNegateMode(&f, 2, 0));
    try std.testing.expectEqual(rt.cr0_rn, f.rmw_clr[0]);
    try std.testing.expectEqual(rt.cr0_rn, f.rmw_set[0]);
    try std.testing.expectEqual(@as(u8, 0), f.rmw_set[1]);
    try std.testing.expectEqual(rt.invalid_arg, rt.setNegateMode(&f, 0, 0));
    try std.testing.expectEqual(rt.not_supported, rt.setNegateMode(&f, 4, 0));
    try std.testing.expectEqual(rt.invalid_arg, rt.setNegateMode(&f, 1, 2));
    f.mem[8] = rt.fcr_rhsel;
    try std.testing.expectEqual(rt.invalid_state, rt.setNegateMode(&f, 1, 1));
    try std.testing.expectEqual(rt.ok, rt.setNegateMode(&f, 1, 0));
    try std.testing.expectEqual(@as(u8, 3), f.rmw_calls);
}

test "get_status decodes DET/MON and checks its arguments" {
    var f = Fake{};
    var st = rt.Status{ .crossed = false, .above = false };
    f.mem[1] = rt.sr_det | rt.sr_mon;
    try std.testing.expectEqual(rt.ok, rt.getStatus(&f, 2, &st));
    try std.testing.expect(st.crossed and st.above);
    f.mem[0] = rt.sr_mon;
    try std.testing.expectEqual(rt.ok, rt.getStatus(&f, 1, &st));
    try std.testing.expect(!st.crossed and st.above);
    try std.testing.expectEqual(rt.null_ptr, rt.getStatus(&f, 1, null));
    try std.testing.expectEqual(rt.invalid_arg, rt.getStatus(&f, 6, &st));
    try std.testing.expectEqual(rt.not_supported, rt.getStatus(&f, 4, &st));
    try std.testing.expectEqual(@as(u8, 2), f.errs);
}

test "clear_status drops only DET on m channels" {
    var f = Fake{};
    f.mem[0] = rt.sr_det | rt.sr_mon | 0x80;
    try std.testing.expectEqual(rt.ok, rt.clearStatus(&f, 1));
    try std.testing.expectEqual(rt.sr_mon | 0x80, f.mem[0]);
    try std.testing.expectEqual(rt.not_supported, rt.clearStatus(&f, 4));
    try std.testing.expectEqual(@as(u8, 1), f.write_calls);
}

test "Status mirrors ra8_lvd_status_t" {
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(rt.Status));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(rt.Status, "above"));
}
