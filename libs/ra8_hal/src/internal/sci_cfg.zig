//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SCI_B async-UART config encoders (RA8FW-905, was part of ra8_sci.c).
//! HUM Ch 38.2.6 "CCR1" p 2185, 38.2.7 "CCR2" p 2189 Table 38.7,
//! 38.2.8 "CCR3" p 2203.

/// Mirrors ra8_sci_cfg_t.
pub const Cfg = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    pclk_hz: u32,
};

pub const parity_none: u8 = 0;
pub const parity_odd: u8 = 2;
pub const data_7: u8 = 7;
pub const stop_2: u8 = 1;

const ccr1_spb2dt: u32 = 1 << 4;
const ccr1_spb2io: u32 = 1 << 5;
const ccr1_pe: u32 = 1 << 8;
const ccr1_pm: u32 = 1 << 9;
const ccr2_shift_brr = 8;
const ccr2_shift_mddr = 24;
const mddr_default: u32 = 0xFF;
const ccr3_bpen: u32 = 1 << 7;
const ccr3_shift_chr = 8;
const ccr3_lsbf: u32 = 1 << 12;
const ccr3_stp: u32 = 1 << 14;
const chr_8bit: u32 = 0x2;
const chr_7bit: u32 = 0x3;
const async_divisor: u64 = 32; // 64 * 2^(2n - 1), n = 0
const brr_max: u64 = 255;
const cks_max: u8 = 3;

/// N = PCLK / (32 * B) - 1, saturating at 0 when unreachable.
pub fn brr(pclk_hz: u32, baud: u32) u8 {
    if (baud == 0 or pclk_hz == 0) return 0;
    const n = @as(u64, pclk_hz) / (async_divisor * baud);
    if (n == 0) return 0;
    return @truncate(n - 1);
}

/// SPB2DT + SPB2IO keep TXD idling high while TE = 0.
pub fn ccr1(cfg: Cfg) u32 {
    var v = ccr1_spb2dt | ccr1_spb2io;
    if (cfg.parity != parity_none) {
        v |= ccr1_pe;
        if (cfg.parity == parity_odd) v |= ccr1_pm;
    }
    return v;
}

/// BRR in [15:8], MDDR left at its 0xFF reset value.
pub fn ccr2(cfg: Cfg) u32 {
    const b: u32 = brr(cfg.pclk_hz, cfg.baud);
    return (b << ccr2_shift_brr) | (mddr_default << ccr2_shift_mddr);
}

/// LSB first + synchronizer bypass, async mode, CHR and STP from cfg.
pub fn ccr3(cfg: Cfg) u32 {
    var v = ccr3_lsbf | ccr3_bpen;
    const chr = if (cfg.data_bits == data_7) chr_7bit else chr_8bit;
    v |= chr << ccr3_shift_chr;
    if (cfg.stop_bits == stop_2) v |= ccr3_stp;
    return v;
}

pub const Baud = struct { brr: u16, cks: u8 };

/// Smallest CKS (divisor 32 * 4^n) giving BRR <= 255; null when none fits.
pub fn baudCalculate(baud: u32, pclk_hz: u32) ?Baud {
    if (baud == 0 or pclk_hz == 0) return null;
    var divisor: u64 = async_divisor;
    var n: u8 = 0;
    while (n <= cks_max) : (n += 1) {
        const q = @as(u64, pclk_hz) / (divisor * baud);
        if (q > 0 and q - 1 <= brr_max) return .{ .brr = @intCast(q - 1), .cks = n };
        divisor *= 4;
    }
    return null;
}
