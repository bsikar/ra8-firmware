//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/pdm.zig.

const std = @import("std");
const pdm = @import("pdm");

/// 1 KiB register block; PDDRR reads pop from `fifo`, PDCSR reads count down.
const Regs = struct {
    mem: [256]u32 = @splat(0),
    fifo: [40]u32 = @splat(0),
    head: usize = 0,
    busy_reads: u32 = 0,
    pddrr_reads: u32 = 0,

    pub fn read32(self: *Regs, off: usize) u32 {
        if (off >= 0x100 and (off - 0x100) % 0x100 == pdm.pddrr) {
            self.pddrr_reads += 1;
            const v = self.fifo[self.head % self.fifo.len];
            self.head += 1;
            return v;
        }
        if (off == pdm.pdcsr and self.busy_reads > 0) {
            self.busy_reads -= 1;
            return 0x7;
        }
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
    }
    fn at(self: *const Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
};

const Svc = struct {
    mstp_err: u16 = 0,
    isr_err: u16 = 0,
    last_err: [*:0]const u8 = "",
    fails: u32 = 0,
    registered: ?u16 = null,
    reg_ch: u8 = 0xFF,
    unregistered: ?u16 = null,

    pub fn mstpEnable(self: *Svc, id: u16) u16 {
        std.debug.assert(id == pdm.mstp_pdmif);
        return self.mstp_err;
    }
    pub fn mstpDisable(_: *Svc, _: u16) u16 {
        return 0;
    }
    pub fn isrRegister(self: *Svc, event: u16, ch: u8, _: u8) u16 {
        self.registered = event;
        self.reg_ch = ch;
        return self.isr_err;
    }
    pub fn isrUnregister(self: *Svc, event: u16) u16 {
        self.unregistered = event;
        return 0;
    }
    pub fn info(_: *Svc, _: [*:0]const u8) void {}
    pub fn err(self: *Svc, msg: [*:0]const u8) void {
        self.last_err = msg;
    }
    pub fn fail(self: *Svc, msg: [*:0]const u8, _: u16) void {
        self.last_err = msg;
        self.fails += 1;
    }
};

fn cfg() pdm.Config {
    var k = std.mem.zeroes(pdm.Config);
    k.sinc_order = 0xF;
    k.edge = 3;
    k.hpf_shift = 1;
    k.cf_shift = 2;
    k.lpf_shift = 3;
    k.data_shift = 0x1F;
    k.clock_div = 0x12;
    k.sinc_dec = 0x34;
    k.sinc_range = 0x56;
    k.rx_threshold = 9;
    k.hpf_s0 = 0x100;
    k.hpf_h = .{ 1, 2 };
    k.comp_h[10] = 0xBEEF;
    k.lpf_h0 = 0x77;
    k.lpf_h1[19] = 0xCAFE;
    return k;
}

var got_count: u32 = 0;
var got_first: i32 = 0;
fn sink(_: ?*anyopaque, s: [*]const i32, n: u32) callconv(.c) void {
    got_count = n;
    got_first = s[0];
}

test "Config matches ra8_pdm_channel_cfg_t" {
    try std.testing.expectEqual(@as(usize, 82), @sizeOf(pdm.Config));
    try std.testing.expectEqual(@as(usize, 18), @offsetOf(pdm.Config, "comp_h"));
    try std.testing.expectEqual(@as(usize, 42), @offsetOf(pdm.Config, "lpf_h1"));
}

test "signExtend20 keeps positives and extends negatives" {
    try std.testing.expectEqual(@as(i32, 0x7FFFF), pdm.signExtend20(0xFFF7_FFFF));
    try std.testing.expectEqual(@as(i32, -1), pdm.signExtend20(0x000F_FFFF));
    try std.testing.expectEqual(@as(i32, -0x80000), pdm.signExtend20(0x0008_0000));
}

test "init enables the module and clears the common controls" {
    var r = Regs{};
    var c = Svc{};
    r.mem[pdm.pdcicr / 4] = 5;
    try std.testing.expectEqual(pdm.ok, pdm.init(&r, &c));
    try std.testing.expectEqual(@as(u32, 0), r.at(pdm.pdcicr));
    c.mstp_err = 0x201;
    try std.testing.expectEqual(@as(u16, 0x201), pdm.init(&r, &c));
    try std.testing.expectEqual(@as(u32, 1), c.fails);
}

test "configure packs mode, filter and coefficients, null first then range" {
    var r = Regs{};
    var c = Svc{};
    const k = cfg();
    try std.testing.expectEqual(pdm.err_null_ptr, pdm.configure(&r, &c, 9, null));
    try std.testing.expectEqual(pdm.err_invalid_arg, pdm.configure(&r, &c, 3, &k));
    try std.testing.expectEqual(pdm.ok, pdm.configure(&r, &c, 2, &k));
    try std.testing.expectEqual(@as(u32, 0xF003_2171), r.at(pdm.chOff(2, pdm.pdmdsr)));
    try std.testing.expectEqual(@as(u32, 0x5634_0012), r.at(pdm.chOff(2, pdm.pdsfcr)));
    try std.testing.expectEqual(@as(u32, 2), r.at(pdm.chOff(2, pdm.pdhfchr + 4)));
    try std.testing.expectEqual(@as(u32, 0xBEEF), r.at(pdm.chOff(2, pdm.pdcfchr + 40)));
    try std.testing.expectEqual(@as(u32, 0x77), r.at(pdm.chOff(2, pdm.pdlfch010r)));
    try std.testing.expectEqual(@as(u32, 0xCAFE), r.at(pdm.chOff(2, pdm.pdlfch1r + 76)));
    try std.testing.expectEqual(@as(u32, 9), r.at(pdm.chOff(2, pdm.pddbcr)));
}

test "start sets the channel bit and readEnable primes 30 reads" {
    var r = Regs{};
    var c = Svc{};
    try std.testing.expectEqual(pdm.ok, pdm.start(&r, &c, 1));
    try std.testing.expectEqual(@as(u32, 2), r.at(pdm.pdcstrtr));
    try std.testing.expectEqual(pdm.ok, pdm.readEnable(&r, &c, 1));
    try std.testing.expectEqual(@as(u32, 0x0807_0002), r.at(pdm.chOff(1, pdm.pdscr)));
    try std.testing.expectEqual(@as(u32, 1), r.at(pdm.chOff(1, pdm.pddrcr)));
    try std.testing.expectEqual(@as(u32, 30), r.pddrr_reads);
}

test "read checks its arguments and drains min(fill, max)" {
    var r = Regs{};
    var c = Svc{};
    var out: [4]i32 = undefined;
    var n: u32 = 0;
    try std.testing.expectEqual(pdm.err_null_ptr, pdm.read(&r, &c, 0, null, 4, &n));
    try std.testing.expectEqual(pdm.err_null_ptr, pdm.read(&r, &c, 0, &out, 4, null));
    try std.testing.expectEqual(pdm.err_invalid_arg, pdm.read(&r, &c, 0, &out, 0, &n));
    r.mem[pdm.chOff(0, pdm.pddsr) / 4] = 0x106;
    r.fifo[0] = 0xFFFFF;
    try std.testing.expectEqual(pdm.ok, pdm.read(&r, &c, 0, &out, 4, &n));
    try std.testing.expectEqual(@as(u32, 4), n);
    try std.testing.expectEqual(@as(i32, -1), out[0]);
}

test "stream enable registers once, sets IDRE and rolls back on failure" {
    var r = Regs{};
    var c = Svc{};
    var s: [3]pdm.Stream = @splat(.{});
    try std.testing.expectEqual(pdm.err_null_ptr, pdm.streamEnable(&r, &c, &s, 0, null, null, 1));
    try std.testing.expectEqual(pdm.ok, pdm.streamEnable(&r, &c, &s, 1, sink, null, 1));
    try std.testing.expectEqual(@as(?u16, 0x0C0), c.registered);
    try std.testing.expectEqual(@as(u8, 1), c.reg_ch);
    try std.testing.expectEqual(@as(u32, 4), r.at(pdm.chOff(1, pdm.pdicr)));
    try std.testing.expectEqual(pdm.err_exists, pdm.streamEnable(&r, &c, &s, 1, sink, null, 1));
    c.isr_err = 0x109;
    try std.testing.expectEqual(@as(u16, 0x109), pdm.streamEnable(&r, &c, &s, 2, sink, null, 1));
    try std.testing.expect(s[2].callback == null);
}

test "stream disable clears IDRE and refuses an idle channel" {
    var r = Regs{};
    var c = Svc{};
    var s: [3]pdm.Stream = @splat(.{});
    try std.testing.expectEqual(pdm.err_not_initialized, pdm.streamDisable(&r, &c, &s, 0));
    _ = pdm.streamEnable(&r, &c, &s, 0, sink, null, 1);
    try std.testing.expectEqual(pdm.ok, pdm.streamDisable(&r, &c, &s, 0));
    try std.testing.expectEqual(@as(u32, 0), r.at(pdm.chOff(0, pdm.pdicr)));
    try std.testing.expectEqual(@as(?u16, 0x0BF), c.unregistered);
    try std.testing.expect(s[0].callback == null);
}

test "stop tears down a stream, then polls PDCSR and times out" {
    var r = Regs{};
    var c = Svc{};
    var s: [3]pdm.Stream = @splat(.{});
    _ = pdm.streamEnable(&r, &c, &s, 2, sink, null, 1);
    r.busy_reads = 5;
    try std.testing.expectEqual(pdm.ok, pdm.stop(&r, &c, &s, 2));
    try std.testing.expect(s[2].callback == null);
    try std.testing.expectEqual(@as(u32, 4), r.at(pdm.pdcstptr));
    r.busy_reads = 2000;
    try std.testing.expectEqual(pdm.err_hw_timeout, pdm.stop(&r, &c, &s, 2));
}

test "dataIsr clamps to the FIFO depth and skips empty or unbound channels" {
    var r = Regs{};
    var s: [3]pdm.Stream = @splat(.{});
    got_count = 0;
    r.mem[pdm.chOff(1, pdm.pddsr) / 4] = 0xFF;
    pdm.dataIsr(&r, &s, 1);
    try std.testing.expectEqual(@as(u32, 0), got_count);
    s[1] = .{ .callback = sink };
    r.fifo[0] = 5;
    pdm.dataIsr(&r, &s, 1);
    try std.testing.expectEqual(@as(u32, 32), got_count);
    try std.testing.expectEqual(@as(i32, 5), got_first);
    pdm.dataIsr(&r, &s, 3);
}
