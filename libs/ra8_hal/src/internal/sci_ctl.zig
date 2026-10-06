//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SCI_B error status, CCR0 interrupt-enable toggles and MSTP ids
//! (RA8FW-906, was part of ra8_sci.c). HUM Ch 38.2.5 "CCR0" p 2182,
//! 38.2.17 "CSR" p 2225, 38.2.24 "CFCLR" p 2238, Ch 11.2.7 "MSTPCRB".

pub const channel_max: u8 = 9;

pub const err_overrun: u8 = 0x01;
pub const err_framing: u8 = 0x02;
pub const err_parity: u8 = 0x04;

const csr_orer: u32 = 1 << 24;
const csr_per: u32 = 1 << 27;
const csr_fer: u32 = 1 << 28;

/// ORERC | PERC | FERC, write-1-to-clear.
pub const clear_mask: u32 = csr_orer | csr_per | csr_fer;

pub const ccr0_rie: u32 = 1 << 16;
pub const ccr0_tie: u32 = 1 << 20;

const mstp_reg_b: u16 = 1;

/// Fold the three CSR error flags into the ra8_sci_err_mask_t bits.
pub fn errMask(csr: u32) u8 {
    var m: u8 = 0;
    if (csr & csr_orer != 0) m |= err_overrun;
    if (csr & csr_fer != 0) m |= err_framing;
    if (csr & csr_per != 0) m |= err_parity;
    return m;
}

/// Set or clear one CCR0 interrupt-enable bit.
pub fn withIe(ccr0: u32, bit: u32, on: bool) u32 {
    return if (on) ccr0 | bit else ccr0 & ~bit;
}

/// SCI0 is MSTPB31 down to SCI9 at MSTPB22.
pub fn mstpId(channel: u8) u16 {
    return (mstp_reg_b << 8) | (31 - @as(u16, channel));
}
