//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The widget tree rendered through ra8_gfx's host argb8888 surface (RA8FW-950):
//! a colour label and buttons, a blended overlay, a line and a clip, checked against a golden.

const std = @import("std");
const abi = @import("abi");
const surface_paint = @import("surface_paint");
const gfx = @import("ra8_gfx");

comptime {
    _ = @import("host_box_shims.zig");
    _ = @import("host_box_shims.zig").Shims(abi);
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

test "the surface table binds every Paint member and desktop capabilities" {
    var surface = try gfx.Surface.init(std.testing.allocator, 4, 4, 0);
    defer surface.deinit(std.testing.allocator);
    const paint = surface_paint.table(abi.types.Paint, &surface);
    inline for (comptime std.meta.fieldNames(abi.types.Paint)) |name| {
        try std.testing.expect(@field(paint, name) != null);
    }
    const caps = paint.caps.?;
    try std.testing.expectEqual(abi.types.PixelFormat.argb8888, caps.pixel_format);
    try std.testing.expectEqual(@as(u8, 1), caps.color);
    try std.testing.expectEqual(@as(u8, 1), caps.alpha);
    try std.testing.expectEqual(@as(u8, 0), caps.gray4);
    try std.testing.expectEqual(@as(u8, 0), caps.dither);
    try std.testing.expectEqual(@as(u8, 1), caps.line);
    try std.testing.expectEqual(@as(u8, 1), caps.blit);
    try std.testing.expectEqual(@as(u8, 1), caps.clip);
    var w: i32 = 0;
    var h: i32 = 0;
    paint.text_size.?(paint.user, "Run", &w, &h);
    try std.testing.expect(w > 0 and h > 0);
    paint.blit.?(paint.user, 0, 0, 1, 1, &[_]u8{0}, 1, 9);
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), surface.at(0, 0));
}

fn renderPageFill(user: ?*anyopaque, _: u16, body: *const abi.types.Rect) callconv(.c) void {
    const paint: *const abi.types.Paint = @ptrCast(@alignCast(user orelse return));
    // Intentionally overshoot the body rect; clip capability must contain it.
    paint.fill_rect.?(paint.user, body.x - 10, body.y - 10, body.w + 20, body.h + 20, 0x38BDF8);
}

test "generic widgets paint through shared Paint ops on host surface" {
    const allocator = std.testing.allocator;
    var surface = try gfx.Surface.init(allocator, 320, 260, 0x112233);
    defer surface.deinit(allocator);
    const paint = surface_paint.table(abi.types.Paint, &surface);

    var sbar = abi.status_bar.StatusBar{
        .paint = &paint,
        .left = "Home",
        .right = "12:00",
        .bg = 0x1E293B,
        .fg = 0xF8FAFC,
        .fg_right = 0x94A3B8,
        .rule = 0x38BDF8,
        .pad = 8,
        .rule_h = 1,
    };
    var sbar_w = widget(.{ .x = 0, .y = 0, .w = 320, .h = 28 });
    try std.testing.expectEqual(abi.types.err.ok, abi.status_bar.ra8_widget_status_bar_init(&sbar_w, &sbar));
    sbar_w.vt.?.render.?(&sbar_w);
    try std.testing.expectEqual(@as(u32, 0xFF38_BDF8), surface.at(160, 27));

    var tog = abi.toggle.Toggle{
        .paint = &paint,
        .label = "Sync",
        .fg = 0xF8FAFC,
        .bg = 0x112233,
        .border = 0x64748B,
        .mark = 0x22C55E,
        .box_size = 18,
        .gap = 8,
        .checked = true,
        .reserved = .{ 0, 0, 0 },
    };
    var tog_w = widget(.{ .x = 12, .y = 36, .w = 120, .h = 24 });
    try std.testing.expectEqual(abi.types.err.ok, abi.toggle.ra8_widget_toggle_init(&tog_w, &tog));
    tog_w.vt.?.render.?(&tog_w);
    try std.testing.expectEqual(@as(u32, 0xFF22_C55E), surface.at(21, 48));

    const seg_labels = [_][*:0]const u8{ "A", "B" };
    var seg = abi.segmented.Segmented{
        .paint = &paint,
        .labels = &seg_labels,
        .fg = 0x94A3B8,
        .selected_fg = 0xFFFFFF,
        .bg = 0x1E293B,
        .selected_bg = 0x0284C7,
        .border = 0x475569,
        .count = 2,
        .selected = 1,
        .pad = 4,
    };
    var seg_w = widget(.{ .x = 150, .y = 36, .w = 150, .h = 24 });
    try std.testing.expectEqual(abi.types.err.ok, abi.segmented.ra8_widget_segmented_init(&seg_w, &seg));
    seg_w.vt.?.render.?(&seg_w);
    try std.testing.expectEqual(@as(u32, 0xFF02_84C7), surface.at(290, 48));

    var pbar = abi.progress_bar.ProgressBar{
        .paint = &paint,
        .track = 0x334155,
        .fill = 0xF59E0B,
        .value = 1,
        .total = 2,
    };
    var pbar_w = widget(.{ .x = 12, .y = 70, .w = 200, .h = 10 });
    try std.testing.expectEqual(abi.types.err.ok, abi.progress_bar.ra8_widget_progress_bar_init(&pbar_w, &pbar));
    pbar_w.vt.?.render.?(&pbar_w);
    try std.testing.expectEqual(@as(u32, 0xFFF5_9E0B), surface.at(50, 75));
    try std.testing.expectEqual(@as(u32, 0xFF33_4155), surface.at(180, 75));

    const rows = [_]abi.list.Row{
        .{ .title = "Brightness", .subtitle = "Warm", .trailing_text = "80%", .action_id = 1, .trailing = .value },
    };
    var list = abi.list.List{
        .paint = &paint,
        .rows = &rows,
        .count = 1,
        .on_select = null,
        .bg = 0x0F172A,
        .title_fg = 0xF8FAFC,
        .subtitle_fg = 0x94A3B8,
        .trailing_fg = 0x38BDF8,
        .divider = 0x475569,
        .row_height = 56,
        .pad = 4,
        .selected = 0,
        .has_selection = false,
        .damage = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
    };
    var list_w = widget(.{ .x = 12, .y = 90, .w = 296, .h = 56 });
    try std.testing.expectEqual(abi.types.err.ok, abi.list.ra8_widget_list_init(&list_w, &list));
    list_w.vt.?.render.?(&list_w);
    try std.testing.expectEqual(@as(u32, 0xFF47_5569), surface.at(13, 145));
    try std.testing.expectEqual(@as(u32, 0xFF47_5569), surface.at(200, 145));

    const nav_items = [_]?[*:0]const u8{"Home"};
    const nav_icons = [_]abi.nav_bar.Icon{.home};
    var nav = abi.nav_bar.NavBar{
        .paint = &paint,
        .items = &nav_items,
        .on_select = null,
        .bg = 0x1E293B,
        .fg_active = 0xF8FAFC,
        .fg_muted = 0x64748B,
        .count = 1,
        .active = 0,
        .selected = 0,
        .icons = &nav_icons,
    };
    var nav_w = widget(.{ .x = 12, .y = 156, .w = 120, .h = 40 });
    try std.testing.expectEqual(abi.types.err.ok, abi.nav_bar.ra8_widget_nav_bar_init(&nav_w, &nav));
    nav_w.vt.?.render.?(&nav_w);

    const img_px = [_]u8{ 0x80, 0x80, 0x80, 0x80 };
    var img = abi.image_widget.ImageWidget{
        .paint = &paint,
        .pixels = &img_px,
        .width = 2,
        .height = 2,
        .scale = .fill,
        .placeholder_fill = 0xFF,
        .placeholder_border = 0x20,
        .reserved = 0,
        .placeholder_border_width = 1,
    };
    var img_w = widget(.{ .x = 150, .y = 156, .w = 2, .h = 2 });
    try std.testing.expectEqual(abi.types.err.ok, abi.image_widget.ra8_widget_image_init(&img_w, &img));
    img_w.vt.?.render.?(&img_w);
    try std.testing.expectEqual(@as(u32, 0xFF80_8080), surface.at(150, 156));

    var r_ops = abi.reflow_view.Ops{
        .user = @constCast(&paint),
        .render_page = renderPageFill,
        .follow_link = null,
    };
    var view = abi.reflow_view.ReflowView{
        .paint = &paint,
        .ops = &r_ops,
        .bg = 0x1E293B,
        .page = 0,
        .page_count = 1,
        .margin_x = 10,
        .margin_y = 6,
    };
    var view_w = widget(.{ .x = 180, .y = 156, .w = 60, .h = 40 });
    try std.testing.expectEqual(abi.types.err.ok, abi.reflow_view.ra8_widget_reflow_view_init(&view_w, &view));
    view_w.vt.?.render.?(&view_w);
    try std.testing.expectEqual(@as(u32, 0xFF1E_293B), surface.at(182, 158));
    try std.testing.expectEqual(@as(u32, 0xFF38_BDF8), surface.at(200, 170));
}

fn countBlit(_: ?*anyopaque, _: i32, _: i32, _: i32, _: i32, _: [*]const u8, _: i32, _: u8) callconv(.c) void {
    @panic("image renderer must skip blit when capabilities advertise blit=0");
}

fn noopFill(_: ?*anyopaque, _: i32, _: i32, _: i32, _: i32, _: u32) callconv(.c) void {}

test "widgets respect explicit capabilities on fallback backends" {
    const no_blit_caps = abi.types.CanvasCaps{ .pixel_format = .gray8, .blit = 0 };
    const panel_paint = abi.types.Paint{
        .user = null,
        .fill_rect = noopFill,
        .draw_text = null,
        .text_size = null,
        .blit = countBlit,
        .caps = &no_blit_caps,
    };
    const px = [_]u8{ 0x10, 0x20, 0x30, 0x40 };
    var img = abi.image_widget.ImageWidget{
        .paint = &panel_paint,
        .pixels = &px,
        .width = 2,
        .height = 2,
        .scale = .fill,
        .placeholder_fill = 0xFF,
        .placeholder_border = 0x20,
        .reserved = 0,
        .placeholder_border_width = 1,
    };
    var img_w = widget(.{ .x = 0, .y = 0, .w = 2, .h = 2 });
    try std.testing.expectEqual(abi.types.err.ok, abi.image_widget.ra8_widget_image_init(&img_w, &img));
    img_w.vt.?.render.?(&img_w);
}
