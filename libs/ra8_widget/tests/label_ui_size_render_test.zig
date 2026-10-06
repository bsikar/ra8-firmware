//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host ink check for the UI text sizes: ui_26 and ui_30 labels must draw at
//! their own line height through the real atlas, not fall back to size_3.
//! Reached from host_render_test.zig so it shares that module's imports.

const std = @import("std");
const abi = @import("abi");
const host = @import("host");

const ink_threshold: u8 = 128;

fn widget(rect: abi.label.Rect) abi.label.Widget {
    return .{ .vt = null, .ctx = null, .rect = rect, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}

fn paintFor(canvas: *host.Canvas) abi.types.Paint {
    return .{
        .user = canvas,
        .fill_rect = host.Canvas.fillRect,
        .draw_text = host.Canvas.drawText,
        .text_size = host.Canvas.textSize,
        .draw_text_face = host.Canvas.drawTextFace,
        .text_size_face = host.Canvas.textSizeFace,
        .draw_text_style = host.Canvas.drawTextStyle,
        .text_size_style = host.Canvas.textSizeStyle,
    };
}

/// Dark pixels inside rows [top, bottom) of the canvas.
fn inkRows(canvas: host.Canvas, top: usize, bottom: usize) usize {
    var count: usize = 0;
    for (canvas.pixels[top * canvas.width .. bottom * canvas.width]) |p| {
        if (p < ink_threshold) count += 1;
    }
    return count;
}

fn renderLabel(canvas: *host.Canvas, text: [*:0]const u8, size: abi.paint.TextSize, wrap: abi.label.WrapMode, rect: abi.label.Rect) !void {
    const paint = paintFor(canvas);
    var label = abi.label.Label{ .paint = &paint, .text = text, .fg = 0x000000, .bg = 0xffffff, .pad = 0, .alignment = .left, .face = .sans, .size = size, .wrap = wrap };
    var w = widget(rect);
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&w, &label));
    w.vt.?.render.?(&w);
}

test "a sans ui_26 clip label draws ink in a 32 px box" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 480, 64, 255);
    defer canvas.deinit(allocator);
    try renderLabel(&canvas, "Short text", .ui_26, .clip, .{ .x = 0, .y = 0, .w = 440, .h = 32 });
    try std.testing.expect(inkRows(canvas, 0, 32) > 0);
}

test "a sans ui_30 word label wraps onto a second 36 px line" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 480, 120, 255);
    defer canvas.deinit(allocator);
    try renderLabel(&canvas, "Short\ntext", .ui_30, .word, .{ .x = 0, .y = 0, .w = 440, .h = 72 });
    try std.testing.expect(inkRows(canvas, 0, 36) > 0);
    try std.testing.expect(inkRows(canvas, 36, 72) > 0);
    try std.testing.expectEqual(@as(usize, 0), inkRows(canvas, 72, 120));
}
