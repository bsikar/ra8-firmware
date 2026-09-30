//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Whole-buffer driver for the baseline decoder (#2799).
//!
//! Decodes a complete JPEG held in memory into one RGB888 buffer the caller
//! sized up front. The striped driver in `stream.zig` solves the same problem
//! under a memory bound; this one trades that for a single pass and no
//! callbacks.

const bitreader = @import("bitreader");
const dec_ctx = @import("dec_ctx");
const dispatch = @import("dispatch");
const mcu = @import("mcu");
const spec = @import("spec");
const ycc = @import("ycc");

const Ctx = dec_ctx.Ctx;
const Error = dec_ctx.Error;

/// An image's pixel dimensions, reported alongside the decode.
pub const Dimensions = struct {
    width: u16,
    height: u16,
};

/// Smallest stream worth looking at: SOI plus one marker.
pub const min_stream_len: usize = 4;

/// Write one decoded MCU into the frame buffer as RGB888, stopping at the
/// image edge so a partial MCU does not write outside it.
fn emitMcu(
    d: *const Ctx,
    tiles: *const mcu.Tiles,
    mx: u16,
    my: u16,
    mcu_w: u16,
    mcu_h: u16,
    out: []u8,
) void {
    for (0..mcu_h) |r| {
        const py = (@as(usize, my) * mcu_h) + r;
        if (py >= d.height) break;

        for (0..mcu_w) |c| {
            const px = (@as(usize, mx) * mcu_w) + c;
            if (px >= d.width) break;

            const y: i32 = tiles.luma[(r * mcu_w) + c];
            var cb: i32 = spec.Block.level_offset;
            var cr: i32 = spec.Block.level_offset;

            if (d.ncomp == 3) {
                // Chroma is stored at the subsampled resolution, so the luma
                // position divides down into it.
                const cx = c / d.hmax;
                const cy = r / d.vmax;
                cb = tiles.cb[(cy * spec.Block.dim) + cx];
                cr = tiles.cr[(cy * spec.Block.dim) + cx];
            }

            const rgb = ycc.toRgb(y, cb, cr);
            const idx = ((py * @as(usize, d.width)) + px) * spec.Limits.rgb_channels;
            out[idx] = rgb[0];
            out[idx + 1] = rgb[1];
            out[idx + 2] = rgb[2];
        }
    }
}

/// Run the entropy-coded scan the cursor is sitting on, MCU by MCU in raster
/// order, into `out`.
fn decodeScan(d: *Ctx, out: []u8) Error!void {
    const needed = @as(usize, d.width) * @as(usize, d.height) * spec.Limits.rgb_channels;
    if (out.len < needed) return Error.InvalidSize;

    // SOF0 acceptance guarantees both are at least 1, so the MCU geometry
    // below cannot divide by zero.
    if (d.hmax == 0 or d.vmax == 0) return Error.Protocol;

    var br = bitreader.BitReader{
        .buf = d.src.ptr,
        .len = @intCast(d.src.len),
        .pos = @intCast(d.cursor),
        .acc = 0,
        .nbits = 0,
        .had_eoi = 0,
    };
    for (&d.comps) |*comp| comp.dc_pred = 0;

    const mcu_w: u16 = spec.Block.dim * d.hmax;
    const mcu_h: u16 = spec.Block.dim * d.vmax;
    const mcus_x = (d.width + mcu_w - 1) / mcu_w;
    const mcus_y = (d.height + mcu_h - 1) / mcu_h;

    var tiles = mcu.Tiles{};

    var my: u16 = 0;
    while (my < mcus_y) : (my += 1) {
        var mx: u16 = 0;
        while (mx < mcus_x) : (mx += 1) {
            try mcu.decode(d, &br, &tiles, mcu_w);
            emitMcu(d, &tiles, mx, my, mcu_w, mcu_h, out);
        }
    }

    d.cursor = br.pos;
}

/// Walk markers until a scan starts, then decode it. A stream that ends or
/// hits EOI before any scan carried no image.
fn run(d: *Ctx, out: []u8) Error!Dimensions {
    var got_sof = false;

    while (d.cursor < d.src.len) {
        var action = dispatch.Action.cont;
        try dispatch.step(d, &got_sof, &action);

        switch (action) {
            .scan => {
                const found = Dimensions{ .width = d.width, .height = d.height };
                try decodeScan(d, out);
                return found;
            },
            .eoi => break,
            .cont => {},
        }
    }
    return Error.Protocol;
}

/// Decode a baseline JPEG into RGB888. `out` must hold width * height * 3
/// bytes, which the caller learns from `dims.probe` when it does not already
/// know the geometry.
pub fn decode(d: *Ctx, stream: []const u8, out: []u8) Error!Dimensions {
    if (stream.len < min_stream_len) return Error.InvalidSize;

    d.reset();
    d.src = stream;

    if (((@as(u16, stream[0]) << 8) | stream[1]) != spec.Marker.soi) return Error.Protocol;
    d.cursor = 2;

    return run(d, out);
}
