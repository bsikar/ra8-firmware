//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_err_t` values this library returns, and nothing else.
//!
//! Mirrors `libs/ra8_core/inc/ra8_err.h`. The C header is the source of truth
//! for the numbering; these are the subset `ra8_secure_app` actually hands
//! back, kept as a `u16` enum so the values cross the ABI unchanged.

/// The `ra8_err_t` subset this library returns.
pub const Err = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    invalid_size = 0x105,
    not_found = 0x106,
    not_supported = 0x107,
    no_data = 0x10A,
    null_ptr = 0x504,

    /// The wire value, for an `export fn` that has to hand back a bare `u16`.
    pub fn code(self: Err) u16 {
        return @backingInt(self);
    }
};
