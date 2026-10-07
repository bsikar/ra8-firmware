//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host golden checks for the image widget in a composed panel and for nav
//! bar icons. Reached from host_render_test.zig so it shares that module's
//! imports.

const std = @import("std");
const abi = @import("abi");
const host = @import("host");
const debug = @import("debug");

fn widget(rect: abi.label.Rect) abi.label.Widget {
    return .{ .vt = null, .ctx = null, .rect = rect, .fixed = 0, .flex = 0, .action_id = 0, .refresh = 0, .visible = false, .dirty = false };
}

test "host backend composes an image widget and publishes its panel record" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{ .user = &canvas, .fill_rect = host.Canvas.fillRect, .draw_text = null, .text_size = null };
    const pixels = [_]u8{ 20, 40, 60, 80, 100, 120, 140, 160, 180 };
    var descriptor = abi.image_widget.ImageWidget{
        .paint = &paint,
        .pixels = &pixels,
        .width = 3,
        .height = 3,
        .scale = .fit,
        .placeholder_fill = 255,
        .placeholder_border = 80,
        .reserved = 0,
        .placeholder_border_width = 2,
    };
    var image_widget = widget(.{ .x = 0, .y = 0, .w = 0, .h = 0 });
    try std.testing.expectEqual(abi.types.err.ok, abi.image_widget.ra8_widget_image_init(&image_widget, &descriptor));
    image_widget.fixed = 640;
    var spacer = widget(.{ .x = 0, .y = 0, .w = 0, .h = 0 });
    spacer.visible = true;
    spacer.fixed = 808;
    var kids = [_]abi.types.Widget{ image_widget, spacer };
    try std.testing.expectEqual(abi.types.err.ok, debug.ra8_widget_debug_register(@ptrCast(&kids[0]), "cover", "image", "loaded"));
    defer _ = debug.ra8_widget_debug_unregister(@ptrCast(&kids[0]));
    var scratch: [3]abi.core.Box = @splat(.{});
    var panel = abi.panel.Panel{
        .children = &kids,
        .box_scratch = @ptrCast(&scratch),
        .count = 2,
        .box_cap = 3,
        .gap = 0,
        .pad = 0,
        .axis = .col,
        .reserved = 0,
        .paint = &paint,
        .bg = 255,
    };
    var panel_widget = widget(.{ .x = 0, .y = 0, .w = 0, .h = 0 });
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_init(&panel_widget, &panel));
    const frame: abi.types.Rect = .{ .x = 0, .y = 0, .w = 1072, .h = 1448 };
    var damage: abi.types.Rect = undefined;
    var hint: abi.types.Refresh = .none;
    var dirty: u16 = 0;
    kids[0].dirty = true;
    kids[0].refresh = @backingInt(abi.types.Refresh.quality);
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_compose(&panel_widget, &frame, &damage, &hint, &dirty));
    try std.testing.expectEqual(kids[0].rect, damage);
    try std.testing.expectEqual(@as(u16, 1), dirty);
    try std.testing.expectEqual(@as(u16, 3), debug.ra8_widget_debug_tree.count);
    try std.testing.expectEqualSlices(u8, "image", std.mem.sliceTo(&debug.ra8_widget_debug_tree.records[1].kind, 0));

    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    if (std.testing.environ.getAlloc(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = "tests/golden/image_panel.ppm", .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, @embedFile("golden/image_panel.ppm"), rendered);
    }
}

test "host backend renders nav icons into the strip golden" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 1072, 1448, 255);
    defer canvas.deinit(allocator);
    const paint = abi.types.Paint{ .user = &canvas, .fill_rect = host.Canvas.fillRect, .draw_text = host.Canvas.drawText, .text_size = host.Canvas.textSize };
    const labels = [_]?[*:0]const u8{ "Home", "Library", "Music", "Settings", "Search" };
    const icons = [_]abi.nav_bar.Icon{ .home, .library, .music, .settings, .search };
    var nav = abi.nav_bar.NavBar{
        .paint = &paint,
        .items = &labels,
        .on_select = null,
        .bg = 0xffffff,
        .fg_active = 0x202020,
        .fg_muted = 0x888888,
        .count = labels.len,
        .active = 1,
        .selected = 0xffff,
        .icons = &icons,
    };
    var nav_widget = widget(.{ .x = 0, .y = 1280, .w = 1072, .h = 120 });
    try std.testing.expectEqual(abi.types.err.ok, abi.nav_bar.ra8_widget_nav_bar_init(&nav_widget, &nav));
    nav_widget.vt.?.render.?(&nav_widget);
    const rendered = try canvas.ppm(allocator);
    defer allocator.free(rendered);
    if (std.testing.environ.getAlloc(allocator, "RA8_WIDGET_UPDATE_GOLDENS")) |update| {
        defer allocator.free(update);
        try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = "tests/golden/nav_bar_icons.ppm", .data = rendered });
    } else |_| {
        try std.testing.expectEqualSlices(u8, @embedFile("golden/nav_bar_icons.ppm"), rendered);
    }
}
