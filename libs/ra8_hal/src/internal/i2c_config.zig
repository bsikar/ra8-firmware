//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC bring-up plane (RA8FW-702), moved out of ra8_i2c_config.c: the
//! HUM Ch 39.3.2 "Initial Settings" register sequence (p 2395) and the
//! channel to MSTP gate map. Mirrors the static-inline `ra8_i2c_regs`
//! channel table from inc/ra8_i2c_regs.h.

/// HUM Ch 39.2.1, p 2369.
pub const base_addr: usize = 0x4025E000;
/// HUM Ch 39.2.1, p 2369.
pub const channel_stride: usize = 0x100;
/// HUM Ch 39.1, p 2367 (3 channels).
pub const channel_count: u8 = 3;

/// r_i2c_regs_t offsets (inc/ra8_i2c_regs.h).
pub const off_iccr1: usize = 0x00;
pub const off_icmr1: usize = 0x02;
pub const off_icfer: usize = 0x05;
pub const off_icbrl: usize = 0x10;
pub const off_icbrh: usize = 0x11;
/// Bytes of the register block the bring-up sequence touches.
pub const regs_span: usize = 0x12;

/// HUM 39.2.1 ICCR1.IICRST, p 2369.
pub const iccr1_iicrst: u8 = 1 << 6;
/// HUM 39.2.1 ICCR1.ICE, p 2369.
pub const iccr1_ice: u8 = 1 << 7;
/// HUM 39.2.3 ICMR1.CKS[6:4], p 2374.
pub const icmr1_cks_pos: u5 = 4;
/// HUM 39.2.6 ICFER.MALE, p 2378.
pub const icfer_male: u8 = 1 << 1;
/// HUM 39.2.6 ICFER.NACKE, p 2378.
pub const icfer_nacke: u8 = 1 << 4;
/// HUM 39.2.6 ICFER.SCLE, p 2378.
pub const icfer_scle: u8 = 1 << 6;
/// HUM 39.2.6 ICFER.FMPE, p 2378.
pub const icfer_fmpe: u8 = 1 << 7;
/// `k_ra8_i2c_speed_fast_plus` (inc/ra8_i2c.h).
pub const fast_plus_hz: u32 = 1_000_000;

/// `k_ra8_mstp_reg_b` (inc/ra8_mstp_regs.h).
const mstp_reg_b: u16 = 1;

/// Bit-rate register values from the RA8FW-695 solver.
pub const Rate = struct { cks: u8, brh: u8, brl: u8 };

/// `k_ra8_mstp_iicN`: IIC0 = MSTPB9, IIC1 = MSTPB8, IIC2 = MSTPB7
/// (HUM Ch 11.2.7 "MSTPCRB", p 444). Out-of-range channels map to IIC2,
/// as the C did; callers range-check first.
pub fn mstpId(channel: u8) u16 {
    const bit: u16 = switch (channel) {
        0 => 9,
        1 => 8,
        else => 7,
    };
    return (mstp_reg_b << 8) | bit;
}

/// `ra8_i2c_regs`: the register block address, or null when the channel
/// is out of range.
pub fn regsAddr(channel: u8) ?usize {
    if (channel >= channel_count) return null;
    return base_addr + @as(usize, channel) * channel_stride;
}

/// ICFER with arbitration-lost detect, NACK suspension and SCL sync, plus
/// FMPE for Fast-mode Plus.
pub fn icferValue(fast_plus: bool) u8 {
    const base = icfer_male | icfer_nacke | icfer_scle;
    return if (fast_plus) base | icfer_fmpe else base;
}

/// HUM 39.3.2 initial settings: hold IIC reset with ICE = 0, enable the
/// internal reset, program CKS / ICBRL / ICBRH / ICFER, release the reset.
pub fn applyInit(regs: [*]volatile u8, rate: Rate, fast_plus: bool) void {
    regs[off_iccr1] = 0;
    regs[off_iccr1] = iccr1_iicrst;
    regs[off_iccr1] = iccr1_iicrst | iccr1_ice;
    regs[off_icmr1] = @truncate(@as(u32, rate.cks) << icmr1_cks_pos);
    regs[off_icbrl] = rate.brl;
    regs[off_icbrh] = rate.brh;
    regs[off_icfer] = icferValue(fast_plus);
    regs[off_iccr1] = iccr1_ice;
}
