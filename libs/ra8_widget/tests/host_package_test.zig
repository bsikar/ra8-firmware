//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host checks verifying `ra8_widget_host` (`host_package.zig`, RA8FW-1016)
//! cleanly exports the desktop RGBA backend (`DesktopSurface`, `Surface`,
//! `surface_paint`), the e-ink panel backend (`EinkSurface`, `eink_paint`),
//! and legacy gray8 `Canvas` without duplicate-module conflicts.

const std = @import("std");
const host = @import("host_pkg");

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {}

test "ra8_widget_host exposes DesktopSurface RGBA backend and SDL pixel accessors" {
    const allocator = std.testing.allocator;
    var desktop = try host.DesktopSurface.init(allocator, 120, 60, 0x0F172A);
    defer desktop.deinit(allocator);

    const caps = desktop.capabilities();
    try std.testing.expectEqual(host.PixelFormat.argb8888, desktop.pixelFormat());
    try std.testing.expect(caps.color and caps.alpha and caps.line and caps.blit and caps.clip);
    try std.testing.expect(!caps.gray4 and !caps.dither);
    try std.testing.expectEqual(@as(usize, 120 * 4), desktop.pitchBytes());
    try std.testing.expectEqual(@as(usize, 120 * 60 * 4), desktop.rgbaBytes().len);

    const paint = desktop.paint(host.widget.types.Paint);
    var button = host.widget.button.Button{
        .paint = &paint,
        .text = "Go",
        .on_press = null,
        .fg = 0xFFFFFF,
        .face = 0x2563EB,
        .face_pressed = 0x1D4ED8,
        .border = 0x60A5FA,
        .presses = 0,
        .pad = 4,
        .border_w = 2,
        .alignment = .center,
        .pressed = false,
        .reserved = 0,
    };
    var button_w = host.widget.types.Widget{
        .vt = null,
        .ctx = null,
        .rect = .{ .x = 10, .y = 10, .w = 80, .h = 30 },
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = false,
        .dirty = false,
    };
    try std.testing.expectEqual(host.widget.types.err.ok, host.widget.button.ra8_widget_button_init(&button_w, &button));
    button_w.vt.?.render.?(&button_w);
    try std.testing.expectEqual(@as(u32, 0xFF60_A5FA), desktop.at(10, 10));
    try std.testing.expectEqual(@as(u32, 0xFF25_63EB), desktop.at(13, 13));

    var rgba_out = try allocator.alloc(u8, 120 * 60 * 4);
    defer allocator.free(rgba_out);
    try desktop.writeRgbaBytes(rgba_out);
    const border_off = (10 * 120 + 10) * 4;
    try std.testing.expectEqualSlices(u8, &.{ 0x60, 0xA5, 0xFA, 0xFF }, rgba_out[border_off .. border_off + 4]);
    try std.testing.expectError(error.BufferTooSmall, desktop.writeRgbaBytes(rgba_out[0 .. rgba_out.len - 1]));

    const encoded = try desktop.ppm(allocator);
    defer allocator.free(encoded);
    try std.testing.expect(std.mem.startsWith(u8, encoded, "P6\n120 60\n255\n"));
}

test "ra8_widget_host exposes gray8 Canvas and e-ink panel Paint cleanly" {
    const allocator = std.testing.allocator;
    var canvas = try host.Canvas.init(allocator, 8, 8, 255);
    defer canvas.deinit(allocator);
    host.Canvas.fillRect(&canvas, 1, 1, 2, 2, 0x000000);
    try std.testing.expectEqual(@as(u8, 0), canvas.pixels[8 + 1]);

    var panel_buf: [4 * 8]u8 = @splat(0);
    var eink = try host.EinkSurface.init(&panel_buf, 8, 8, 0, 0xFFFFFF, null);
    const eink_p = host.eink_paint.table(host.widget.types.Paint, &eink);
    try std.testing.expectEqual(host.widget.types.PixelFormat.gray4, host.ops.capabilities(&eink_p).pixel_format);
    host.ops.line(&eink_p, 0, 0, 7, 0, 0x000000);
    try std.testing.expectEqual(@as(u4, 0), eink.nibbleAt(4, 0));
}
