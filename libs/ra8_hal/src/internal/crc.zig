//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8 CRC calculator register file (HUM Ch 48). The block is a base
//! address so host tests can point it at a fake register array.

pub const base_addr: usize = 0x40310000;
pub const off_crccr0: usize = 0x00;
pub const off_crccr1: usize = 0x01;
pub const off_crcdir: usize = 0x04;
pub const off_crcdor: usize = 0x08;
pub const block_size: usize = 0x10;

/// CRCCR0.DORCLR, write-only: clears CRCDOR (HUM 48.2.1 p 3181).
pub const dorclr: u8 = 1 << 7;
/// CRCCR0.GPS[2:0] polynomial select.
pub const gps_mask: u8 = 0x07;
/// CRC-32 / CRC-32C init and xor-out; the hardware applies neither.
pub const seed32: u32 = 0xFFFF_FFFF;

pub const poly_32_ieee802_3: u8 = 4;
pub const poly_32c_rev: u8 = 5;

pub fn is32Bit(poly: u8) bool {
    return poly == poly_32_ieee802_3 or poly == poly_32c_rev;
}

pub const Block = struct {
    base: usize = base_addr,

    fn reg8(b: Block, off: usize) *volatile u8 {
        return @ptrFromInt(b.base + off);
    }

    fn reg32(b: Block, off: usize) *volatile u32 {
        return @ptrFromInt(b.base + off);
    }

    pub fn crccr0(b: Block) u8 {
        return b.reg8(off_crccr0).*;
    }

    /// GPS plus DORCLR in one store, as FSP R_CRC_Open does.
    pub fn select(b: Block, poly: u8) void {
        b.reg8(off_crccr0).* = poly | dorclr;
    }

    /// Snoop off (CRCCR1 = 0).
    pub fn snoopOff(b: Block) void {
        b.reg8(off_crccr1).* = 0;
    }

    /// Read-modify-write so GPS/LMS survive the DORCLR pulse.
    pub fn reset(b: Block) void {
        const p = b.reg8(off_crccr0);
        p.* = p.* | dorclr;
    }

    pub fn clear(b: Block) void {
        b.reg8(off_crccr0).* = 0;
        b.reg8(off_crccr1).* = 0;
    }

    pub fn stop(b: Block) void {
        b.reg8(off_crccr0).* = 0;
    }

    /// Runs data through the selected polynomial and returns the result.
    /// CRC-32/32C feed whole little-endian words; a trailing partial word
    /// is not fed, matching the C driver.
    pub fn compute(b: Block, data: []const u8) u32 {
        if (is32Bit(b.crccr0() & gps_mask)) {
            b.reg32(off_crcdor).* = seed32;
            b.feedWords(data);
            return b.reg32(off_crcdor).* ^ seed32;
        }
        b.feedBytes(data);
        return b.reg32(off_crcdor).*;
    }

    fn feedWords(b: Block, data: []const u8) void {
        const dir = b.reg32(off_crcdir);
        var i: usize = 0;
        while (i + 4 <= data.len) : (i += 4) {
            dir.* = @as(u32, data[i]) | (@as(u32, data[i + 1]) << 8) |
                (@as(u32, data[i + 2]) << 16) | (@as(u32, data[i + 3]) << 24);
        }
    }

    fn feedBytes(b: Block, data: []const u8) void {
        const dir = b.reg8(off_crcdir);
        for (data) |byte| dir.* = byte;
    }
};
