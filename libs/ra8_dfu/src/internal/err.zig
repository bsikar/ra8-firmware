//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_err_t` subset the DFU host driver speaks. `ra8_err_t` is a C23
//! `enum : uint16_t`, so the tag type here is the ABI type and no conversion
//! is needed at the membrane.

/// Every code this driver can return, and nothing it cannot.
pub const Err = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    invalid_size = 0x105,
    hw_timeout = 0x203,
    hw_error = 0x204,
    null_ptr = 0x504,

    /// Widen an opaque `ra8_err_t` from the HAL. Codes outside the set above
    /// pass through untouched: a HAL failure is the caller's to read, not
    /// this driver's to reinterpret.
    pub fn from(code: u16) Err {
        return @fromBackingInt(@intCast(code));
    }

    pub fn raw(self: Err) u16 {
        return @backingInt(self);
    }

    pub fn isOk(self: Err) bool {
        return self == .ok;
    }
};
