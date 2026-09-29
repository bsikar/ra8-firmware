//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Blue-noise ordered dither for the e-ink panel (#477): the toroidal mask
//! lookup, the quantise rules (flat and per-panel tone curve alike), the gray4
//! nibble packer and the level-to-color expansion. Pure integer arithmetic
//! over caller memory, so a tile quantises identically on host, emulator and
//! silicon.

const std = @import("std");
const tone_impl = @import("tone.zig");

/// The library core, re-exported so a consumer of this module reaches the
/// same error codes without importing the file twice.
pub const core = @import("root.zig");

/// The tone curve, re-exported for the same reason.
pub const tone = tone_impl;

/// The committed blue-noise threshold texture.
pub const mask = @import("dither_mask.zig").mask;

/// `ra8_gfx_dither_const_t` and `ra8_gfx_dither_scale_t`.
pub const dither = struct {
    pub const levels: u8 = 16;
    pub const step: u8 = 17;
    pub const max_level: u8 = 15;
    pub const nib_shift: u3 = 4;
    pub const ppb: u8 = 2;
    pub const mask_dim: u32 = 64;
    pub const mask_index_mask: u32 = 63;
    pub const rgb_g_shift: u5 = 8;
    pub const rgb_r_shift: u5 = 16;
    pub const byte_levels: u16 = 256;
    pub const mask_len: u16 = 4096;
};

comptime {
    std.debug.assert(mask.len == dither.mask_len);
    std.debug.assert(dither.mask_index_mask == dither.mask_dim - 1);
    std.debug.assert(dither.max_level == dither.levels - 1);
}

/// The toroidal mask cell for an absolute panel coordinate. The edge is a
/// power of two, so the wrap is a bitmask and abutting tiles share one phase.
pub fn maskIndex(x: i32, y: i32) u32 {
    const mx = @as(u32, @bitCast(x)) & dither.mask_index_mask;
    const my = @as(u32, @bitCast(y)) & dither.mask_index_mask;
    return (my * dither.mask_dim) + mx;
}

/// The threshold this pixel dithers against.
pub fn thresholdAt(x: i32, y: i32) u8 {
    return mask[maskIndex(x, y)];
}

/// The flat quantise rule: the even 17-per-level palette, rounded up when the
/// pixel's threshold falls inside the remainder. The test is the exact integer
/// form `thr * step < rem * 256`, so it never clamps and never biases.
pub fn quantise(gray8: u8, thr: u8) u8 {
    const base = gray8 / dither.step;
    const rem = gray8 - (base * dither.step);
    const rounds_up = (@as(u32, thr) * dither.step) < (@as(u32, rem) * dither.byte_levels);
    return if (rounds_up) base + 1 else base;
}

/// The same rule against a prepared per-panel curve (#479), falling back to
/// the flat palette when the caller has no calibration data.
pub fn quantiseAny(map: ?*const tone_impl.Map, gray8: u8, thr: u8) u8 {
    const curve = map orelse return quantise(gray8, thr);
    return tone_impl.quantise(curve, gray8, thr);
}

/// A 4-bit level as the `(n << 4) | n` gray the rest of the reader uses,
/// broadcast into all three channels.
pub fn levelToColor(level: u8) u32 {
    const gray: u32 = (@as(u32, level) << dither.nib_shift) | @as(u32, level);
    return (gray << dither.rgb_r_shift) | (gray << dither.rgb_g_shift) | gray;
}

/// The packed-gray4 byte count a `w * h` tile needs: two pixels per byte, the
/// odd pixel rounding up into a byte of its own.
pub fn packedBytes(w: u32, h: u32) u32 {
    return ((w * h) + 1) / dither.ppb;
}

/// Quantise a gray8 tile into packed gray4, high nibble first. `origin` is the
/// tile's absolute panel position, which is what keeps the mask phase
/// continuous across tiles.
pub fn packTile(
    map: ?*const tone_impl.Map,
    src: []const u8,
    w: i32,
    h: i32,
    origin_x: i32,
    origin_y: i32,
    out: []u8,
) void {
    var row: i32 = 0;
    while (row < h) : (row += 1) {
        var col: i32 = 0;
        while (col < w) : (col += 1) {
            const i = (@as(u32, @bitCast(row)) * @as(u32, @bitCast(w))) + @as(u32, @bitCast(col));
            const level = quantiseAny(map, src[i], thresholdAt(origin_x +% col, origin_y +% row));
            const byte_index = i / dither.ppb;
            if ((i & 1) == 0) {
                out[byte_index] = level << dither.nib_shift;
            } else {
                out[byte_index] |= level;
            }
        }
    }
}
