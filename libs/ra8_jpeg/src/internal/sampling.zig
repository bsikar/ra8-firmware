//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Block sampling for the baseline encoder: pulling one 8x8 block out
//! of a plane, with clamp-to-edge at the padded borders and 2x2 averaging for
//! the 4:2:0 chroma planes. Both kernels level-shift by -128 on the way out,
//! which is what the forward DCT expects.

const spec = @import("spec");

/// Clamp a coordinate to the last valid index of a `size`-long axis.
fn clampAxis(coordinate: usize, size: usize) usize {
    return if (coordinate >= size) size - 1 else coordinate;
}

/// Copy the 8x8 luma window at (`x0`, `y0`), level-shifted.
pub fn luma(
    plane: []const i32,
    plane_w: usize,
    plane_h: usize,
    x0: usize,
    y0: usize,
    out: *[spec.Block.size]i32,
) void {
    for (0..spec.Block.dim) |r| {
        const sy = clampAxis(y0 + r, plane_h);
        for (0..spec.Block.dim) |c| {
            const sx = clampAxis(x0 + c, plane_w);
            out[(r * spec.Block.dim) + c] =
                plane[(sy * plane_w) + sx] - spec.Block.level_offset;
        }
    }
}

/// Mean of the 2x2 window at (`sx`, `sy`), each coordinate clamped to the
/// plane. The window never shrinks, so the divisor is always four.
fn average2x2(plane: []const i32, plane_w: usize, plane_h: usize, sx: usize, sy: usize) i32 {
    var sum: i32 = 0;
    for (0..2) |dy| {
        for (0..2) |dx| {
            const yy = clampAxis(sy + dy, plane_h);
            const xx = clampAxis(sx + dx, plane_w);
            sum += plane[(yy * plane_w) + xx];
        }
    }
    return @divTrunc(sum, 4);
}

/// Build one 8x8 chroma block by 2x2 averaging the 16x16 region at
/// (`x0`, `y0`), level-shifted. This is the 4:2:0 sub-sampling step.
pub fn chroma420(
    plane: []const i32,
    plane_w: usize,
    plane_h: usize,
    x0: usize,
    y0: usize,
    out: *[spec.Block.size]i32,
) void {
    for (0..spec.Block.dim) |r| {
        for (0..spec.Block.dim) |c| {
            out[(r * spec.Block.dim) + c] =
                average2x2(plane, plane_w, plane_h, x0 + (c * 2), y0 + (r * 2)) -
                spec.Block.level_offset;
        }
    }
}
