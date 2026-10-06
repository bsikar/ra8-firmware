//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Wire framing for the house-I2C binder: the staged `[reg][payload]` write
//! frame and its cap.
//!
//! Nothing here touches a bus. The binder in `../ra8_lsm6dso_bind_abi.zig`
//! owns the seam calls; this file owns only the bytes, so the framing and its
//! refusals are testable without a mock.

const std = @import("std");

/// Wire sizes the binder stages on the stack, matching
/// `k_lsm6dso_i2c_frame_bytes_max`.
pub const wire = struct {
    /// Staged frame cap: one register byte plus fifteen payload bytes.
    pub const frame_bytes_max: usize = 16;
    /// Largest payload one staged frame can carry.
    pub const payload_bytes_max: usize = frame_bytes_max - 1;
};

/// Highest valid 7-bit I2C address.
pub const addr_7b_max: u8 = 0x7F;

/// One staged write frame: the register byte followed by the payload.
pub const Frame = struct {
    bytes: [wire.frame_bytes_max]u8 = @splat(0),
    len: usize = 0,

    /// The staged bytes, ready for one framed write.
    pub fn slice(self: *const Frame) []const u8 {
        return self.bytes[0..self.len];
    }
};

/// Stage `[reg][payload]` contiguously. The driver only ever writes one
/// payload byte; the cap is here so a future multi-byte write fails loudly
/// rather than overruns, which is why the caller checks `payloadFits` first.
pub fn stageWrite(reg: u8, payload: []const u8) Frame {
    var frame = Frame{ .len = payload.len + 1 };
    frame.bytes[0] = reg;
    @memcpy(frame.bytes[1..][0..payload.len], payload);
    return frame;
}

/// Whether a payload of `len` bytes fits one staged frame beside its register.
pub fn payloadFits(len: u32) bool {
    return len <= wire.payload_bytes_max;
}
