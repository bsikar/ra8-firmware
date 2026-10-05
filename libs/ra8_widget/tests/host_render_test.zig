//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! End-to-end host preview check: bind real label and button widgets to the
//! CPU backend, render a panel-sized screen, and compare its PPM with a golden.

const std = @import("std");
const abi = @import("abi");
const host = @import("host");

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {}
export fn ra8_box_tree_init(_: *abi.core.BoxTree, _: [*]abi.core.Box, _: u16) callconv(.c) u16 {
    return 0;
}
export fn ra8_box_add(_: *abi.core.BoxTree, _: i16, _: *const abi.core.Box) callconv(.c) i16 {
    return -1;
}
export fn ra8_box_layout(_: *abi.core.BoxTree, _: i16, _: *const abi.types.Rect) callconv(.c) u16 {
    return 0;
}
export fn ra8_ui_rect_contains(rect: *const abi.types.Rect, x: i32, y: i32) callconv(.c) bool {
    return x >= rect.x and y >= rect.y and x < rect.x + rect.w and y < rect.y + rect.h;
}

const expected = @embedFile("golden/font_faces.ppm");

fn widget(rect: abi.label.Rect) abi.label.Widget {
    return .{ .vt = null, .ctx = null, .rect = rect, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}

test "host backend renders selectable serif and sans faces into panel golden" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{
        .user = &canvas,
        .fill_rect = host.Canvas.fillRect,
        .draw_text = host.Canvas.drawText,
        .text_size = host.Canvas.textSize,
        .draw_text_face = host.Canvas.drawTextFace,
        .text_size_face = host.Canvas.textSizeFace,
    };

    var title = abi.label.Label{ .paint = &paint, .text = "A Quiet Reader", .fg = 0x111111, .bg = 0xffffff, .pad = 24, .alignment = .left, .face = .serif };
    var title_widget = widget(.{ .x = 64, .y = 96, .w = 944, .h = 120 });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&title_widget, &title));
    title_widget.vt.?.render.?(&title_widget);

    var subtitle = abi.label.Label{ .paint = &paint, .text = "Settings | Books | Listen", .fg = 0x222222, .bg = 0xffffff, .pad = 24, .alignment = .left, .face = .sans };
    var subtitle_widget = widget(.{ .x = 64, .y = 264, .w = 944, .h = 120 });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&subtitle_widget, &subtitle));
    subtitle_widget.vt.?.render.?(&subtitle_widget);

    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    var temp = std.testing.tmpDir(.{});
    defer temp.cleanup();
    try temp.dir.writeFile(.{ .sub_path = "rendered.ppm", .data = rendered });
    const written = try temp.dir.readFileAlloc(allocator, "rendered.ppm", rendered.len);
    defer allocator.free(written);
    try std.testing.expectEqualSlices(u8, expected, written);
}
