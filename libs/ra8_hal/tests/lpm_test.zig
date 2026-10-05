//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/lpm.zig against a sparse fake register map that
//! logs every write in order.

const std = @import("std");
const lpm = @import("lpm");
const off = lpm.off;
const sysc = lpm.sysc;
const icu = lpm.icu;

const Write = struct { addr: usize, val: u32 };

const Fake = struct {
    addrs: [48]usize = undefined,
    vals: [48]u32 = undefined,
    n: usize = 0,
    log: [48]Write = undefined,
    nlog: usize = 0,
    reads: usize = 0,

    fn slot(self: *Fake, a: usize) ?usize {
        for (self.addrs[0..self.n], 0..) |x, i| if (x == a) return i;
        return null;
    }
    fn set(self: *Fake, a: usize, v: u32) void {
        const i = self.slot(a) orelse blk: {
            self.addrs[self.n] = a;
            self.n += 1;
            break :blk self.n - 1;
        };
        self.vals[i] = v;
    }
    fn get(self: *Fake, a: usize) u32 {
        self.reads += 1;
        return if (self.slot(a)) |i| self.vals[i] else 0;
    }
    fn put(self: *Fake, a: usize, v: u32) void {
        self.log[self.nlog] = .{ .addr = a, .val = v };
        self.nlog += 1;
        self.set(a, v);
    }
    pub fn read8(self: *Fake, a: usize) u8 {
        return @truncate(self.get(a));
    }
    pub fn write8(self: *Fake, a: usize, v: u8) void {
        self.put(a, v);
    }
    pub fn read16(self: *Fake, a: usize) u16 {
        return @truncate(self.get(a));
    }
    pub fn write16(self: *Fake, a: usize, v: u16) void {
        self.put(a, v);
    }
    pub fn read32(self: *Fake, a: usize) u32 {
        return self.get(a);
    }
    pub fn write32(self: *Fake, a: usize, v: u32) void {
        self.put(a, v);
    }
    fn expectLog(self: *Fake, want: []const Write) !void {
        try std.testing.expectEqualSlices(Write, want, self.log[0..self.nlog]);
    }
};

test "init encodes SBYCR, DPSBYCR, SSCR1 and clears LPSCR in order" {
    var f = Fake{};
    lpm.init(&f, .{ .io_port_keep = true, .opa_bus_keep = true, .sscr_fast_return = true, .dcdc_softstart = 2, .sscr_low_power = 1 });
    try f.expectLog(&.{
        .{ .addr = sysc + off.sbycr, .val = 0x40 },
        .{ .addr = sysc + off.dpsbycr, .val = 0x48 },
        .{ .addr = sysc + off.sscr1, .val = 0x05 },
        .{ .addr = sysc + off.lpscr, .val = 0 },
    });
    const none = lpm.Config{ .io_port_keep = false, .opa_bus_keep = false, .sscr_fast_return = false, .dcdc_softstart = 1, .sscr_low_power = 0 };
    try std.testing.expectEqual(@as(u8, 0), lpm.sbycrOf(none));
    try std.testing.expectEqual(@as(u8, 0x04), lpm.dpsbycrOf(none));
    try std.testing.expectEqual(@as(u8, 0), lpm.sscr1Of(none));
}

test "deinit restores reset values and clears wake enables" {
    var f = Fake{};
    lpm.deinit(&f);
    try f.expectLog(&.{
        .{ .addr = sysc + off.sbycr, .val = 0x40 },
        .{ .addr = sysc + off.dpsbycr, .val = 0x14 },
        .{ .addr = sysc + off.sscr1, .val = 0 },
        .{ .addr = sysc + off.lpscr, .val = 0 },
        .{ .addr = icu + off.wupen0, .val = 0 },
        .{ .addr = icu + off.wupen1, .val = 0 },
        .{ .addr = sysc + 0xA08, .val = 0 },
        .{ .addr = sysc + 0xA0C, .val = 0 },
        .{ .addr = sysc + 0xA10, .val = 0 },
        .{ .addr = sysc + 0xA14, .val = 0 },
    });
}

test "PRC1 unlock and relock keep the other low-byte bits" {
    var f = Fake{};
    f.set(sysc + off.prcr, 0x0009);
    lpm.setPrc1(&f, true);
    try std.testing.expectEqual(@as(u32, 0xA50B), f.vals[0]);
    lpm.setPrc1(&f, false);
    try std.testing.expectEqual(@as(u32, 0xA509), f.vals[0]);
    try std.testing.expectEqual(@as(u16, 0xA500), lpm.prcrValue(0xFF02, false));
}

test "arm_dpsier writes the enable, reads then zeroes the flag" {
    var f = Fake{};
    lpm.armDpsier(&f, 2, 0x1F);
    try std.testing.expectEqual(@as(usize, 1), f.reads);
    try f.expectLog(&.{
        .{ .addr = sysc + 0xA10, .val = 0x1F },
        .{ .addr = sysc + 0xA20, .val = 0 },
    });
    try std.testing.expectEqual(@as(usize, 0xA30), lpm.edgeOff(3));
    try std.testing.expectEqual(@as(usize, 0xA28), lpm.edgeOff(0));
}

test "snooze request and end sources set the right bits" {
    var f = Fake{};
    f.set(icu + off.wupen1, 0x1);
    f.set(sysc + 0xA14, 0x20);
    lpm.snoozeRequest(&f, true, true, true);
    lpm.snoozeEnd(&f, true, false, true, true);
    try f.expectLog(&.{
        .{ .addr = icu + off.wupen1, .val = 0x1101 },
        .{ .addr = icu + off.wupen0, .val = 1 << 18 },
        .{ .addr = sysc + 0xA14, .val = 0x27 },
        .{ .addr = sysc + 0xA24, .val = 0 },
    });
}

test "LDO standby refuses OPCM != 0 and merges SKEEP otherwise" {
    var f = Fake{};
    f.set(sysc + off.opccr, 0x01);
    try std.testing.expectEqual(lpm.codes.invalid_state, lpm.ldoStandby(&f, .{ .pll1 = 1, .pll2 = 1, .hoco = 1 }));
    try std.testing.expectEqual(@as(usize, 0), f.nlog);
    f.set(sysc + off.opccr, 0x10);
    f.set(sysc + off.pll2ldocr, 0x03);
    try std.testing.expectEqual(lpm.codes.ok, lpm.ldoStandby(&f, .{ .pll1 = 1, .pll2 = 0, .hoco = 1 }));
    try f.expectLog(&.{
        .{ .addr = sysc + off.pll1ldocr, .val = 0x02 },
        .{ .addr = sysc + off.pll2ldocr, .val = 0x01 },
        .{ .addr = sysc + off.hocoldocr, .val = 0x02 },
    });
}

test "clock stop opens PRC0, flips STOP, then locks all" {
    var f = Fake{};
    f.set(sysc + 0x400, 0x80);
    lpm.clockStop(&f, 2, true);
    try std.testing.expect(lpm.clockStopped(&f, 2));
    lpm.clockStop(&f, 2, false);
    try f.expectLog(&.{
        .{ .addr = sysc + off.prcr, .val = 0xA501 },
        .{ .addr = sysc + 0x400, .val = 0x81 },
        .{ .addr = sysc + off.prcr, .val = 0xA500 },
        .{ .addr = sysc + off.prcr, .val = 0xA501 },
        .{ .addr = sysc + 0x400, .val = 0x80 },
        .{ .addr = sysc + off.prcr, .val = 0xA500 },
    });
    try std.testing.expectEqualSlices(usize, &.{ 0x038, 0x036, 0x400, 0x032, 0xC00 }, &off.clock);
}

test "sleep modes arm LPSCR and SLEEPDEEP, then disarm" {
    for ([_]u8{ 0, 2, 5, 8, 9, 10 }) |m| try std.testing.expect(lpm.validMode(m));
    for ([_]u8{ 1, 3, 4, 6, 7, 11, 0xFF }) |m| try std.testing.expect(!lpm.validMode(m));
    var f = Fake{};
    f.set(lpm.scr, 0x10);
    lpm.armSleep(&f, 9);
    lpm.disarmSleep(&f);
    try f.expectLog(&.{
        .{ .addr = sysc + off.lpscr, .val = 9 },
        .{ .addr = lpm.scr, .val = 0x14 },
        .{ .addr = lpm.scr, .val = 0x10 },
        .{ .addr = sysc + off.lpscr, .val = 0 },
    });
    try std.testing.expectEqual(@as(u8, 0), lpm.lpscrFor(2));
    try std.testing.expect(lpm.sleepdeepFor(2) and !lpm.sleepdeepFor(0));
}

test "status, exit cause, retention mask and OPCCR wait" {
    try std.testing.expectEqual(@as(u32, 0x0508_1440), lpm.statusWord(0x40, 0x14, 0x08, 0x05));
    try std.testing.expectEqual(@as(u64, 0x0000_1000_8000_0001), lpm.exitCause(0x8000_0001, 0x1000));
    var f = Fake{};
    lpm.ramRetention(&f, .{ .pdramscr0_bits = 0xFFFF, .cpu0_tcm_keep = false, .cpu1_tcm_keep = true });
    try f.expectLog(&.{
        .{ .addr = sysc + off.pdramscr0, .val = 0x7FFF },
        .{ .addr = sysc + off.pdramscr1, .val = 0x02 },
    });
    f.set(sysc + off.opccr, 0x10);
    try std.testing.expect(!lpm.waitOpccr(&f, 3));
    f.set(sysc + off.opccr, 0x00);
    try std.testing.expect(lpm.waitOpccr(&f, 1));
}
