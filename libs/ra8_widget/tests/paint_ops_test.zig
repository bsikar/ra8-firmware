//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8FW-951: the line, blit, clip and alpha ops on a recording fake Paint,
//! both with the callbacks bound and with the null defaults a panel backend
//! leaves, where each op falls back or reports that it did nothing.

const std = @import("std");
const ops = @import("ops");

comptime {
    _ = @import("host_box_shims.zig");
}

const Call = union(enum) {
    fill: [5]i64,
    line: [5]i64,
    blit: [7]i64,
    push: [4]i64,
    pop,
    alpha: [6]i64,
    text: [4]i64,
    text_face: [5]i64,
    text_style: [7]i64,
};

var calls_buffer: [64]Call = undefined;
var calls: std.ArrayList(Call) = .initBuffer(&calls_buffer);

fn record(call: Call) void {
    calls.appendBounded(call) catch unreachable;
}

fn fill(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
    record(.{ .fill = .{ x, y, w, h, color } });
}
fn line(_: ?*anyopaque, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) void {
    record(.{ .line = .{ x0, y0, x1, y1, color } });
}
fn blit(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, pixels: [*]const u8, stride: i32, format: u8) callconv(.c) void {
    record(.{ .blit = .{ x, y, w, h, pixels[0], stride, format } });
}
fn push(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32) callconv(.c) void {
    record(.{ .push = .{ x, y, w, h } });
}
fn pop(_: ?*anyopaque) callconv(.c) void {
    record(.pop);
}
fn alpha(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32, a: u8) callconv(.c) void {
    record(.{ .alpha = .{ x, y, w, h, color, a } });
}
fn drawText(_: ?*anyopaque, x: i32, y: i32, _: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void {
    record(.{ .text = .{ x, y, fg, bg } });
}
fn drawTextFace(_: ?*anyopaque, x: i32, y: i32, _: [*:0]const u8, family: u8, fg: u32, bg: u32) callconv(.c) void {
    record(.{ .text_face = .{ x, y, family, fg, bg } });
}
fn drawTextStyle(_: ?*anyopaque, x: i32, y: i32, _: [*:0]const u8, family: u8, stroke: u8, size: u8, fg: u32, bg: u32) callconv(.c) void {
    record(.{ .text_style = .{ x, y, family, stroke, size, fg, bg } });
}
fn textSize(_: ?*anyopaque, _: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    out_w.* = 12;
    out_h.* = 16;
}
fn textSizeFace(_: ?*anyopaque, _: [*:0]const u8, family: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    out_w.* = 20 + family;
    out_h.* = 22;
}
fn textSizeStyle(_: ?*anyopaque, _: [*:0]const u8, family: u8, stroke: u8, size: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    out_w.* = 30 + family + stroke + size;
    out_h.* = 34;
}

const panel: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = null, .text_size = null };
const desktop: ops.Paint = .{
    .user = null,
    .fill_rect = fill,
    .draw_text = null,
    .text_size = null,
    .draw_line = line,
    .blit = blit,
    .push_clip = push,
    .pop_clip = pop,
    .fill_rect_alpha = alpha,
};
const box: ops.Rect = .{ .x = 2, .y = 3, .w = 4, .h = 5 };

fn reset() void {
    calls.clearRetainingCapacity();
}

test "a bound draw_line gets the endpoints as given" {
    reset();
    ops.line(&desktop, 9, 1, 2, 7, 0xABCDEF);
    try std.testing.expectEqualSlices(Call, &.{.{ .line = .{ 9, 1, 2, 7, 0xABCDEF } }}, calls.items);
}

test "without draw_line a straight run is one fill_rect either way round" {
    reset();
    ops.line(&panel, 8, 4, 3, 4, 1);
    ops.line(&panel, 5, 9, 5, 6, 2);
    try std.testing.expectEqualSlices(Call, &.{ .{ .fill = .{ 3, 4, 6, 1, 1 } }, .{ .fill = .{ 5, 6, 1, 4, 2 } } }, calls.items);
}

test "without draw_line a slope steps one pixel at a time from end to end" {
    reset();
    ops.line(&panel, 0, 0, 3, 3, 7);
    try std.testing.expectEqual(@as(usize, 4), calls.items.len);
    try std.testing.expectEqual(Call{ .fill = .{ 0, 0, 1, 1, 7 } }, calls.items[0]);
    try std.testing.expectEqual(Call{ .fill = .{ 3, 3, 1, 1, 7 } }, calls.items[3]);
}

test "blit reaches a bound backend and reports false to a panel one" {
    reset();
    const pixels = [_]u8{ 0x11, 0x22 };
    try std.testing.expect(ops.blit(&desktop, box, &pixels, 2, .argb8888));
    try std.testing.expect(!ops.blit(&panel, box, &pixels, 2, .gray8));
    try std.testing.expectEqualSlices(Call, &.{.{ .blit = .{ 2, 3, 4, 5, 0x11, 2, 4 } }}, calls.items);
}

test "clip push and pop pass through, and are no-ops when unbound" {
    reset();
    ops.pushClip(&desktop, box);
    ops.popClip(&desktop);
    ops.pushClip(&panel, box);
    ops.popClip(&panel);
    try std.testing.expectEqualSlices(Call, &.{ .{ .push = .{ 2, 3, 4, 5 } }, .pop }, calls.items);
}

test "alpha zero paints nothing, partial alpha blends, opaque is a plain fill" {
    reset();
    ops.fillAlpha(&desktop, box, 9, 0);
    ops.fillAlpha(&desktop, box, 9, 128);
    ops.fillAlpha(&desktop, box, 9, 255);
    try std.testing.expectEqualSlices(Call, &.{ .{ .alpha = .{ 2, 3, 4, 5, 9, 128 } }, .{ .fill = .{ 2, 3, 4, 5, 9 } } }, calls.items);
}

test "without a blended fill any visible alpha paints solid" {
    reset();
    ops.fillAlpha(&panel, box, 9, 64);
    try std.testing.expectEqualSlices(Call, &.{.{ .fill = .{ 2, 3, 4, 5, 9 } }}, calls.items);
}

test "capabilities return explicit caps when bound and infer from callbacks when null" {
    const inferred_panel = ops.capabilities(&panel);
    try std.testing.expectEqual(ops.PixelFormat.gray8, inferred_panel.pixel_format);
    try std.testing.expectEqual(@as(u8, 0), inferred_panel.color);
    try std.testing.expectEqual(@as(u8, 0), inferred_panel.alpha);
    try std.testing.expectEqual(@as(u8, 0), inferred_panel.line);
    try std.testing.expectEqual(@as(u8, 0), inferred_panel.blit);
    try std.testing.expectEqual(@as(u8, 0), inferred_panel.clip);

    const inferred_desktop = ops.capabilities(&desktop);
    try std.testing.expectEqual(@as(u8, 1), inferred_desktop.alpha);
    try std.testing.expectEqual(@as(u8, 1), inferred_desktop.line);
    try std.testing.expectEqual(@as(u8, 1), inferred_desktop.blit);
    try std.testing.expectEqual(@as(u8, 1), inferred_desktop.clip);

    const eink_caps: ops.CanvasCaps = .{ .pixel_format = .gray4, .gray4 = 1, .dither = 1, .line = 1 };
    var eink_paint = panel;
    eink_paint.caps = &eink_caps;
    const explicit = ops.capabilities(&eink_paint);
    try std.testing.expectEqual(ops.PixelFormat.gray4, explicit.pixel_format);
    try std.testing.expectEqual(@as(u8, 1), explicit.gray4);
    try std.testing.expectEqual(@as(u8, 1), explicit.dither);
    try std.testing.expectEqual(@as(u8, 1), explicit.line);
}

test "text and fill canvas ops dispatch with style -> face -> basic fallbacks" {
    reset();
    ops.fillRect(&panel, box, 0x123456);
    ops.drawTextStyle(&panel, 1, 2, "hi", .serif, .bold, .size_4, 10, 20);
    try std.testing.expectEqualSlices(Call, &.{.{ .fill = .{ 2, 3, 4, 5, 0x123456 } }}, calls.items);

    const basic_ext = ops.measureText(&panel, "hi");
    try std.testing.expectEqual(@as(i32, 0), basic_ext.w);
    try std.testing.expectEqual(@as(i32, 0), basic_ext.h);

    const base_text: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = drawText, .text_size = textSize };
    reset();
    ops.drawTextStyle(&base_text, 4, 5, "a", .serif, .bold, .size_2, 7, 8);
    try std.testing.expectEqualSlices(Call, &.{.{ .text = .{ 4, 5, 7, 8 } }}, calls.items);
    const base_m = ops.measureTextStyle(&base_text, "a", .serif, .bold, .size_2);
    try std.testing.expectEqual(@as(i32, 12), base_m.w);

    const face_text: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = drawText, .text_size = textSize, .draw_text_face = drawTextFace, .text_size_face = textSizeFace };
    reset();
    ops.drawTextStyle(&face_text, 6, 7, "b", .serif, .bold, .size_2, 9, 10);
    try std.testing.expectEqualSlices(Call, &.{.{ .text_face = .{ 6, 7, 1, 9, 10 } }}, calls.items);
    const face_m = ops.measureTextStyle(&face_text, "b", .serif, .bold, .size_2);
    try std.testing.expectEqual(@as(i32, 21), face_m.w);

    const full_text: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = drawText, .text_size = textSize, .draw_text_face = drawTextFace, .text_size_face = textSizeFace, .draw_text_style = drawTextStyle, .text_size_style = textSizeStyle };
    reset();
    ops.drawTextStyle(&full_text, 8, 9, "c", .serif, .bold, .ui_26, 11, 12);
    try std.testing.expectEqualSlices(Call, &.{.{ .text_style = .{ 8, 9, 1, 1, 9, 11, 12 } }}, calls.items);
    const full_m = ops.measureTextStyle(&full_text, "c", .serif, .bold, .ui_26);
    try std.testing.expectEqual(@as(i32, 41), full_m.w);
}

test "normalizeTextSize, hasText, hasMeasure, fillBox, and drawTextInRect share fallback rules" {
    try std.testing.expectEqual(ops.TextSize.size_3, ops.normalizeTextSize(.default));
    try std.testing.expectEqual(ops.TextSize.ui_26, ops.normalizeTextSize(.ui_26));
    try std.testing.expectEqual(ops.TextSize.size_1, ops.normalizeTextSize(.size_1));

    try std.testing.expect(!ops.hasText(&panel));
    try std.testing.expect(!ops.hasMeasure(&panel));

    reset();
    ops.fillBox(&panel, .{ .x = 10, .y = 20, .w = 40, .h = 16 }, 0xAA, 0xBB, 2);
    try std.testing.expectEqualSlices(Call, &.{
        .{ .fill = .{ 10, 20, 40, 16, 0xBB } },
        .{ .fill = .{ 12, 22, 36, 12, 0xAA } },
    }, calls.items);

    const basic_only: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = drawText, .text_size = textSize };
    try std.testing.expect(ops.hasText(&basic_only));
    try std.testing.expect(ops.hasMeasure(&basic_only));
    reset();
    ops.drawTextInRect(&basic_only, .{ .x = 0, .y = 0, .w = 60, .h = 30 }, "ok", 4, .center, .sans, .regular, .default, 1, 2);
    try std.testing.expectEqualSlices(Call, &.{.{ .text = .{ 24, 7, 1, 2 } }}, calls.items);

    const styled_no_measure: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = drawText, .text_size = textSize, .draw_text_face = drawTextFace };
    reset();
    ops.drawTextInRect(&styled_no_measure, .{ .x = 10, .y = 20, .w = 80, .h = 40 }, "ok", 4, .center, .serif, .bold, .default, 3, 4);
    try std.testing.expectEqualSlices(Call, &.{.{ .text_face = .{ 14, 24, 1, 3, 4 } }}, calls.items);
}
