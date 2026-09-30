//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The vocabulary of the Zig side of `ra8_mem`: the error codes `ra8_err.h`
//! publishes and the limits `ra8_slab.h` publishes. Named once here so the
//! membrane and the implementation cannot drift apart from the headers.

const std = @import("std");

/// `ra8_err_t` as `ra8_err.h` spells it: `enum : uint16_t`.
pub const Err = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    invalid_size = 0x105,
    null_ptr = 0x504,

    /// The value the C membrane returns.
    pub fn code(self: Err) u16 {
        return @intFromEnum(self);
    }
};

/// `ra8_slab_const_t`.
pub const SlabLimits = struct {
    /// `k_ra8_slab_nil`: freelist terminator.
    pub const nil: u32 = 0xFFFF_FFFF;
    /// `k_ra8_slab_min_cell_bytes`: a cell must hold one index.
    pub const min_cell_bytes: u32 = 4;
    /// `k_ra8_slab_align_bytes`: required cell-size and buffer alignment.
    pub const align_bytes: u32 = 4;
};

comptime {
    std.debug.assert(@sizeOf(Err) == 2);
    std.debug.assert(SlabLimits.min_cell_bytes == @sizeOf(u32));
}
