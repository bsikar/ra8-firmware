//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ln = @import("mipi_dsi_lanes");

const Fake = struct {
    regs: [0x400 / 4]u32 = [_]u32{0} ** (0x400 / 4),
    writes: [8]struct { off: u16, value: u32 } = undefined,
    n: usize = 0,
    errs: u8 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
        if (f.n < f.writes.len) f.writes[f.n] = .{ .off = off, .value = value };
        f.n += 1;
    }
    pub fn errMsg(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
};

const State = struct {
    cont: bool = false,
    clk: bool = false,
    dat: bool = false,
    fn flags(s: *State) ln.Flags {
        return .{ .continuous_clock = &s.cont, .clock_ulps = &s.clk, .data_ulps = &s.dat };
    }
};

const Counter = struct {
    reads: *u32,
    ready_after: u32,
    pub fn read(c: Counter) u32 {
        c.reads.* += 1;
        return if (c.reads.* > c.ready_after) 0x5 else 0x1;
    }
};

test "waitEq returns ok once the masked value matches, else hw_timeout" {
    var reads: u32 = 0;
    try std.testing.expectEqual(ln.ok, ln.waitEq(Counter{ .reads = &reads, .ready_after = 3 }, 0x4, 0x4));
    try std.testing.expectEqual(@as(u32, 4), reads);
    reads = 0;
    try std.testing.expectEqual(ln.hw_timeout, ln.waitEq(Counter{ .reads = &reads, .ready_after = ln.busy_loop_max }, 0x4, 0x4));
    try std.testing.expectEqual(ln.busy_loop_max, reads);
}

test "softReset pulses SWRST then clears it" {
    var f = Fake{};
    try std.testing.expectEqual(ln.ok, ln.softReset(&f));
    try std.testing.expectEqual(@as(usize, 2), f.n);
    try std.testing.expectEqual(ln.rstcr_swrst, f.writes[0].value);
    try std.testing.expectEqual(@as(u32, 0), f.writes[1].value);
    try std.testing.expectEqual(ln.off_rstcr, f.writes[1].off);
}

test "hsClockStart sets HSCLMD only in continuous mode and waits for LP2HS" {
    var f = Fake{};
    var s = State{};
    f.regs[ln.off_plsr / 4] = ln.plsr_cllp2hs;
    try std.testing.expectEqual(ln.ok, ln.hsClockStart(&f, s.flags()));
    try std.testing.expectEqual(ln.hsclk_start, f.regs[ln.off_hsclksetr / 4]);
    s.cont = true;
    try std.testing.expectEqual(ln.ok, ln.hsClockStart(&f, s.flags()));
    try std.testing.expectEqual(ln.hsclk_start | ln.hsclk_continuous, f.regs[ln.off_hsclksetr / 4]);
    f.regs[ln.off_plsr / 4] = 0;
    try std.testing.expectEqual(ln.hw_timeout, ln.hsClockStart(&f, s.flags()));
}

test "hsClockStop clears HSCLKSETR and waits for HS2LP" {
    var f = Fake{};
    f.regs[ln.off_hsclksetr / 4] = 0x3;
    f.regs[ln.off_plsr / 4] = ln.plsr_clhs2lp;
    try std.testing.expectEqual(ln.ok, ln.hsClockStop(&f));
    try std.testing.expectEqual(@as(u32, 0), f.regs[ln.off_hsclksetr / 4]);
    f.regs[ln.off_plsr / 4] = ln.plsr_cllp2hs;
    try std.testing.expectEqual(ln.hw_timeout, ln.hsClockStop(&f));
}

test "ulpsEnter rejects none and clock lane in continuous mode" {
    var f = Fake{};
    var s = State{ .cont = true };
    try std.testing.expectEqual(ln.invalid_arg, ln.ulpsEnter(&f, s.flags(), ln.lane_none));
    try std.testing.expectEqual(@as(u8, 0), f.errs);
    try std.testing.expectEqual(ln.invalid_arg, ln.ulpsEnter(&f, s.flags(), ln.lane_clock));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "ulpsEnter pulses each lane once and tracks state" {
    var f = Fake{};
    var s = State{};
    try std.testing.expectEqual(ln.ok, ln.ulpsEnter(&f, s.flags(), ln.lane_clock | ln.lane_data));
    try std.testing.expectEqual(ln.ulpscr_dlent | ln.ulpscr_clent, f.regs[ln.off_ulpscr / 4]);
    try std.testing.expect(s.clk and s.dat);
    try std.testing.expectEqual(ln.ok, ln.ulpsEnter(&f, s.flags(), ln.lane_data));
    try std.testing.expectEqual(@as(u32, 0), f.regs[ln.off_ulpscr / 4]);
}

test "ulpsExit only exits lanes that are in ULPS" {
    var f = Fake{};
    var s = State{ .dat = true };
    try std.testing.expectEqual(ln.invalid_arg, ln.ulpsExit(&f, s.flags(), ln.lane_none));
    try std.testing.expectEqual(ln.ok, ln.ulpsExit(&f, s.flags(), ln.lane_clock | ln.lane_data));
    try std.testing.expectEqual(ln.ulpscr_dlexit, f.regs[ln.off_ulpscr / 4]);
    try std.testing.expect(!s.dat and !s.clk);
    s.clk = true;
    try std.testing.expectEqual(ln.ok, ln.ulpsExit(&f, s.flags(), ln.lane_clock));
    try std.testing.expectEqual(ln.ulpscr_clexit, f.regs[ln.off_ulpscr / 4]);
}
