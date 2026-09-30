//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Quantisation for the baseline encoder (#2795): the T.81 Annex K.1 base
//! tables, the IJG quality curve that scales them, and the per-coefficient
//! divide.

const std = @import("std");
const spec = @import("spec");

/// T.81 Annex K.1 luminance quantisation table, raster order.
pub const base_luma: [spec.Block.size]u8 = .{
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};

/// T.81 Annex K.1 chrominance quantisation table, raster order.
pub const base_chroma: [spec.Block.size]u8 = .{
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

/// The IJG quality curve pivots here: below it the scale is a reciprocal,
/// at or above it a straight line down to zero.
const pivot: u16 = 50;
const reciprocal_numerator: u32 = 5000;
const linear_intercept: u16 = 200;
const round_bias: u32 = 50;
const percent: u32 = 100;

/// A DQT entry is one byte and may not be zero, T.81 sec B.2.4.1.
const entry_min: u32 = 1;

/// Map a 1..100 quality onto the IJG percentage scale.
pub fn qualityScale(quality: u8) u16 {
    if (quality < pivot) {
        return @intCast(reciprocal_numerator / @as(u32, quality));
    }
    return linear_intercept - (2 * @as(u16, quality));
}

/// Scale a base table by `scale/100`, clamped into the legal 1..255 range.
pub fn scaleTable(base: *const [spec.Block.size]u8, out: *[spec.Block.size]u8, scale: u16) void {
    for (base, out) |entry, *slot| {
        const scaled = ((@as(u32, entry) * @as(u32, scale)) + round_bias) / percent;
        slot.* = @intCast(std.math.clamp(scaled, entry_min, spec.Block.sample_max));
    }
}

/// Divide one DCT coefficient by its table entry, rounding half away from
/// zero. Widened to 64-bit so negating the most-negative coefficient cannot
/// overflow; the result is unchanged for every value a real block produces.
pub fn apply(coefficient: i32, divisor: u8) i32 {
    const q: i64 = if (divisor == 0) 1 else divisor;
    const half = @divTrunc(q, 2);
    if (coefficient < 0) {
        const magnitude = -@as(i64, coefficient) + half;
        return @intCast(-@divTrunc(magnitude, q));
    }
    return @intCast(@divTrunc(@as(i64, coefficient) + half, q));
}
