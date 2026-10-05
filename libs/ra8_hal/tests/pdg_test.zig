//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/pdg.zig (RA8FW-799, port of ra8_pdg.c).

const std = @import("std");
const pdg = @import("pdg");

const expectEqual = std.testing.expectEqual;
const codes = pdg.codes;

const Op = struct { kind: u8, addr: usize = 0, val: u16 = 0 };

const Fake = struct {
    ops: [48]Op = undefined,
    n: usize = 0,
    cr2: u16 = 0,

    fn push(self: *Fake, o: Op) void {
        self.ops[self.n] = o;
        self.n += 1;
    }
    pub fn read16(self: *Fake, a: usize) u16 {
        return if (a == pdg.gtdlycr2) self.cr2 else 0;
    }
    pub fn write16(self: *Fake, a: usize, v: u16) void {
        if (a == pdg.gtdlycr2) self.cr2 = v;
        self.push(.{ .kind = 'w', .addr = a, .val = v });
    }
    pub fn waitUs(self: *Fake, us: u16) void {
        self.push(.{ .kind = 'u', .val = us });
    }
    pub fn wait5Gtclk(self: *Fake) void {
        self.push(.{ .kind = 'g' });
    }
};

fn w(a: usize, v: u16) Op {
    return .{ .kind = 'w', .addr = a, .val = v };
}

fn expectOps(f: *const Fake, want: []const Op) !void {
    try expectEqual(want.len, f.n);
    for (want, f.ops[0..f.n]) |x, y| {
        try expectEqual(x.kind, y.kind);
        try expectEqual(x.addr, y.addr);
        try expectEqual(x.val, y.val);
    }
}

test "cfg validation and frange pick" {
    try expectEqual(codes.ok, pdg.validateCfg(.{ .frange = 1, .channel_mask = 0x0F, .auto_tune = 0, .gptclk_hz = 0 }));
    try expectEqual(codes.invalid_arg, pdg.validateCfg(.{ .frange = 0, .channel_mask = 0x10, .auto_tune = 0, .gptclk_hz = 0 }));
    try expectEqual(codes.invalid_arg, pdg.validateCfg(.{ .frange = 3, .channel_mask = 1, .auto_tune = 0, .gptclk_hz = 0 }));
    try expectEqual(codes.invalid_arg, pdg.validateCfg(.{ .frange = 3, .channel_mask = 1, .auto_tune = 1, .gptclk_hz = 0 }));
    try expectEqual(codes.out_of_range, pdg.validateCfg(.{ .frange = 3, .channel_mask = 1, .auto_tune = 1, .gptclk_hz = 79_999_999 }));
    try expectEqual(codes.ok, pdg.validateCfg(.{ .frange = 3, .channel_mask = 1, .auto_tune = 1, .gptclk_hz = 200_000_000 }));
    try expectEqual(pdg.frange_low, pdg.pickFrange(160_000_000).frange);
    try expectEqual(pdg.frange_high, pdg.pickFrange(160_000_001).frange);
    try expectEqual(codes.out_of_range, pdg.pickFrange(300_000_001).rc);
    try expectEqual(codes.invalid_arg, pdg.pickFrange(0).rc);
}

test "delay cell addressing and slot checks" {
    try expectEqual(@as(usize, 0x4032_4018), pdg.cellAddr(0, pdg.pin_a, pdg.edge_rising));
    try expectEqual(@as(usize, 0x4032_401A), pdg.cellAddr(0, pdg.pin_b, pdg.edge_rising));
    try expectEqual(@as(usize, 0x4032_4028), pdg.cellAddr(0, pdg.pin_a, pdg.edge_falling));
    try expectEqual(@as(usize, 0x4032_4036), pdg.cellAddr(3, pdg.pin_b, pdg.edge_falling));
    try expectEqual(codes.ok, pdg.slotOk(3, 1, 1, 0x7F));
    try expectEqual(codes.invalid_arg, pdg.slotOk(4, 0, 0, 0));
    try expectEqual(codes.invalid_arg, pdg.slotOk(0, 2, 0, 0));
    try expectEqual(codes.invalid_arg, pdg.slotOk(0, 0, 2, 0));
    try expectEqual(codes.invalid_arg, pdg.slotOk(0, 0, 0, 0x80));
}

test "ns to code rounds half up and clamps" {
    // 1 ns at 100 MHz, low band: 1 * 128 * 1e8 / 1e9 = 12.8 -> 13.
    try expectEqual(@as(u8, 13), pdg.nsToCode(1, 100_000_000, pdg.frange_low));
    // high band uses 64: 6.4 -> 6.
    try expectEqual(@as(u8, 6), pdg.nsToCode(1, 100_000_000, pdg.frange_high));
    try expectEqual(@as(u8, 0x7F), pdg.nsToCode(100, 200_000_000, pdg.frange_high));
    try expectEqual(@as(u8, 0), pdg.nsToCode(0, 200_000_000, pdg.frange_low));
}

test "status decode" {
    const s = pdg.decodeStatus(0x0101, 0x0A05);
    try expectEqual(@as(u8, 1), s.dll_enabled);
    try expectEqual(@as(u8, 0), s.in_reset);
    try expectEqual(@as(u8, 1), s.frange);
    try expectEqual([4]u8{ 1, 0, 1, 0 }, s.per_channel_bypass_off);
    try expectEqual([4]u8{ 1, 0, 1, 0 }, s.per_channel_powered);
    try expectEqual(@as(u16, 0x0A05), s.raw_gtdlycr2);
    try expectEqual(true, pdg.isInitialized(0x0001));
    try expectEqual(false, pdg.isInitialized(0x0003));
    try expectEqual(false, pdg.isInitialized(0x0000));
}

test "Table 23.4 constraints and write interval" {
    try expectEqual(codes.ok, pdg.checkConstraints(pdg.wave_saw, pdg.dir_up, 97, 100));
    try expectEqual(codes.invalid_state, pdg.checkConstraints(pdg.wave_saw, pdg.dir_up, 98, 100));
    try expectEqual(codes.invalid_state, pdg.checkConstraints(pdg.wave_saw, pdg.dir_up, 0, 1));
    try expectEqual(codes.invalid_state, pdg.checkConstraints(pdg.wave_saw, pdg.dir_down, 2, 100));
    try expectEqual(codes.ok, pdg.checkConstraints(pdg.wave_saw, pdg.dir_down, 3, 100));
    try expectEqual(codes.ok, pdg.checkConstraints(pdg.wave_triangle, pdg.dir_up, 0, 0));
    try expectEqual(codes.invalid_state, pdg.checkConstraints(pdg.wave_triangle, pdg.dir_down, 1, 100));
    try expectEqual(codes.invalid_arg, pdg.checkConstraints(2, pdg.dir_up, 5, 100));
    try expectEqual(codes.invalid_arg, pdg.checkConstraints(pdg.wave_saw, 2, 5, 100));
    // 100 MHz PCLKA (10 ns x 6) + 200 MHz GPTCLK (5 ns x 4).
    try expectEqual(@as(u32, 80), pdg.requiredWriteNs(100_000_000, 200_000_000));
}

test "DLL bring-up follows Figure 23.2" {
    var f = Fake{};
    pdg.programDll(&f, 0x05, pdg.frange_high);
    try expectOps(&f, &.{
        w(pdg.gtdlycr, 0x0102),  w(pdg.gtdlycr2, 0),
        w(pdg.gtdlycr, 0x0103),  .{ .kind = 'u', .val = 20 },
        w(pdg.gtdlycr, 0x0101),  .{ .kind = 'g' },
        w(pdg.gtdlycr2, 0x0005),
    });
}

test "FRANGE switch restores GTDLYCR2" {
    var f = Fake{ .cr2 = 0x0203 };
    pdg.switchFrange(&f, pdg.frange_low);
    try expectOps(&f, &.{
        w(pdg.gtdlycr, 0x0002),      w(pdg.gtdlycr2, 0),
        w(pdg.gtdlycr, 0x0002),      w(pdg.gtdlycr, 0x0003),
        .{ .kind = 'u', .val = 20 }, w(pdg.gtdlycr, 0x0001),
        .{ .kind = 'g' },            w(pdg.gtdlycr2, 0x0203),
    });
}

test "park wipes every cell and bit helpers RMW" {
    var f = Fake{};
    pdg.park(&f);
    try expectEqual(@as(usize, 18), f.n);
    try expectEqual(pdg.cellAddr(0, pdg.pin_a, pdg.edge_rising), f.ops[2].addr);
    try expectEqual(pdg.cellAddr(3, pdg.pin_b, pdg.edge_falling), f.ops[17].addr);
    var g = Fake{ .cr2 = 0x0001 };
    pdg.setCr2Bit(&g, pdg.dlyenBit(2), true);
    try expectEqual(@as(u16, 0x0401), g.cr2);
    pdg.setCr2Bit(&g, pdg.dlybsBit(0), false);
    try expectEqual(@as(u16, 0x0400), g.cr2);
}
