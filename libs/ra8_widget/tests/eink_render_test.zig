//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host parity checks proving the same `ra8_widget` code renders identically
//! in luminance onto the chip e-ink gray4 panel backend (`eink_panel_paint`)
//! and the desktop argb8888 surface backend (`host_surface_paint`), while
//! preserving e-ink blue-noise dithering and declared capabilities.

const std = @import("std");
const abi = @import("abi");
const eink_paint = @import("eink_paint");
const surface_paint = @import("surface_paint");
const gfx = @import("ra8_gfx");

comptime {
    _ = @import("host_box_shims.zig");
    _ = @import("host_box_shims.zig").Shims(abi);
}

fn blankWidget() abi.types.Widget {
    return .{
        .vt = null,
        .ctx = null,
        .rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = true,
        .dirty = true,
    };
}

fn renderSampleTree(paint: *const abi.types.Paint, frame: abi.types.Rect) ![5]abi.types.Rect {
    var status = abi.status_bar.StatusBar{
        .paint = paint,
        .left = "Library",
        .right = "12:30",
        .bg = 0xFFFFFF,
        .fg = 0x000000,
        .fg_right = 0x404040,
        .rule = 0x000000,
        .pad = 6,
        .rule_h = 1,
    };
    var status_w = blankWidget();
    try std.testing.expectEqual(abi.types.err.ok, abi.status_bar.ra8_widget_status_bar_init(&status_w, &status));
    status_w.fixed = 28;

    var label = abi.label.Label{
        .paint = paint,
        .text = "Paper UI",
        .fg = 0x000000,
        .bg = 0xFFFFFF,
        .pad = 8,
        .alignment = .left,
        .face = .serif,
    };
    var label_w = blankWidget();
    try std.testing.expectEqual(abi.types.err.ok, abi.label.ra8_widget_label_init(&label_w, &label));
    label_w.fixed = 40;

    var button = abi.button.Button{
        .paint = paint,
        .text = "Read",
        .on_press = null,
        .fg = 0xFFFFFF,
        .face = 0x000000,
        .face_pressed = 0x202020,
        .border = 0x000000,
        .presses = 0,
        .pad = 6,
        .border_w = 2,
        .alignment = .center,
        .pressed = false,
        .reserved = 0,
    };
    var button_w = blankWidget();
    try std.testing.expectEqual(abi.types.err.ok, abi.button.ra8_widget_button_init(&button_w, &button));
    button_w.fixed = 40;

    var progress = abi.progress_bar.ProgressBar{
        .paint = paint,
        .track = 0xE0E0E0,
        .fill = 0x808080,
        .value = 1,
        .total = 2,
    };
    var progress_w = blankWidget();
    try std.testing.expectEqual(abi.types.err.ok, abi.progress_bar.ra8_widget_progress_bar_init(&progress_w, &progress));
    progress_w.fixed = 16;

    const img_px = [_]u8{ 0x00, 0x00, 0xFF, 0xFF };
    var image = abi.image_widget.ImageWidget{
        .paint = paint,
        .pixels = &img_px,
        .width = 2,
        .height = 2,
        .scale = .fit,
        .placeholder_fill = 0xFF,
        .placeholder_border = 0x00,
        .reserved = 0,
        .placeholder_border_width = 1,
    };
    var image_w = blankWidget();
    try std.testing.expectEqual(abi.types.err.ok, abi.image_widget.ra8_widget_image_init(&image_w, &image));
    image_w.fixed = 2;

    var kids = [_]abi.types.Widget{ status_w, label_w, button_w, progress_w, image_w };
    var scratch: [6]abi.core.Box = @splat(.{});
    var panel = abi.panel.Panel{
        .children = &kids,
        .box_scratch = @ptrCast(&scratch),
        .count = kids.len,
        .box_cap = scratch.len,
        .gap = 4,
        .pad = 4,
        .axis = .col,
        .reserved = 0,
        .paint = paint,
        .bg = 0xFFFFFF,
    };
    var root_w = blankWidget();
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_init(&root_w, &panel));
    var damage: abi.types.Rect = undefined;
    var hint: abi.types.Refresh = .none;
    var dirty: u16 = 0;
    try std.testing.expectEqual(abi.types.err.ok, abi.panel.ra8_widget_panel_compose(&root_w, &frame, &damage, &hint, &dirty));
    return .{ kids[0].rect, kids[1].rect, kids[2].rect, kids[3].rect, kids[4].rect };
}

test "eink Paint declares gray4 + dither caps while desktop Paint declares colour + alpha" {
    var fb: [8]u8 = @splat(0);
    var eink = try gfx.EinkSurface.init(&fb, 4, 4, 0, 0xFFFFFF, null);
    const panel_paint = eink_paint.table(abi.types.Paint, &eink);

    try std.testing.expect(panel_paint.fill_rect != null);
    try std.testing.expect(panel_paint.draw_line != null);
    try std.testing.expect(panel_paint.blit != null);
    try std.testing.expect(panel_paint.push_clip != null);
    try std.testing.expect(panel_paint.pop_clip != null);
    try std.testing.expect(panel_paint.draw_text_style != null);
    try std.testing.expect(panel_paint.text_size_style != null);
    try std.testing.expect(panel_paint.fill_rect_alpha == null);

    const caps = panel_paint.caps.?;
    try std.testing.expectEqual(abi.types.PixelFormat.gray4, caps.pixel_format);
    try std.testing.expectEqual(@as(u8, 0), caps.color);
    try std.testing.expectEqual(@as(u8, 0), caps.alpha);
    try std.testing.expectEqual(@as(u8, 1), caps.gray4);
    try std.testing.expectEqual(@as(u8, 1), caps.dither);
    try std.testing.expectEqual(@as(u8, 1), caps.line);
    try std.testing.expectEqual(@as(u8, 1), caps.blit);
    try std.testing.expectEqual(@as(u8, 1), caps.clip);
}

test "same widget code renders in parity on desktop RGBA Surface and chip e-ink gray4 panel" {
    const allocator = std.testing.allocator;
    const width: u32 = 160;
    const height: u32 = 160;
    const frame: abi.types.Rect = .{ .x = 0, .y = 0, .w = width, .h = height };

    var desktop = try gfx.Surface.init(allocator, width, height, 0xFFFFFF);
    defer desktop.deinit(allocator);
    const desktop_paint = surface_paint.table(abi.types.Paint, &desktop);
    const desktop_rects = try renderSampleTree(&desktop_paint, frame);

    var panel_buf: [80 * 160]u8 = @splat(0);
    var eink = try gfx.EinkSurface.init(&panel_buf, width, height, 0, 0xFFFFFF, null);
    const panel_paint = eink_paint.table(abi.types.Paint, &eink);
    const eink_rects = try renderSampleTree(&panel_paint, frame);
    try std.testing.expectEqualSlices(abi.types.Rect, &desktop_rects, &eink_rects);

    // Both renderers land the black button face cleanly, white background cleanly,
    // progress fill vs track contrast cleanly, and mid-gray dithered bar fills cleanly.
    var y: u32 = 0;
    var max_error: i32 = 0;
    while (y < height) : (y += 1) {
        var x: u32 = 0;
        while (x < width) : (x += 1) {
            const rgb_luma: i32 = gfx.eink_surface.colorLuma(desktop.at(x, y));
            const eink_luma: i32 = eink.gray8At(x, y);
            const diff = @as(i32, @intCast(@abs(rgb_luma - eink_luma)));
            if (diff > max_error) max_error = diff;
        }
    }
    // Ordered blue-noise dithering steps at most one 4-bit quantization interval (~17)
    // across uniform fills and blended 2-bit glyph edges; verify per-pixel deviation stays bounded.
    try std.testing.expect(max_error <= 24);

    // Status bar hairline and solid black button interior match on both backends.
    const status_r = eink_rects[0];
    const rule_y: u32 = @intCast(status_r.y + status_r.h - 1);
    const rule_x: u32 = @intCast(status_r.x + 10);
    try std.testing.expectEqual(@as(u4, 0), eink.nibbleAt(rule_x, rule_y));
    try std.testing.expectEqual(@as(u32, 0xFF00_0000), desktop.at(rule_x, rule_y));

    // Mid-gray filled region of progress bar exhibits both level 7 and level 8 dither nibbles on chip panel.
    const prog_r = eink_rects[3];
    const prog_y: u32 = @intCast(prog_r.y + @divTrunc(prog_r.h, 2));
    var saw_7 = false;
    var saw_8 = false;
    var px: u32 = @intCast(prog_r.x + 2);
    const prog_end: u32 = @intCast(prog_r.x + @divTrunc(prog_r.w, 2) - 2);
    while (px < prog_end) : (px += 1) {
        const lvl = eink.nibbleAt(px, prog_y);
        if (lvl == 7) saw_7 = true;
        if (lvl == 8) saw_8 = true;
    }
    try std.testing.expect(saw_7 and saw_8);

    // Image widget lands top row black and bottom row white centered under fit scale.
    const img_r = eink_rects[4];
    const mid_x: u32 = @intCast(img_r.x + @divTrunc(img_r.w, 2));
    try std.testing.expectEqual(@as(u4, 0), eink.nibbleAt(mid_x, @intCast(img_r.y)));
    try std.testing.expectEqual(@as(u4, 15), eink.nibbleAt(mid_x, @intCast(img_r.y + img_r.h - 1)));
}
