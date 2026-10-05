//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Toggle render, binding, input, and widget-rect damage tests.
const std = @import("std");
const abi = @import("abi");

var invalidations: u32 = 0;
var damaged: abi.Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
var fills: u32 = 0;
var last_color: u32 = 0;
var draws: u32 = 0;
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
    fills += 1;
    last_color = color;
}
fn drawText(_: ?*anyopaque, _: i32, _: i32, text: [*:0]const u8, _: u32, _: u32) callconv(.c) void {
    draws += 1;
    last_text = text;
}
const paint: abi.Paint = .{ .user = null, .fill_rect = fillRect, .draw_text = drawText, .text_size = null };

fn widget() abi.Widget {
    return .{ .vt = null, .ctx = null, .rect = .{ .x = 20, .y = 30, .w = 140, .h = 40 }, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}
fn toggle() abi.Toggle {
    return .{ .paint = &paint, .label = "Wifi", .fg = 1, .bg = 2, .border = 3, .mark = 4, .box_size = 24, .gap = 8, .checked = false, .reserved = .{ 0, 0, 0 } };
}
fn touch() abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 22, .y = 32 };
}
fn reset() void {
    invalidations = 0;
    fills = 0;
    draws = 0;
    last_color = 0;
    last_text = null;
    last_message = null;
    damaged = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
}

test "init binds descriptor and vtable" {
    reset();
    var w = widget();
    var t = toggle();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_toggle_init(&w, &t));
    try std.testing.expect(w.visible);
    try std.testing.expectEqual(abi.ra8_widget_toggle_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&t)), w.ctx);
}

test "init rejects null widget and descriptor" {
    reset();
    var t = toggle();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_toggle_init(null, &t));
    try std.testing.expectEqualStrings("w must not be nullptr", std.mem.span(last_message.?));
    var w = widget();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_toggle_init(&w, null));
}

test "render paints checkbox and label through paint backend" {
    reset();
    var w = widget();
    var t = toggle();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_toggle_init(&w, &t));
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(u32, 2), fills);
    try std.testing.expectEqual(@as(u32, 2), last_color);
    try std.testing.expectEqual(@as(u32, 1), draws);
    try std.testing.expectEqualStrings("Wifi", std.mem.span(last_text.?));
}

test "checked render paints mark" {
    reset();
    var w = widget();
    var t = toggle();
    t.checked = true;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_toggle_init(&w, &t));
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(u32, 3), fills);
    try std.testing.expectEqual(@as(u32, 4), last_color);
}

test "touch toggles state and invalidates exactly the widget rect" {
    reset();
    var w = widget();
    var t = toggle();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_toggle_init(&w, &t));
    const event = touch();
    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(t.checked);
    try std.testing.expectEqual(@as(u32, 1), invalidations);
    try std.testing.expectEqual(@as(u8, 1), w.refresh);
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(w.rect, damaged);
}

test "physical button event does not change toggle or damage" {
    reset();
    var w = widget();
    var t = toggle();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_toggle_init(&w, &t));
    const event: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 1, .x = 0, .y = 0 };
    try std.testing.expect(!w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(!t.checked);
    try std.testing.expectEqual(@as(u32, 0), invalidations);
}
