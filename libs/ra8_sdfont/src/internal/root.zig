//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared constants for the SD-card font store: the `ra8_err_t` values this
//! library can produce on its own, and the two limits that are policy rather
//! than hardware.
//!
//! No externs and no exported symbols, so the whole file is host-testable.

/// `ra8_err_t` values `ra8_sdfont` produces itself. Anything else a lower ring
/// returns is forwarded untouched.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const not_found: u16 = 0x106;
    pub const no_data: u16 = 0x10A;
    pub const null_ptr: u16 = 0x504;
};

/// Policy limits, as opposed to anything the bus imposes.
pub const limits = struct {
    /// Smallest plausible TTF/OTF header. A shorter read is a truncated or
    /// stub file, and returning it would fault the shaper downstream.
    pub const min_font_bytes: u32 = 16;
};

/// Filename used when the caller leaves `filename` NULL.
pub const default_font_name: [:0]const u8 = "FONT.OTF";
