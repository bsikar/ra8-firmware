//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/sram.zig (RA8FW-798, port of ra8_sram.c).

const std = @import("std");
const sram = @import("sram");

const Write = struct { a: usize, v: u64 };

const Fake = struct {
    log: [64]Write = undefined,
    n: usize = 0,
    fills: usize = 0,
    esr: u16 = 0,
    mem: u64 = 0x5A,
    ear: u32 = 0,

    pub fn read16(self: *Fake, _: usize) u16 {
        return self.esr;
    }
    pub fn read32(self: *Fake, _: usize) u32 {
        return self.ear;
    }
    pub fn read64(self: *Fake, _: usize) u64 {
        return self.mem;
    }
    fn rec(self: *Fake, a: usize, v: u64) void {
        if (self.n < self.log.len) self.log[self.n] = .{ .a = a, .v = v };
        self.n += 1;
    }
    pub fn write8(self: *Fake, a: usize, v: u8) void {
        self.rec(a, v);
    }
    pub fn write16(self: *Fake, a: usize, v: u16) void {
        self.rec(a, v);
    }
    pub fn write32(self: *Fake, a: usize, v: u32) void {
        self.rec(a, v);
    }
    pub fn write64(self: *Fake, a: usize, v: u64) void {
        if (v == 0 and a != sram.bankInfo(0).data_base + 0x40) {
            self.fills += 1;
            return;
        }
        self.mem = v;
        self.rec(a, v);
    }
};

test "bank geometry and info" {
    const info = sram.bankInfo(3);
    try std.testing.expectEqual(@as(usize, 0x2218_0000), info.data_base);
    try std.testing.expectEqual(@as(u32, 0x2_0000), info.data_size);
    try std.testing.expectEqual(@as(usize, 0x221D_0000), info.ecc_base);
    try std.testing.expectEqual(@as(u32, 0x4000), info.ecc_size);
    try std.testing.expectEqual(@as(usize, 0x4000_2014), sram.reg.cr(1));
    try std.testing.expectEqual(@as(usize, 0x4000_203C), sram.reg.eccrgn(3));
    try std.testing.expectEqual(@as(usize, 0x4000_206C), sram.reg.ear(3, 1));
}

test "bank cfg validation and CR encoding" {
    var c = sram.BankCfg{ .ecc_mode = 2, .on_error = 1, .enable_1bit_latch = true, .eccrgn = 4, .zero_init = false };
    try std.testing.expect(sram.bankCfgOk(c, 0));
    try std.testing.expect(!sram.bankCfgOk(c, 3));
    try std.testing.expectEqual(@as(u8, 0x1D), sram.encodeCr(c));
    c.ecc_mode = 1;
    c.on_error = 0;
    c.enable_1bit_latch = false;
    try std.testing.expectEqual(@as(u8, 0x08), sram.encodeCr(c));
    c.ecc_mode = 3;
    try std.testing.expect(!sram.bankCfgOk(c, 0));
    c.ecc_mode = 0;
    c.on_error = 2;
    try std.testing.expect(!sram.bankCfgOk(c, 0));
}

test "ESR decode, error bits and EAR mapping" {
    const m = sram.decodeEsr(0x00A5);
    try std.testing.expectEqual(@as(u8, 0b0011), m.one);
    try std.testing.expectEqual(@as(u8, 0b1100), m.two);
    try std.testing.expectEqual(@as(u16, 0x0080), sram.errBit(3, 1));
    try std.testing.expectEqual(@as(u16, 0x0004), sram.errBit(1, 0));
    try std.testing.expectEqual(@as(usize, 0), sram.earToAbs(0));
    try std.testing.expectEqual(@as(usize, 0x2200_1000), sram.earToAbs(0x1000));
}

test "wait state threshold is half of max" {
    try std.testing.expectEqual(@as(u8, 0), sram.wtscFor(125_000_000, 250_000_000));
    try std.testing.expectEqual(@as(u8, 1), sram.wtscFor(125_000_001, 250_000_000));
}

test "locked CR write and security scope order" {
    var f = Fake{};
    sram.writeCr(&f, 2, 0x0C);
    try std.testing.expectEqual(Write{ .a = 0x4000_2000, .v = 0xA501 }, f.log[0]);
    try std.testing.expectEqual(Write{ .a = 0x4000_2018, .v = 0x0C }, f.log[1]);
    try std.testing.expectEqual(Write{ .a = 0x4000_2000, .v = 0xA500 }, f.log[2]);
    f.n = 0;
    const s = sram.SecurityCfg{ .bank_ns = .{ true, false, true, false }, .wtsc_ns = true, .ecc_region_ns = true, .boundary_offset = .{ 0x2345, 0, 0, 0x1A_0000 } };
    sram.applySecurity(&f, s);
    try std.testing.expectEqual(@as(usize, 8), f.n);
    try std.testing.expectEqual(Write{ .a = 0x4001_E3FA, .v = 0xA510 }, f.log[0]);
    try std.testing.expectEqual(Write{ .a = 0x4000_8010, .v = 0x105 }, f.log[1]);
    try std.testing.expectEqual(Write{ .a = 0x4000_8510, .v = 1 }, f.log[2]);
    try std.testing.expectEqual(Write{ .a = 0x4000_8400, .v = 0x2000 }, f.log[3]);
    try std.testing.expectEqual(Write{ .a = 0x4001_E3FA, .v = 0xA500 }, f.log[7]);
}

test "zero init fills the bank in 64-bit words between ECC on and off" {
    var f = Fake{};
    sram.zeroInit(&f, 3);
    try std.testing.expectEqual(@as(usize, 0x2_0000 / 8), f.fills);
    try std.testing.expectEqual(@as(u64, 0x08), f.log[1].v);
    try std.testing.expectEqual(@as(u64, 0x00), f.log[4].v);
}

test "self test flips the syndrome and checks the slot bit" {
    var f = Fake{ .esr = 0x0002 };
    try std.testing.expect(sram.selfTest(&f, 0, 0x40, true));
    try std.testing.expectEqual(@as(u64, 0x08), f.log[1].v);
    try std.testing.expectEqual(@as(u64, 0x80), f.log[5].v);
    try std.testing.expectEqual(@as(u64, 0x03), f.log[7].v);
    try std.testing.expectEqual(@as(u64, 0x1C), f.log[9].v);
    var g = Fake{ .esr = 0x0002 };
    try std.testing.expect(!sram.selfTest(&g, 0, 0x40, false));
}

test "status reads ESR and maps both EAR slots" {
    var f = Fake{ .esr = 0x0041, .ear = 0x80 };
    const s = sram.readStatus(&f);
    try std.testing.expectEqual(@as(u8, 0b1001), s.one_bit_mask);
    try std.testing.expectEqual(@as(usize, 0x2200_0080), s.addr_2bit[3]);
}
