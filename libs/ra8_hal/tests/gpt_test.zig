//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/gpt.zig (RA8FW-801, port of ra8_gpt.c).

const std = @import("std");
const gpt = @import("gpt");

const expectEqual = std.testing.expectEqual;

const Op = struct { addr: usize, val: u32 };

/// Register file for one channel window plus a write log.
const Fake = struct {
    mem: [64]u32 = [_]u32{0} ** 64,
    ops: [32]Op = undefined,
    n: usize = 0,

    pub fn read32(self: *Fake, a: usize) u32 {
        return self.mem[a / 4];
    }
    pub fn write32(self: *Fake, a: usize, v: u32) void {
        self.mem[a / 4] = v;
        self.ops[self.n] = .{ .addr = a, .val = v };
        self.n += 1;
    }
};

fn expectOps(f: *const Fake, want: []const Op) !void {
    try expectEqual(want.len, f.n);
    for (want, f.ops[0..f.n]) |x, y| {
        try expectEqual(x.addr, y.addr);
        try expectEqual(x.val, y.val);
    }
}

const o = gpt.off;
const unlock = Op{ .addr = o.gtwp, .val = 0xA500 };
const lock = Op{ .addr = o.gtwp, .val = 0xA501 };

test "error codes match ra8_err.h" {
    try expectEqual(@as(u16, 0), gpt.codes.ok);
    try expectEqual(@as(u16, 0x103), gpt.codes.invalid_arg);
    try expectEqual(@as(u16, 0x104), gpt.codes.invalid_state);
    try expectEqual(@as(u16, 0x504), gpt.codes.null_ptr);
}

test "channel map, bits and MSTP ids" {
    try expectEqual(@as(?usize, 0x40322000), gpt.regs(0));
    try expectEqual(@as(?usize, 0x40322D00), gpt.regs(13));
    try expectEqual(@as(?usize, null), gpt.regs(14));
    try expectEqual(@as(u32, 0x2000), gpt.bit(13));
    try expectEqual(@as(u16, 0x41F), gpt.mstp_ids[0]);
    try expectEqual(@as(u16, 0x41B), gpt.mstp_ids[9]);
    try expectEqual(@as(u16, 0x412), gpt.mstp_ids[13]);
    try expectEqual(@as(usize, 0x58), gpt.ccr(gpt.ccr_e));
}

test "GTCR compose" {
    try expectEqual(@as(u32, (4 << 16) | (5 << 23)), gpt.gtcr(4, 5));
    try expectEqual(@as(u32, 0), gpt.gtcr(0, 0));
}

test "GTIOR packing keeps the other pin" {
    const a = gpt.PinCfg{ .output_enable = true, .polarity = 0, .stop_level = 1, .disable_on_fault = 2 };
    try expectEqual(@as(u32, 0xFFFF0000 & ~@as(u32, 0) | 0x9 | 0x40 | 0x100 | 0x400), gpt.packGtior(0xFFFF0000, false, a));
    const b = gpt.PinCfg{ .output_enable = false, .polarity = 1, .stop_level = 0, .disable_on_fault = 3 };
    try expectEqual(@as(u32, 0x0000FFFF | (0x6 << 16) | 0x06000000), gpt.packGtior(0x0000FFFF | 0x015F0000, true, b));
    try expectEqual(@as(u32, 0x9), gpt.pattern(0));
    try expectEqual(@as(u32, 0x6), gpt.pattern(1));
}

test "free-run start sequence" {
    var f = Fake{};
    gpt.startFreeRun(&f, 0, 3, 1000);
    try expectOps(&f, &.{ unlock, .{ .addr = o.gtstp, .val = 8 }, .{ .addr = o.gtcr, .val = 1 }, .{ .addr = o.gtpr, .val = 1000 }, .{ .addr = o.gtcnt, .val = 0 }, .{ .addr = o.gtstr, .val = 8 }, lock });
}

test "init with and without auto start" {
    var f = Fake{};
    const cfg = gpt.Cfg{ .mode = 4, .prescaler = 2, .period = 500, .duty_a = 100, .duty_b = 200, .auto_start = false };
    gpt.initRegs(&f, 0, 1, &cfg);
    try expectOps(&f, &.{ unlock, .{ .addr = o.gtstp, .val = 2 }, .{ .addr = o.gtcr, .val = gpt.gtcr(4, 2) }, .{ .addr = o.gtpr, .val = 500 }, .{ .addr = o.gtpbr, .val = 500 }, .{ .addr = 0x4C, .val = 100 }, .{ .addr = 0x50, .val = 200 }, .{ .addr = o.gtcnt, .val = 0 }, lock });
    var g = Fake{};
    var auto = cfg;
    auto.auto_start = true;
    gpt.initRegs(&g, 0, 1, &auto);
    try expectEqual(@as(usize, 11), g.n);
    try expectEqual(gpt.gtcr(4, 2) | 1, g.mem[o.gtcr / 4]);
    try expectEqual(Op{ .addr = o.gtstr, .val = 2 }, g.ops[9]);
}

test "period set writes GTPR only while stopped" {
    var f = Fake{};
    gpt.periodSet(&f, 0, 77);
    try expectOps(&f, &.{ unlock, .{ .addr = o.gtpbr, .val = 77 }, .{ .addr = o.gtpr, .val = 77 }, .{ .addr = o.gtcnt, .val = 0 }, lock });
    var g = Fake{};
    g.mem[o.gtcr / 4] = 1;
    gpt.periodSet(&g, 0, 77);
    try expectOps(&g, &.{ unlock, .{ .addr = o.gtpbr, .val = 77 }, lock });
}

test "buffered duty and three-phase duty" {
    var f = Fake{};
    f.mem[o.gtber / 4] = 0x1;
    gpt.bufferedDuty(&f, 0, false, 10);
    try expectOps(&f, &.{ unlock, .{ .addr = 0x54, .val = 10 }, .{ .addr = o.gtber, .val = 0x10001 }, lock });
    var g = Fake{};
    gpt.bufferedDuty(&g, 0, true, 20);
    try expectOps(&g, &.{ unlock, .{ .addr = 0x58, .val = 20 }, .{ .addr = o.gtber, .val = 0x40000 }, lock });
    var h = Fake{};
    gpt.phaseDuty(&h, 0, 30);
    try expectOps(&h, &.{ unlock, .{ .addr = 0x54, .val = 30 }, .{ .addr = 0x58, .val = 30 }, .{ .addr = o.gtber, .val = 0x50000 }, lock });
}

test "dead time and status clear" {
    var f = Fake{};
    gpt.deadTime(&f, 0, 0, 0);
    try expectOps(&f, &.{ unlock, .{ .addr = o.gtdvu, .val = 0 }, .{ .addr = o.gtdvd, .val = 0 }, .{ .addr = o.gtdtcr, .val = 0 }, lock });
    var g = Fake{};
    gpt.deadTime(&g, 0, 0, 5);
    try expectEqual(@as(u32, 1), g.mem[o.gtdtcr / 4]);
    var h = Fake{};
    h.mem[o.gtst / 4] = 0xFFC3;
    gpt.clearFlags(&h, 0, 0x41);
    try expectOps(&h, &.{.{ .addr = o.gtst, .val = 0xFF82 }});
}
