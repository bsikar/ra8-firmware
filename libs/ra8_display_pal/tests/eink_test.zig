//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the e-ink backend's decisions: luma conversion, waveform
//! mapping, config validation order, the snapshots, rect bounds and the 4 bpp
//! row packing.

const std = @import("std");
const implementation = @import("implementation");
const core = implementation.core;
const eink = implementation.eink;

fn goodCfg() core.CfgView {
    return .{
        .has_framebuffer = true,
        .width_px = 320,
        .height_px = 240,
        .pixfmt = core.pixfmt_rgb565,
        .has_panel_timing = true,
        .framebuffer_bytes = 320 * 240 * 2,
    };
}

test "luma of pure black and pure white" {
    try std.testing.expectEqual(@as(u8, 0), implementation.lumaFromRgb565(0x0000));
    try std.testing.expectEqual(@as(u8, 255), implementation.lumaFromRgb565(0xFFFF));
}

test "luma weights the green channel most" {
    const red = implementation.lumaFromRgb565(0xF800);
    const green = implementation.lumaFromRgb565(0x07E0);
    const blue = implementation.lumaFromRgb565(0x001F);
    try std.testing.expect(green > red);
    try std.testing.expect(red > blue);
    // Rec.601 at 8 bits: 0.299, 0.587, 0.114 of 255.
    try std.testing.expectEqual(@as(u8, 76), red);
    try std.testing.expectEqual(@as(u8, 149), green);
    try std.testing.expectEqual(@as(u8, 28), blue);
}

test "channel expansion is bit replication, not a bare shift" {
    // 5-bit 0x10 replicates to 0x84, not 0x80, and 6-bit 0x20 to 0x82.
    const grey = implementation.lumaFromRgb565((0x10 << 11) | (0x20 << 5) | 0x10);
    try std.testing.expectEqual(@as(u8, 130), grey);
}

test "waveform mapping: fast is A2, init is INIT, quality is GC16" {
    try std.testing.expectEqual(implementation.Waveform.a2, implementation.waveformFor(core.refresh_fast));
    try std.testing.expectEqual(implementation.Waveform.init, implementation.waveformFor(core.refresh_init));
    try std.testing.expectEqual(implementation.Waveform.gc16, implementation.waveformFor(core.refresh_quality));
}

test "an unknown hint falls back to GC16" {
    try std.testing.expectEqual(implementation.Waveform.gc16, implementation.waveformFor(0xEE));
}

test "waveform numbering matches the HAL enum" {
    try std.testing.expectEqual(@as(u32, 0), @backingInt(implementation.Waveform.init));
    try std.testing.expectEqual(@as(u32, 2), @backingInt(implementation.Waveform.gc16));
    try std.testing.expectEqual(@as(u32, 3), @backingInt(implementation.Waveform.a2));
    try std.testing.expectEqual(@as(u32, 2), @backingInt(implementation.PixelFormat.bpp4));
}

test "a good cfg is accepted" {
    try std.testing.expectEqual(core.err_ok, implementation.validateCfg(goodCfg()));
}

test "a null framebuffer is refused first" {
    var cfg = goodCfg();
    cfg.has_framebuffer = false;
    cfg.width_px = 0;
    try std.testing.expectEqual(core.err_null_ptr, implementation.validateCfg(cfg));
}

test "either zero dimension is refused" {
    var w = goodCfg();
    w.width_px = 0;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(w));
    var h = goodCfg();
    h.height_px = 0;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(h));
}

test "a non-RGB565 format is not supported" {
    var cfg = goodCfg();
    cfg.pixfmt = core.pixfmt_grey4;
    try std.testing.expectEqual(core.err_not_supported, implementation.validateCfg(cfg));
}

test "a row wider than the bounded scratch line is refused" {
    var cfg = goodCfg();
    cfg.width_px = @intCast(eink.line_max_px);
    cfg.height_px = 1;
    cfg.framebuffer_bytes = eink.line_max_px * 2;
    try std.testing.expectEqual(core.err_ok, implementation.validateCfg(cfg));
    cfg.width_px = @intCast(eink.line_max_px + 1);
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
}

test "the BSP must supply the IT8951 descriptor" {
    var cfg = goodCfg();
    cfg.has_panel_timing = false;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
}

test "a buffer one byte short of the geometry is refused" {
    var cfg = goodCfg();
    cfg.framebuffer_bytes -= 1;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
}

test "the buffer-size math does not wrap at the widest geometry" {
    // The row cap keeps a real e-ink cfg inside u32, so this pins the
    // arithmetic itself rather than a rejection: 65535 rows of a 4096-pixel
    // row is 536862720 bytes, and nothing about it wraps.
    try std.testing.expectEqual(@as(u64, 4096) * 2 * 65535, implementation.neededBytes(4096, 65535));
    try std.testing.expectEqual(@as(u64, 65535) * 2 * 65535, implementation.neededBytes(65535, 65535));
}

test "caps report partial update, no continuous refresh, and GC16 latency" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(@as(u16, 320), caps.width_px);
    try std.testing.expectEqual(@as(u32, 640), caps.stride_bytes);
    try std.testing.expectEqual(eink.refresh_quality_us, caps.refresh_latency_us_typ);
    try std.testing.expect(caps.supports_partial_update);
    try std.testing.expect(!caps.continuous_refresh);
    try std.testing.expectEqual(core.pixfmt_rgb565, caps.pixfmt);
}

test "the framebuffer snapshot hands back the caller's own buffer" {
    var buf: [8]u16 = undefined;
    const fb = implementation.fbFor(&buf, 4, 2);
    try std.testing.expectEqual(@as(?*anyopaque, &buf), fb.pixels);
    try std.testing.expectEqual(@as(u32, 8), fb.stride_bytes);
    try std.testing.expectEqual(@as(u32, 8), implementation.pixelCount(fb));
}

test "a rectangle inside the panel is accepted" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{ .x = 8, .y = 8, .w = 16, .h = 16 }));
}

test "each rect bound is refused on its own" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(core.err_invalid_arg, implementation.checkRect(caps, .{ .x = 321, .y = 0, .w = 0, .h = 0 }));
    try std.testing.expectEqual(core.err_invalid_arg, implementation.checkRect(caps, .{ .x = 0, .y = 241, .w = 0, .h = 0 }));
    try std.testing.expectEqual(core.err_invalid_arg, implementation.checkRect(caps, .{ .x = 300, .y = 0, .w = 21, .h = 1 }));
    try std.testing.expectEqual(core.err_invalid_arg, implementation.checkRect(caps, .{ .x = 0, .y = 220, .w = 1, .h = 21 }));
}

test "a full-panel rectangle sits exactly on the bound" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{ .x = 0, .y = 0, .w = 320, .h = 240 }));
}

test "packed row bytes round up for an odd width" {
    try std.testing.expectEqual(@as(u32, 2), implementation.packedRowBytes(4));
    try std.testing.expectEqual(@as(u32, 3), implementation.packedRowBytes(5));
    try std.testing.expectEqual(@as(u32, 1), implementation.packedRowBytes(1));
}

test "packing keeps the high nibble, first pixel high" {
    const src = [_]u16{ 0x0000, 0xFFFF };
    var dst: [1]u8 = undefined;
    implementation.packRow4bpp(&src, &dst);
    // black -> luma 0 -> nibble 0; white -> luma 255 -> nibble 0xF.
    try std.testing.expectEqual(@as(u8, 0x0F), dst[0]);
}

test "an odd trailing pixel is paired with white" {
    const src = [_]u16{0x0000};
    var dst: [1]u8 = undefined;
    implementation.packRow4bpp(&src, &dst);
    try std.testing.expectEqual(@as(u8, 0x0F), dst[0]);
}

test "packing a longer row fills exactly the packed byte count" {
    const src = [_]u16{ 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF };
    var dst: [4]u8 = .{ 0xAA, 0xAA, 0xAA, 0xAA };
    implementation.packRow4bpp(&src, dst[0..3]);
    try std.testing.expectEqual(@as(u8, 0xFF), dst[0]);
    try std.testing.expectEqual(@as(u8, 0xF0), dst[1]);
    try std.testing.expectEqual(@as(u8, 0xFF), dst[2]);
    // The byte past the packed row is untouched.
    try std.testing.expectEqual(@as(u8, 0xAA), dst[3]);
}

test "row offsets walk the framebuffer by whole rows" {
    const rect: core.Rect = .{ .x = 4, .y = 2, .w = 8, .h = 3 };
    try std.testing.expectEqual(@as(u32, 2 * 320 + 4), implementation.rowOffsetPx(320, rect, 0));
    try std.testing.expectEqual(@as(u32, 4 * 320 + 4), implementation.rowOffsetPx(320, rect, 2));
}

test "a released context forgets the buffer but keeps its geometry" {
    var buf: [8]u16 = undefined;
    const ctx: implementation.Ctx = .{
        .caps = implementation.capsFor(4, 2),
        .fb = implementation.fbFor(&buf, 4, 2),
        .initialised = true,
    };
    const after = implementation.released(ctx);
    try std.testing.expect(!after.initialised);
    try std.testing.expectEqual(@as(?*anyopaque, null), after.fb.pixels);
    try std.testing.expectEqual(@as(u16, 4), after.caps.width_px);
}
