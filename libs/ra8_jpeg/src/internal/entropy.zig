//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Per-block entropy coding for the baseline encoder (#2795): forward DCT,
//! quantisation in zig-zag order, the DC difference and the AC run-length
//! pass of T.81 sec F.1.2.

const spec = @import("spec");
const dct = @import("dct");
const quant = @import("quant");
const huffman = @import("huffman");
const sink_mod = @import("sink");

const Sink = sink_mod.Sink;

/// T.81 Table F.1 SSSS category: the bit length of `|value|`, zero for zero.
pub fn magnitudeBits(value: i32) u8 {
    const magnitude: u32 = @abs(value);
    return @intCast(@bitSizeOf(u32) - @clz(magnitude));
}

/// The bit pattern T.81 sec F.1.2.1 appends after a category symbol:
/// `value` for a positive coefficient, `value - 1` for a negative one, kept
/// to its low `width` bits.
fn magnitudePattern(value: i32, width: u8) u32 {
    const biased: i32 = if (value < 0) value - 1 else value;
    return @as(u32, @bitCast(biased)) & sink_mod.mask(@intCast(width));
}

/// Emit the 63 AC coefficients of one zig-zag-ordered block.
fn emitAc(sink: *Sink, block: *const [spec.Block.size]i32, table: *const huffman.Table) void {
    var run: u8 = 0;
    for (block[1..]) |coefficient| {
        if (coefficient == 0) {
            run += 1;
            continue;
        }
        while (run >= spec.Huff.zrl_run) {
            sink.bits(table.codes[spec.Huff.zrl], table.sizes[spec.Huff.zrl]);
            run -= spec.Huff.zrl_run;
        }
        const width = magnitudeBits(coefficient);
        if (width > spec.Huff.max_magnitude) {
            // Unreachable for 8-bit input: a quantised AC coefficient cannot
            // need 16 bits. Fail closed rather than fold the overflow back
            // into the RRRR nibble and emit a wrong symbol.
            sink.overflow = true;
            return;
        }
        const symbol: u8 = (run << 4) | width;
        sink.bits(table.codes[symbol], table.sizes[symbol]);
        sink.bits(magnitudePattern(coefficient, width), width);
        run = 0;
    }
    if (run > 0) {
        sink.bits(table.codes[spec.Huff.eob], table.sizes[spec.Huff.eob]);
    }
}

/// Encode one 8x8 block of level-shifted samples into the entropy stream and
/// advance this component's DC predictor.
pub fn emitBlock(
    sink: *Sink,
    samples: *const [spec.Block.size]i32,
    table: *const [spec.Block.size]u8,
    predictor: *i32,
    dc: *const huffman.Table,
    ac: *const huffman.Table,
) void {
    var block = samples.*;
    dct.forward(&block);

    var scan: [spec.Block.size]i32 = undefined;
    for (&scan, spec.zigzag) |*slot, raster| {
        slot.* = quant.apply(block[raster], table[raster]);
    }

    const difference = scan[0] - predictor.*;
    predictor.* = scan[0];
    const width = magnitudeBits(difference);
    sink.bits(dc.codes[width], dc.sizes[width]);
    if (width != 0) {
        sink.bits(magnitudePattern(difference, width), width);
    }

    emitAc(sink, &scan, ac);
}
