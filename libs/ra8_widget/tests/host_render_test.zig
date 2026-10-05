//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! End-to-end host preview check: bind real label and button widgets to the
//! CPU backend, render a panel-sized screen, and compare its PPM with a golden.

const std = @import("std");
const abi = @import("abi");
const host = @import("host");

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {}
export fn ra8_box_tree_init(tree: *abi.core.BoxTree, storage: [*]abi.core.Box, cap: u16) callconv(.c) u16 {
    tree.nodes = storage;
    tree.cap = cap;
    tree.count = 0;
    return 0;
}
export fn ra8_box_add(tree: *abi.core.BoxTree, _: i16, node: *const abi.core.Box) callconv(.c) i16 {
    if (tree.count >= tree.cap) return -1;
    const index = tree.count;
    tree.nodes.?[index] = node.*;
    tree.count += 1;
    return @intCast(index);
}
export fn ra8_box_layout(tree: *abi.core.BoxTree, root: i16, frame: *const abi.types.Rect) callconv(.c) u16 {
    const nodes = tree.nodes.?[0..tree.count];
    const root_index: usize = @intCast(root);
    nodes[root_index].rect = frame.*;
    const stack = nodes[root_index];
    const children = nodes.len - 1;
    if (children == 0) return 0;
    const vertical = stack.kind == abi.core.box.stack_v;
    const extent = if (vertical) frame.h else frame.w;
    const inner = @max(extent - 2 * @as(i32, stack.pad) - @as(i32, stack.gap) * @as(i32, @intCast(children - 1)), 0);
    const cell = @divTrunc(inner, @as(i32, @intCast(children)));
    for (nodes[1..], 0..) |*child, index| {
        const offset = @as(i32, stack.pad) + @as(i32, @intCast(index)) * (cell + @as(i32, stack.gap));
        child.rect = if (vertical)
            .{ .x = frame.x + stack.pad, .y = frame.y + offset, .w = frame.w - 2 * stack.pad, .h = cell }
        else
            .{ .x = frame.x + offset, .y = frame.y + stack.pad, .w = cell, .h = frame.h - 2 * stack.pad };
    }
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
const reading_sizes_expected = @embedFile("golden/reading_sizes.ppm");
const display_sizes_expected = @embedFile("golden/display_sizes.ppm");

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
    if (std.process.getEnvVarOwned(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.fs.cwd().writeFile(.{ .sub_path = "tests/golden/font_faces.ppm", .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, expected, written);
    }
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

test "host backend renders the five reading sizes with matching face metrics" {
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
    var previous_w: i32 = 0;
    var previous_h: i32 = 0;
    for (1..6) |size_value| {
        const size: abi.paint.TextSize = @enumFromInt(size_value);
        const label_text: [*:0]const u8 = switch (size_value) {
            1 => "Size 1",
            2 => "Size 2",
            3 => "Size 3",
            4 => "Size 4",
            else => "Size 5",
        };
        var label = abi.label.Label{
            .paint = &paint,
            .text = label_text,
            .fg = 0x111111,
            .bg = 0xffffff,
            .pad = 0,
            .alignment = .center,
            .face = .serif,
            .weight = .regular,
            .size = size,
        };
        const y: i32 = 120 + @as(i32, @intCast(size_value - 1)) * 250;
        var label_widget = widget(.{ .x = 72, .y = y, .w = 928, .h = 180 });
        try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&label_widget, &label));
        label_widget.vt.?.render.?(&label_widget);
        var measured_w: i32 = 0;
        var measured_h: i32 = 0;
        host.Canvas.textSizeStyle(&canvas, label_text, @intFromEnum(abi.paint.Face.serif), @intFromEnum(abi.paint.Weight.regular), @intFromEnum(size), &measured_w, &measured_h);
        try std.testing.expect(measured_w > previous_w);
        try std.testing.expect(measured_h > previous_h);
        previous_w = measured_w;
        previous_h = measured_h;
    }
    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    if (std.process.getEnvVarOwned(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.fs.cwd().writeFile(.{ .sub_path = "tests/golden/reading_sizes.ppm", .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, reading_sizes_expected, rendered);
    }
}

test "host backend renders bold text at a non-default reading size" {
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
    var label = abi.label.Label{
        .paint = &paint,
        .text = "Bold size four",
        .fg = 0x111111,
        .bg = 0xffffff,
        .pad = 8,
        .alignment = .left,
        .face = .serif,
        .weight = .bold,
        .size = .size_4,
    };
    var label_widget = widget(.{ .x = 40, .y = 80, .w = 900, .h = 100 });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&label_widget, &label));
    label_widget.vt.?.render.?(&label_widget);
    try std.testing.expect(std.mem.indexOfNone(u8, canvas.pixels, &.{255}) != null);
}

test "host backend renders native display text sizes into panel golden" {
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
    const Sample = struct {
        text: [*:0]const u8,
        face: abi.paint.Face,
        weight: abi.paint.Weight,
        size: abi.paint.TextSize,
        y: i32,
    };
    const samples = [_]Sample{
        .{ .text = "Large body at 38 px", .face = .serif, .weight = .regular, .size = .body_38, .y = 64 },
        .{ .text = "Large body at 38 px", .face = .sans, .weight = .bold, .size = .body_38, .y = 150 },
        .{ .text = "A Quiet Reader", .face = .serif, .weight = .bold, .size = .title_68, .y = 260 },
        .{ .text = "Screen title", .face = .sans, .weight = .regular, .size = .title_68, .y = 440 },
        .{ .text = "09:41", .face = .serif, .weight = .regular, .size = .clock_120, .y = 640 },
        .{ .text = "09:41", .face = .sans, .weight = .bold, .size = .clock_120, .y = 890 },
    };
    for (samples) |sample| {
        var label = abi.label.Label{
            .paint = &paint,
            .text = sample.text,
            .fg = 0x111111,
            .bg = 0xffffff,
            .pad = 24,
            .alignment = .center,
            .face = sample.face,
            .weight = sample.weight,
            .size = sample.size,
        };
        var label_widget = widget(.{ .x = 64, .y = sample.y, .w = 944, .h = 180 });
        try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&label_widget, &label));
        label_widget.vt.?.render.?(&label_widget);
        var width: i32 = 0;
        var height: i32 = 0;
        host.Canvas.textSizeStyle(&canvas, sample.text, @intFromEnum(sample.face), @intFromEnum(sample.weight), @intFromEnum(sample.size), &width, &height);
        try std.testing.expect(width > 0);
        try std.testing.expect(height >= @as(i32, switch (sample.size) {
            .body_38 => 38,
            .title_68 => 68,
            .clock_120 => 120,
            else => 0,
        }));
    }
    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    if (std.process.getEnvVarOwned(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.fs.cwd().writeFile(.{ .sub_path = "tests/golden/display_sizes.ppm", .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, display_sizes_expected, rendered);
    }
}

test "a serif label survives the image widget below it clearing its own rect" {
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
    const line_y: i32 = 500;
    var text_w: i32 = 0;
    var text_h: i32 = 0;
    host.Canvas.textSizeStyle(&canvas, "Hj", @intFromEnum(abi.paint.Face.serif), @intFromEnum(abi.paint.Weight.regular), @intFromEnum(abi.paint.TextSize.size_3), &text_w, &text_h);
    var label = abi.label.Label{
        .paint = &paint,
        .text = "Hj",
        .fg = 0x111111,
        .bg = 0xffffff,
        .pad = 0,
        .alignment = .left,
        .face = .serif,
    };
    var label_widget = widget(.{ .x = 100, .y = line_y, .w = text_w, .h = text_h });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&label_widget, &label));
    label_widget.vt.?.render.?(&label_widget);

    abi.image.render(
        &paint,
        .{ .x = 100, .y = line_y + text_h, .w = text_w, .h = 64 },
        null,
        .fit,
        .{ .fill = 0, .border = 0, .border_width = 0 },
    );

    var inked_label_pixels: usize = 0;
    var y: usize = @intCast(line_y);
    while (y < @as(usize, @intCast(line_y + text_h))) : (y += 1) {
        var x: usize = 100;
        while (x < 100 + @as(usize, @intCast(text_w))) : (x += 1) {
            if (canvas.pixels[y * canvas.width + x] < 255) inked_label_pixels += 1;
        }
    }
    try std.testing.expect(inked_label_pixels > 0);
}

const panel_recompose_expected = @embedFile("golden/panel_recompose.ppm");

test "full panel compose clears gaps left by the previous screen" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 128, 96, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{
        .user = &canvas,
        .fill_rect = host.Canvas.fillRect,
        .draw_text = null,
        .text_size = null,
    };

    var first = abi.label.Label{ .paint = &paint, .text = null, .fg = 0, .bg = 0x333333, .pad = 0, .alignment = .left, .face = .sans };
    var second = abi.label.Label{ .paint = &paint, .text = null, .fg = 0, .bg = 0x666666, .pad = 0, .alignment = .left, .face = .sans };
    var kids = [_]abi.label.Widget{ widget(.{ .x = 0, .y = 0, .w = 0, .h = 0 }), widget(.{ .x = 0, .y = 0, .w = 0, .h = 0 }) };
    try std.testing.expectEqual(abi.types.err.ok, abi.label.ra8_widget_label_init(&kids[0], &first));
    try std.testing.expectEqual(abi.types.err.ok, abi.label.ra8_widget_label_init(&kids[1], &second));
    kids[1].visible = false;

    var scratch: [3]abi.core.Box = @splat(.{});
    var descriptor = abi.panel.Panel{
        .children = &kids,
        .box_scratch = @ptrCast(&scratch),
        .count = 2,
        .box_cap = 3,
        .gap = 8,
        .pad = 0,
        .axis = .col,
        .reserved = 0,
        .paint = &paint,
        .bg = 0xffffff,
    };
    var panel_widget = widget(.{ .x = 0, .y = 0, .w = 0, .h = 0 });
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_init(&panel_widget, &descriptor));

    const frame: abi.types.Rect = .{ .x = 0, .y = 0, .w = 128, .h = 96 };
    var damage: abi.types.Rect = undefined;
    var hint: abi.types.Refresh = .none;
    var dirty: u16 = 0;
    kids[0].dirty = true;
    kids[0].refresh = @intFromEnum(abi.types.Refresh.quality);
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_compose(&panel_widget, &frame, &damage, &hint, &dirty));

    descriptor.pad = 10;
    kids[0].dirty = true;
    kids[0].refresh = @intFromEnum(abi.types.Refresh.quality);
    kids[1].visible = true;
    kids[1].dirty = true;
    kids[1].refresh = @intFromEnum(abi.types.Refresh.quality);
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_compose(&panel_widget, &frame, &damage, &hint, &dirty));

    var expected_canvas = try host.Canvas.init(allocator, 128, 96, 255);
    defer expected_canvas.deinit(allocator);
    host.Canvas.fillRect(&expected_canvas, kids[0].rect.x, kids[0].rect.y, kids[0].rect.w, kids[0].rect.h, first.bg);
    host.Canvas.fillRect(&expected_canvas, kids[1].rect.x, kids[1].rect.y, kids[1].rect.w, kids[1].rect.h, second.bg);
    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    const expected_ppm = try expected_canvas.ppm(allocator);
    defer allocator.free(expected_ppm);
    if (std.process.getEnvVarOwned(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.fs.cwd().writeFile(.{ .sub_path = "tests/golden/panel_recompose.ppm", .data = expected_ppm });
    } else |_| {
        try std.testing.expectEqualSlices(u8, panel_recompose_expected, expected_ppm);
    }
    try std.testing.expectEqualSlices(u8, panel_recompose_expected, rendered);
    try std.testing.expectEqual(frame, damage);

    first.bg = 0x222222;
    kids[0].dirty = true;
    kids[0].refresh = @intFromEnum(abi.types.Refresh.quality);
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_compose(&panel_widget, &frame, &damage, &hint, &dirty));
    try std.testing.expectEqual(kids[0].rect, damage);
}
