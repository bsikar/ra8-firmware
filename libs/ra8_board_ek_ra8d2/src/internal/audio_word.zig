//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which SSIE data-word / system-word pair carries a given PCM bit depth.
//!
//! The DA7212 is native at 16/24/32; 8/18/20/22 are accepted because the
//! SSIE supports those widths and an application may feed pre-padded
//! streams.

const vocab = @import("vocab.zig");

const Err = vocab.Err;

/// SSIE DWL codes. HUM SSICR.DWL.
pub const Dwl = struct {
    pub const bits_8: u32 = 0;
    pub const bits_16: u32 = 1;
    pub const bits_18: u32 = 2;
    pub const bits_20: u32 = 3;
    pub const bits_22: u32 = 4;
    pub const bits_24: u32 = 5;
    pub const bits_32: u32 = 6;
};

/// SSIE SWL codes. HUM SSICR.SWL.
pub const Swl = struct {
    pub const bits_8: u32 = 0;
    pub const bits_16: u32 = 1;
    pub const bits_24: u32 = 2;
    pub const bits_32: u32 = 3;
};

pub const Words = struct {
    data: u32,
    system: u32,
};

/// The data word is the significant bits; the system word is the slot they
/// sit in, which is why 18, 20 and 22 all ride in a 24-bit slot.
pub fn forBits(bit_depth: u8) ?Words {
    return switch (bit_depth) {
        8 => .{ .data = Dwl.bits_8, .system = Swl.bits_8 },
        16 => .{ .data = Dwl.bits_16, .system = Swl.bits_16 },
        18 => .{ .data = Dwl.bits_18, .system = Swl.bits_24 },
        20 => .{ .data = Dwl.bits_20, .system = Swl.bits_24 },
        22 => .{ .data = Dwl.bits_22, .system = Swl.bits_24 },
        24 => .{ .data = Dwl.bits_24, .system = Swl.bits_24 },
        32 => .{ .data = Dwl.bits_32, .system = Swl.bits_32 },
        else => null,
    };
}

/// The same mapping through the C-style out-parameter shape the callers use.
pub fn resolve(bit_depth: u8, out: *Words) u32 {
    out.* = forBits(bit_depth) orelse return Err.invalid_arg;
    return Err.ok;
}
