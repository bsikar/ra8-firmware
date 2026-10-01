//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared vocabulary for the EK-RA8D2 board layer: the error codes, the
//! levels, the clock ids and the packed port/pin encoding every other file
//! here speaks. Values mirror the C headers they came from, named rather than
//! repeated at each use site.

/// `ra8_err_t` values this layer returns or forwards.
pub const Err = struct {
    pub const ok: u32 = 0;
    pub const invalid_arg: u32 = 0x103;
    pub const not_found: u32 = 0x106;
    pub const not_initialized: u32 = 0x10F;
    pub const not_supported: u32 = 0x107;
    pub const null_ptr: u32 = 0x504;
};

/// `ra8_level_t`.
pub const Level = struct {
    pub const low: u32 = 0;
    pub const high: u32 = 1;
};

/// `ra8_clock_id_t` members this layer reads.
pub const ClockId = struct {
    pub const cpuclk0: u32 = 0;
    pub const pclka: u32 = 3;
};

/// `ra8_psel_t` members this layer routes.
pub const Psel = struct {
    pub const sci_async: u32 = 0x04;
    pub const usb_fs: u32 = 0x13;
};

/// Packed `ra8_port_pin_t`: port in the high byte, pin in the low byte.
pub const Pin = struct {
    pub fn pack(port_id: u16, pin_index: u16) u16 {
        return (port_id << 8) | pin_index;
    }
};
