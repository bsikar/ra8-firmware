//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CANFD channel and global mode handshakes, the default acceptance rule,
//! RX FIFO0 setup and open_channel (RA8FW-863, was part of ra8_canfd.c).
//! Exports live in src/canfd_mode_abi.zig. HUM Ch 41 p 2742, p 2762-2766.

const afl = @import("canfd_afl.zig");

/// `k_ra8_canfd_spin`; `k_ra8_err_hw_timeout`.
pub const spin: u32 = 20000;
pub const hw_timeout: u16 = 0x203;

pub const off_ctr: usize = 0x004;
pub const off_sts: usize = 0x008;
pub const off_gctr: usize = 0x018;
pub const off_gsts: usize = 0x01C;
pub const off_rfcc0: usize = 0x03C;

/// CHMDC[1:0] / GMDC[1:0] plus the sleep request bit 2 (CSLPR / GSLPR).
pub const mode_mask: u32 = 0x3;
pub const sleep_bit: u32 = 1 << 2;
/// CRSTSTS / GRSTSTS bit 0, CHLTSTS / GHLTSTS bit 1.
pub const reset_sts: u32 = 1 << 0;
pub const halt_sts: u32 = 1 << 1;
/// RFDC = 4 entries, RFPLS = 64 bytes; RFE bit 0.
pub const rfcc_default: u32 = (1 << 8) | (7 << 4);
pub const rfe: u32 = 1 << 0;

/// Channel (`ra8_chmdc_mode_t`) and global (`k_ra8_gctr_value_*`) share values.
pub const operation: u32 = 0;
pub const reset: u32 = 1;
pub const halt: u32 = 2;

pub const Wait = struct { mask: u32, set: bool };

pub fn modeWord(v: u32, mode: u32) u32 {
    return (v & ~(mode_mask | sleep_bit)) | (mode & mode_mask);
}

/// Halt and reset wait for their status bit; any other value waits for both
/// to clear (operation).
pub fn waitFor(mode: u32) Wait {
    return switch (mode) {
        halt => .{ .mask = halt_sts, .set = true },
        reset => .{ .mask = reset_sts, .set = true },
        else => .{ .mask = reset_sts | halt_sts, .set = false },
    };
}

fn handshake(hw: anytype, off_ctl: usize, off_st: usize, mode: u32) u16 {
    hw.write(off_ctl, modeWord(hw.read(off_ctl), mode));
    const w = waitFor(mode);
    return if (hw.wait(off_st, w.mask, w.set)) 0 else hw_timeout;
}

pub fn setChannelMode(hw: anytype, mode: u32) u16 {
    return handshake(hw, off_ctr, off_sts, mode);
}

pub fn setGlobalMode(hw: anytype, mode: u32) u16 {
    return handshake(hw, off_gctr, off_gsts, mode);
}

/// One rule on page 0 that accepts every ID into RX FIFO 0.
pub fn installDefaultAfl(hw: anytype) void {
    hw.write(afl.off_cfg0, 1 << afl.cfg0_rnc0_shift);
    hw.write(afl.off_ectr, afl.ectr_afldae);
    hw.write(afl.off_gafl + 0x0, 0);
    hw.write(afl.off_gafl + 0x4, 0);
    hw.write(afl.off_gafl + 0x8, 0);
    hw.write(afl.off_gafl + 0xC, afl.gaflp1_fdp0);
    hw.write(afl.off_ectr, 0);
}

pub fn configureRxFifo0(hw: anytype) void {
    hw.write(off_rfcc0, rfcc_default);
}

pub fn enableRxFifo0(hw: anytype) void {
    hw.write(off_rfcc0, hw.read(off_rfcc0) | rfe);
}

/// Reset both state machines, install the default rule and FIFO0, back to
/// global operation, arm FIFO0 (RFE is only writable outside GL_RESET),
/// then channel operation.
pub fn openChannel(hw: anytype) u16 {
    _ = setGlobalMode(hw, reset);
    _ = setChannelMode(hw, reset);
    installDefaultAfl(hw);
    configureRxFifo0(hw);
    const g = setGlobalMode(hw, operation);
    if (g != 0) return g;
    enableRxFifo0(hw);
    return setChannelMode(hw, operation);
}
