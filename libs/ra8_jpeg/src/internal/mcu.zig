//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Minimum coded unit assembly for the baseline decoder.
//!
//! An MCU is one luma tile plus, in colour, one block of each chroma plane.
//! This is where the interleaved block order in the stream becomes a
//! rectangular tile the emitters can index by pixel.

const bitreader = @import("bitreader");
const block = @import("block");
const dec_ctx = @import("dec_ctx");
const spec = @import("spec");

const Ctx = dec_ctx.Ctx;
const Error = dec_ctx.Error;
const Limit = dec_ctx.Limit;

/// Largest luma tile this decoder assembles, a 4:2:0 MCU at 16x16.
pub const max_luma_tile: usize = Limit.mcu_max_dim * Limit.mcu_max_dim;

/// Scratch an MCU decode needs, owned by the driver so the decode itself
/// allocates nothing.
pub const Tiles = struct {
    luma: [max_luma_tile]u8 = .{0} ** max_luma_tile,
    cb: [spec.Block.size]u8 = .{0} ** spec.Block.size,
    cr: [spec.Block.size]u8 = .{0} ** spec.Block.size,
};

/// Place one decoded block at its position inside the luma tile.
fn placeBlock(samples: *const block.Samples, tile: []u8, bx: u8, by: u8, stride: u16) void {
    const dim = spec.Block.dim;
    for (0..dim) |r| {
        for (0..dim) |c| {
            const ty = (@as(usize, by) * dim) + r;
            const tx = (@as(usize, bx) * dim) + c;
            tile[(ty * stride) + tx] = samples[(r * dim) + c];
        }
    }
}

/// Decode the luma blocks of one MCU. A 4:2:0 MCU carries four of them in
/// raster order, 4:4:4 carries one.
pub fn luma(d: *Ctx, br: *bitreader.BitReader, tile: []u8, stride: u16) Error!void {
    var coeffs: block.Coefficients = undefined;
    var samples: block.Samples = undefined;

    var by: u8 = 0;
    while (by < d.vmax) : (by += 1) {
        var bx: u8 = 0;
        while (bx < d.hmax) : (bx += 1) {
            try block.decode(d, br, 0, &coeffs);
            block.toSamples(&coeffs, &samples);
            placeBlock(&samples, tile, bx, by, stride);
        }
    }
}

/// Decode the two chroma blocks of one MCU. Both planes are a single 8x8
/// block in the layouts this decoder accepts, so there is no tiling.
pub fn chroma(
    d: *Ctx,
    br: *bitreader.BitReader,
    cb: *block.Samples,
    cr: *block.Samples,
) Error!void {
    var coeffs: block.Coefficients = undefined;

    try block.decode(d, br, 1, &coeffs);
    block.toSamples(&coeffs, cb);

    try block.decode(d, br, 2, &coeffs);
    block.toSamples(&coeffs, cr);
}

/// Decode one whole MCU into `tiles`, chroma included when the frame has it.
pub fn decode(d: *Ctx, br: *bitreader.BitReader, tiles: *Tiles, stride: u16) Error!void {
    try luma(d, br, &tiles.luma, stride);
    if (d.ncomp == 3) try chroma(d, br, &tiles.cb, &tiles.cr);
}
