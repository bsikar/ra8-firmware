//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! PFS pin-mux helpers (HUM Ch 20.2). Port of ra8_mpc.c (RA8FW-560).
//! Every write sits inside a PWPR/PWPRS unlock..lock window, as in the C.

/// ra8_err_t values used here (libs/ra8_core/inc/ra8_err.h).
pub const Code = struct {
    pub const ok: u16 = 0;
    pub const gpio_invalid_port: u16 = 0x206;
    pub const gpio_invalid_pin: u16 = 0x207;
};

pub const pfs_base: usize = 0x40400800;
pub const pwpr_off: usize = 0x50C; // PMISC 0x40400D00 + 0x0C
pub const pwprs_off: usize = 0x514; // PMISC 0x40400D00 + 0x14
pub const port_max: u8 = 14;
pub const pin_max: u8 = 15;
pub const pin_count: usize = 16;

pub const pwpr_pfswe: u8 = 1 << 6;
pub const pwpr_b0wi: u8 = 1 << 7;

pub const mask_pdr: u32 = 0x0000_0004;
pub const mask_pcr: u32 = 0x0000_0010;
pub const mask_ncodr: u32 = 0x0000_0040;
pub const mask_isel: u32 = 0x0000_4000;
pub const mask_asel: u32 = 0x0000_8000;
pub const mask_pmr: u32 = 0x0001_0000;
pub const mask_psel: u32 = 0x1F00_0000;
pub const psel_shift: u5 = 24;

/// The bounds check every entry point runs first.
pub fn check(port: u8, pin: u8) u16 {
    if (port > port_max) return Code.gpio_invalid_port;
    if (pin > pin_max) return Code.gpio_invalid_pin;
    return Code.ok;
}

/// PSEL field for `psel`, masked to 5 bits.
pub fn pselBits(psel: u8) u32 {
    return (@as(u32, psel) << psel_shift) & mask_psel;
}

/// The PFS array and its PMISC write-protect registers.
pub const Block = struct {
    base: usize = pfs_base,

    /// PmnPFS; the caller has already run `check`.
    pub fn pmn(b: Block, port: u8, pin: u8) *volatile u32 {
        const idx = @as(usize, port) * pin_count + pin;
        return @ptrFromInt(b.base + idx * 4);
    }

    fn reg8(b: Block, off: usize) *volatile u8 {
        return @ptrFromInt(b.base + off);
    }

    /// Clear B0WI, then set PFSWE, on both the NS and Secure paths.
    pub fn unlock(b: Block) void {
        b.reg8(pwpr_off).* = 0;
        b.reg8(pwpr_off).* = pwpr_pfswe;
        b.reg8(pwprs_off).* = 0;
        b.reg8(pwprs_off).* = pwpr_pfswe;
    }

    /// Clear PFSWE, then set B0WI, on both paths.
    pub fn lock(b: Block) void {
        b.reg8(pwpr_off).* = 0;
        b.reg8(pwpr_off).* = pwpr_b0wi;
        b.reg8(pwprs_off).* = 0;
        b.reg8(pwprs_off).* = pwpr_b0wi;
    }

    fn reset(b: Block, port: u8, pin: u8, value: u32) u16 {
        const err = check(port, pin);
        if (err != Code.ok) return err;
        b.unlock();
        b.pmn(port, pin).* = value;
        b.lock();
        return Code.ok;
    }

    fn update(b: Block, port: u8, pin: u8, clear: u32, set: u32) u16 {
        const err = check(port, pin);
        if (err != Code.ok) return err;
        const pfs = b.pmn(port, pin);
        b.unlock();
        pfs.* = (pfs.* & ~clear) | set;
        b.lock();
        return Code.ok;
    }

    /// HUM 20.2.4: PMR := 0, then PSEL, then PSEL | PMR, so the pin never
    /// drives the previous function with PMR already set.
    pub fn routePeripheral(b: Block, port: u8, pin: u8, psel: u8) u16 {
        const err = check(port, pin);
        if (err != Code.ok) return err;
        const bits = pselBits(psel);
        const pfs = b.pmn(port, pin);
        b.unlock();
        pfs.* = 0;
        pfs.* = bits;
        pfs.* = bits | mask_pmr;
        b.lock();
        return Code.ok;
    }

    pub fn setGpio(b: Block, port: u8, pin: u8, output: bool) u16 {
        return b.reset(port, pin, if (output) mask_pdr else 0);
    }

    pub fn setAnalog(b: Block, port: u8, pin: u8) u16 {
        return b.reset(port, pin, mask_asel);
    }

    pub fn setIrq(b: Block, port: u8, pin: u8) u16 {
        return b.reset(port, pin, mask_isel);
    }

    pub fn setPull(b: Block, port: u8, pin: u8, up: bool) u16 {
        return b.update(port, pin, mask_pcr, if (up) mask_pcr else 0);
    }

    pub fn setOpenDrain(b: Block, port: u8, pin: u8, enable: bool) u16 {
        return b.update(port, pin, mask_ncodr, if (enable) mask_ncodr else 0);
    }

    pub fn readPfs(b: Block, port: u8, pin: u8, out: *u32) u16 {
        const err = check(port, pin);
        if (err != Code.ok) return err;
        out.* = b.pmn(port, pin).*;
        return Code.ok;
    }
};
