//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! 12-bit DAC_B driver (RA8FW-585, was ra8_dac_b.c). Pure: the two DAC_B
//! instances come in through a `regs` value (read32/write32/write16 by
//! channel and offset) and MSTP and logging through an `ops` value.
//! HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490..3496; FSP r_dac_b.

/// FSP R_DAC_B0_BASE; DAC_B1 sits one stride above it.
pub const base0: usize = 0x4023_3000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 2;
pub const max_value: u16 = 4095;

/// Register offsets in one instance (inc/ra8_dac_b_regs.h).
pub const off_dadr: usize = 0x00;
pub const off_dacr0: usize = 0x04;
pub const off_dacr1: usize = 0x08;
pub const off_dacr2: usize = 0x0C;

pub const dacen: u32 = 0x0000_0001;
pub const daoutdis: u32 = 0x8000_0000;
pub const dpsel_shift: u5 = 16;
pub const ofssel_shift: u5 = 8;
pub const ofssel_mask: u32 = 0x0000_0100;

/// MSTPD20 (DAC12 ch 0) and MSTPD19 (ch 1): (k_ra8_mstp_reg_d << 8) | bit.
pub const mstp_dac0: u16 = (3 << 8) | 20;
pub const mstp_dac1: u16 = (3 << 8) | 19;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;

/// `ra8_dac_b_cfg_t` (inc/ra8_dac_b.h).
pub const Cfg = extern struct {
    vref: u8,
    data_format: u8,
    internal_output_enabled: bool,
    enable_channel0: bool,
    enable_channel1: bool,
};

pub fn clamp(value: u16) u16 {
    return @min(value, max_value);
}

/// RA8_RETURN_ON_ERROR: on error, log `msg` (then the code) and hand it up.
fn check(ops: anytype, err: u16, msg: [*:0]const u8) ?u16 {
    if (err == ok) return null;
    ops.fail(msg, err);
    return err;
}

fn setBits(regs: anytype, ch: u8, off: usize, mask: u32) void {
    regs.write32(ch, off, regs.read32(ch, off) | mask);
}

fn clearBits(regs: anytype, ch: u8, off: usize, mask: u32) void {
    regs.write32(ch, off, regs.read32(ch, off) & ~mask);
}

fn disableChannel(regs: anytype, ch: u8) void {
    regs.write32(ch, off_dacr0, 0);
    regs.write16(ch, off_dadr, 0);
}

/// Release both module stops, DAC_B0 first, logging `msg0`/`msg1` on error.
fn mstpOn(ops: anytype, msg0: [*:0]const u8, msg1: [*:0]const u8) u16 {
    if (check(ops, ops.mstpEnable(mstp_dac0), msg0)) |e| return e;
    if (check(ops, ops.mstpEnable(mstp_dac1), msg1)) |e| return e;
    return ok;
}

/// Gate both module stops, DAC_B1 first; the DAC_B0 result is returned.
fn mstpOff(ops: anytype) u16 {
    _ = ops.mstpDisable(mstp_dac1);
    return ops.mstpDisable(mstp_dac0);
}

pub fn init(regs: anytype, ops: anytype) u16 {
    const err = mstpOn(ops, "dac_b_init: mstp dac0", "dac_b_init: mstp dac1");
    if (err != ok) return err;
    disableChannel(regs, 0);
    disableChannel(regs, 1);
    ops.info("dac_b_init");
    return ok;
}

pub fn write(regs: anytype, ops: anytype, ch: u8, value: u16) u16 {
    if (ch >= channel_count) return invalid_arg;
    const clamped = clamp(value);
    regs.write16(ch, off_dadr, clamped);
    ops.infoVal("dac_b_write value", clamped);
    return ok;
}

/// FSP R_DAC_B_Open register image for one instance.
fn applyCfg(regs: anytype, ch: u8, cfg: *const Cfg) void {
    regs.write32(ch, off_dacr0, 0);
    regs.write32(ch, off_dacr1, @as(u32, cfg.data_format) << dpsel_shift);
    regs.write32(ch, off_dacr2, @as(u32, cfg.vref) << ofssel_shift);
    regs.write16(ch, off_dadr, 0);
    if (!cfg.internal_output_enabled) setBits(regs, ch, off_dacr0, daoutdis);
}

pub fn initConfigured(regs: anytype, ops: anytype, cfg: *const Cfg) u16 {
    const err = mstpOn(ops, "dac_b_init_cfg: mstp dac0", "dac_b_init_cfg: mstp dac1");
    if (err != ok) return err;
    applyCfg(regs, 0, cfg);
    applyCfg(regs, 1, cfg);
    if (cfg.enable_channel0) setBits(regs, 0, off_dacr0, dacen);
    if (cfg.enable_channel1) setBits(regs, 1, off_dacr0, dacen);
    ops.info("dac_b_init_configured");
    return ok;
}

/// FSP R_DAC_B_Close: outputs disabled, data cleared, modules stopped.
pub fn deinit(regs: anytype, ops: anytype) u16 {
    var ch: u8 = 0;
    while (ch < channel_count) : (ch += 1) {
        regs.write32(ch, off_dacr0, daoutdis);
        regs.write16(ch, off_dadr, 0);
    }
    return mstpOff(ops);
}

/// DACR2.OFSSEL on both instances (the VREFH range is board-wide).
pub fn setVref(regs: anytype, vref: u8) u16 {
    const shifted = (@as(u32, vref) << ofssel_shift) & ofssel_mask;
    var ch: u8 = 0;
    while (ch < channel_count) : (ch += 1) {
        regs.write32(ch, off_dacr2, (regs.read32(ch, off_dacr2) & ~ofssel_mask) | shifted);
    }
    return ok;
}

pub fn setOutputEnable(regs: anytype, ch: u8, enable: bool) u16 {
    if (ch >= channel_count) return invalid_arg;
    if (enable) setBits(regs, ch, off_dacr0, dacen) else clearBits(regs, ch, off_dacr0, dacen);
    return ok;
}

/// Bit n set when instance n has DACEN set.
pub fn status(regs: anytype) u8 {
    var flags: u8 = 0;
    if (regs.read32(0, off_dacr0) & dacen != 0) flags |= 0x1;
    if (regs.read32(1, off_dacr0) & dacen != 0) flags |= 0x2;
    return flags;
}

pub fn clearStatus(regs: anytype) u16 {
    clearBits(regs, 0, off_dacr0, dacen);
    clearBits(regs, 1, off_dacr0, dacen);
    return ok;
}

pub fn enterStop(regs: anytype, ops: anytype) u16 {
    disableChannel(regs, 0);
    disableChannel(regs, 1);
    return mstpOff(ops);
}

pub fn exitStop(ops: anytype) u16 {
    if (check(ops, ops.mstpEnable(mstp_dac0), "dac_b_exit_stop: mstp0")) |e| return e;
    return ops.mstpEnable(mstp_dac1);
}
