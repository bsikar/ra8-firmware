//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RTC init, count-source start and clock init (RA8FW-855; ra8_rtc.c
//! is gone). Exports live in src/rtc_init_abi.zig. `hw` supplies
//! wait(reg, mask, expect), delay(ms), prcr(value), running(reg, mask),
//! info(msg), infoVal(msg, value) and err(msg). HUM Ch 26 and Ch 9.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const hw_init_failed: u16 = 0x201;

/// `ra8_rtc_clk_src_t`: RCKSEL 0 = sub-clock, 1 = LOCO.
pub const clk_subclock: u8 = 0;
pub const clk_loco: u8 = 1;

/// RCR2 bits (HUM Ch 26.2.21 p 1232).
pub const rcr2_start: u8 = 0x01;
pub const rcr2_reset: u8 = 0x02;
pub const rcr2_hr24: u8 = 0x40;
pub const rcr2_cntmd: u8 = 0x80;

/// PRCR: 0xA5 key | PRC0 (CGC), then the key alone to relock.
pub const prcr_unlock_cgc: u16 = 0xA501;
pub const prcr_lock_all: u16 = 0xA500;
/// LCSTP / SOSTP, bit 0 of LOCOCR / SOSCCR.
pub const osc_stop: u8 = 0x01;
pub const somcr_drv_standard: u8 = 0;

pub const stab_sub_ms: u32 = 1000;
pub const stab_loco_ms: u32 = 5;
pub const six_clocks_ms: u32 = 1;
pub const reset_ms: u32 = 5;
/// RFRL for a 32.768 kHz LOCO: (32768 / 128) - 1.
pub const rfrl_32768: u16 = 0x00FF;

pub const View = struct {
    rcr1: *volatile u8,
    rcr2: *volatile u8,
    rcr4: *volatile u8,
    rfrh: *volatile u16,
    rfrl: *volatile u16,
    lococr: *volatile u8,
    sosccr: *volatile u8,
    somcr: *volatile u8,
};

/// 24-hour calendar mode, IRQs masked, counter running.
pub fn init(hw: anytype, v: View) u16 {
    v.rcr2.* = 0;
    hw.wait(v.rcr2, rcr2_cntmd, 0);
    v.rcr1.* = 0;
    hw.wait(v.rcr1, 0xFF, 0);
    v.rcr2.* = rcr2_hr24;
    hw.wait(v.rcr2, rcr2_hr24, rcr2_hr24);
    v.rcr2.* = rcr2_hr24 | rcr2_start;
    hw.wait(v.rcr2, rcr2_start, rcr2_start);
    hw.info("rtc_init (24h calendar)");
    return ok;
}

/// Starts LOCO or the 32.768 kHz crystal and checks it is running.
pub fn startCountSource(hw: anytype, v: View, src: u8) u16 {
    hw.prcr(prcr_unlock_cgc);
    if (src == clk_loco) {
        v.lococr.* = 0;
    } else {
        v.somcr.* = somcr_drv_standard;
        v.sosccr.* = 0;
    }
    hw.prcr(prcr_lock_all);
    const reg = if (src == clk_loco) v.lococr else v.sosccr;
    hw.delay(if (src == clk_loco) stab_loco_ms else stab_sub_ms);
    return if (hw.running(reg, osc_stop)) ok else hw_init_failed;
}

/// Selects the count source, then soft-resets the prescaler against it.
pub fn clockInit(hw: anytype, v: View, src: u8) u16 {
    if (src > clk_loco) return invalid_arg;
    const osc = startCountSource(hw, v, src);
    if (osc != ok) {
        hw.err("rtc count source not running");
        return osc;
    }
    v.rcr4.* = src;
    hw.delay(six_clocks_ms);
    v.rcr2.* = v.rcr2.* & ~rcr2_start;
    hw.wait(v.rcr2, rcr2_start, 0);
    if (src == clk_loco) {
        v.rfrh.* = 0;
        v.rfrl.* = rfrl_32768;
    }
    v.rcr2.* = rcr2_reset;
    hw.delay(reset_ms);
    hw.wait(v.rcr2, rcr2_reset, 0);
    hw.infoVal("rtc clock init src", src);
    return ok;
}
