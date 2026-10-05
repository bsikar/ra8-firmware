//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Packed MSTP id decoders (RA8FW-704). An `ra8_mstp_t` id carries the
//! MSTPCRA..E register index in its high byte and the bit in its low byte.

/// Register index 0..4 (MSTPCRA..E) from a packed id.
pub fn reg(id: u16) u8 {
    return @truncate(id >> 8);
}

/// Bit position from a packed id.
pub fn bit(id: u16) u8 {
    return @truncate(id);
}
