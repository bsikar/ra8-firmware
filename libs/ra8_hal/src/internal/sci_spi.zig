//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SCI simple-SPI helpers (HUM Ch 38). Pure code; the C ABI, register
//! access and flag waits live in sci_spi_abi.zig (RA8FW-579).

/// SCI0 base; channels sit 0x100 apart (SCI0..SCI9).
pub const base: usize = 0x40358000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 10;

pub const off_rdr: usize = 0x00;
pub const off_tdr: usize = 0x04;
pub const off_ccr0: usize = 0x08;
pub const off_ccr1: usize = 0x0C;
pub const off_ccr2: usize = 0x10;
pub const off_ccr3: usize = 0x14;
pub const off_ccr4: usize = 0x18;
pub const off_csr: usize = 0x48;
pub const off_cfclr: usize = 0x68;

pub const ccr0_te_re: u32 = (1 << 4) | (1 << 0);
pub const csr_tdre: u32 = 1 << 29;
pub const csr_rdrf: u32 = 1 << 31;
pub const cfclr_rdrfc: u32 = 1 << 31;
/// Clears every W1C bit at once.
pub const cfclr_default: u32 = 0x9D070010;
pub const rdr_data8: u32 = 0xFF;
pub const idle_byte: u8 = 0xFF;
/// k_ra8_hw_budget_long, about 100 us at 1 GHz.
pub const wait_budget: u32 = 0x00040000;

const cks_max: u32 = 3;
const brr_max: u32 = 255;
const div_base: u32 = 4;
const mddr_default: u32 = 0xFF;

/// Mirror of ra8_sci_spi_cfg_t.
pub const Cfg = extern struct {
    baud_hz: u32,
    pclk_hz: u32,
    mode: u8,
    lsb_first: bool,
};

pub const Rate = struct { cks: u32, brr: u32 };

pub fn channelOk(channel: u8) bool {
    return channel < channel_count;
}

pub fn regAddr(channel: u8, offset: usize) usize {
    return base + @as(usize, channel) * stride + offset;
}

/// MSTPB31..22 (k_ra8_mstp_reg_b = 1) for SCI0..SCI9.
pub fn mstpId(channel: u8) u16 {
    return (1 << 8) | (31 - @as(u16, channel));
}

/// B = PCLK / (4 * 4^n * (N + 1)); the first CKS whose BRR fits wins.
/// A wrapped-to-zero denominator (C UB) counts as the fastest rate.
pub fn resolveRate(baud_hz: u32, pclk_hz: u32) Rate {
    var cks: u32 = 0;
    while (cks <= cks_max) : (cks += 1) {
        const divisor = div_base << @intCast(2 * cks);
        const denom = divisor *% baud_hz;
        const q = if (denom == 0) 0 else pclk_hz / denom;
        if (q == 0) return .{ .cks = cks, .brr = 0 };
        if (q - 1 <= brr_max) return .{ .cks = cks, .brr = q - 1 };
    }
    return .{ .cks = cks_max, .brr = brr_max };
}

/// CCR2: BRR[15:8], CKS[21:20], MDDR[31:24] at its reset value.
pub fn ccr2(baud_hz: u32, pclk_hz: u32) u32 {
    const r = resolveRate(baud_hz, pclk_hz);
    var v: u32 = (r.brr << 8) & 0x0000FF00;
    v |= (r.cks << 20) & 0x00300000;
    return v | (mddr_default << 24);
}

/// CCR3: simple-SPI mode, 8-bit data, CPOL/CPHA from mode bits 1/0.
pub fn ccr3(cfg: Cfg) u32 {
    var v: u32 = (3 << 16) | (2 << 8);
    if (cfg.mode & 2 != 0) v |= 1 << 1;
    if (cfg.mode & 1 != 0) v |= 1 << 0;
    if (cfg.lsb_first) v |= 1 << 12;
    return v;
}
