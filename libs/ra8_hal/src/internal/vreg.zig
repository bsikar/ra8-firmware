//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Internal voltage regulator (RA8FW-788), ported from ra8_vreg.c:
//! cfg validation, LDO/DCDC switch sequences, LVOCR profile and status
//! decode. Register access goes through a duck-typed `hw` with
//! read(off) u8 and write(off, u8). The C ABI is in vreg_abi.zig.

const std = @import("std");

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_null_ptr: u16 = 0x504;

pub const off_dcdcctl: u16 = 0x440;
pub const off_vccsel: u16 = 0x441;
pub const off_lvocr: u16 = 0xAB0;

pub const dcdcon: u8 = 0x01;
pub const ocpen: u8 = 0x02;
pub const stopza: u8 = 0x10;
pub const lcboost: u8 = 0x20;
pub const fst: u8 = 0x40;
pub const pd: u8 = 0x80;
pub const dcdcctl_all: u8 = 0xF3;

pub const step_lp_vref: u8 = 0x10;
pub const step_dcdc_on: u8 = 0x11;
pub const step_with_ocp: u8 = 0x13;
pub const step_fast_on: u8 = 0x53;

pub const vccsel_mask: u8 = 3;
pub const vccsel_max: u8 = 2;
pub const lvo0e: u8 = 0x01;
pub const lvo1e: u8 = 0x02;
pub const lvo_all: u8 = 0x03;

pub const mode_ldo: u8 = 0;
pub const mode_dcdc: u8 = 1;
pub const lv_off: u8 = 0;
pub const lv_p0: u8 = 1;
pub const lv_p1: u8 = 2;
pub const ocp_off: u8 = 0;
pub const ocp_normal: u8 = 1;
pub const ocp_high: u8 = 3;
pub const standby_software: u8 = 0;
pub const standby_max: u8 = 4;

/// Mirror of ra8_vreg_cfg_t.
pub const Cfg = extern struct {
    mode: u8,
    vccsel: u8,
    ocp: u8,
    fast_startup: bool,
    ldo_boost: bool,
    lv_profile: u8,
};

/// Mirror of ra8_vreg_status_t.
pub const Status = extern struct {
    dcdcctl: u8,
    vccsel: u8,
    lvocr: u8,
    mode: u8,
    vccsel_dec: u8,
    lv_profile: u8,
    ocp: u8,
    dcdc_ready: bool,
    fast_startup: bool,
    ldo_boost: bool,
    io_buf_on: bool,
};

comptime {
    std.debug.assert(@sizeOf(Cfg) == 6);
    std.debug.assert(@offsetOf(Cfg, "lv_profile") == 5);
    std.debug.assert(@sizeOf(Status) == 11);
    std.debug.assert(@offsetOf(Status, "io_buf_on") == 10);
}

/// The C s_state.
pub const State = struct {
    dcdcctl: u8 = 0,
    vccsel: u8 = 0,
    lvocr: u8 = 0,
    ocp: u8 = ocp_off,
    lv_profile: u8 = lv_off,
    last_standby: u8 = standby_software,
    initialized: bool = false,
    fast_startup: bool = false,
    ldo_boost: bool = false,
};

pub fn validate(cfg: *const Cfg) ?u16 {
    if (cfg.mode > mode_dcdc or cfg.vccsel > vccsel_max) return err_invalid_arg;
    if (cfg.ocp > ocp_high or cfg.lv_profile > lv_p1) return err_invalid_arg;
    return null;
}

pub fn lvocrOf(profile: u8) u8 {
    if (profile == lv_p0) return lvo0e;
    if (profile == lv_p1) return lvo1e;
    return 0;
}

pub fn profileOf(lvocr: u8) u8 {
    const bits = lvocr & lvo_all;
    if (bits == lvo0e) return lv_p0;
    if (bits == lvo1e) return lv_p1;
    return lv_off;
}

/// Hardware only reports OCPEN; the level comes from the cache.
pub fn ocpOf(dcdcctl: u8, cached: u8) u8 {
    if (dcdcctl & ocpen == 0) return ocp_off;
    return if (cached == ocp_off) ocp_normal else cached;
}

pub fn packLdo(cfg: *const Cfg) u8 {
    var v: u8 = 0;
    if (cfg.ocp != ocp_off) v |= ocpen;
    if (cfg.fast_startup) v |= fst;
    if (cfg.ldo_boost) v |= lcboost;
    return v;
}

pub fn setBit(cur: u8, mask: u8, on: bool) u8 {
    return if (on) cur | mask else cur & ~mask;
}

pub fn decode(dcdcctl: u8, vccsel: u8, lvocr: u8, cached_ocp: u8) Status {
    return .{
        .dcdcctl = dcdcctl,
        .vccsel = vccsel,
        .lvocr = lvocr,
        .mode = if (dcdcctl & dcdcon != 0) mode_dcdc else mode_ldo,
        .vccsel_dec = vccsel & vccsel_mask,
        .lv_profile = profileOf(lvocr),
        .ocp = ocpOf(dcdcctl, cached_ocp),
        .dcdc_ready = dcdcctl & dcdcon != 0 and dcdcctl & pd == 0,
        .fast_startup = dcdcctl & fst != 0,
        .ldo_boost = dcdcctl & lcboost != 0,
        .io_buf_on = dcdcctl & stopza != 0,
    };
}

/// LDO -> DCDC: STOPZA, PD clear, then fast or staged steps.
/// Returns the value to cache as DCDCCTL.
pub fn enableDcdc(hw: anytype, fast: bool) u8 {
    var cur = hw.read(off_dcdcctl);
    cur |= stopza;
    hw.write(off_dcdcctl, cur);
    cur &= ~pd;
    hw.write(off_dcdcctl, cur);
    if (fast) {
        hw.write(off_dcdcctl, step_fast_on);
        return step_fast_on;
    }
    hw.write(off_dcdcctl, step_lp_vref);
    hw.write(off_dcdcctl, step_dcdc_on);
    hw.write(off_dcdcctl, step_with_ocp);
    return step_with_ocp;
}

/// DCDC -> LDO with everything off, optionally keeping LCBOOST.
pub fn disableDcdc(hw: anytype, keep_lcboost: bool) void {
    hw.write(off_dcdcctl, if (keep_lcboost) lcboost else 0);
}
