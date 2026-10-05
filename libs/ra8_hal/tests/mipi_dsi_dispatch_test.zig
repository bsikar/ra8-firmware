//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const dp = @import("mipi_dsi_dispatch");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

const off_isr: u16 = 0x000;
const off_rxsr: u16 = 0x200;
const off_rxscr: u16 = 0x204;
const off_rxrinfoowscr: u16 = 0x23C;
const off_rxppd0r: u16 = 0x2C0;
const off_ferrscr: u16 = 0x304;
const off_plscr: u16 = 0x324;
const off_vmscr: u16 = 0x414;
const off_sqch0scr: u16 = 0x5D4;
const off_sqch1scr: u16 = 0x614;

const Fake = struct {
    regs: [0x640 / 4]u32 = [_]u32{0} ** (0x640 / 4),
    calls: u8 = 0,
    events: [8]u8 = [_]u8{0xFF} ** 8,
    last_mask: u32 = 0,
    rstcr_writes: u8 = 0,
    rstcr_last: u32 = 0xFFFF_FFFF,

    pub fn read32(f: *Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        if (off == dp.off_rstcr) {
            f.rstcr_writes += 1;
            f.rstcr_last = value;
        }
        f.regs[off / 4] = value;
    }
    pub fn err(_: *Fake, _: [*:0]const u8) void {}
    pub fn notify(f: *Fake, event: u8, mask: u32) void {
        f.events[f.calls] = event;
        f.calls += 1;
        f.last_mask = mask;
    }
    fn reg(f: *const Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
};

const Rx = struct {
    buf: ?[*]u8 = null,
    len: u16 = 0,
    fn view(r: *Rx) dp.PendingRx {
        return .{ .buffer = &r.buf, .len = &r.len };
    }
};

test "seq0 and seq1 clear only defined bits and report the snapshot" {
    var f = Fake{};
    f.regs[dp.off_sqch0sr / 4] = 0xFFFF_FFFF;
    dp.seq0(&f);
    try expectEqual(@as(u32, 0x7D09_0110), f.reg(off_sqch0scr));
    try expectEqual(@as(u8, 0), f.events[0]);
    try expectEqual(@as(u32, 0xFFFF_FFFF), f.last_mask);
    f.regs[dp.off_sqch1sr / 4] = 0x0000_0110;
    dp.seq1(&f);
    try expectEqual(@as(u32, 0x0000_0110), f.reg(off_sqch1scr));
    try expectEqual(@as(u8, 1), f.events[1]);
}

test "video without buffer faults does not reset" {
    var f = Fake{};
    f.regs[dp.off_vmsr / 4] = 0x0000_0001;
    dp.video(&f);
    try expectEqual(@as(u32, 0x0000_0001), f.reg(off_vmscr));
    try expectEqual(@as(u8, 0), f.rstcr_writes);
    try expectEqual(@as(u8, 2), f.events[0]);
}

test "video overflow and underflow pulse the soft reset" {
    for ([_]u32{ dp.vmsr_vbufovf, dp.vmsr_vbufudf }) |bit| {
        var f = Fake{};
        f.regs[dp.off_vmsr / 4] = bit;
        dp.video(&f);
        try expectEqual(@as(u8, 2), f.rstcr_writes);
        try expectEqual(@as(u32, 0), f.rstcr_last);
        try expectEqual(bit, f.last_mask);
    }
}

test "receive drains an armed buffer on a response and disarms it" {
    var f = Fake{};
    var out = [_]u8{0} ** 6;
    var rx = Rx{ .buf = &out, .len = 6 };
    f.regs[off_rxsr / 4] = dp.rxsr_rxresp;
    f.regs[off_rxppd0r / 4] = 0x4433_2211;
    f.regs[off_rxppd0r / 4 + 1] = 0x8877_6655;
    dp.receive(&f, rx.view());
    try expectEqual(@as(u8, 0x11), out[0]);
    try expectEqual(@as(u8, 0x66), out[5]);
    try expect(rx.buf == null);
    try expectEqual(@as(u16, 0), rx.len);
    try expectEqual(dp.rxsr_rxresp, f.reg(off_rxscr));
    try expectEqual(dp.rxrinfoow_sl0, f.reg(off_rxrinfoowscr));
    try expectEqual(@as(u8, 3), f.events[0]);
}

test "receive leaves the buffer armed without a response or with zero length" {
    var f = Fake{};
    var out = [_]u8{0xAA} ** 2;
    var rx = Rx{ .buf = &out, .len = 2 };
    dp.receive(&f, rx.view());
    try expect(rx.buf != null);
    var zero = Rx{ .buf = &out, .len = 0 };
    f.regs[off_rxsr / 4] = dp.rxsr_rxresp;
    dp.receive(&f, zero.view());
    try expect(zero.buf != null);
    try expectEqual(@as(u8, 0xAA), out[0]);
    try expectEqual(@as(u8, 2), f.calls);
}

test "fatal and phy clear through their own registers" {
    var f = Fake{};
    f.regs[dp.off_ferrsr / 4] = 0xFFFF_FFFF;
    f.regs[dp.off_plsr / 4] = 0xFFFF_FFFF;
    dp.fatal(&f);
    dp.phy(&f);
    try expectEqual(@as(u32, 0x001F_0007), f.reg(off_ferrscr));
    try expectEqual(@as(u32, 0x3F00_3000), f.reg(off_plscr));
    try expectEqual(@as(u8, 4), f.events[0]);
    try expectEqual(@as(u8, 5), f.events[1]);
}

test "dispatch fans out every set source in order" {
    var f = Fake{};
    var rx = Rx{};
    f.regs[off_isr / 4] = dp.isr_all;
    dp.dispatch(&f, rx.view());
    try expectEqual(@as(u8, 6), f.calls);
    for (0..6) |i| try expectEqual(@as(u8, @intCast(i)), f.events[i]);
}

test "dispatch with a single source fires only that class" {
    var f = Fake{};
    var rx = Rx{};
    f.regs[off_isr / 4] = 1 << 16;
    f.regs[dp.off_ferrsr / 4] = 0x1;
    dp.dispatch(&f, rx.view());
    try expectEqual(@as(u8, 1), f.calls);
    try expectEqual(@as(u8, 4), f.events[0]);
    try expectEqual(@as(u32, 0x1), f.last_mask);
}

test "dispatch with no source still calls back once as phy with mask 0" {
    var f = Fake{};
    var rx = Rx{};
    f.regs[off_isr / 4] = 1 << 31;
    f.regs[dp.off_plsr / 4] = 0xFFFF;
    dp.dispatch(&f, rx.view());
    try expectEqual(@as(u8, 1), f.calls);
    try expectEqual(@as(u8, 5), f.events[0]);
    try expectEqual(@as(u32, 0), f.last_mask);
    try expectEqual(@as(u32, 0), f.reg(off_plscr));
}
