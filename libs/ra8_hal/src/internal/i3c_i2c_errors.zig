//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C legacy-I2C controller error flags (RA8FW-693): decode BST into the
//! `k_ra8_i3c_i2c_err_*` mask and clear the latched error bits. Works on
//! the BST word by pointer so host tests can use a plain u32.

/// HUM 40.2 BST.NACKDF, p 2482.
pub const bst_nackdf: u32 = 1 << 4;
/// HUM 40.2 BST.ALF, p 2482.
pub const bst_alf: u32 = 1 << 16;
/// HUM 40.2 BST.TODF, p 2482.
pub const bst_todf: u32 = 1 << 20;
/// Every BST flag that `decode` reports.
pub const clear_mask: u32 = bst_alf | bst_nackdf | bst_todf;

/// `k_ra8_i3c_i2c_err_*` (inc/ra8_i3c_i2c.h).
pub const Err = struct {
    pub const none: u8 = 0x00;
    pub const arb_lost: u8 = 0x01;
    pub const nack: u8 = 0x02;
    pub const timeout: u8 = 0x04;
};

/// `internal_i3c_i2c_decode_errors`: BST flags to the error mask.
pub fn decode(bst: u32) u8 {
    var mask: u8 = Err.none;
    if ((bst & bst_alf) != 0) mask |= Err.arb_lost;
    if ((bst & bst_nackdf) != 0) mask |= Err.nack;
    if ((bst & bst_todf) != 0) mask |= Err.timeout;
    return mask;
}

/// Clear ALF, NACKDF and TODF (HUM Ch 40.2.46, p 2490); other BST bits
/// are written back as read.
pub fn clear(bst: *volatile u32) void {
    bst.* = bst.* & ~clear_mask;
}
