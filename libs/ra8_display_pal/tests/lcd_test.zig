//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the GLCDC/LCD backend's decision logic: the configuration
//! gate in its documented order, the geometry snapshot, the rectangle bounds,
//! and the spans a cache clean covers.

const std = @import("std");
const implementation = @import("implementation");
const core = implementation.core;

fn goodCfg() implementation.CfgView {
    return .{
        .has_framebuffer = true,
        .width_px = 320,
        .height_px = 240,
        .pixfmt = core.pixfmt_rgb565,
        .has_panel_timing = true,
        .framebuffer_bytes = 320 * 240 * 2,
    };
}

test "a packed RGB565 buffer of exactly the needed size is accepted" {
    try std.testing.expectEqual(core.err_ok, implementation.validateCfg(goodCfg()));
}

test "a null framebuffer is the first thing refused" {
    var cfg = goodCfg();
    cfg.has_framebuffer = false;
    // Every other field is also wrong, and the pointer still wins.
    cfg.width_px = 0;
    cfg.pixfmt = core.pixfmt_grey4;
    try std.testing.expectEqual(core.err_null_ptr, implementation.validateCfg(cfg));
}

test "a zero dimension is refused before the pixel format" {
    var cfg = goodCfg();
    cfg.width_px = 0;
    cfg.pixfmt = core.pixfmt_grey1;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
    cfg = goodCfg();
    cfg.height_px = 0;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
}

test "a non-RGB565 format is not supported rather than invalid" {
    var cfg = goodCfg();
    cfg.pixfmt = core.pixfmt_grey4;
    try std.testing.expectEqual(core.err_not_supported, implementation.validateCfg(cfg));
    cfg.pixfmt = core.pixfmt_rgb888;
    try std.testing.expectEqual(core.err_not_supported, implementation.validateCfg(cfg));
}

test "a missing panel timing is refused, and after the format check" {
    var cfg = goodCfg();
    cfg.has_panel_timing = false;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
    // A grey4 buffer with no timing is still reported as the format problem.
    cfg.pixfmt = core.pixfmt_grey4;
    try std.testing.expectEqual(core.err_not_supported, implementation.validateCfg(cfg));
}

test "a geometry too large for any u32 byte count is refused, not wrapped" {
    var cfg = goodCfg();
    cfg.width_px = 65535;
    cfg.height_px = 65535;
    cfg.framebuffer_bytes = 0xFFFF_FFFF;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
}

test "a buffer one byte short of the geometry is refused" {
    var cfg = goodCfg();
    cfg.framebuffer_bytes = (320 * 240 * 2) - 1;
    try std.testing.expectEqual(core.err_invalid_arg, implementation.validateCfg(cfg));
    cfg.framebuffer_bytes = (320 * 240 * 2) + 4096;
    try std.testing.expectEqual(core.err_ok, implementation.validateCfg(cfg));
}

test "stride is one packed RGB565 row and needed bytes is the whole buffer" {
    try std.testing.expectEqual(@as(u32, 640), implementation.strideBytes(320));
    try std.testing.expectEqual(@as(u32, 0), implementation.strideBytes(0));
    try std.testing.expectEqual(@as(u64, 640 * 240), implementation.neededBytes(320, 240));
    // Computed wide, so the largest geometry expressible does not wrap into a
    // figure some u32 byte count could satisfy.
    try std.testing.expectEqual(@as(u64, 65535) * 2 * 65535, implementation.neededBytes(65535, 65535));
}

test "caps report a continuously refreshed RGB565 panel with partial update" {
    const caps = implementation.capsFor(1024, 600);
    try std.testing.expectEqual(@as(u16, 1024), caps.width_px);
    try std.testing.expectEqual(@as(u16, 600), caps.height_px);
    try std.testing.expectEqual(core.pixfmt_rgb565, caps.pixfmt);
    try std.testing.expectEqual(@as(u32, 2048), caps.stride_bytes);
    try std.testing.expectEqual(@as(u32, 0), caps.refresh_latency_us_typ);
    try std.testing.expect(caps.supports_partial_update);
    try std.testing.expect(caps.continuous_refresh);
}

test "the framebuffer snapshot describes the caller's own buffer" {
    var storage: [16]u16 = @splat(0);
    const fb = implementation.fbFor(&storage, 4, 2);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&storage)), fb.pixels);
    try std.testing.expectEqual(@as(u32, 8), fb.stride_bytes);
    try std.testing.expectEqual(core.pixfmt_rgb565, fb.pixfmt);
    try std.testing.expectEqual(@as(u32, 8), implementation.pixelCount(fb));
}

test "a rectangle filling the panel fits, and one pixel past it does not" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{ .x = 0, .y = 0, .w = 320, .h = 240 }));
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{ .x = 319, .y = 239, .w = 1, .h = 1 }));
    try std.testing.expectEqual(
        core.err_invalid_arg,
        implementation.checkRect(caps, .{ .x = 0, .y = 0, .w = 321, .h = 240 }),
    );
    try std.testing.expectEqual(
        core.err_invalid_arg,
        implementation.checkRect(caps, .{ .x = 0, .y = 0, .w = 320, .h = 241 }),
    );
}

test "an origin on the far edge is allowed only with an empty extent" {
    const caps = implementation.capsFor(320, 240);
    // x == width is not itself out of range: the C compares with >.
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{ .x = 320, .y = 240, .w = 0, .h = 0 }));
    try std.testing.expectEqual(
        core.err_invalid_arg,
        implementation.checkRect(caps, .{ .x = 321, .y = 0, .w = 0, .h = 0 }),
    );
    try std.testing.expectEqual(
        core.err_invalid_arg,
        implementation.checkRect(caps, .{ .x = 0, .y = 241, .w = 0, .h = 0 }),
    );
}

test "a rectangle whose sum would overflow u16 is refused, not wrapped" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(
        core.err_invalid_arg,
        implementation.checkRect(caps, .{ .x = 300, .y = 0, .w = 65535, .h = 0 }),
    );
    try std.testing.expectEqual(
        core.err_invalid_arg,
        implementation.checkRect(caps, .{ .x = 0, .y = 200, .w = 0, .h = 65535 }),
    );
}

test "an empty rectangle is accepted anywhere inside the panel" {
    const caps = implementation.capsFor(320, 240);
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{}));
    try std.testing.expectEqual(core.err_ok, implementation.checkRect(caps, .{ .x = 160, .y = 120, .w = 0, .h = 0 }));
}

test "clear keeps only the low sixteen bits of the colour" {
    try std.testing.expectEqual(@as(u16, 0xF800), implementation.rgb565Of(0xF800));
    try std.testing.expectEqual(@as(u16, 0xFFFF), implementation.rgb565Of(0xFFFF_FFFF));
    try std.testing.expectEqual(@as(u16, 0x0000), implementation.rgb565Of(0xDEAD_0000));
    try std.testing.expectEqual(@as(u16, 0x1234), implementation.rgb565Of(0xABCD_1234));
}

test "a flush cleans whole rows, a superset of the rectangle" {
    const fb = implementation.fbFor(null, 320, 240);
    const rect: core.Rect = .{ .x = 8, .y = 10, .w = 16, .h = 4 };
    // Offset and span ignore x and w on purpose: whole rows, never a strided slice.
    try std.testing.expectEqual(@as(u32, 10 * 640), implementation.rowSpanOffset(fb, rect));
    try std.testing.expectEqual(@as(u32, 4 * 640), implementation.rowSpanBytes(fb, rect));
    const full: core.Rect = .{ .x = 0, .y = 0, .w = 320, .h = 240 };
    try std.testing.expectEqual(@as(u32, 0), implementation.rowSpanOffset(fb, full));
    try std.testing.expectEqual(implementation.wholeFbBytes(fb), implementation.rowSpanBytes(fb, full));
}

test "a clear cleans the whole contiguous framebuffer" {
    const fb = implementation.fbFor(null, 1024, 600);
    try std.testing.expectEqual(@as(u32, 2048 * 600), implementation.wholeFbBytes(fb));
    try std.testing.expectEqual(@as(u32, 1024 * 600), implementation.pixelCount(fb));
}

test "teardown drops the framebuffer pointer and keeps the geometry" {
    var storage: [4]u16 = @splat(0);
    const bound = implementation.fbFor(&storage, 2, 2);
    const released = implementation.releasedFb(bound);
    try std.testing.expectEqual(@as(?*anyopaque, null), released.pixels);
    try std.testing.expectEqual(bound.width_px, released.width_px);
    try std.testing.expectEqual(bound.height_px, released.height_px);
    try std.testing.expectEqual(bound.stride_bytes, released.stride_bytes);
    try std.testing.expectEqual(bound.pixfmt, released.pixfmt);
}

test "a zeroed context is not started and holds no buffer" {
    const ctx: implementation.Ctx = .{};
    try std.testing.expect(!ctx.started);
    try std.testing.expectEqual(@as(?*anyopaque, null), ctx.fb.pixels);
    try std.testing.expectEqual(@as(u16, 0), ctx.caps.width_px);
}

test "the bring-up constants are the ones the panel sequence documents" {
    try std.testing.expectEqual(@as(u32, 200), implementation.lcd.settle_ms);
    try std.testing.expectEqual(@as(u32, 0), implementation.lcd.bg_color_black);
    try std.testing.expectEqual(@as(u32, 64), implementation.lcd.fb_align_bytes);
    try std.testing.expectEqual(@as(u32, 2), implementation.lcd.rgb565_bpp);
    try std.testing.expectEqual(@as(u32, 0xFFFF), implementation.lcd.rgb565_mask);
}
