//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The image leaf binds the shared renderer into the widget vtable.

const std = @import("std");
const abi = @import("abi");

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {}

const Fill = struct { x: i32, y: i32, w: i32, h: i32, color: u32 };
var fills: std.BoundedArray(Fill, 64) = .{};

fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
    fills.append(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
}

const paint: abi.Paint = .{ .user = null, .fill_rect = fillRect, .draw_text = null, .text_size = null };

test "image widget binds its paint callback and draws in its assigned rect" {
    const pixels = [_]u8{ 20, 20, 80, 80 };
    var descriptor: abi.ImageWidget = .{
        .paint = &paint,
        .pixels = &pixels,
        .width = 2,
        .height = 2,
        .scale = .fill,
        .placeholder_fill = 255,
        .placeholder_border = 0,
        .reserved = 0,
        .placeholder_border_width = 1,
    };
    var widget = abi.Widget{
        .vt = null,
        .ctx = null,
        .rect = .{ .x = 10, .y = 20, .w = 2, .h = 2 },
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = false,
        .dirty = false,
    };
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_image_init(&widget, &descriptor));
    try std.testing.expect(widget.visible);
    try std.testing.expect(widget.vt != null);
    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 2), fills.len);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 20, .w = 2, .h = 1, .color = 0x141414 }, fills.get(0));
    try std.testing.expectEqual(Fill{ .x = 10, .y = 21, .w = 2, .h = 1, .color = 0x505050 }, fills.get(1));
}
