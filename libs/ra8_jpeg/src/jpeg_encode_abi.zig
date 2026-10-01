//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the baseline JPEG encoder: `ra8_jpeg_sw_encode`
//! from `libs/ra8_jpeg/inc/ra8_jpeg_sw.h`. The decisions live in the
//! `internal/` modules; this file owns the exported symbol, the argument
//! guards, the `ra8_err_t` mapping and the module-static working set.
//!
//! The working set is static for the same reason it was in C: the project
//! budgets stack with `-Wstack-usage` and forbids the heap, so the strip
//! buffers and the 2 KiB of Huffman look-ups cannot live on the caller's
//! stack. That is what makes this entry point non-re-entrant, which is the
//! documented contract in `ra8_jpeg_sw.h`; every call zeroes its state on
//! entry, so sequential reuse leaks nothing.

const spec = @import("spec");
const quant = @import("quant");
const color = @import("color");
const sampling = @import("sampling");
const huffman = @import("huffman");
const headers = @import("headers");
const entropy = @import("entropy");
const sink_mod = @import("sink");

/// Subset of `ra8_err_t` this entry point returns.
pub const Error = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_size = 0x105,
    null_ptr = 0x504,
};

/// Component tag on the log lines, matching the C unit's `s_tag`.
const tag: [*:0]const u8 = "JPEG_SW";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Everything one encode call mutates.
const Encoder = struct {
    sink: sink_mod.Sink = .{ .dst = &.{} },
    quant_luma: [spec.Block.size]u8 = @splat(0),
    quant_chroma: [spec.Block.size]u8 = @splat(0),
    predictors: [spec.Limits.components]i32 = @splat(0),
    tables: huffman.Set = .{},
};

const strip_samples = spec.Mcu.dim * spec.Limits.max_width;

var state: Encoder = .{};
var strip_luma: [strip_samples]i32 = @splat(0);
var strip_cb: [strip_samples]i32 = @splat(0);
var strip_cr: [strip_samples]i32 = @splat(0);
var strip_rgb: [spec.Limits.rgb_channels * spec.Limits.max_width]u8 = @splat(0);

/// Fill the three YCbCr strips for the 16 rows starting at `top`, replicating
/// the right and bottom source edges out to the padded width.
fn convertStrip(rgb: []const u8, width: usize, height: usize, padded_w: usize, top: usize) void {
    for (0..spec.Mcu.dim) |row| {
        const source_y = @min(top + row, height - 1);

        for (0..padded_w) |column| {
            const source_x = @min(column, width - 1);
            const from = ((source_y * width) + source_x) * spec.Limits.rgb_channels;
            const to = column * spec.Limits.rgb_channels;
            strip_rgb[to] = rgb[from];
            strip_rgb[to + 1] = rgb[from + 1];
            strip_rgb[to + 2] = rgb[from + 2];
        }

        const base = row * padded_w;
        color.rowToYcc(
            strip_rgb[0 .. padded_w * spec.Limits.rgb_channels],
            strip_luma[base..][0..padded_w],
            strip_cb[base..][0..padded_w],
            strip_cr[base..][0..padded_w],
        );
    }
}

/// Encode every 16x16 MCU across one converted strip: four luma blocks, then
/// one sub-sampled Cb and one Cr.
fn encodeStrip(encoder: *Encoder, padded_w: usize) void {
    const half = spec.Block.dim;
    var x: usize = 0;
    while (x < padded_w) : (x += spec.Mcu.dim) {
        var block: [spec.Block.size]i32 = undefined;

        for (0..2) |by| {
            for (0..2) |bx| {
                sampling.luma(
                    &strip_luma,
                    padded_w,
                    spec.Mcu.dim,
                    x + (bx * half),
                    by * half,
                    &block,
                );
                entropy.emitBlock(
                    &encoder.sink,
                    &block,
                    &encoder.quant_luma,
                    &encoder.predictors[0],
                    &encoder.tables.dc_luma,
                    &encoder.tables.ac_luma,
                );
            }
        }

        for ([_]struct { plane: []const i32, component: usize }{
            .{ .plane = &strip_cb, .component = 1 },
            .{ .plane = &strip_cr, .component = 2 },
        }) |chroma| {
            sampling.chroma420(chroma.plane, padded_w, spec.Mcu.dim, x, 0, &block);
            entropy.emitBlock(
                &encoder.sink,
                &block,
                &encoder.quant_chroma,
                &encoder.predictors[chroma.component],
                &encoder.tables.dc_chroma,
                &encoder.tables.ac_chroma,
            );
        }
    }
}

/// Drive a whole encode: tables, headers, the strip loop, then the closing
/// flush and EOI.
fn run(encoder: *Encoder, rgb: []const u8, width: u16, height: u16, quality: u8) Error {
    // Checked before anything is written, and on the un-narrowed value, so an
    // image wide enough to wrap the padded width is rejected rather than
    // silently encoded as an empty scan.
    const padded_w_wide = spec.Mcu.alignUp(width);
    if (padded_w_wide > spec.Limits.max_width) return .invalid_arg;
    const padded_w: usize = padded_w_wide;
    const padded_h: usize = spec.Mcu.alignUp(height);

    const scale = quant.qualityScale(quality);
    quant.scaleTable(&quant.base_luma, &encoder.quant_luma, scale);
    quant.scaleTable(&quant.base_chroma, &encoder.quant_chroma, scale);
    encoder.tables.buildAll();

    headers.emitAll(
        &encoder.sink,
        width,
        height,
        &encoder.quant_luma,
        &encoder.quant_chroma,
        &encoder.tables,
    );

    var top: usize = 0;
    while (top < padded_h) : (top += spec.Mcu.dim) {
        convertStrip(rgb, width, height, padded_w, top);
        encodeStrip(encoder, padded_w);
    }

    encoder.sink.flushBits();
    encoder.sink.word(spec.Marker.eoi);

    if (encoder.sink.overflow) return .invalid_size;
    return .ok;
}

/// Encode a packed RGB888 image as a baseline 4:2:0 JFIF stream.
///
/// Mirrors `ra8_jpeg_sw_encode()` in `inc/ra8_jpeg_sw.h`. Not re-entrant: see
/// the concurrency contract in that header.
pub export fn ra8_jpeg_sw_encode(
    rgb_buf: ?[*]const u8,
    width: u16,
    height: u16,
    quality: u8,
    out_buf: ?[*]u8,
    out_buf_len: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const source = rgb_buf orelse {
        ra8_log_emit_error(tag, "rgb_buf is NULL");
        return @intFromEnum(Error.null_ptr);
    };
    const destination = out_buf orelse {
        ra8_log_emit_error(tag, "out_buf is NULL");
        return @intFromEnum(Error.null_ptr);
    };
    const written = out_len orelse {
        ra8_log_emit_error(tag, "out_len is NULL");
        return @intFromEnum(Error.null_ptr);
    };

    written.* = 0;
    if (width == 0 or height == 0) return @intFromEnum(Error.invalid_arg);
    if (quality < spec.Limits.quality_min or quality > spec.Limits.quality_max) {
        return @intFromEnum(Error.invalid_arg);
    }

    state = .{};
    state.sink = .{ .dst = destination[0..out_buf_len] };

    const pixels = @as(usize, width) * @as(usize, height) * spec.Limits.rgb_channels;
    const result = run(&state, source[0..pixels], width, height, quality);
    if (result != .ok) return @intFromEnum(result);

    written.* = @intCast(state.sink.pos);
    return @intFromEnum(Error.ok);
}
