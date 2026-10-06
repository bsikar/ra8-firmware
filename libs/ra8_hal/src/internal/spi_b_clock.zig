//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SPI_B bit-rate divider and error-flag mapping (RA8FW-892, was part of
//! ra8_spi_b.c). HUM Ch 43.2.6 "SPCR3" p 2891, Ch 43.2.9 "SPSR" p 2898.
//! Exports live in src/spi_b_clock_abi.zig.

pub const spcr3_spbr_shift: u5 = 8;
pub const spcr3_spbr_mask: u32 = 0x0000_FF00;
pub const spsr_errs: u32 = 0x1D00_0000; // OVRF | MODF | PERF | UDRF

/// SPBR = PCLKA / (2 * baud) - 1, clamped to the 8-bit field; 0 when the
/// inputs give nothing usable (f_RSPCK = PCLKA / (2 * (SPBR + 1)), N = 0).
pub fn spbr(baud_hz: u32, pclka_hz: u32) u8 {
    if (baud_hz == 0 or pclka_hz == 0) return 0;
    const n = pclka_hz / (2 *% baud_hz);
    if (n == 0) return 0;
    return @intCast(@min(n - 1, 0xFF));
}

/// SPCR3 with SPBR[15:8] replaced and every other bit kept.
pub fn withSpbr(spcr3: u32, value: u8) u32 {
    return (spcr3 & ~spcr3_spbr_mask) | ((@as(u32, value) << spcr3_spbr_shift) & spcr3_spbr_mask);
}

const flag_map = [_]struct { spsr: u32, err: u8 }{
    .{ .spsr = 0x0100_0000, .err = 0x01 }, // OVRF -> overrun
    .{ .spsr = 0x0400_0000, .err = 0x02 }, // MODF -> mode
    .{ .spsr = 0x0800_0000, .err = 0x04 }, // PERF -> parity
    .{ .spsr = 0x1000_0000, .err = 0x08 }, // UDRF -> underrun
};

/// SPSR error flags as the k_ra8_spi_err_* bit set.
pub fn errMask(spsr: u32) u8 {
    var m: u8 = 0;
    for (flag_map) |f| {
        if (spsr & f.spsr != 0) m |= f.err;
    }
    return m;
}
