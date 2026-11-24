//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Canonical `ra8_err_t` ABI values used across `ra8_update`.

/// ABI error constants matching `libs/ra8_core/inc/ra8_err.h`.
pub const Err = enum(u16) {
    ok = 0x0000,
    fail = 0x0101,
    no_mem = 0x0102,
    invalid_arg = 0x0103,
    invalid_state = 0x0104,
    invalid_size = 0x0105,
    not_found = 0x0106,
    not_supported = 0x0107,
    hw_error = 0x0204,
    crc_mismatch = 0x0405,
    validation_failed = 0x0501,
    checksum_mismatch = 0x0502,
    null_ptr = 0x0504,

    pub fn raw(self: Err) u16 {
        return @backingInt(self);
    }

    /// Map arbitrary external error codes safely without panicking on unknown values.
    pub fn from(code: u16) Err {
        return switch (code) {
            0x0000 => .ok,
            0x0101 => .fail,
            0x0102 => .no_mem,
            0x0103 => .invalid_arg,
            0x0104 => .invalid_state,
            0x0105 => .invalid_size,
            0x0106 => .not_found,
            0x0107 => .not_supported,
            0x0204 => .hw_error,
            0x0405 => .crc_mismatch,
            0x0501 => .validation_failed,
            0x0502 => .checksum_mismatch,
            0x0504 => .null_ptr,
            else => .fail,
        };
    }

    pub fn isOk(self: Err) bool {
        return self == .ok;
    }
};
