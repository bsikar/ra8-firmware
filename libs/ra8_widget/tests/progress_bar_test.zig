//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the progress-bar membrane: the descriptor layout, the bind
//! guards, and the two-fill paint for the degenerate, partial and full cases.
//! The fill-fraction maths itself is the shared helper's and is covered in
//! `internal_test.zig`; what is asserted here is which fills the bar issues
//! and with what geometry.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitute for the logger the library leaves undefined.
var last_message: ?[*:0]const u8 = null;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

const Fill = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    color: u32,
};

/// Recording paint backend: every fill appends to a module-level log.
const Recorder = struct {
    var fills: std.BoundedArray(Fill, 8) = .{};

    fn reset() void {
        fills = .{};
        last_message = null;
    }

    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills.append(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
    }
};

const fill_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = null,
    .text_size = null,
};

const no_primitive_backend: abi.Paint = .{
    .user = null,
    .fill_rect = null,
    .draw_text = null,
    .text_size = null,
};

const track_color: u32 = 0x00303030;
const fill_color: u32 = 0x00E0A020;

fn widgetAt(rect: abi.Rect) abi.Widget {
    return .{
        .vt = null,
        .ctx = null,
        .rect = rect,
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = false,
        .dirty = false,
    };
}

fn barOf(backend: ?*const abi.Paint, value: u16, total: u16) abi.ProgressBar {
    return .{
        .paint = backend,
        .track = track_color,
        .fill = fill_color,
        .value = value,
        .total = total,
    };
}

/// Bind a bar to a widget and render it, returning the recorded fills.
fn render(widget: *abi.Widget, bar: *abi.ProgressBar) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_progress_bar_init(widget, bar));
    widget.vt.?.render.?(widget);
}

test "init refuses a null widget with the C's message" {
    Recorder.reset();
    var bar = barOf(&fill_backend, 1, 2);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_progress_bar_init(null, &bar));
    try std.testing.expectEqualStrings("w must not be nullptr", std.mem.span(last_message.?));
}

test "init refuses a null descriptor with the C's message" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 10, .h = 4 });

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_progress_bar_init(&widget, null));
    try std.testing.expectEqualStrings("bar must not be nullptr", std.mem.span(last_message.?));
}

test "init binds the vtable, the context and visibility" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 10, .h = 4 });
    var bar = barOf(&fill_backend, 1, 2);

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_progress_bar_init(&widget, &bar));
    try std.testing.expectEqual(abi.ra8_widget_progress_bar_vtable(), widget.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&bar)), widget.ctx);
    try std.testing.expect(widget.visible);
    try std.testing.expectEqual(@as(?[*:0]const u8, null), last_message);
}

test "the bar vtable is display only" {
    const vtable = abi.ra8_widget_progress_bar_vtable();

    try std.testing.expectEqual(@as(?*const anyopaque, null), @as(?*const anyopaque, @ptrCast(vtable.measure)));
    try std.testing.expect(vtable.render != null);
    try std.testing.expectEqual(@as(?*const anyopaque, null), @as(?*const anyopaque, @ptrCast(vtable.on_input)));
    try std.testing.expectEqual(vtable, abi.ra8_widget_progress_bar_vtable());
}

test "render is a no-op without a descriptor, a backend or fill_rect" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 10, .h = 4 });
    widget.vt = abi.ra8_widget_progress_bar_vtable();

    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);

    var no_paint = barOf(null, 1, 2);
    try render(&widget, &no_paint);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);

    var no_primitive = barOf(&no_primitive_backend, 1, 2);
    try render(&widget, &no_primitive);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);
}

test "a half bar fills the track then the left half" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 10, .y = 20, .w = 100, .h = 8 });
    var bar = barOf(&fill_backend, 5, 10);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.len);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 20, .w = 100, .h = 8, .color = track_color }, Recorder.fills.buffer[0]);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 20, .w = 50, .h = 8, .color = fill_color }, Recorder.fills.buffer[1]);
}

test "a zero value paints the track alone" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 64, .h = 6 });
    var bar = barOf(&fill_backend, 0, 10);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    try std.testing.expectEqual(track_color, Recorder.fills.buffer[0].color);
}

test "a zero total paints the track alone rather than dividing" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 64, .h = 6 });
    var bar = barOf(&fill_backend, 7, 0);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    try std.testing.expectEqual(@as(i32, 64), Recorder.fills.buffer[0].w);
}

test "a value at or past total fills the whole rect width" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 4, .y = 4, .w = 80, .h = 10 });
    var bar = barOf(&fill_backend, 40, 10);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.len);
    try std.testing.expectEqual(@as(i32, 80), Recorder.fills.buffer[1].w);
    try std.testing.expectEqual(fill_color, Recorder.fills.buffer[1].color);
}

test "a zero-width rect paints no fill over the track" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 0, .h = 6 });
    var bar = barOf(&fill_backend, 5, 10);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    try std.testing.expectEqual(@as(i32, 0), Recorder.fills.buffer[0].w);
}

test "the fill shares the track's origin and height" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = -12, .y = 33, .w = 30, .h = 3 });
    var bar = barOf(&fill_backend, 1, 3);

    try render(&widget, &bar);

    const track = Recorder.fills.buffer[0];
    const filled = Recorder.fills.buffer[1];
    try std.testing.expectEqual(track.x, filled.x);
    try std.testing.expectEqual(track.y, filled.y);
    try std.testing.expectEqual(track.h, filled.h);
    try std.testing.expect(filled.w < track.w);
}

test "the descriptor matches the C layout and the empty sentinel is zero" {
    const ptr = @sizeOf(usize);

    try std.testing.expectEqual(@as(usize, 0), @offsetOf(abi.ProgressBar, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.ProgressBar, "track"));
    try std.testing.expectEqual(ptr + 4, @offsetOf(abi.ProgressBar, "fill"));
    try std.testing.expectEqual(ptr + 8, @offsetOf(abi.ProgressBar, "value"));
    try std.testing.expectEqual(ptr + 10, @offsetOf(abi.ProgressBar, "total"));
    try std.testing.expectEqual(@as(i32, 0), abi.geometry.empty);
}
