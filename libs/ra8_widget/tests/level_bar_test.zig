//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Paint, hit-test, and damage checks for the signed vertical level bar.
const std = @import("std");
const abi = @import("abi");

var last_message: ?[*:0]const u8 = null;
var invalidations: u32 = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

export fn ra8_widget_invalidate(w: *abi.Widget, refresh: abi.Refresh) callconv(.c) u16 {
    invalidations += 1;
    w.dirty = true;
    w.refresh = @intFromEnum(refresh);
    return abi.err.ok;
}

const Fill = struct { rect: abi.Rect, color: u32 };
const Recorder = struct {
    var fills: std.BoundedArray(Fill, 16) = .{};
    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills.append(.{ .rect = .{ .x = x, .y = y, .w = w, .h = h }, .color = color }) catch unreachable;
    }
};
const backend: abi.Paint = .{ .user = null, .fill_rect = Recorder.fillRect, .draw_text = null, .text_size = null };

fn widget(rect: abi.Rect) abi.Widget {
    return .{ .vt = null, .ctx = null, .rect = rect, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}
fn bar(value: i8) abi.LevelBar {
    return .{ .paint = &backend, .track = 10, .fill = 20, .center_mark = 30, .value = value, .damage = .{ .x = 0, .y = 0, .w = 0, .h = 0 } };
}
fn bind(w: *abi.Widget, b: *abi.LevelBar) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_level_bar_init(w, b));
}
fn touch(x: i32, y: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = y };
}

test "each signed value lights exactly its cells around the marked centre" {
    for ([_]i8{ -6, -1, 0, 1, 6 }) |value| {
        for (0..abi.geometry.cell_count) |raw| {
            const index: u8 = @intCast(raw);
            const expected = if (index == abi.geometry.center) false else if (value > 0) index < abi.geometry.center and index >= abi.geometry.center - @as(u8, @intCast(value)) else if (value < 0) index > abi.geometry.center and index <= abi.geometry.center + @as(u8, @intCast(-value)) else false;
            try std.testing.expectEqual(expected, abi.cellFilled(value, index));
        }
    }
}

test "cell geometry tiles the whole height without overlap" {
    const rect = abi.Rect{ .x = 5, .y = 7, .w = 12, .h = 101 };
    var next_y = rect.y;
    for (0..abi.geometry.cell_count) |raw| {
        const cell = abi.cellRect(rect, @intCast(raw));
        try std.testing.expectEqual(next_y, cell.y);
        try std.testing.expectEqual(rect.w, cell.w);
        next_y += cell.h;
    }
    try std.testing.expectEqual(rect.y + rect.h, next_y);
}

test "tap and drag update the value and report only changed cell bounds" {
    invalidations = 0;
    var w = widget(.{ .x = 10, .y = 20, .w = 20, .h = 130 });
    var b = bar(0);
    try bind(&w, &b);
    const callback = w.vt.?.on_input.?;
    try std.testing.expect(callback(&w, &touch(15, 70)));
    try std.testing.expectEqual(@as(i8, 1), b.value);
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 70, .w = 20, .h = 9 }, b.damage);
    try std.testing.expectEqual(@as(u32, 1), invalidations);
    try std.testing.expect(callback(&w, &touch(15, 80)));
    try std.testing.expectEqual(@as(i8, 0), b.value);
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 70, .w = 20, .h = 9 }, b.damage);
    try std.testing.expectEqual(@as(u32, 2), invalidations);
    try std.testing.expect(callback(&w, &touch(15, 140)));
    try std.testing.expectEqual(@as(i8, -6), b.value);
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 90, .w = 20, .h = 60 }, b.damage);
    try std.testing.expectEqual(@as(u32, 3), invalidations);
    try std.testing.expect(!callback(&w, &touch(30, 140)));
}

test "unchanged value has empty damage and does not invalidate" {
    invalidations = 0;
    var w = widget(.{ .x = 0, .y = 0, .w = 10, .h = 130 });
    var b = bar(0);
    try bind(&w, &b);
    try std.testing.expect(w.vt.?.on_input.?(&w, &touch(1, 65)));
    try std.testing.expectEqual(@as(i8, 0), b.value);
    try std.testing.expectEqual(@as(i32, 0), b.damage.h);
    try std.testing.expectEqual(@as(u32, 0), invalidations);
}

test "render marks the centre and paints the selected side" {
    Recorder.fills = .{};
    var w = widget(.{ .x = 0, .y = 0, .w = 10, .h = 130 });
    var b = bar(-6);
    try bind(&w, &b);
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, abi.geometry.cell_count), Recorder.fills.len);
    try std.testing.expectEqual(@as(u32, 30), Recorder.fills.buffer[abi.geometry.center].color);
    try std.testing.expectEqual(@as(u32, 20), Recorder.fills.buffer[7].color);
    try std.testing.expectEqual(@as(u32, 10), Recorder.fills.buffer[5].color);
    try std.testing.expectEqual(@as(i32, 9), Recorder.fills.buffer[0].rect.h);
    try std.testing.expectEqual(@as(i32, 10), Recorder.fills.buffer[12].rect.h);
}

test "init rejects values outside the signed cell range" {
    var w = widget(.{ .x = 0, .y = 0, .w = 10, .h = 130 });
    var b = bar(7);
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_level_bar_init(&w, &b));
}
