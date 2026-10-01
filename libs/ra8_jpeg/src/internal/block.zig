//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One-block entropy decode for the baseline decoder.
//!
//! Turns the entropy stream into 64 dequantised coefficients, then hands them
//! to the inverse transform. The DC predictor lives on the component, so a
//! block only ever advances the state of its own component.

const bitreader = @import("bitreader");
const dec_ctx = @import("dec_ctx");
const huffdec = @import("huffdec");
const idct = @import("idct");
const spec = @import("spec");
const ycc = @import("ycc");

const Ctx = dec_ctx.Ctx;
const Error = dec_ctx.Error;
const Nibble = dec_ctx.Nibble;

/// One 8x8 block of dequantised coefficients in natural order.
pub const Coefficients = [spec.Block.size]i32;
/// One 8x8 block of spatial samples.
pub const Samples = [spec.Block.size]u8;

/// Decode the AC band, run-length coded as (zero run, magnitude) pairs.
///
/// Two symbols end a run early: ZRL skips sixteen zeros without emitting a
/// coefficient, and EOB means every remaining coefficient is zero.
fn decodeAc(
    d: *Ctx,
    br: *bitreader.BitReader,
    ci: usize,
    out: *Coefficients,
) Error!void {
    const table = d.acTable(ci);
    const quant = &d.quant[d.comps[ci].quant_id];

    var k: u8 = 1;
    while (k < spec.Block.size) {
        const symbol = table.decode(br) orelse return Error.Protocol;
        const run = symbol >> Nibble.shift;
        const magnitude = symbol & Nibble.mask;

        if (magnitude == 0) {
            // ZRL carries a run of sixteen with no coefficient of its own;
            // anything else with a zero magnitude is end of block.
            if (run == Nibble.mask) {
                k += spec.Huff.zrl_run;
                continue;
            }
            return;
        }

        k += run;
        if (k >= spec.Block.size) return Error.Protocol;

        const raw = br.getBits(magnitude) orelse return Error.Protocol;
        const value = huffdec.extend(@intCast(raw), magnitude);

        const index = spec.zigzag[k];
        out[index] = value * @as(i32, quant[index]);
        k += 1;
    }
}

/// Decode one block: the DC difference against the component's predictor,
/// then the AC band. Coefficients come out dequantised.
pub fn decode(
    d: *Ctx,
    br: *bitreader.BitReader,
    ci: usize,
    out: *Coefficients,
) Error!void {
    @memset(out, 0);

    const magnitude = d.dcTable(ci).decode(br) orelse return Error.Protocol;
    // A zero magnitude requests no bits, so this cannot fail for it.
    const raw = br.getBits(magnitude) orelse return Error.Protocol;
    const diff = huffdec.extend(@intCast(raw), magnitude);

    const comp = &d.comps[ci];
    comp.dc_pred += diff;
    out[0] = comp.dc_pred * @as(i32, d.quant[comp.quant_id][0]);

    return decodeAc(d, br, ci, out);
}

/// Invert one block and level-shift it back into unsigned samples.
pub fn toSamples(coeffs: *Coefficients, out: *Samples) void {
    idct.inverse(coeffs);
    for (coeffs, out) |coefficient, *sample| {
        sample.* = ycc.clamp(coefficient + spec.Block.level_offset);
    }
}
