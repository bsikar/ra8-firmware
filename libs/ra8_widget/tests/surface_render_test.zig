//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The widget tree rendered through ra8_gfx's host argb8888 surface (RA8FW-950):
//! a colour label and buttons, a blended overlay, a line and a clip, checked against a golden.

const std = @import("std");
const abi = @import("abi");
const surface_paint = @import("surface_paint");
const gfx = @import("ra8_gfx");

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

const golden = @embedFile("golden/surface_widgets.ppm");
const golden_path = "tests/golden/surface_widgets.ppm";

fn widget(rect: abi.label.Rect) abi.label.Widget {
    return .{ .vt = null, .ctx = null, .rect = rect, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}

fn render(surface: *gfx.Surface) !void {
    const paint = surface_paint.table(abi.types.Paint, surface);
    var title = abi.label.Label{ .paint = &paint, .text = "Desktop colour", .fg = 0xE2E8F0, .bg = 0x0F172A, .pad = 12, .alignment = .left, .face = .serif };
    var title_widget = widget(.{ .x = 16, .y = 16, .w = 448, .h = 64 });
    try std.testing.expectEqual(abi.label.err.ok, abi.label.ra8_widget_label_init(&title_widget, &title));
    title_widget.vt.?.render.?(&title_widget);
    var run = abi.button.Button{ .paint = &paint, .text = "Run", .on_press = null, .fg = 0xF8FAFC, .face = 0x0284C7, .face_pressed = 0x0369A1, .border = 0x38BDF8, .presses = 0, .pad = 8, .border_w = 2, .alignment = .center, .pressed = false, .reserved = 0, .text_size = .ui_26 };
    var run_widget = widget(.{ .x = 16, .y = 96, .w = 200, .h = 72 });
    try std.testing.expectEqual(abi.button.err.ok, abi.button.ra8_widget_button_init(&run_widget, &run));
    run_widget.vt.?.render.?(&run_widget);
    var stop = run;
    stop.text = "Stop";
    stop.face = 0xDC2626;
    stop.border = 0xF87171;
    var stop_widget = widget(.{ .x = 264, .y = 96, .w = 200, .h = 72 });
    try std.testing.expectEqual(abi.button.err.ok, abi.button.ra8_widget_button_init(&stop_widget, &stop));
    stop_widget.vt.?.render.?(&stop_widget);
    // A translucent highlight over the second button, a rule clipped to the
    // left half, and a blit of a two-tone argb strip with a clear pixel.
    paint.fill_rect_alpha.?(paint.user, 240, 88, 232, 88, 0xFACC15, 96);
    paint.push_clip.?(paint.user, 0, 0, 240, 200);
    paint.draw_line.?(paint.user, 16, 184, 464, 184, 0x22C55E);
    paint.pop_clip.?(paint.user);
    const strip = [_]u8{ 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00 };
    var x: i32 = 400;
    while (x < 460) : (x += 3) paint.blit.?(paint.user, x, 188, 3, 1, &strip, 12, 4);
}

test "a widget tree renders in colour through the host surface into its golden" {
    const allocator = std.testing.allocator;
    var surface = try gfx.Surface.init(allocator, 480, 200, 0x0F172A);
    defer surface.deinit(allocator);
    try render(&surface);
    const rendered = try surface.ppm(allocator);
    defer allocator.free(rendered);
    if (std.testing.environ.getAlloc(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = golden_path, .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, golden, rendered);
    }
}

test "the surface table binds every Paint member" {
    var surface = try gfx.Surface.init(std.testing.allocator, 4, 4, 0);
    defer surface.deinit(std.testing.allocator);
    const paint = surface_paint.table(abi.types.Paint, &surface);
    inline for (comptime std.meta.fieldNames(abi.types.Paint)) |name| {
        try std.testing.expect(@field(paint, name) != null);
    }
    var w: i32 = 0;
    var h: i32 = 0;
    paint.text_size.?(paint.user, "Run", &w, &h);
    try std.testing.expect(w > 0 and h > 0);
    paint.blit.?(paint.user, 0, 0, 1, 1, &[_]u8{0}, 1, 9);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), surface.at(0, 0));
}
