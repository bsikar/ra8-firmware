//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Segmented-control layout, render, input, and widget-rect damage tests.
const std = @import("std");
const abi = @import("abi");

var invalidations: u32 = 0;
var damaged: abi.Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
var fills: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 };
var fill_count: usize = 0;
var draws: usize = 0;
var last_text: ?[*:0]const u8 = null;
var last_message: ?[*:0]const u8 = null;
export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}
export fn ra8_widget_invalidate(w: *abi.Widget, refresh: u8) callconv(.c) u16 {
    invalidations += 1;
    damaged = w.rect;
    w.dirty = true;
    w.refresh = refresh;
    return abi.err.ok;
}
fn fillRect(_: ?*anyopaque, _: i32, _: i32, _: i32, _: i32, color: u32) callconv(.c) void {
    fills[fill_count] = color;
    fill_count += 1;
}
fn drawText(_: ?*anyopaque, _: i32, _: i32, text: [*:0]const u8, _: u32, _: u32) callconv(.c) void {
    draws += 1;
    last_text = text;
}
const paint: abi.Paint = .{ .user = null, .fill_rect = fillRect, .draw_text = drawText, .text_size = null };
const labels = [_][*:0]const u8{ "Serif", "Sans", "Mono" };
fn widget() abi.Widget {
    return .{ .vt = null, .ctx = null, .rect = .{ .x = 10, .y = 20, .w = 101, .h = 36 }, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}
fn control() abi.Segmented {
    return .{ .paint = &paint, .labels = &labels, .fg = 1, .selected_fg = 2, .bg = 3, .selected_bg = 4, .border = 5, .count = 3, .selected = 0, .pad = 2 };
}
fn touch(x: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = 25 };
}
fn reset() void {
    invalidations = 0;
    damaged = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
    fills = .{ 0, 0, 0, 0, 0, 0, 0, 0 };
    fill_count = 0;
    draws = 0;
    last_text = null;
    last_message = null;
}

test "segment widths distribute remainder without losing pixels" {
    try std.testing.expectEqual(@as(i32, 34), abi.segmentWidth(101, 3, 0));
    try std.testing.expectEqual(@as(i32, 34), abi.segmentWidth(101, 3, 1));
    try std.testing.expectEqual(@as(i32, 33), abi.segmentWidth(101, 3, 2));
    try std.testing.expectEqual(@as(i32, 0), abi.segmentWidth(101, 3, 3));
}

test "init binds descriptor and vtable" {
    reset();
    var w = widget();
    var c = control();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_segmented_init(&w, &c));
    try std.testing.expect(w.visible);
    try std.testing.expectEqual(abi.ra8_widget_segmented_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&c)), w.ctx);
}

test "init rejects missing labels and invalid selection" {
    reset();
    var w = widget();
    var c = control();
    c.labels = null;
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_segmented_init(&w, &c));
    c = control();
    c.selected = c.count;
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_segmented_init(&w, &c));
}

test "render paints each segment and label" {
    reset();
    var w = widget();
    var c = control();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_segmented_init(&w, &c));
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, 6), fill_count);
    try std.testing.expectEqual(@as(u32, 5), fills[0]);
    try std.testing.expectEqual(@as(u32, 4), fills[1]);
    try std.testing.expectEqual(@as(u32, 5), fills[2]);
    try std.testing.expectEqual(@as(usize, 3), draws);
    try std.testing.expectEqualStrings("Mono", std.mem.span(last_text.?));
}

test "tap selects matching segment and invalidates its widget rect" {
    reset();
    var w = widget();
    var c = control();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_segmented_init(&w, &c));
    const event = touch(80);
    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expectEqual(@as(u8, 2), c.selected);
    try std.testing.expectEqual(@as(u32, 1), invalidations);
    try std.testing.expectEqual(@as(u8, 1), w.refresh);
    try std.testing.expectEqual(w.rect, damaged);
}

test "tap on selected segment is consumed without damage" {
    reset();
    var w = widget();
    var c = control();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_segmented_init(&w, &c));
    const event = touch(20);
    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expectEqual(@as(u8, 0), c.selected);
    try std.testing.expectEqual(@as(u32, 0), invalidations);
}

test "tap outside width and button event do not change selection" {
    reset();
    var w = widget();
    var c = control();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_segmented_init(&w, &c));
    const outside = touch(111);
    try std.testing.expect(!w.vt.?.on_input.?(&w, &outside));
    const button: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 1, .x = 0, .y = 0 };
    try std.testing.expect(!w.vt.?.on_input.?(&w, &button));
    try std.testing.expectEqual(@as(u8, 0), c.selected);
    try std.testing.expectEqual(@as(u32, 0), invalidations);
}
