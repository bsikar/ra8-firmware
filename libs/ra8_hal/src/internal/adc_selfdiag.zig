//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ADC_B self-diagnosis and internal-channel reads (RA8FW-610), behind
//! ra8_adc.h. `hw` provides read32/write32(off) on the ADC_B block,
//! err(msg) and errVal(code).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const hw_timeout: u16 = 0x203;
pub const out_of_range: u16 = 0x208;
pub const null_ptr: u16 = 0x504;

pub const base: usize = 0x4033_8000;
pub const off_adsger: usize = 0x0048;
pub const off_adsgdcr0: usize = 0x0200;
pub const off_adchcr0: usize = 0x0600;
pub const off_addopcrc0: usize = 0x060C;
pub const off_adstr0: usize = 0x0C20;
pub const off_adsr: usize = 0x0C80;
pub const off_adexdr0: usize = 0x2180;

pub const max_channels: u8 = 24;
pub const scan_groups: u8 = 9;
pub const ext_data_regs: u8 = 23;
pub const ext_chan_base: u8 = 0x60;
pub const chan_selfdiag_adc0: u8 = 0x60;
pub const chan_temperature: u8 = 0x64;
pub const chan_int_ref_volt: u8 = 0x65;

pub const diag_vchan: u8 = 23;
pub const diag_group: u8 = 8;
pub const busy_wait_limit: u32 = 2_000_000;

pub const adexdr_mask_data: u32 = 0x0000_FFFF;
pub const adexdr_mask_err: u32 = 0x8000_0000;
pub const adsgdcr_mask_diagval: u32 = 0x7;
pub const adstr_adst: u32 = 0x1;
pub const adsr_adact0: u32 = 0x1;
pub const adprc_16bit: u8 = 0;
pub const adprc_12bit: u8 = 2;
pub const signsel_signed: u8 = 0;
pub const signsel_unsigned: u8 = 1;
pub const tol_lsb: i32 = 256;

pub fn adchcr(vch: u8) usize {
    return off_adchcr0 + @as(usize, vch) * 0x10;
}

pub fn addopcrc(vch: u8) usize {
    return off_addopcrc0 + @as(usize, vch) * 0x10;
}

pub fn adstr(group: u8) usize {
    return off_adstr0 + @as(usize, group) * 4;
}

pub fn adsgdcr(group: u8) usize {
    return off_adsgdcr0 + @as(usize, group) * 4;
}

pub fn adexdr(n: u8) usize {
    return off_adexdr0 + @as(usize, n) * 4;
}

/// ADSGDCR.DIAGVAL for a self-diagnosis mode (1..3), or null.
pub fn diagvalForMode(mode: u8) ?u32 {
    return switch (mode) {
        1 => 0x4,
        2 => 0x5,
        3 => 0x6,
        else => null,
    };
}

/// Ideal signed 16-bit code for a mode; anything else reads as mode 1.
pub fn expected(mode: u8) i32 {
    return switch (mode) {
        2 => -32768,
        3 => 32767,
        else => 0,
    };
}

pub fn inBand(diff: i32) bool {
    return diff <= tol_lsb and diff >= -tol_lsb;
}

pub fn isSupportedExtChan(chan: u8) bool {
    return chan == chan_temperature or chan == chan_int_ref_volt;
}

fn programExtChannel(hw: anytype, vch: u8, phys: u8, group: u8, differential: bool) void {
    if (vch >= max_channels) return;
    const cnvcs = (@as(u32, phys) << 8) & 0x7F00;
    const sgsel = @as(u32, group) & 0x1F;
    const ainmd: u32 = if (differential) 0x8000 else 0;
    hw.write32(adchcr(vch), cnvcs | sgsel | ainmd);
}

fn setDataFormat(hw: anytype, vch: u8, adprc: u8, signsel: u8) void {
    if (vch >= max_channels) return;
    const mask: u32 = 0x0003_0000 | 0x0010_0000;
    const fields = ((@as(u32, adprc) << 16) & 0x0003_0000) |
        ((@as(u32, signsel) << 20) & 0x0010_0000);
    const off = addopcrc(vch);
    hw.write32(off, (hw.read32(off) & ~mask) | fields);
}

fn enableDiagGroup(hw: anytype) void {
    hw.write32(off_adsger, hw.read32(off_adsger) | (@as(u32, 1) << diag_group));
}

pub fn startAndWait(hw: anytype, group: u8) u16 {
    if (group >= scan_groups) return out_of_range;
    hw.write32(adstr(group), adstr_adst);
    var i: u32 = 0;
    while (i < busy_wait_limit) : (i += 1) {
        if (hw.read32(off_adsr) & adsr_adact0 == 0) return ok;
    }
    return hw_timeout;
}

fn selfdiagRun(hw: anytype, diagval: u32) u16 {
    programExtChannel(hw, diag_vchan, chan_selfdiag_adc0, diag_group, true);
    setDataFormat(hw, diag_vchan, adprc_16bit, signsel_signed);
    enableDiagGroup(hw);
    const sg = adsgdcr(diag_group);
    hw.write32(sg, (hw.read32(sg) & ~adsgdcr_mask_diagval) | (diagval & adsgdcr_mask_diagval));
    const rc = startAndWait(hw, diag_group);
    // DIAGVAL always goes back to off so later scans on the group are normal.
    hw.write32(sg, hw.read32(sg) & ~adsgdcr_mask_diagval);
    return rc;
}

pub fn selfDiagnose(hw: anytype, mode: u8, out_code: ?*u16, out_pass: ?*bool) u16 {
    const code = out_code orelse {
        hw.err("out_code must not be nullptr");
        return null_ptr;
    };
    const pass = out_pass orelse {
        hw.err("out_pass must not be nullptr");
        return null_ptr;
    };
    code.* = 0;
    pass.* = false;
    const diagval = diagvalForMode(mode) orelse return invalid_arg;
    const rc = selfdiagRun(hw, diagval);
    if (rc != ok) {
        hw.err("self_diagnose: conversion");
        hw.errVal(rc);
        return rc;
    }
    const exd = hw.read32(adexdr(chan_selfdiag_adc0 - ext_chan_base));
    const data: u16 = @truncate(exd & adexdr_mask_data);
    const actual: i32 = @as(i16, @bitCast(data));
    code.* = data;
    pass.* = (exd & adexdr_mask_err == 0) and inBand(actual - expected(mode));
    return ok;
}

pub fn readInternalChannel(hw: anytype, chan: u8, out_raw: ?*u16) u16 {
    const raw = out_raw orelse {
        hw.err("out_raw must not be nullptr");
        return null_ptr;
    };
    raw.* = 0;
    if (!isSupportedExtChan(chan)) return invalid_arg;
    programExtChannel(hw, diag_vchan, chan, diag_group, false);
    setDataFormat(hw, diag_vchan, adprc_12bit, signsel_unsigned);
    enableDiagGroup(hw);
    const rc = startAndWait(hw, diag_group);
    if (rc != ok) {
        hw.err("read_internal: conversion");
        hw.errVal(rc);
        return rc;
    }
    const idx = chan - ext_chan_base;
    if (idx >= ext_data_regs) return out_of_range;
    raw.* = @truncate(hw.read32(adexdr(idx)) & adexdr_mask_data);
    return ok;
}
