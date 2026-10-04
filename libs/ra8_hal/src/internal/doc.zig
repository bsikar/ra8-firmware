//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Data Operation Circuit (DOC_B, HUM Ch 57). Port of ra8_doc.c
//! (RA8FW-561). Access widths match the C: init zeroes the data
//! registers with 32-bit stores, every other data access is 16-bit.

pub const base_addr: usize = 0x40311000;
pub const off_docr: usize = 0x00;
pub const off_dosr: usize = 0x04;
pub const off_doscr: usize = 0x08;
pub const off_dodir: usize = 0x0C;
pub const off_dodsr0: usize = 0x10;
pub const off_dodsr1: usize = 0x14;

pub const mode_compare: u8 = 0;
pub const mode_add: u8 = 1;
pub const mode_subtract: u8 = 2;
pub const mask_oms: u8 = 0x03;
pub const mask_dcsel: u8 = 0x70;
pub const bit_dcsel: u3 = 4;
pub const dcsel_inside: u8 = 4;
pub const dcsel_outside: u8 = 5;
pub const mask_dopcf: u8 = 0x01;
pub const mask_dopcfcl: u8 = 0x01;

/// `ra8_doc_window_polarity_t`.
pub const window_inside: u8 = 0;
pub const window_outside: u8 = 1;

pub const WindowError = error{ BadRange, BadPolarity };
pub const CompareError = error{NotCompareMode};

pub const Block = struct {
    base: usize = base_addr,

    fn r8(b: Block, off: usize) *volatile u8 {
        return @ptrFromInt(b.base + off);
    }

    fn r16(b: Block, off: usize) *volatile u16 {
        return @ptrFromInt(b.base + off);
    }

    fn r32(b: Block, off: usize) *volatile u32 {
        return @ptrFromInt(b.base + off);
    }

    /// The register half of `ra8_doc_init`: compare/16-bit, flag
    /// cleared, data registers zeroed.
    pub fn reset(b: Block) void {
        b.r8(off_docr).* = 0;
        b.r8(off_doscr).* = mask_dopcfcl;
        b.r32(off_dodir).* = 0;
        b.r32(off_dodsr0).* = 0;
        b.r32(off_dodsr1).* = 0;
    }

    /// Seed DODSR0, write DODIR to trigger, read the accumulator back.
    fn run16(b: Block, mode: u8, seed: u16, operand: u16) u16 {
        b.r8(off_docr).* = mode;
        b.r16(off_dodsr0).* = seed;
        b.r16(off_dodir).* = operand;
        return b.r16(off_dodsr0).*;
    }

    pub fn add16(b: Block, a: u16, c: u16) u16 {
        return b.run16(mode_add, a, c);
    }

    pub fn sub16(b: Block, a: u16, c: u16) u16 {
        return b.run16(mode_subtract, a, c);
    }

    /// OMS=00, DOBW=0, DCSEL inside (4) or outside (5); thresholds; clear.
    pub fn setWindow(b: Block, lower: u16, upper: u16, polarity: u8) WindowError!void {
        if (lower >= upper) return error.BadRange;
        if (polarity > window_outside) return error.BadPolarity;
        const dcsel = if (polarity == window_outside) dcsel_outside else dcsel_inside;
        b.r8(off_docr).* = (dcsel << bit_dcsel) & mask_dcsel;
        b.r16(off_dodsr0).* = lower;
        b.r16(off_dodsr1).* = upper;
        b.r8(off_doscr).* = mask_dopcfcl;
    }

    /// Clear, write DODIR to compare, read DOPCF, clear again.
    pub fn windowCompare(b: Block, value: u16) CompareError!bool {
        if ((b.r8(off_docr).* & mask_oms) != 0) return error.NotCompareMode;
        b.r8(off_doscr).* = mask_dopcfcl;
        b.r16(off_dodir).* = value;
        const flag = (b.r8(off_dosr).* & mask_dopcf) != 0;
        b.r8(off_doscr).* = mask_dopcfcl;
        return flag;
    }
};
