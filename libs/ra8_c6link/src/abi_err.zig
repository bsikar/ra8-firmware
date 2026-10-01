//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The subset of `ra8_err_t` the `ra8_c6link` exports return, shared by the
//! RA8 ABI and the C6 service ABI so the codes have one definition.

const storage_ram = @import("internal/storage_ram.zig");

pub const ok: u16 = 0;
pub const no_mem: u16 = 0x102;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const invalid_size: u16 = 0x105;
pub const busy: u16 = 0x109;
pub const not_initialized: u16 = 0x10F;
pub const null_ptr: u16 = 0x504;
pub const protocol_error: u16 = 0x406;

/// Flatten one refused storage transition.
pub fn of(e: storage_ram.Error) u16 {
    return switch (e) {
        error.InvalidArg => invalid_arg,
        error.InvalidState => invalid_state,
        error.NoMem => no_mem,
    };
}
