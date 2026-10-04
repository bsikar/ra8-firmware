//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Cortex-M85 L1 cache maintenance (RA8FW-612), behind ra8_cache.h.
//! `hw` provides read32/write32(addr) on the SCB cache registers,
//! dsb(), isb() and err(msg).

pub const ok: u16 = 0;
pub const null_ptr: u16 = 0x504;

pub const ccr: usize = 0xE000_ED14;
pub const ctr: usize = 0xE000_ED7C;
pub const ccsidr: usize = 0xE000_ED80;
pub const csselr: usize = 0xE000_ED84;
pub const iciallu: usize = 0xE000_EF50;
pub const dcimvac: usize = 0xE000_EF5C;
pub const dcisw: usize = 0xE000_EF60;
pub const dccmvac: usize = 0xE000_EF68;
pub const dccimvac: usize = 0xE000_EF70;
pub const dccisw: usize = 0xE000_EF74;

pub const word_bytes: u32 = 4;
pub const ccr_ic: u32 = 1 << 17;
pub const ccr_dc: u32 = 1 << 16;

pub fn lineBytes(hw: anytype) u32 {
    const dmin: u5 = @intCast((hw.read32(ctr) >> 16) & 0xF);
    return word_bytes << dmin;
}

pub const Span = struct { start: usize, lines: u32 };

/// Line-aligned start and line count covering [addr, addr + size).
pub fn span(addr: usize, size: u32, line: u32) Span {
    const mask: usize = @as(usize, line) -% 1;
    const start = addr & ~mask;
    const last = (addr +% @as(usize, size) -% 1) & ~mask;
    return .{ .start = start, .lines = @as(u32, @truncate((last -% start) / line)) +% 1 };
}

pub fn maintainRange(hw: anytype, addr: usize, size: u32, reg: usize) u16 {
    if (addr == 0) {
        hw.err("maintain: addr");
        return null_ptr;
    }
    if (size == 0) return ok;
    const line = lineBytes(hw);
    const s = span(addr, size, line);
    hw.dsb();
    var i: u32 = 0;
    while (i < s.lines) : (i += 1) {
        hw.write32(reg, @truncate(s.start +% @as(usize, i) * line));
    }
    hw.dsb();
    hw.isb();
    return ok;
}

/// Apply a set/way op to every line of the L1 D-cache, as the C did:
/// both counters run down to 0 inclusive.
pub fn setwayAll(hw: anytype, op_reg: usize) void {
    hw.write32(csselr, 0);
    hw.dsb();
    const id = hw.read32(ccsidr);
    if (id == 0 or id == 0xFFFF_FFFF) return;
    var sets: u32 = (id >> 13) & 0x7FFF;
    while (true) {
        var ways: u32 = (id >> 3) & 0x3FF;
        while (true) {
            hw.write32(op_reg, (sets << 5) | (ways << 30));
            if (ways == 0) break;
            ways -= 1;
        }
        if (sets == 0) break;
        sets -= 1;
    }
    hw.dsb();
    hw.isb();
}

pub fn icacheInvalidateAll(hw: anytype) void {
    hw.dsb();
    hw.isb();
    hw.write32(iciallu, 0);
    hw.dsb();
    hw.isb();
}

pub fn icacheEnable(hw: anytype) void {
    icacheInvalidateAll(hw);
    hw.write32(ccr, hw.read32(ccr) | ccr_ic);
    hw.dsb();
    hw.isb();
}

pub fn icacheDisable(hw: anytype) void {
    hw.dsb();
    hw.isb();
    hw.write32(ccr, hw.read32(ccr) & ~ccr_ic);
    hw.write32(iciallu, 0);
    hw.dsb();
    hw.isb();
}

pub fn dcacheEnable(hw: anytype) void {
    setwayAll(hw, dcisw);
    hw.write32(ccr, hw.read32(ccr) | ccr_dc);
    hw.dsb();
    hw.isb();
}

pub fn dcacheDisable(hw: anytype) void {
    hw.write32(ccr, hw.read32(ccr) & ~ccr_dc);
    setwayAll(hw, dccisw);
}
