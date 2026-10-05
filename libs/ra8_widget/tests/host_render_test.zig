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
const expected_weights = @embedFile("golden/font_weights.ppm");
const pager_expected = @embedFile("golden/pager.ppm");
const toggle_segmented_expected = @embedFile("golden/toggle_segmented.ppm");
const expected_image = @embedFile("golden/image_widget.ppm");
const list_expected = @embedFile("golden/list.ppm");

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

test "host backend writes panel PPM matching pager golden" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{ .user = &canvas, .fill_rect = host.Canvas.fillRect, .draw_text = host.Canvas.drawText, .text_size = host.Canvas.textSize };
    var pager = abi.pager.Pager{ .paint = &paint, .item_count = 42, .page_capacity = 10, .page = 2, .bg = 0x00FFFFFF, .fg = 0x00101010, .fg_disabled = 0x00808080 };
    var pager_widget = widget(.{ .x = 24, .y = 1280, .w = 1024, .h = 80 });
    try std.testing.expectEqual(abi.types.err.ok, abi.pager.ra8_widget_pager_init(&pager_widget, &pager));
    pager_widget.vt.?.render.?(&pager_widget);
    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    try std.testing.expectEqualSlices(u8, pager_expected, rendered);
}

test "host backend writes panel PPM matching toggle and segmented golden" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{ .user = &canvas, .fill_rect = host.Canvas.fillRect, .draw_text = host.Canvas.drawText, .text_size = host.Canvas.textSize };

    var toggle = abi.toggle.Toggle{ .paint = &paint, .label = "Wi-Fi", .fg = 0x111111, .bg = 0xffffff, .border = 0x333333, .mark = 0x111111, .box_size = 32, .gap = 16, .checked = true, .reserved = .{ 0, 0, 0 } };
    var toggle_widget = widget(.{ .x = 96, .y = 160, .w = 440, .h = 88 });
    try std.testing.expectEqual(abi.label.err.ok, abi.toggle.ra8_widget_toggle_init(&toggle_widget, &toggle));
    toggle_widget.vt.?.render.?(&toggle_widget);

    const labels = [_][*:0]const u8{ "Serif", "Sans", "Mono" };
    var segmented = abi.segmented.Segmented{ .paint = &paint, .labels = &labels, .fg = 0x111111, .selected_fg = 0xffffff, .bg = 0xffffff, .selected_bg = 0x333333, .border = 0x111111, .count = 3, .selected = 0, .pad = 8 };
    var segmented_widget = widget(.{ .x = 96, .y = 320, .w = 720, .h = 96 });
    try std.testing.expectEqual(abi.label.err.ok, abi.segmented.ra8_widget_segmented_init(&segmented_widget, &segmented));
    segmented_widget.vt.?.render.?(&segmented_widget);

    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    var temp = std.testing.tmpDir(.{});
    defer temp.cleanup();
    try temp.dir.writeFile(.{ .sub_path = "rendered.ppm", .data = rendered });
    const written = try temp.dir.readFileAlloc(allocator, "rendered.ppm", rendered.len);
    defer allocator.free(written);
    try std.testing.expectEqualSlices(u8, toggle_segmented_expected, written);
}

test "host backend writes panel PPM matching the image-widget golden" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{ .user = &canvas, .fill_rect = host.Canvas.fillRect, .draw_text = null, .text_size = null };
    const pixels = [_]u8{
        24,  48,  72,  96,  120, 144, 168, 192,
        48,  72,  96,  120, 144, 168, 192, 216,
        72,  96,  120, 144, 168, 192, 216, 232,
        96,  120, 144, 168, 192, 216, 232, 248,
        120, 144, 168, 192, 216, 232, 248, 232,
        144, 168, 192, 216, 232, 248, 232, 216,
        168, 192, 216, 232, 248, 232, 216, 192,
        192, 216, 232, 248, 232, 216, 192, 168,
    };
    abi.image.render(
        &paint,
        .{ .x = 356, .y = 420, .w = 360, .h = 360 },
        .{ .pixels = &pixels, .width = 8, .height = 8 },
        .fit,
        .{},
    );
    abi.image.render(
        &paint,
        .{ .x = 780, .y = 1030, .w = 120, .h = 160 },
        null,
        .fit,
        .{ .fill = 255, .border = 64, .border_width = 3 },
    );

    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    if (std.process.getEnvVarOwned(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.fs.cwd().writeFile(.{ .sub_path = "tests/golden/image_widget.ppm", .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, expected_image, rendered);
    }
}

test "host backend writes panel PPM matching list widget golden" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{
        .user = &canvas,
        .fill_rect = host.Canvas.fillRect,
        .draw_text = host.Canvas.drawText,
        .text_size = host.Canvas.textSize,
    };
    const rows = [_]abi.list.Row{
        .{ .title = "Settings", .subtitle = "Account and display", .trailing_text = null, .action_id = 11, .trailing = .chevron },
        .{ .title = "Listen", .subtitle = "Continue audiobook", .trailing_text = "12 min", .action_id = 22, .trailing = .value },
        .{ .title = "Activity", .subtitle = "Reading history", .trailing_text = null, .action_id = 33, .trailing = .chevron },
    };
    var list = abi.list.List{
        .paint = &paint,
        .rows = &rows,
        .count = rows.len,
        .on_select = null,
        .bg = 0xFFFFFF,
        .title_fg = 0x111111,
        .subtitle_fg = 0x666666,
        .trailing_fg = 0x333333,
        .divider = 0xDDDDDD,
        .row_height = 130,
        .pad = 18,
        .selected = 0,
        .has_selection = false,
        .damage = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
    };
    var list_widget = widget(.{ .x = 70, .y = 180, .w = 932, .h = 390 });
    try std.testing.expectEqual(abi.label.err.ok, abi.list.ra8_widget_list_init(&list_widget, &list));
    list_widget.vt.?.render.?(&list_widget);

    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    var temp = std.testing.tmpDir(.{});
    defer temp.cleanup();
    try temp.dir.writeFile(.{ .sub_path = "rendered.ppm", .data = rendered });
    const written = try temp.dir.readFileAlloc(allocator, "rendered.ppm", rendered.len);
    defer allocator.free(written);
    try std.testing.expectEqualSlices(u8, list_expected, written);
}
test "host backend renders regular and bold headings beside each other" {
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

    var regular = abi.label.Label{
        .paint = &paint,
        .text = "Screen title",
        .fg = 0x111111,
        .bg = 0xffffff,
        .pad = 24,
        .alignment = .left,
        .face = .sans,
        .weight = .regular,
    };
    var regular_widget = widget(.{ .x = 64, .y = 96, .w = 456, .h = 120 });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&regular_widget, &regular));
    regular_widget.vt.?.render.?(&regular_widget);

    var bold = regular;
    bold.weight = .bold;
    var bold_widget = widget(.{ .x = 552, .y = 96, .w = 456, .h = 120 });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&bold_widget, &bold));
    bold_widget.vt.?.render.?(&bold_widget);

    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    var temp = std.testing.tmpDir(.{});
    defer temp.cleanup();
    try temp.dir.writeFile(.{ .sub_path = "rendered.ppm", .data = rendered });
    const written = try temp.dir.readFileAlloc(allocator, "rendered.ppm", rendered.len);
    defer allocator.free(written);
    try std.testing.expectEqualSlices(u8, expected_weights, written);
}
