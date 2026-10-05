//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC error-status flags (RA8FW-694): decode ICSR2 into the
//! `k_ra8_i2c_err_*` mask and clear the latched error bits. Mirrors the
//! static-inline `ra8_i2c_regs` channel table from inc/ra8_i2c_regs.h.

/// HUM Ch 39.2.1, p 2369.
pub const base_addr: usize = 0x4025E000;
/// HUM Ch 39.2.1, p 2369.
pub const channel_stride: usize = 0x100;
/// HUM Ch 39.1, p 2367 (3 channels).
pub const channel_count: u8 = 3;
/// ICSR2 offset in r_i2c_regs_t (HUM 39.2.10, p 2384).
pub const off_icsr2: usize = 0x09;

/// HUM 39.2.10 ICSR2.TMOF, p 2384.
pub const icsr2_tmof: u8 = 1 << 0;
/// HUM 39.2.10 ICSR2.AL, p 2384.
pub const icsr2_al: u8 = 1 << 1;
/// HUM 39.2.10 ICSR2.NACKF, p 2384.
pub const icsr2_nackf: u8 = 1 << 4;
/// Every ICSR2 flag that `decode` reports (W0C).
pub const clear_mask: u8 = icsr2_al | icsr2_nackf | icsr2_tmof;

/// `k_ra8_i2c_err_*` (inc/ra8_i2c.h).
pub const Err = struct {
    pub const none: u8 = 0x00;
    pub const arb_lost: u8 = 0x01;
    pub const nack: u8 = 0x02;
    pub const timeout: u8 = 0x04;
};

/// `ra8_i2c_regs` + ICSR2: the register address, or null when the channel
/// is out of range.
pub fn icsr2Addr(channel: u8) ?usize {
    if (channel >= channel_count) return null;
    return base_addr + @as(usize, channel) * channel_stride + off_icsr2;
}

/// `internal_i2c_decode_errors`: ICSR2 flags to the error mask.
pub fn decode(icsr2: u8) u8 {
    var mask: u8 = Err.none;
    if ((icsr2 & icsr2_al) != 0) mask |= Err.arb_lost;
    if ((icsr2 & icsr2_nackf) != 0) mask |= Err.nack;
    if ((icsr2 & icsr2_tmof) != 0) mask |= Err.timeout;
    return mask;
}

/// Clear AL, NACKF and TMOF (HUM 39.2.10 W0C, p 2384); other ICSR2 bits
/// are written back as read.
pub fn clear(icsr2: *volatile u8) void {
    icsr2.* = icsr2.* & ~clear_mask;
}
