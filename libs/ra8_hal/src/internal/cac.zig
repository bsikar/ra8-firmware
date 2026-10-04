//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Clock Frequency Accuracy Measurement Circuit (CAC, HUM Ch 10). Port of
//! ra8_cac.c (RA8FW-568). Every register is accessed at the width the C
//! r_cac_regs_t gave it: the control/status bytes are 8-bit, the limit
//! and counter-buffer registers 16-bit.

pub const base_addr: usize = 0x40202400;
pub const off_cacr0: usize = 0x00;
pub const off_cacr1: usize = 0x01;
pub const off_cacr2: usize = 0x02;
pub const off_caicr: usize = 0x03;
pub const off_castr: usize = 0x04;
pub const off_caulvr: usize = 0x06;
pub const off_callvr: usize = 0x08;
pub const off_cacntbr: usize = 0x0A;

/// `ra8_cac_status_mask_t` (CASTR bits 0..2).
pub const status_ferrf: u8 = 0x01;
pub const status_mendf: u8 = 0x02;
pub const status_ovff: u8 = 0x04;
pub const status_mask_all: u8 = status_ferrf | status_mendf | status_ovff;

/// CACR0.CFME (bit 0).
pub const cfme: u8 = 0x01;
/// CASTR flag n is cleared by CAICR bit n+4 (FERRFCL/MENDFCL/OVFFCL).
pub const castr_to_caicr_shift: u3 = 4;
pub const cfme_settle_iters: u32 = 1024;
pub const poll_limit: u32 = 200000;

pub const MeasureError = error{Timeout};

pub const Block = struct {
    base: usize = base_addr,

    fn r8(b: Block, off: usize) *volatile u8 {
        return @ptrFromInt(b.base + off);
    }

    fn r16(b: Block, off: usize) *volatile u16 {
        return @ptrFromInt(b.base + off);
    }

    /// Write CACR0 and wait (bounded) for it to read back.
    fn setCfme(b: Block, value: u8) void {
        b.r8(off_cacr0).* = value;
        var i: u32 = 0;
        while (i < cfme_settle_iters) : (i += 1) {
            if (b.r8(off_cacr0).* == value) return;
        }
    }

    /// CAICR value that clears the CASTR flags in `mask`.
    pub fn clearBits(mask: u8) u8 {
        return (mask & status_mask_all) << castr_to_caicr_shift;
    }

    /// The register half of `ra8_cac_init`.
    pub fn configure(b: Block, upper: u16, lower: u16) void {
        b.setCfme(0);
        b.r8(off_caicr).* = clearBits(status_mask_all);
        b.r8(off_cacr1).* = 0;
        b.r8(off_cacr2).* = 0;
        b.r16(off_caulvr).* = upper;
        b.r16(off_callvr).* = lower;
    }

    /// Start a measurement, poll MENDF, return CACNTBR; CFME is cleared
    /// on both the success and the timeout path.
    pub fn measure(b: Block) MeasureError!u16 {
        b.setCfme(cfme);
        var i: u32 = 0;
        while (i < poll_limit) : (i += 1) {
            if ((b.r8(off_castr).* & status_mendf) != 0) {
                const count = b.r16(off_cacntbr).*;
                b.setCfme(0);
                return count;
            }
        }
        b.setCfme(0);
        return error.Timeout;
    }

    /// The register half of `ra8_cac_deinit`.
    pub fn shutdown(b: Block) void {
        b.setCfme(0);
        b.r8(off_cacr1).* = 0;
        b.r8(off_cacr2).* = 0;
        b.r8(off_caicr).* = 0;
    }

    /// Stop measuring (the register half of `ra8_cac_enter_stop`).
    pub fn stop(b: Block) void {
        b.setCfme(0);
    }

    pub fn status(b: Block) u8 {
        return b.r8(off_castr).* & status_mask_all;
    }

    pub fn clear(b: Block, mask: u8) void {
        b.r8(off_caicr).* = clearBits(mask);
    }

    /// Read and acknowledge the pending flags (the ISR half of dispatch).
    pub fn takePending(b: Block) u8 {
        const mask = b.status();
        b.r8(off_caicr).* = clearBits(mask);
        return mask;
    }
};
