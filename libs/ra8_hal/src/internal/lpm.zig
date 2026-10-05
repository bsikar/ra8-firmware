//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Low-power mode logic (RA8FW-792), ported from ra8_lpm.c. Addresses are
//! absolute so one Hw reaches SYSC, the ICU WUPEN pair and SCB SCR. Offsets
//! and bits follow ra8_lpm_regs.h (HUM Ch 9.2, 11.2 and 14.2).

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const hw_timeout: u16 = 0x203;
    pub const null_ptr: u16 = 0x504;
};

pub const sysc: usize = 0x4001_E000;
pub const icu: usize = 0x4000_C000;
pub const scr: usize = 0xE000_ED10;

pub const off = struct {
    pub const sbycr: usize = 0x00C;
    pub const opccr: usize = 0x0A0;
    pub const pdramscr0: usize = 0x140;
    pub const pdramscr1: usize = 0x142;
    pub const prcr: usize = 0x3FA;
    pub const dpsbycr: usize = 0xA00;
    pub const lpscr: usize = 0xA90;
    pub const sscr1: usize = 0xA98;
    pub const pll1ldocr: usize = 0xB04;
    pub const pll2ldocr: usize = 0xB08;
    pub const hocoldocr: usize = 0xB0C;
    pub const dpsier = [4]usize{ 0xA08, 0xA0C, 0xA10, 0xA14 };
    pub const dpsifr = [4]usize{ 0xA18, 0xA1C, 0xA20, 0xA24 };
    pub const dpsiegr = [3]usize{ 0xA28, 0xA2C, 0xA30 };
    /// MOCOCR, HOCOCR, LOCOCR, MOSCCR, SOSCCR in ra8_lpm_clock_t order.
    pub const clock = [5]usize{ 0x038, 0x036, 0x400, 0x032, 0xC00 };
    pub const wupen0: usize = 0x1A0;
    pub const wupen1: usize = 0x1A4;
};

pub const dpsi_count: u8 = 4;
pub const clock_count: u8 = 5;
pub const prcr_key: u16 = 0xA500;
pub const prcr_prc0: u16 = 0x0001;
pub const prcr_prc1: u16 = 0x0002;
pub const sbycr_reset: u8 = 0x40;
pub const dpsbycr_reset: u8 = 0x14;
pub const sleepdeep: u32 = 0x4;
pub const deep_standby_1: u8 = 0x8;

pub const Config = extern struct {
    io_port_keep: bool,
    opa_bus_keep: bool,
    sscr_fast_return: bool,
    dcdc_softstart: u8,
    sscr_low_power: u8,
};

pub const LdoCfg = extern struct { pll1: u8, pll2: u8, hoco: u8 };

pub const RamRetention = extern struct {
    pdramscr0_bits: u16,
    cpu0_tcm_keep: bool,
    cpu1_tcm_keep: bool,
};

comptime {
    if (@sizeOf(Config) != 5 or @offsetOf(Config, "sscr_low_power") != 4) @compileError("Config");
    if (@sizeOf(LdoCfg) != 3) @compileError("LdoCfg");
    if (@sizeOf(RamRetention) != 4 or @offsetOf(RamRetention, "cpu1_tcm_keep") != 3) @compileError("RamRetention");
}

fn bit(on: bool, comptime m: u8) u8 {
    return if (on) m else 0;
}

pub fn sbycrOf(c: Config) u8 {
    return bit(c.opa_bus_keep, 0x40);
}

pub fn dpsbycrOf(c: Config) u8 {
    return (c.dcdc_softstart << 2) | bit(c.io_port_keep, 0x40);
}

pub fn sscr1Of(c: Config) u8 {
    return (c.sscr_low_power << 2) | bit(c.sscr_fast_return, 0x01);
}

pub fn validMode(mode: u8) bool {
    return switch (mode) {
        0, 2, 5, 8, 9, 10 => true,
        else => false,
    };
}

/// Sleep and Deep Sleep keep LPMD 0; the standby modes put the mode in LPMD.
pub fn lpscrFor(mode: u8) u8 {
    return if (mode == 0 or mode == 2) 0 else mode & 0x0F;
}

pub fn sleepdeepFor(mode: u8) bool {
    return mode != 0;
}

/// DPSIEGR3 is not exposed, so index 3 lands on DPSIEGR2 as in the C.
pub fn edgeOff(idx: u8) usize {
    return off.dpsiegr[@min(idx, 2)];
}

pub fn prcrValue(cur: u16, unlock: bool) u16 {
    const low = cur & 0xFF;
    return prcr_key | (if (unlock) low | prcr_prc1 else low & ~prcr_prc1);
}

pub fn statusWord(sbycr: u8, dpsbycr: u8, lpscr: u8, sscr1: u8) u32 {
    return @as(u32, sbycr) | (@as(u32, dpsbycr) << 8) | (@as(u32, lpscr) << 16) | (@as(u32, sscr1) << 24);
}

pub fn exitCause(w0: u32, w1: u32) u64 {
    return @as(u64, w0) | (@as(u64, w1) << 32);
}

pub fn init(hw: anytype, c: Config) void {
    hw.write8(sysc + off.sbycr, sbycrOf(c));
    hw.write8(sysc + off.dpsbycr, dpsbycrOf(c));
    hw.write8(sysc + off.sscr1, sscr1Of(c));
    hw.write8(sysc + off.lpscr, 0);
}

pub fn deinit(hw: anytype) void {
    hw.write8(sysc + off.sbycr, sbycr_reset);
    hw.write8(sysc + off.dpsbycr, dpsbycr_reset);
    hw.write8(sysc + off.sscr1, 0);
    hw.write8(sysc + off.lpscr, 0);
    hw.write32(icu + off.wupen0, 0);
    hw.write32(icu + off.wupen1, 0);
    for (off.dpsier) |o| hw.write8(sysc + o, 0);
}

pub fn setPrc1(hw: anytype, unlock: bool) void {
    hw.write16(sysc + off.prcr, prcrValue(hw.read16(sysc + off.prcr), unlock));
}

pub fn setWupen(hw: anytype, o: usize, bits: u32, on: bool) void {
    const v = hw.read32(icu + o);
    hw.write32(icu + o, if (on) v | bits else v & ~bits);
}

/// Read DPSIFRn, then write 0: the flags clear on a 0 write after a read.
pub fn clearFlags(hw: anytype, idx: u8) void {
    _ = hw.read8(sysc + off.dpsifr[idx]);
    hw.write8(sysc + off.dpsifr[idx], 0);
}

pub fn armDpsier(hw: anytype, idx: u8, value: u8) void {
    hw.write8(sysc + off.dpsier[idx], value);
    clearFlags(hw, idx);
}

pub fn snoozeRequest(hw: anytype, ulpt0: bool, ulpt1: bool, acmphs0: bool) void {
    var v = hw.read32(icu + off.wupen1);
    if (ulpt0) v |= 1 << 8;
    if (ulpt1) v |= 1 << 12;
    hw.write32(icu + off.wupen1, v);
    if (acmphs0) setWupen(hw, off.wupen0, 1 << 18, true);
}

pub fn snoozeEnd(hw: anytype, ulpt0: bool, ulpt1: bool, usbfs: bool, usbhs: bool) void {
    const en = sysc + off.dpsier[3];
    const add = bit(ulpt0, 0x04) | bit(ulpt1, 0x08) | bit(usbfs, 0x01) | bit(usbhs, 0x02);
    hw.write8(en, hw.read8(en) | add);
    clearFlags(hw, 3);
}

pub fn ramRetention(hw: anytype, c: RamRetention) void {
    hw.write16(sysc + off.pdramscr0, c.pdramscr0_bits & 0x7FFF);
    hw.write8(sysc + off.pdramscr1, bit(c.cpu0_tcm_keep, 0x01) | bit(c.cpu1_tcm_keep, 0x02));
}

fn skeep(hw: anytype, o: usize, state: u8) void {
    const v = hw.read8(sysc + o);
    hw.write8(sysc + o, (v & ~@as(u8, 0x02)) | (state << 1));
}

/// LDOCR writes need OPCCR.OPCM == 0 (high-speed mode).
pub fn ldoStandby(hw: anytype, c: LdoCfg) u16 {
    if (hw.read8(sysc + off.opccr) & 0x03 != 0) return codes.invalid_state;
    skeep(hw, off.pll1ldocr, c.pll1);
    skeep(hw, off.pll2ldocr, c.pll2);
    skeep(hw, off.hocoldocr, c.hoco);
    return codes.ok;
}

/// RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc): PRC0 open, RMW, lock all.
pub fn clockStop(hw: anytype, clock: u8, stop: bool) void {
    const a = sysc + off.clock[clock];
    hw.write16(sysc + off.prcr, prcr_key | prcr_prc0);
    const v = hw.read8(a);
    hw.write8(a, if (stop) v | 0x01 else v & ~@as(u8, 0x01));
    hw.write16(sysc + off.prcr, prcr_key);
}

pub fn clockStopped(hw: anytype, clock: u8) bool {
    return hw.read8(sysc + off.clock[clock]) & 0x01 != 0;
}

pub fn waitOpccr(hw: anytype, limit: u32) bool {
    var i: u32 = 0;
    while (i < limit) : (i += 1) {
        if (hw.read8(sysc + off.opccr) & 0x10 == 0) return true;
    }
    return false;
}

pub fn setSleepdeep(hw: anytype, on: bool) void {
    const v = hw.read32(scr);
    hw.write32(scr, if (on) v | sleepdeep else v & ~sleepdeep);
}

pub fn armSleep(hw: anytype, mode: u8) void {
    hw.write8(sysc + off.lpscr, lpscrFor(mode));
    setSleepdeep(hw, sleepdeepFor(mode));
}

pub fn disarmSleep(hw: anytype) void {
    setSleepdeep(hw, false);
    hw.write8(sysc + off.lpscr, 0);
}

pub fn status(hw: anytype) u32 {
    return statusWord(
        hw.read8(sysc + off.sbycr),
        hw.read8(sysc + off.dpsbycr),
        hw.read8(sysc + off.lpscr),
        hw.read8(sysc + off.sscr1),
    );
}

pub fn cause(hw: anytype) u64 {
    return exitCause(hw.read32(icu + off.wupen0), hw.read32(icu + off.wupen1));
}

pub fn dpsiState(hw: anytype, en: *[4]u8, fl: *[4]u8, eg: *[3]u8) void {
    for (off.dpsier, 0..) |o, i| en[i] = hw.read8(sysc + o);
    for (off.dpsifr, 0..) |o, i| fl[i] = hw.read8(sysc + o);
    for (off.dpsiegr, 0..) |o, i| eg[i] = hw.read8(sysc + o);
}
