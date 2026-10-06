//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Guard shared by the I3C I2C-compatibility delegators (RA8FW-824, was part
//! of ra8_i3c.c). The exports live in src/i3c_compat_abi.zig.

pub const mode_i2c: u8 = 1;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;

/// `ra8_i3c_chan_state_t`.
pub const Chan = extern struct { initialized: bool, mode: u8 };

/// Channel range then I2C mode, the C order; null when the call may forward.
/// `initialized` is not checked, as in the C.
pub fn guard(chans: []const Chan, channel: u8) ?u16 {
    if (channel >= chans.len) return invalid_arg;
    if (chans[channel].mode != mode_i2c) return invalid_state;
    return null;
}
