//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared vocabulary for the RA8P1 board-support layer: the `ra8_err_t` values
//! this layer can return, the GPIO level / pull encodings it hands the HAL, and
//! the `ra8_port_pin_t` packing. One place so no module re-spells a magic
//! number.

/// `ra8_err_t` values this layer returns (libs/ra8_core/inc/ra8_err.h).
pub const Err = struct {
    pub const ok: u32 = 0;
    pub const invalid_arg: u32 = 0x103;
    pub const not_initialized: u32 = 0x10F;
};

/// `ra8_level_t` (libs/ra8_core/inc/ra8_port_constants.h).
pub const Level = struct {
    pub const low: u8 = 0;
    pub const high: u8 = 1;
};

/// `ra8_pin_pull_t`.
pub const Pull = struct {
    pub const none: u8 = 0;
    pub const up: u8 = 1;
};

/// `ra8_port_pin_t`: port in the high byte, pin in the low byte.
pub const Pin = struct {
    pub const none: u16 = 0xFFFF;

    /// `RA8_PIN(port, pin)`.
    pub fn pack(port_id: u8, pin_index: u8) u16 {
        return (@as(u16, port_id) << 8) | @as(u16, pin_index);
    }

    pub fn port(packed_pin: u16) u8 {
        return @truncate(packed_pin >> 8);
    }

    pub fn index(packed_pin: u16) u8 {
        return @truncate(packed_pin);
    }
};
