//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host golden check for the text field: empty, mid-entry and full buffers.
//! Reached from host_render_test.zig so it shares that module's imports.

const std = @import("std");
const abi = @import("abi");
const host = @import("host");

const field_empty_expected = @embedFile("golden/text_field_empty.ppm");
const field_mid_expected = @embedFile("golden/text_field_mid.ppm");
const field_full_expected = @embedFile("golden/text_field_full.ppm");

fn widget(rect: abi.label.Rect) abi.label.Widget {
    return .{ .vt = null, .ctx = null, .rect = rect, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}

fn renderTextFieldGolden(name: []const u8, buffer: []u8, len: u16, golden: []const u8) !void {
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
        .draw_text_style = host.Canvas.drawTextStyle,
        .text_size_style = host.Canvas.textSizeStyle,
    };
    var field = abi.text_field.TextField{
        .paint = &paint,
        .buffer = buffer.ptr,
        .capacity = @intCast(buffer.len),
        .len = len,
        .placeholder = "Search books",
        .fg = 0x222222,
        .bg = 0xffffff,
        .caret = 0x111111,
        .pad = 24,
        .face = .sans,
        .focused = true,
    };
    var field_widget = widget(.{ .x = 128, .y = 180, .w = 816, .h = 96 });
    try std.testing.expectEqual(abi.types.err.ok, abi.text_field.ra8_widget_text_field_init(&field_widget, &field));
    field_widget.vt.?.render.?(&field_widget);
    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    if (std.testing.environ.getAlloc(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        const path = try std.fmt.allocPrint(allocator, "tests/golden/{s}", .{name});
        defer allocator.free(path);
        try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, golden, rendered);
    }
}

test "host text-field states match empty, mid-entry and full-buffer goldens" {
    var empty: [32]u8 = @splat(0);
    try renderTextFieldGolden("text_field_empty.ppm", &empty, 0, field_empty_expected);
    var mid: [32]u8 = @splat(0);
    @memcpy(mid[0..5], "shelf");
    try renderTextFieldGolden("text_field_mid.ppm", &mid, 5, field_mid_expected);
    var full: [8]u8 = @splat(0);
    @memcpy(full[0..7], "archive");
    try renderTextFieldGolden("text_field_full.ppm", &full, 7, field_full_expected);
}
