//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gptp.zig.

const std = @import("std");
const g = @import("eth_gptp");

/// The GPTP block as words; records every write in order.
const Regs = struct {
    mem: [0x40]u32 = [_]u32{0} ** 0x40,
    log: [32][2]u32 = undefined,
    n: usize = 0,
    reads: [4]usize = undefined,
    nr: usize = 0,

    pub fn read32(self: *Regs, off: usize) u32 {
        if (self.nr < self.reads.len) self.reads[self.nr] = off;
        self.nr += 1;
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        if (self.n < self.log.len) self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
        self.mem[off / 4] = v;
    }
};

const Ops = struct {
    mstp_err: u16 = 0,
    on: usize = 0,
    off: usize = 0,
    err: ?[]const u8 = null,
    fail_code: u16 = 0,
    info: usize = 0,

    pub fn mstpEnable(self: *Ops, id: u16) u16 {
        std.debug.assert(id == g.mstp_eswm);
        self.on += 1;
        return self.mstp_err;
    }
    pub fn mstpDisable(self: *Ops, id: u16) u16 {
        std.debug.assert(id == g.mstp_eswm);
        self.off += 1;
        return 0x55;
    }
    pub fn logError(self: *Ops, msg: [*:0]const u8) void {
        self.err = std.mem.span(msg);
    }
    pub fn logInfo(self: *Ops, _: [*:0]const u8) void {
        self.info += 1;
    }
    pub fn fail(self: *Ops, msg: [*:0]const u8, code: u16) void {
        self.err = std.mem.span(msg);
        self.fail_code = code;
    }
};

fn ready(r: *Regs, o: *Ops) g.State {
    var s = g.State{};
    std.debug.assert(s.init(r, o, 250_000_000) == g.ok);
    r.n = 0;
    r.nr = 0;
    return s;
}

test "timer offsets follow the 0x20 + 0x40 x t layout" {
    try std.testing.expectEqual(@as(usize, 0x20), g.timerOff(0, g.t_tivc));
    try std.testing.expectEqual(@as(usize, 0x98), g.timerOff(1, g.t_gptpu));
    try std.testing.expectEqual(@as(u16, 0x21E), g.mstp_eswm);
}

test "tivFromHz rounds 1e9 * 2^27 / clk" {
    try std.testing.expectEqual(@as(u32, 4 << 27), try g.tivFromHz(250_000_000));
    try std.testing.expectEqual(@as(u32, 447392427), try g.tivFromHz(300_000_000));
    try std.testing.expectError(error.InvalidArg, g.tivFromHz(0));
    try std.testing.expectError(error.OutOfRange, g.tivFromHz(31_250_000));
    try std.testing.expectEqual(g.invalid_arg, g.tivCode(g.tivFromHz(0)));
    try std.testing.expectEqual(g.out_of_range, g.tivCode(g.tivFromHz(1)));
}

test "init stops each timer, sets TIV and commits a zero offset U, M, L" {
    var r = Regs{};
    var o = Ops{};
    var s = g.State{};
    try std.testing.expectEqual(g.ok, s.init(&r, &o, 250_000_000));
    try std.testing.expect(s.configured);
    try std.testing.expectEqual(@as(usize, 10), r.n);
    try std.testing.expectEqual([2]u32{ 0x14, 1 }, r.log[0]);
    try std.testing.expectEqual([2]u32{ 0x20, 4 << 27 }, r.log[1]);
    try std.testing.expectEqual([2]u32{ 0x38, 0 }, r.log[2]);
    try std.testing.expectEqual([2]u32{ 0x34, 0 }, r.log[3]);
    try std.testing.expectEqual([2]u32{ 0x30, 0 }, r.log[4]);
    try std.testing.expectEqual([2]u32{ 0x14, 2 }, r.log[5]);
    try std.testing.expectEqual([2]u32{ 0x60, 4 << 27 }, r.log[6]);
    try std.testing.expectEqual(@as(usize, 1), o.info);
}

test "init failures touch no register" {
    var r = Regs{};
    var o = Ops{};
    var s = g.State{};
    try std.testing.expectEqual(g.invalid_arg, s.init(&r, &o, 0));
    try std.testing.expectEqualStrings("gptp_init: clk_hz", o.err.?);
    try std.testing.expectEqual(@as(usize, 0), o.on);
    o = .{ .mstp_err = 0x201 };
    try std.testing.expectEqual(@as(u16, 0x201), s.init(&r, &o, 250_000_000));
    try std.testing.expectEqualStrings("gptp_init: mstp enable", o.err.?);
    try std.testing.expect(!s.configured);
    try std.testing.expectEqual(@as(usize, 0), r.n);
}

test "every entry point but init refuses before init" {
    var r = Regs{};
    var o = Ops{};
    var s = g.State{};
    var w: u32 = 0;
    var b = false;
    var t: g.Time = undefined;
    var ns: u64 = 0;
    try std.testing.expectEqual(g.not_initialized, s.deinit(&r, &o));
    try std.testing.expectEqualStrings("ra8_eth_gptp_init has not run", o.err.?);
    try std.testing.expectEqual(g.not_initialized, s.ipVersion(&r, &o, &w));
    try std.testing.expectEqual(g.not_initialized, s.enable(&r, &o, 0));
    try std.testing.expectEqual(g.not_initialized, s.disable(&r, &o, 0));
    try std.testing.expectEqual(g.not_initialized, s.isEnabled(&r, &o, 0, &b));
    try std.testing.expectEqual(g.not_initialized, s.setIncrement(&r, &o, 0, 1));
    try std.testing.expectEqual(g.not_initialized, s.getIncrement(&r, &o, 0, &w));
    try std.testing.expectEqual(g.not_initialized, s.setOffset(&r, &o, 0, 0, 0));
    try std.testing.expectEqual(g.not_initialized, s.time(&r, &o, 0, &t));
    try std.testing.expectEqual(g.not_initialized, s.avtpNs(&r, &o, 0, &ns));
    try std.testing.expectEqual(g.not_initialized, s.enterStop(&r, &o));
    try std.testing.expectEqual(g.not_initialized, s.exitStop(&o));
    try std.testing.expectEqual(@as(usize, 0), r.n + r.nr);
}

test "a timer index past 1 is invalid_arg" {
    var r = Regs{};
    var o = Ops{};
    const s = ready(&r, &o);
    var w: u32 = 0;
    try std.testing.expectEqual(g.invalid_arg, s.enable(&r, &o, 2));
    try std.testing.expectEqual(g.invalid_arg, s.getIncrement(&r, &o, 2, &w));
    try std.testing.expectEqual(@as(usize, 0), r.n + r.nr);
}

test "enable, disable and is_enabled use the TE bit per timer" {
    var r = Regs{};
    var o = Ops{};
    const s = ready(&r, &o);
    var b = false;
    try std.testing.expectEqual(g.ok, s.enable(&r, &o, 1));
    try std.testing.expectEqual([2]u32{ 0x10, 2 }, r.log[0]);
    try std.testing.expectEqual(g.ok, s.isEnabled(&r, &o, 1, &b));
    try std.testing.expect(b);
    try std.testing.expectEqual(g.ok, s.isEnabled(&r, &o, 0, &b));
    try std.testing.expect(!b);
    try std.testing.expectEqual(g.ok, s.disable(&r, &o, 1));
    try std.testing.expectEqual([2]u32{ 0x14, 2 }, r.log[1]);
}

test "increment set and get, zero refused" {
    var r = Regs{};
    var o = Ops{};
    const s = ready(&r, &o);
    var w: u32 = 0;
    try std.testing.expectEqual(g.invalid_arg, s.setIncrement(&r, &o, 1, 0));
    try std.testing.expectEqual(g.ok, s.setIncrement(&r, &o, 1, 0x1234));
    try std.testing.expectEqual(g.ok, s.getIncrement(&r, &o, 1, &w));
    try std.testing.expectEqual(@as(u32, 0x1234), w);
    try std.testing.expectEqual(@as(usize, 0x60), r.reads[0]);
}

test "set_offset splits 48-bit seconds and range-checks" {
    var r = Regs{};
    var o = Ops{};
    const s = ready(&r, &o);
    try std.testing.expectEqual(g.invalid_arg, s.setOffset(&r, &o, 0, g.sec_max + 1, 0));
    try std.testing.expectEqual(g.invalid_arg, s.setOffset(&r, &o, 0, 0, g.nsec_max + 1));
    try std.testing.expectEqual(@as(usize, 0), r.n);
    try std.testing.expectEqual(g.ok, s.setOffset(&r, &o, 1, 0xABCD_1234_5678, 999_999_999));
    try std.testing.expectEqual([2]u32{ 0x78, 0xABCD }, r.log[0]);
    try std.testing.expectEqual([2]u32{ 0x74, 0x1234_5678 }, r.log[1]);
    try std.testing.expectEqual([2]u32{ 0x70, 999_999_999 }, r.log[2]);
}

test "get_time reads L first and masks the fields" {
    var r = Regs{};
    var o = Ops{};
    const s = ready(&r, &o);
    r.mem[0x50 / 4] = 0xC000_0005;
    r.mem[0x54 / 4] = 0x8765_4321;
    r.mem[0x58 / 4] = 0xFFFF_0042;
    var t: g.Time = undefined;
    try std.testing.expectEqual(g.ok, s.time(&r, &o, 0, &t));
    try std.testing.expectEqual(@as(u32, 5), t.nsec);
    try std.testing.expectEqual(@as(u64, 0x42_8765_4321), t.sec);
    try std.testing.expectEqual([3]usize{ 0x50, 0x54, 0x58 }, r.reads[0..3].*);
}

test "get_avtp_ns reads L then U" {
    var r = Regs{};
    var o = Ops{};
    const s = ready(&r, &o);
    r.mem[0x80 / 4] = 0x1111_2222;
    r.mem[0x84 / 4] = 0x3;
    var ns: u64 = 0;
    try std.testing.expectEqual(g.ok, s.avtpNs(&r, &o, 1, &ns));
    try std.testing.expectEqual(@as(u64, 0x3_1111_2222), ns);
    try std.testing.expectEqual([2]usize{ 0x80, 0x84 }, r.reads[0..2].*);
}

test "stop keeps the configuration; deinit zeroes it and gates MSTP" {
    var r = Regs{};
    var o = Ops{};
    var s = ready(&r, &o);
    var w: u32 = 0;
    r.mem[0] = 0x0100_0000;
    try std.testing.expectEqual(g.ok, s.ipVersion(&r, &o, &w));
    try std.testing.expectEqual(@as(u32, 0x0100_0000), w);
    try std.testing.expectEqual(@as(u16, 0x55), s.enterStop(&r, &o));
    try std.testing.expectEqual(@as(usize, 2), r.n);
    try std.testing.expect(s.configured);
    try std.testing.expectEqual(g.ok, s.exitStop(&o));
    r.n = 0;
    try std.testing.expectEqual(@as(u16, 0x55), s.deinit(&r, &o));
    try std.testing.expect(!s.configured);
    try std.testing.expectEqual([2]u32{ 0x20, 0 }, r.log[1]);
    try std.testing.expectEqual(@as(usize, 2), o.off);
}
