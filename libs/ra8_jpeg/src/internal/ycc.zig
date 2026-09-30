//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Y'CbCr to RGB888 for the baseline decoder (#2799).
//!
//! The inverse of `color.zig`, and a separate set of constants: the forward
//! transform scales by 65536 into three sums, this one scales the two chroma
//! differences.

const spec = @import("spec");

/// BT.601 inverse coefficients, scaled by 2^16.
pub const Coefficient = struct {
    pub const cr_r: i32 = 91881; //  1.40200
    pub const cb_b: i32 = 116130; // 1.77200
    pub const cr_g: i32 = -46802;
    pub const cb_g: i32 = -22554;

    pub const shift: u5 = 16;
};

/// Clamp a computed sample into the 8-bit output range.
pub fn clamp(value: i32) u8 {
    if (value < 0) return 0;
    if (value > spec.Block.sample_max) return @intCast(spec.Block.sample_max);
    return @intCast(value);
}

/// One pixel: level-shift the chroma pair, then three clamped sums.
pub fn toRgb(y: i32, cb_in: i32, cr_in: i32) [spec.Limits.rgb_channels]u8 {
    const cb = cb_in - spec.Block.level_offset;
    const cr = cr_in - spec.Block.level_offset;

    const r = y + ((Coefficient.cr_r * cr) >> Coefficient.shift);
    const g = y + (((Coefficient.cb_g * cb) + (Coefficient.cr_g * cr)) >> Coefficient.shift);
    const b = y + ((Coefficient.cb_b * cb) >> Coefficient.shift);

    return .{ clamp(r), clamp(g), clamp(b) };
}
