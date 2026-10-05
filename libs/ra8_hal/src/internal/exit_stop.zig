//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Packed MSTP ids the `*_exit_stop` wrappers release (RA8FW-710), in the
//! `reg << 8 | bit` form of ra8_mstp_regs.h (reg B=1, C=2, D=3).

fn id(reg: u8, bit: u8) u16 {
    return (@as(u16, reg) << 8) | bit;
}

/// MSTPB4 I3C.
pub const i3c = id(1, 4);
/// MSTPC4 GLCDC.
pub const glcdc = id(2, 4);
/// MSTPC16 CEU.
pub const ceu = id(2, 16);
/// MSTPC30 ESWM, shared by ra8_eth and ra8_eth_gwca.
pub const eswm = id(2, 30);
/// MSTPD21 ADC16H.
pub const adc16h = id(3, 21);
