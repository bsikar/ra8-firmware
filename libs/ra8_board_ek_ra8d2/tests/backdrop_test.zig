//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The flat-colour panel backdrop: what it tells the controller, and that it
//! stops at the first refusal instead of starting scanout anyway.

const std = @import("std");
const backdrop = @import("backdrop");

var last_cfg: backdrop.Cfg = undefined;
var init_calls: usize = 0;
var colours: [4]u32 = .{ 0, 0, 0, 0 };
var colour_calls: usize = 0;
var starts: [2]bool = .{ false, false };
var start_calls: usize = 0;

var init_err: u32 = 0;
var colour_err: u32 = 0;
var start_err: u32 = 0;

export fn ra8_glcdc_init(cfg: *const backdrop.Cfg) u32 {
    last_cfg = cfg.*;
    init_calls += 1;
    return init_err;
}

export fn ra8_glcdc_set_background_color(argb: u32) u32 {
    if (colour_calls < colours.len) colours[colour_calls] = argb;
    colour_calls += 1;
    return colour_err;
}

export fn ra8_glcdc_start(enable: bool) u32 {
    if (start_calls < starts.len) starts[start_calls] = enable;
    start_calls += 1;
    return start_err;
}

fn reset() void {
    init_calls = 0;
    colour_calls = 0;
    start_calls = 0;
    init_err = 0;
    colour_err = 0;
    start_err = 0;
}

test "begin configures, colours, then starts, in that order" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), backdrop.begin(0x00FF8040));
    try std.testing.expectEqual(@as(usize, 1), init_calls);
    try std.testing.expectEqual(@as(usize, 1), colour_calls);
    try std.testing.expectEqual(@as(usize, 1), start_calls);
    try std.testing.expectEqual(@as(u32, 0x00FF8040), colours[0]);
    try std.testing.expect(starts[0]);
}

test "no framebuffer address goes to the controller" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), backdrop.begin(0));
    try std.testing.expectEqual(@as(u32, 0), last_cfg.framebuffer_addr);
}

test "the panel's own geometry is what gets configured" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), backdrop.begin(0));
    try std.testing.expectEqual(@as(u16, 1024), last_cfg.width_px);
    try std.testing.expectEqual(@as(u16, 600), last_cfg.height_px);
    try std.testing.expectEqual(@as(u16, 1024), last_cfg.timing.h_active);
    try std.testing.expectEqual(@as(u16, 600), last_cfg.timing.v_active);
}

test "ER-TFT070-6 blanking, not the generic 1024x600 numbers" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), backdrop.begin(0));
    try std.testing.expectEqual(@as(u16, 160), last_cfg.timing.h_front);
    try std.testing.expectEqual(@as(u16, 160), last_cfg.timing.h_back);
    try std.testing.expectEqual(@as(u16, 4), last_cfg.timing.h_sync);
    try std.testing.expectEqual(@as(u16, 12), last_cfg.timing.v_front);
    try std.testing.expectEqual(@as(u16, 23), last_cfg.timing.v_back);
    try std.testing.expectEqual(@as(u16, 3), last_cfg.timing.v_sync);
}

test "the stated pixel format is rgb565" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), backdrop.begin(0));
    try std.testing.expectEqual(@as(u8, 0x2), last_cfg.format);
}

test "a refused timing stops before the colour and the start" {
    reset();
    init_err = 0x103;
    try std.testing.expectEqual(@as(u32, 0x103), backdrop.begin(0x112233));
    try std.testing.expectEqual(@as(usize, 0), colour_calls);
    try std.testing.expectEqual(@as(usize, 0), start_calls);
}

test "a refused colour stops before scanout" {
    reset();
    colour_err = 0x104;
    try std.testing.expectEqual(@as(u32, 0x104), backdrop.begin(0x112233));
    try std.testing.expectEqual(@as(usize, 1), init_calls);
    try std.testing.expectEqual(@as(usize, 0), start_calls);
}

test "a refused start is what begin returns" {
    reset();
    start_err = 0x104;
    try std.testing.expectEqual(@as(u32, 0x104), backdrop.begin(0x112233));
}

test "set is the colour write and nothing else" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), backdrop.set(0x00AABBCC));
    try std.testing.expectEqual(@as(usize, 1), colour_calls);
    try std.testing.expectEqual(@as(u32, 0x00AABBCC), colours[0]);
    try std.testing.expectEqual(@as(usize, 0), init_calls);
    try std.testing.expectEqual(@as(usize, 0), start_calls);
}

test "set forwards the controller's refusal" {
    reset();
    colour_err = 0x10F;
    try std.testing.expectEqual(@as(u32, 0x10F), backdrop.set(0));
}

test "config is pure: two calls agree" {
    const a = backdrop.config();
    const b = backdrop.config();
    // Field by field: the padding bytes of an extern struct are undefined.
    try std.testing.expectEqual(a, b);
}

test "the configuration matches the C struct the controller reads" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(backdrop.Cfg, "framebuffer_addr"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(backdrop.Cfg, "width_px"));
    try std.testing.expectEqual(@as(usize, 6), @offsetOf(backdrop.Cfg, "height_px"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(backdrop.Cfg, "format"));
    try std.testing.expectEqual(@as(usize, 10), @offsetOf(backdrop.Cfg, "timing"));
    try std.testing.expectEqual(@as(usize, 28), @sizeOf(backdrop.Cfg));
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(@FieldType(backdrop.Cfg, "timing")));
}
