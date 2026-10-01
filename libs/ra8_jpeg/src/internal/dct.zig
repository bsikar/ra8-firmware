//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Forward 8x8 DCT for the baseline encoder.
//!
//! Shares the Q14 cosine basis with the decoder's inverse transform, so the
//! pair is numerically symmetric. The C carried these two tables as
//! `static const` inside `ra8_jpeg_sw_internal.h`, meaning every translation
//! unit already held a private copy; nothing is shared away by owning them
//! here.

const spec = @import("spec");

/// `cos_q14[k][n]` = `cos((2n+1) * k * pi / 16)` in Q14.
pub const cos_q14: [spec.Block.dim][spec.Block.dim]i32 = .{
    .{
        16384,
        16384,
        16384,
        16384,
        16384,
        16384,
        16384,
        16384,
    },
    .{
        16069,
        13623,
        9102,
        3196,
        -3196,
        -9102,
        -13623,
        -16069,
    },
    .{
        15137,
        6270,
        -6270,
        -15137,
        -15137,
        -6270,
        6270,
        15137,
    },
    .{
        13623,
        -3196,
        -16069,
        -9102,
        9102,
        16069,
        3196,
        -13623,
    },
    .{
        11585,
        -11585,
        -11585,
        11585,
        11585,
        -11585,
        -11585,
        11585,
    },
    .{
        9102,
        -16069,
        3196,
        13623,
        -13623,
        -3196,
        16069,
        -9102,
    },
    .{
        6270,
        -15137,
        15137,
        -6270,
        -6270,
        15137,
        -15137,
        6270,
    },
    .{
        3196,
        -9102,
        13623,
        -16069,
        16069,
        -13623,
        9102,
        -3196,
    },
};

/// Per-frequency normalisation weight in Q14: `sqrt(2/N) * C(k)`.
pub const weight_q14: [spec.Block.dim]i32 = .{
    5793, 8192, 8192, 8192, 8192, 8192, 8192, 8192,
};

/// Q14 basis times Q14 weight lands in Q28.
const result_shift: u6 = 28;
/// Round-to-nearest bias for that shift.
const bias_shift: u6 = 27;

/// One normalised 1-D forward DCT pass over eight samples.
fn pass(in: *const [spec.Block.dim]i32, out: *[spec.Block.dim]i32) void {
    for (out, 0..) |*slot, k| {
        var acc: i64 = 0;
        for (in, cos_q14[k]) |sample, basis| {
            acc += @as(i64, sample) * @as(i64, basis);
        }
        const scaled = (acc * weight_q14[k]) + (@as(i64, 1) << bias_shift);
        slot.* = @truncate(scaled >> result_shift);
    }
}

/// Separable 2-D forward DCT of one block, in place: rows, then columns.
pub fn forward(block: *[spec.Block.size]i32) void {
    var tmp: [spec.Block.size]i32 = undefined;
    var line: [spec.Block.dim]i32 = undefined;
    var out: [spec.Block.dim]i32 = undefined;

    for (0..spec.Block.dim) |r| {
        const row = block[r * spec.Block.dim ..][0..spec.Block.dim];
        pass(row, &out);
        @memcpy(tmp[r * spec.Block.dim ..][0..spec.Block.dim], &out);
    }
    for (0..spec.Block.dim) |c| {
        for (&line, 0..) |*slot, r| slot.* = tmp[(r * spec.Block.dim) + c];
        pass(&line, &out);
        for (out, 0..) |v, r| block[(r * spec.Block.dim) + c] = v;
    }
}
