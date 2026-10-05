//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Low-power asynchronous general-purpose timer (AGT) logic (RA8FW-800),
//! ported from ra8_agt.c. HUM Ch 24. Register sequences are generic over a
//! Hw with write8/write16, so tests drive a fake.

const std = @import("std");

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
};

/// AGT0 base; channel n is at base + n * stride (HUM 24.2).
pub const base: usize = 0x4022_1000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 10;

pub const off_agt: usize = 0x00;
pub const off_cma: usize = 0x02;
pub const off_cmb: usize = 0x04;
pub const off_cr: usize = 0x08;
pub const off_mr1: usize = 0x09;
pub const off_mr2: usize = 0x0A;
pub const off_ioc: usize = 0x0C;
pub const off_cmsr: usize = 0x0E;

pub const cr_tstart: u8 = 0x01;
const mr1_tmod_timer: u8 = 0x00;
const mr1_tmod_pulse: u8 = 0x01;
const tck_agt0_underflow: u8 = 0x50;
const ioc_tedgsel: u8 = 0x01;
const ioc_toe: u8 = 0x04;
const cmsr_a: u8 = 0x01 | 0x02; // TCMEA | TOEA
const cmsr_b: u8 = 0x10 | 0x20; // TCMEB | TOEB
const compare_parked: u16 = 0xFFFF;

/// Only AGT0 (MSTPD5) and AGT1 (MSTPD4) have a module-stop bit to hold.
pub const mstp_count: u8 = 2;
pub const mstp_ids = [mstp_count]u16{ (3 << 8) | 5, (3 << 8) | 4 };

pub const cascade_lo: u8 = 0;
pub const cascade_hi: u8 = 1;

pub const polarity_active_high: u8 = 0;
pub const polarity_active_low: u8 = 1;
pub const compare_none: u8 = 0;
pub const compare_a: u8 = 1;
pub const compare_b: u8 = 2;

pub const EventFn = *const fn (ctx: ?*anyopaque, channel: u8) callconv(.C) void;

/// ra8_agt_pulse_cfg_t.
pub const PulseCfg = extern struct {
    period: u16,
    duty: u16,
    mode: u8,
    polarity: u8,
    compare: u8,
};

/// ra8_agt_cascade_cfg_t.
pub const CascadeCfg = extern struct {
    reload32: u32,
    clock: u8,
    on_underflow: ?EventFn,
    ctx: ?*anyopaque,
};

comptime {
    std.debug.assert(@sizeOf(PulseCfg) == 8 and @offsetOf(PulseCfg, "compare") == 6);
    std.debug.assert(@offsetOf(CascadeCfg, "clock") == 4);
}

/// ra8_agt(channel): null past the last channel.
pub fn regs(channel: u8) ?usize {
    if (channel >= channel_count) return null;
    return base + @as(usize, channel) * stride;
}

pub fn pulseCfgOk(c: PulseCfg) bool {
    if (c.compare != compare_none and c.compare != compare_a and c.compare != compare_b) return false;
    return c.polarity == polarity_active_high or c.polarity == polarity_active_low;
}

/// TOE always; TEDGSEL for active high.
pub fn ioc(polarity: u8) u8 {
    return if (polarity == polarity_active_high) ioc_toe | ioc_tedgsel else ioc_toe;
}

pub fn cmsr(compare: u8) u8 {
    return switch (compare) {
        compare_a => cmsr_a,
        compare_b => cmsr_b,
        else => 0,
    };
}

/// Cascade count source for the AGT0 half; null for an unknown enum.
pub fn cascadeTck(clock: u8) ?u8 {
    return switch (clock) {
        0 => 0x00, // PCLKB
        1 => 0x10, // PCLKB / 8
        2 => 0x30, // PCLKB / 2
        else => null,
    };
}

/// Timer mode, PCLKB, reload, then TSTART.
pub fn startFreeRun(hw: anytype, r: usize, reload: u16) void {
    hw.write8(r + off_cr, 0);
    hw.write8(r + off_mr1, 0);
    hw.write8(r + off_mr2, 0);
    hw.write16(r + off_agt, reload);
    hw.write8(r + off_cr, cr_tstart);
}

/// Pulse output mode (HUM 24.3.4); the caller sets TSTART afterwards.
pub fn programPulse(hw: anytype, r: usize, c: PulseCfg) void {
    hw.write8(r + off_cr, 0);
    hw.write8(r + off_mr1, mr1_tmod_pulse);
    hw.write8(r + off_mr2, 0);
    hw.write8(r + off_ioc, ioc(c.polarity));
    hw.write8(r + off_cmsr, cmsr(c.compare));
    hw.write16(r + off_cma, if (c.compare == compare_a) c.duty else compare_parked);
    hw.write16(r + off_cmb, if (c.compare == compare_b) c.duty else compare_parked);
    hw.write16(r + off_agt, c.period);
}

fn armHalf(hw: anytype, r: usize, mr1: u8, reload: u16) void {
    hw.write8(r + off_cr, 0);
    hw.write8(r + off_mr1, mr1);
    hw.write8(r + off_mr2, 0);
    hw.write8(r + off_ioc, 0);
    hw.write8(r + off_cmsr, 0);
    hw.write16(r + off_agt, reload);
}

/// Low 16 bits in AGT0, high 16 in AGT1 counting AGT0 underflows. AGT1
/// starts first so it sees AGT0's first underflow.
pub fn armCascade(hw: anytype, reload32: u32, tck_lo: u8) void {
    const lo = base + @as(usize, cascade_lo) * stride;
    const hi = base + @as(usize, cascade_hi) * stride;
    armHalf(hw, lo, mr1_tmod_timer | tck_lo, @truncate(reload32));
    armHalf(hw, hi, mr1_tmod_timer | tck_agt0_underflow, @truncate(reload32 >> 16));
    hw.write8(hi + off_cr, cr_tstart);
    hw.write8(lo + off_cr, cr_tstart);
}
