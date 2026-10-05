//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC bit-rate solver (RA8FW-695): pick the ICMR1.CKS divider and the
//! ICBRH / ICBRL half-period fields for a bus rate. Pure, so host tests
//! need no register block.

/// ICBRL / ICBRH counter fields are 5 bits (HUM 39.2.15, p 2391).
pub const br_field_max: u32 = 0x1F;
/// Upper 3 reserved bits of ICBRL / ICBRH read as 1 (HUM 39.2.15-16).
pub const br_reserved_hi: u8 = 0xE0;
/// CKS[2:0] selects PCLKB / 2^CKS (HUM 39.2.3, p 2374).
pub const cks_max: u8 = 7;
/// ICMR1.CKS position (HUM 39.2.3, p 2374).
pub const icmr1_cks_pos: u3 = 4;

/// Solver output, ready to write to ICMR1.CKS, ICBRH and ICBRL.
pub const Rate = struct { cks: u8, brh: u8, brl: u8 };

/// `internal_i2c_pick_cks`: halve `total` until each half-period fits the
/// 5-bit field, returning the divider exponent.
pub fn pickCks(total: *u32) u8 {
    var cks: u32 = 0;
    var i: u32 = 0;
    while (i <= cks_max) : (i += 1) {
        if (total.* / 2 <= br_field_max + 1) break;
        cks = i + 1;
        total.* >>= 1;
    }
    return @intCast(@min(cks, cks_max));
}

/// `internal_i2c_clamp_half`: half-period count minus one, clamped to
/// the field.
pub fn clampHalf(total: u32) u8 {
    const half = @max(total / 2, 1);
    return @intCast(@min(half - 1, br_field_max));
}

/// `internal_i2c_bitrate` after the null and zero-clock checks: each bit
/// takes (BRH+1)+(BRL+1) IICphi cycles, split evenly.
pub fn solve(bus_hz: u32, pclkb_hz: u32) Rate {
    var total = pclkb_hz / bus_hz;
    const cks = pickCks(&total);
    const field = br_reserved_hi | clampHalf(total);
    return .{ .cks = cks, .brh = field, .brl = field };
}

/// ICMR1 with CKS[6:4] replaced and the other bits kept.
pub fn icmr1WithCks(icmr1: u8, cks: u8) u8 {
    const mask: u8 = cks_max << icmr1_cks_pos;
    return (icmr1 & ~mask) | (cks << icmr1_cks_pos);
}
