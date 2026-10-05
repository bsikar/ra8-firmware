//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! General PWM Timer (GPT) logic, ported from ra8_gpt.c (RA8FW-801).
//! HUM Ch 22. Register offsets follow r_gpt_channel_regs_t in
//! ra8_gpt_regs.h. Every write sequence is bracketed by GTWP unlock and
//! lock, in the same order as the C.

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const null_ptr: u16 = 0x105;
};

pub const base: usize = 0x40322000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 14;
pub const gtclkcr: usize = 0x40323F10;
pub const gtclkcr_bpen: u32 = 0x1;

pub const off = struct {
    pub const gtwp: usize = 0x00;
    pub const gtstr: usize = 0x04;
    pub const gtstp: usize = 0x08;
    pub const gtupsr: usize = 0x1C;
    pub const gtdnsr: usize = 0x20;
    pub const gticasr: usize = 0x24;
    pub const gticbsr: usize = 0x28;
    pub const gtcr: usize = 0x2C;
    pub const gtior: usize = 0x34;
    pub const gtst: usize = 0x3C;
    pub const gtber: usize = 0x40;
    pub const gtcnt: usize = 0x48;
    pub const gtccr: usize = 0x4C;
    pub const gtpr: usize = 0x64;
    pub const gtpbr: usize = 0x68;
    pub const gtdtcr: usize = 0x88;
    pub const gtdvu: usize = 0x8C;
    pub const gtdvd: usize = 0x90;
};

/// GTCCR array index. Renesas orders the array A, B, C, E, D, F, so
/// index 3 is GTCCRE, the buffer for GTCCRB.
pub const ccr_a: u8 = 0;
pub const ccr_b: u8 = 1;
pub const ccr_c: u8 = 2;
pub const ccr_e: u8 = 3;

pub const gtwp_unlock: u32 = 0xA500;
pub const gtwp_lock: u32 = 0xA501;
pub const gtcr_cst: u32 = 0x1;
pub const gtst_mask: u32 = 0xC3;
pub const gtber_ccra_single: u32 = 0x00010000;
pub const gtber_ccrb_single: u32 = 0x00040000;
pub const gtdtcr_tde: u32 = 0x1;
pub const cap_src_valid: u32 = 0x01FFFFFF;
pub const cnt_src_valid: u32 = 0x00FFFFFF;

pub const status_ccra: u32 = 0x01;
pub const status_ccrb: u32 = 0x02;
pub const status_overflow: u32 = 0x40;
pub const status_underflow: u32 = 0x80;

pub const three_phase_count = 3;

/// MSTPCRE bits: GPT0..3 = 31..28, GPT4..9 share 27, GPT10..13 = 21..18.
pub const mstp_ids = [channel_count]u16{
    0x41F, 0x41E, 0x41D, 0x41C, 0x41B, 0x41B, 0x41B,
    0x41B, 0x41B, 0x41B, 0x415, 0x414, 0x413, 0x412,
};

pub const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

/// Mirror of ra8_gpt_cfg_t.
pub const Cfg = extern struct {
    mode: u8,
    prescaler: u8,
    period: u32,
    duty_a: u32,
    duty_b: u32,
    auto_start: bool,
};

/// Mirror of ra8_gpt_pwm_pin_cfg_t.
pub const PinCfg = extern struct {
    output_enable: bool,
    polarity: u8,
    stop_level: u8,
    disable_on_fault: u8,
};

/// Mirror of ra8_gpt_three_phase_cfg_t.
pub const ThreePhaseCfg = extern struct {
    channels: [three_phase_count]u8,
    mode: u8,
    prescaler: u8,
    period_counts: u32,
    initial_duty_u: u32,
    initial_duty_v: u32,
    initial_duty_w: u32,
};

comptime {
    if (@sizeOf(Cfg) != 20 or @offsetOf(Cfg, "auto_start") != 16) @compileError("ra8_gpt_cfg_t layout");
    if (@sizeOf(PinCfg) != 4) @compileError("ra8_gpt_pwm_pin_cfg_t layout");
    if (@sizeOf(ThreePhaseCfg) != 24 or @offsetOf(ThreePhaseCfg, "period_counts") != 8) @compileError("ra8_gpt_three_phase_cfg_t layout");
}

pub fn regs(channel: u8) ?usize {
    if (channel >= channel_count) return null;
    return base + @as(usize, channel) * stride;
}

pub fn bit(channel: u8) u32 {
    return @as(u32, 1) << @intCast(channel);
}

pub fn gtcr(mode: u8, prescaler: u8) u32 {
    return (@as(u32, mode) << 16) | (@as(u32, prescaler) << 23);
}

pub fn ccr(index: u8) usize {
    return off.gtccr + @as(usize, index) * 4;
}

/// GTIOR pattern: 0x6 for active low (polarity 1), else 0x9.
pub fn pattern(polarity: u8) u32 {
    return if (polarity == 1) 0x6 else 0x9;
}

/// Re-pack one pin's GTIOR fields, leaving the other pin's untouched.
pub fn packGtior(current: u32, pin_b: bool, cfg: PinCfg) u32 {
    const p = pattern(cfg.polarity);
    const shift: u5 = if (pin_b) 16 else 0;
    const dflt: u5 = if (pin_b) 22 else 6;
    const oe: u32 = if (pin_b) 0x01000000 else 0x100;
    const df: u5 = if (pin_b) 25 else 9;
    const clear = (@as(u32, 0x1F) << shift) | (@as(u32, 1) << dflt) | oe | (@as(u32, 3) << df);
    var v = current & ~clear;
    v |= (p & 0x1F) << shift;
    v |= (@as(u32, cfg.stop_level) << dflt) & (@as(u32, 1) << dflt);
    if (cfg.output_enable) v |= oe;
    v |= (@as(u32, cfg.disable_on_fault) << df) & (@as(u32, 3) << df);
    return v;
}

pub fn unlock(hw: anytype, r: usize) void {
    hw.write32(r + off.gtwp, gtwp_unlock);
}

pub fn lock(hw: anytype, r: usize) void {
    hw.write32(r + off.gtwp, gtwp_lock);
}

pub fn orIn(hw: anytype, a: usize, v: u32) void {
    hw.write32(a, hw.read32(a) | v);
}

pub fn startFreeRun(hw: anytype, r: usize, channel: u8, period: u32) void {
    unlock(hw, r);
    hw.write32(r + off.gtstp, bit(channel));
    hw.write32(r + off.gtcr, 1);
    hw.write32(r + off.gtpr, period);
    hw.write32(r + off.gtcnt, 0);
    hw.write32(r + off.gtstr, bit(channel));
    lock(hw, r);
}

pub fn initRegs(hw: anytype, r: usize, channel: u8, cfg: *const Cfg) void {
    unlock(hw, r);
    hw.write32(r + off.gtstp, bit(channel));
    hw.write32(r + off.gtcr, gtcr(cfg.mode, cfg.prescaler));
    hw.write32(r + off.gtpr, cfg.period);
    hw.write32(r + off.gtpbr, cfg.period);
    hw.write32(r + ccr(ccr_a), cfg.duty_a);
    hw.write32(r + ccr(ccr_b), cfg.duty_b);
    hw.write32(r + off.gtcnt, 0);
    if (cfg.auto_start) {
        orIn(hw, r + off.gtcr, gtcr_cst);
        hw.write32(r + off.gtstr, bit(channel));
    }
    lock(hw, r);
}

/// Unlock, write one register by offset, lock.
pub fn guarded(hw: anytype, r: usize, o: usize, v: u32) void {
    unlock(hw, r);
    hw.write32(r + o, v);
    lock(hw, r);
}

pub fn periodSet(hw: anytype, r: usize, period: u32) void {
    unlock(hw, r);
    hw.write32(r + off.gtpbr, period);
    if (hw.read32(r + off.gtcr) & gtcr_cst == 0) {
        hw.write32(r + off.gtpr, period);
        hw.write32(r + off.gtcnt, 0);
    }
    lock(hw, r);
}

/// Buffered duty: shadow register plus its GTBER single-buffer bit.
pub fn bufferedDuty(hw: anytype, r: usize, pin_b: bool, value: u32) void {
    unlock(hw, r);
    if (pin_b) {
        hw.write32(r + ccr(ccr_e), value);
        orIn(hw, r + off.gtber, gtber_ccrb_single);
    } else {
        hw.write32(r + ccr(ccr_c), value);
        orIn(hw, r + off.gtber, gtber_ccra_single);
    }
    lock(hw, r);
}

pub fn phaseDuty(hw: anytype, r: usize, duty: u32) void {
    unlock(hw, r);
    hw.write32(r + ccr(ccr_c), duty);
    hw.write32(r + ccr(ccr_e), duty);
    orIn(hw, r + off.gtber, gtber_ccra_single | gtber_ccrb_single);
    lock(hw, r);
}

pub fn deadTime(hw: anytype, r: usize, rising: u32, falling: u32) void {
    unlock(hw, r);
    hw.write32(r + off.gtdvu, rising);
    hw.write32(r + off.gtdvd, falling);
    hw.write32(r + off.gtdtcr, if (rising != 0 or falling != 0) gtdtcr_tde else 0);
    lock(hw, r);
}

/// GTST flags clear by writing the current value with those bits zero.
pub fn clearFlags(hw: anytype, r: usize, mask: u32) void {
    const cur = hw.read32(r + off.gtst);
    hw.write32(r + off.gtst, cur & ~mask);
}
