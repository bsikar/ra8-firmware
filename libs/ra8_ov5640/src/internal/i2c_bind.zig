//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Wire framing for the house-I2C binder: how a 16-bit SCCB register pointer
//! is laid out on the wire, and how a one-register write is staged.
//!
//! Nothing here touches a bus. The binder in `../ra8_ov5640_bind_abi.zig`
//! owns the seam calls; this file owns only the bytes, so the framing is
//! testable without a mock.

const std = @import("std");

/// Wire sizes the binder stages on the stack, matching
/// `k_ra8_ov5640_i2c_reg_bytes` and `k_ra8_ov5640_i2c_frame_bytes`.
pub const wire = struct {
    /// SCCB register pointer width.
    pub const reg_bytes: usize = 2;
    /// Register pointer plus one value byte.
    pub const frame_bytes: usize = 3;
};

/// A register pointer, big-endian, as SCCB sends it: high byte then low.
pub fn packReg(register: u16) [wire.reg_bytes]u8 {
    var bytes: [wire.reg_bytes]u8 = undefined;
    std.mem.writeInt(u16, &bytes, register, .big);
    return bytes;
}

/// One write frame: `[reg_hi][reg_lo][value]`, sent as a single write.
pub fn packWrite(register: u16, value: u8) [wire.frame_bytes]u8 {
    const pointer = packReg(register);
    return .{ pointer[0], pointer[1], value };
}
