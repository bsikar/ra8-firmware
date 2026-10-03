//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Event Link Controller (inc/ra8_elc.h, RA8FW-547). HUM Ch 19.2
//! "ELCR" p 819, "ELSEGRn" p 819, "ELSRn" p 820.

pub const base: usize = 0x40201000;

pub const off_elcr: usize = 0x000;
pub const off_elsegr0: usize = 0x004;
pub const elsegr_stride: usize = 4;
pub const off_elsr0: usize = 0x020;
pub const elsr_stride: usize = 4;

pub const elsr_count: u8 = 53;
pub const segr_count: u8 = 4;

pub const elcon: u8 = 1 << 7;

/// ELSEGRn three-write sequence (FSP ELC_ELSEGRN_STEP1..3): WI/WE/SEG.
pub const step_unlock: u8 = 0x00;
pub const step_arm: u8 = 0x40;
pub const step_trigger: u8 = 0x41;

pub const Error = error{ OutOfRange, InvalidArg };

/// The ELC register window; host tests point `base` at a fake block.
pub const Window = struct {
    base: usize,

    fn elcr(w: Window) *volatile u8 {
        return @ptrFromInt(w.base + off_elcr);
    }

    fn elsr(w: Window, index: u8) *volatile u16 {
        return @ptrFromInt(w.base + off_elsr0 + @as(usize, index) * elsr_stride);
    }

    fn elsegr(w: Window, group: u8) *volatile u8 {
        return @ptrFromInt(w.base + off_elsegr0 + @as(usize, group) * elsegr_stride);
    }
};

/// Clear every ELSR route, then every ELSEGR, before ELCON is set.
pub fn clearRoutes(w: Window) void {
    for (0..elsr_count) |i| w.elsr(@intCast(i)).* = 0;
    for (0..segr_count) |g| w.elsegr(@intCast(g)).* = step_unlock;
}

pub fn setEnabled(w: Window, enable: bool) void {
    w.elcr().* = if (enable) elcon else 0;
}

pub fn isEnabled(w: Window) bool {
    return (w.elcr().* & elcon) != 0;
}

pub fn link(w: Window, index: u8, event: u16) Error!void {
    if (index >= elsr_count) return error.OutOfRange;
    w.elsr(index).* = event;
}

pub fn unlink(w: Window, index: u8) Error!void {
    return link(w, index, 0);
}

/// SEG latches only after unlock, arm, trigger in that order.
pub fn trigger(w: Window, index: u8) Error!void {
    if (index >= segr_count) return error.InvalidArg;
    const reg = w.elsegr(index);
    reg.* = step_unlock;
    reg.* = step_arm;
    reg.* = step_trigger;
}
