//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SRAM0..3 ECC control logic (RA8FW-798), ported from ra8_sram.c. Offsets
//! are from ra8_sram_regs.h (HUM Ch 58). The attribution setters and ECC
//! error handlers live in sram_security.zig.

const sec = @import("sram_security.zig");

pub const bank_count = 4;
pub const Status = sec.Status;

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
};

pub const reg = struct {
    pub const base: usize = 0x4000_2000;
    pub const prcr_s: usize = base + 0x00;
    pub const wtsc: usize = base + 0x08;
    pub const esr: usize = base + 0x40;
    pub const esclr: usize = base + 0x48;
    pub const cpscu: usize = 0x4000_8000;
    pub const sar: usize = cpscu + 0x010;
    pub const sabar: usize = cpscu + 0x400;
    pub const esar: usize = cpscu + 0x510;
    pub const sys_prcr: usize = 0x4001_E3FA;
    pub const data_base: usize = 0x2200_0000;

    pub fn cr(bank: u8) usize {
        return base + 0x10 + @as(usize, bank) * 4;
    }
    pub fn eccrgn(bank: u8) usize {
        return base + 0x30 + @as(usize, bank) * 4;
    }
    pub fn ear(bank: u8, slot: u8) usize {
        return base + 0x50 + @as(usize, bank) * 8 + @as(usize, slot) * 4;
    }
};

pub const prcr_unlock: u16 = 0xA501;
pub const prcr_lock: u16 = 0xA500;
pub const sys_unlock_sar: u16 = 0xA510;
pub const sys_lock_all: u16 = 0xA500;

pub const cr = struct {
    pub const oad: u8 = 0x01;
    pub const e1stsen: u8 = 0x10;
    pub const eccmod_disabled: u8 = 0x00;
    pub const eccmod_no_check: u8 = 0x08;
    pub const eccmod_with_chk: u8 = 0x0C;
    pub const test_write: u8 = 0x08;
    pub const test_bypass: u8 = 0x80;
    pub const test_verify: u8 = 0x1C;
};

pub const err_all: u16 = 0x00FF;

pub const BankCfg = extern struct {
    ecc_mode: u8,
    on_error: u8,
    enable_1bit_latch: bool,
    eccrgn: u8,
    zero_init: bool,
};

pub const SecurityCfg = extern struct {
    bank_ns: [bank_count]bool,
    wtsc_ns: bool,
    ecc_region_ns: bool,
    boundary_offset: [bank_count]u32,
};

pub const Config = extern struct {
    banks: [bank_count]BankCfg,
    security: SecurityCfg,
    apply_security: bool,
};

pub const BankInfo = extern struct {
    bank: u8,
    data_base: usize,
    data_size: u32,
    ecc_base: usize,
    ecc_size: u32,
};

comptime {
    if (@sizeOf(BankCfg) != 5) @compileError("BankCfg");
    if (@offsetOf(SecurityCfg, "boundary_offset") != 8 or @sizeOf(SecurityCfg) != 24) @compileError("SecurityCfg");
    if (@offsetOf(Config, "security") != 20 or @offsetOf(Config, "apply_security") != 44 or @sizeOf(Config) != 48)
        @compileError("Config");
}

pub fn bankOk(bank: u8) bool {
    return bank < bank_count;
}

pub fn maxRgn(bank: u8) u8 {
    return if (bank == 3) 1 else 4;
}

pub fn bankSize(bank: u8) u32 {
    return if (bank == 3) 0x0002_0000 else 0x0008_0000;
}

pub fn eccSize(bank: u8) u32 {
    return if (bank == 3) 0x0000_4000 else 0x0001_0000;
}

pub fn bankInfo(bank: u8) BankInfo {
    const ecc_off = [bank_count]u32{ 0x001A_0000, 0x001B_0000, 0x001C_0000, 0x001D_0000 };
    return .{
        .bank = bank,
        .data_base = reg.data_base + @as(usize, bank) * 0x0008_0000,
        .data_size = bankSize(bank),
        .ecc_base = reg.data_base + ecc_off[bank],
        .ecc_size = eccSize(bank),
    };
}

pub fn bankCfgOk(c: BankCfg, bank: u8) bool {
    return c.ecc_mode <= 2 and c.on_error <= 1 and c.eccrgn <= maxRgn(bank);
}

pub fn encodeCr(c: BankCfg) u8 {
    var v: u8 = switch (c.ecc_mode) {
        1 => cr.eccmod_no_check,
        2 => cr.eccmod_with_chk,
        else => cr.eccmod_disabled,
    };
    if (c.on_error == 1) v |= cr.oad;
    if (c.enable_1bit_latch) v |= cr.e1stsen;
    return v;
}

pub const Masks = struct { one: u8, two: u8 };

pub fn decodeEsr(raw: u16) Masks {
    var m = Masks{ .one = 0, .two = 0 };
    for (0..bank_count) |i| {
        const b: u3 = @intCast(i);
        const pos: u4 = @intCast(i * 2);
        if (raw & (@as(u16, 1) << pos) != 0) m.one |= @as(u8, 1) << b;
        if (raw & (@as(u16, 1) << (pos + 1)) != 0) m.two |= @as(u8, 1) << b;
    }
    return m;
}

pub fn earToAbs(ear: u32) usize {
    return if (ear == 0) 0 else reg.data_base + ear;
}

/// ESR/ESCLR bit for bank n, slot 0 (1-bit) or 1 (2-bit).
pub fn errBit(bank: u8, slot: u8) u16 {
    return @as(u16, 1) << @intCast(@as(u8, 2) * bank + slot);
}

pub fn wtscFor(iclk_hz: u32, iclk_max_hz: u32) u8 {
    return if (iclk_hz > iclk_max_hz >> 1) 0x01 else 0x00;
}

pub fn sarOf(s: SecurityCfg) u32 {
    var v: u32 = 0;
    for (s.bank_ns, 0..) |ns, i| {
        if (ns) v |= @as(u32, 1) << @intCast(i);
    }
    if (s.wtsc_ns) v |= 0x100;
    return v;
}

pub fn writeCr(hw: anytype, bank: u8, v: u8) void {
    hw.write16(reg.prcr_s, prcr_unlock);
    hw.write8(reg.cr(bank), v);
    hw.write16(reg.prcr_s, prcr_lock);
}

pub fn writeEccrgn(hw: anytype, bank: u8, v: u8) void {
    hw.write16(reg.prcr_s, prcr_unlock);
    hw.write8(reg.eccrgn(bank), v & 0x07);
    hw.write16(reg.prcr_s, prcr_lock);
}

pub fn writeWtsc(hw: anytype, v: u8) void {
    hw.write16(reg.prcr_s, prcr_unlock);
    hw.write8(reg.wtsc, v & 0x01);
    hw.write16(reg.prcr_s, prcr_lock);
}

pub fn applySecurity(hw: anytype, s: SecurityCfg) void {
    hw.write16(reg.sys_prcr, sys_unlock_sar);
    hw.write32(reg.sar, sarOf(s));
    hw.write32(reg.esar, if (s.ecc_region_ns) 1 else 0);
    for (s.boundary_offset, 0..) |off, i| {
        hw.write32(reg.sabar + i * 4, off & ~@as(u32, 0x1FFF));
    }
    hw.write16(reg.sys_prcr, sys_lock_all);
}

/// ECC encode on, 64-bit zero fill of the data window, ECC off.
pub fn zeroInit(hw: anytype, bank: u8) void {
    writeCr(hw, bank, cr.eccmod_no_check);
    const start = bankInfo(bank).data_base;
    const words = bankSize(bank) >> 3;
    var i: usize = 0;
    while (i < words) : (i += 1) hw.write64(start + i * 8, 0);
    writeCr(hw, bank, cr.eccmod_disabled);
}

pub fn applyBanks(hw: anytype, c: Config) void {
    for (c.banks, 0..) |b, i| {
        if (b.zero_init) zeroInit(hw, @intCast(i));
    }
    for (c.banks, 0..) |b, i| {
        writeEccrgn(hw, @intCast(i), b.eccrgn);
        writeCr(hw, @intCast(i), encodeCr(b));
    }
}

pub fn deinitRegs(hw: anytype) void {
    for (0..bank_count) |i| {
        writeCr(hw, @intCast(i), 0);
        writeEccrgn(hw, @intCast(i), 0);
    }
}

pub fn readStatus(hw: anytype) Status {
    const raw = hw.read16(reg.esr);
    const m = decodeEsr(raw);
    var s = Status{ .raw_esr = raw, .one_bit_mask = m.one, .two_bit_mask = m.two };
    for (0..bank_count) |i| {
        const b: u8 = @intCast(i);
        s.addr_1bit[i] = earToAbs(hw.read32(reg.ear(b, 0)));
        s.addr_2bit[i] = earToAbs(hw.read32(reg.ear(b, 1)));
    }
    return s;
}

/// HUM 58.3.4: seed under encode-only, bypass-read and flip, arm verify,
/// read the line, then check SRAMESR.
pub fn selfTest(hw: anytype, bank: u8, offset: u32, two_bit: bool) bool {
    const addr = bankInfo(bank).data_base + offset;
    writeCr(hw, bank, cr.test_write);
    hw.write64(addr, 0);
    writeCr(hw, bank, cr.test_bypass);
    const syndrome = hw.read64(addr);
    hw.write64(addr, syndrome ^ @as(u64, if (two_bit) 0x03 else 0x01));
    writeCr(hw, bank, cr.test_verify);
    _ = hw.read64(addr);
    return hw.read16(reg.esr) & errBit(bank, @intFromBool(two_bit)) != 0;
}
