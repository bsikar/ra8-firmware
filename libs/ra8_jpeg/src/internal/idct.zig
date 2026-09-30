//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Inverse DCT for the baseline decoder (#2799).
//!
//! Shares the Q14 cosine and weight tables with the forward transform in
//! `dct.zig`: same constants, opposite direction.

const spec = @import("spec");
const dct = @import("dct");

/// Fixed-point shape of the two-pass inverse transform.
const Scale = struct {
    /// Both the table scale and the per-pass rounding shift.
    pub const q14: u5 = 14;
    /// Rounding bias applied before the output shift.
    pub const bias_shift: u6 = 13;
};

/// One 8-point inverse transform, normalised, reading `in` and writing `out`.
fn inverse1d(in: *const [spec.Block.dim]i32, out: *[spec.Block.dim]i32) void {
    for (0..spec.Block.dim) |n| {
        var sum: i64 = 0;
        for (0..spec.Block.dim) |k| {
            const weighted = @as(i64, in[k]) * @as(i64, dct.weight_q14[k]);
            sum += (weighted * @as(i64, dct.cos_q14[k][n])) >> Scale.q14;
        }
        const rounded = (sum + (@as(i64, 1) << Scale.bias_shift)) >> Scale.q14;
        out[n] = @intCast(rounded);
    }
}

/// Invert one block in place: rows first, then columns.
pub fn inverse(block: *[spec.Block.size]i32) void {
    const dim = spec.Block.dim;
    var scratch: [spec.Block.size]i32 = undefined;
    var line: [dim]i32 = undefined;
    var out: [dim]i32 = undefined;

    for (0..dim) |r| {
        for (0..dim) |c| line[c] = block[(r * dim) + c];
        inverse1d(&line, &out);
        for (0..dim) |c| scratch[(r * dim) + c] = out[c];
    }

    for (0..dim) |c| {
        for (0..dim) |r| line[r] = scratch[(r * dim) + c];
        inverse1d(&line, &out);
        for (0..dim) |r| block[(r * dim) + c] = out[r];
    }
}
