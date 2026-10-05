//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/lvd.zig (RA8FW-797, port of ra8_lvd.c).

const std = @import("std");
const lvd = @import("lvd");

const Fake = struct {
    regs: std.AutoHashMap(usize, u8),
    log: [32]struct { a: usize, v: u8 } = undefined,
    n: usize = 0,

    fn init() Fake {
        return .{ .regs = std.AutoHashMap(usize, u8).init(std.testing.allocator) };
    }
    fn deinit(self: *Fake) void {
        self.regs.deinit();
    }
    pub fn read8(self: *Fake, a: usize) u8 {
        return self.regs.get(a) orelse 0;
    }
    pub fn write8(self: *Fake, a: usize, v: u8) void {
        self.regs.put(a, v) catch unreachable;
        self.log[self.n] = .{ .a = a, .v = v };
        self.n += 1;
    }
};

fn cfg() lvd.Cfg {
    return .{ .threshold = 0x07, .edge = 1, .irq_type = 1, .response = lvd.response.reset, .negate = 0, .hysteresis = 0, .filter_div = 2, .filter_en = true, .irq_enable = true, .clear_status = true };
}

test "channel map and indices" {
    try std.testing.expectEqual(@as(?u8, 0), lvd.channelIdx(1));
    try std.testing.expectEqual(@as(?u8, 3), lvd.channelIdx(5));
    try std.testing.expectEqual(@as(?u8, null), lvd.channelIdx(3));
    try std.testing.expectEqual(@as(?u8, null), lvd.channelIdx(0));
    try std.testing.expectEqual(@as(usize, 0x4001_EA7C), lvd.map[2].cr1);
    try std.testing.expect(!lvd.map[3].has_irq);
}

test "validate rejects bad threshold, div, edge, n-channel irq and hvd+rn" {
    const m = lvd.map[0];
    var c = cfg();
    try std.testing.expectEqual(lvd.codes.ok, lvd.validate(m, c));
    c.threshold = 2;
    try std.testing.expectEqual(lvd.codes.invalid_arg, lvd.validate(m, c));
    c = cfg();
    c.filter_div = 4;
    try std.testing.expectEqual(lvd.codes.invalid_arg, lvd.validate(m, c));
    c = cfg();
    c.edge = 3;
    try std.testing.expectEqual(lvd.codes.invalid_arg, lvd.validate(m, c));
    try std.testing.expectEqual(lvd.codes.ok, lvd.validate(lvd.map[2], c));
    c = cfg();
    c.response = lvd.response.nmi;
    try std.testing.expectEqual(lvd.codes.not_supported, lvd.validate(lvd.map[2], c));
    c = cfg();
    c.hysteresis = 1;
    c.negate = 1;
    try std.testing.expectEqual(lvd.codes.invalid_arg, lvd.validate(m, c));
}

test "composeCr0 sets fsamp, dfdis, rn and ri only on m channels" {
    var c = cfg();
    c.negate = 1;
    try std.testing.expectEqual(@as(u8, 0x20 | 0x02 | 0x80 | 0x40), lvd.composeCr0(lvd.map[0], c));
    try std.testing.expectEqual(@as(u8, 0x22), lvd.composeCr0(lvd.map[2], c));
    c.response = lvd.response.nmi;
    try std.testing.expectEqual(@as(u8, 0xA2), lvd.composeCr0(lvd.map[1], c));
}

test "reserved bits: bit3 on m, bit6 on n" {
    try std.testing.expectEqual(@as(u8, 0x08), lvd.withReserved(lvd.map[0], 0));
    try std.testing.expectEqual(@as(u8, 0x40), lvd.withReserved(lvd.map[3], 0));
}

test "init sequence on PVD1 matches the C order" {
    var f = Fake.init();
    defer f.deinit();
    const m = lvd.map[0];
    lvd.programCmpcr(&f, m, cfg());
    lvd.programCr0Chain(&f, m, cfg());
    const want = [_]struct { a: usize, v: u8 }{
        .{ .a = m.cr0, .v = 0x08 }, .{ .a = m.cmpcr, .v = 0 },    .{ .a = m.cmpcr, .v = 0x07 },
        .{ .a = m.fcr, .v = 0 },    .{ .a = m.cmpcr, .v = 0x87 }, .{ .a = m.cr0, .v = 0x6A },
        .{ .a = m.cr0, .v = 0x68 }, .{ .a = m.cr1, .v = 0x05 },   .{ .a = m.sr, .v = 0 },
        .{ .a = m.cr0, .v = 0x69 }, .{ .a = m.cr0, .v = 0x6D },
    };
    try std.testing.expectEqual(want.len, f.n);
    for (want, 0..) |w, i| {
        try std.testing.expectEqual(w.a, f.log[i].a);
        try std.testing.expectEqual(w.v, f.log[i].v);
    }
}

test "n channel init skips cr1/sr and rie without a response" {
    var f = Fake.init();
    defer f.deinit();
    const m = lvd.map[3];
    var c = cfg();
    c.response = lvd.response.none;
    c.filter_en = false;
    lvd.programCr0Chain(&f, m, c);
    try std.testing.expectEqual(@as(usize, 2), f.n);
    try std.testing.expectEqual(@as(u8, 0x62), f.log[0].v);
    try std.testing.expectEqual(@as(u8, 0x66), f.log[1].v);
}

test "deinit drops cmpe, rie, sets dfdis, then clears" {
    var f = Fake.init();
    defer f.deinit();
    const m = lvd.map[1];
    try f.regs.put(m.cr0, 0x6D);
    lvd.deinit(&f, m);
    try std.testing.expectEqual(@as(u8, 0x69), f.log[0].v);
    try std.testing.expectEqual(@as(u8, 0x68), f.log[1].v);
    try std.testing.expectEqual(@as(u8, 0x6A), f.log[2].v);
    try std.testing.expectEqual(m.cmpcr, f.log[3].a);
    try std.testing.expectEqual(@as(u8, 0x08), f.log[4].v);
    try std.testing.expectEqual(@as(usize, 8), f.n);
}

test "threshold keeps pvde, edge and kind keep other cr1 bits" {
    var f = Fake.init();
    defer f.deinit();
    const m = lvd.map[0];
    try f.regs.put(m.cmpcr, 0x87);
    lvd.setThreshold(&f, m, 0x0F);
    try std.testing.expectEqual(@as(u8, 0), f.log[0].v);
    try std.testing.expectEqual(@as(u8, 0x8F), f.read8(m.cmpcr));
    try f.regs.put(m.cr1, 0x05);
    lvd.setEdge(&f, m, 2);
    try std.testing.expectEqual(@as(u8, 0x06), f.read8(m.cr1));
    lvd.setKind(&f, m, 0);
    try std.testing.expectEqual(@as(u8, 0x02), f.read8(m.cr1));
    lvd.setKind(&f, m, 1);
    try std.testing.expectEqual(@as(u8, 0x06), f.read8(m.cr1));
}
