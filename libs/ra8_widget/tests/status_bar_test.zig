//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the status-bar membrane: the descriptor layout, the bind guards,
//! the paint order across the band fill, the two aligned labels and the bottom
//! hairline, and every early-out on the way. Text placement itself belongs to
//! the shared helper and is covered in `internal_test.zig`.

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

const Draw = struct {
    x: i32,
    y: i32,
    text: [*:0]const u8,
    fg: u32,
    bg: u32,
};

/// Recording paint backend: every primitive appends to a module-level log.
const Recorder = struct {
    var fills_buffer: [8]Fill = undefined;
    var fills: std.ArrayList(Fill) = .initBuffer(&fills_buffer);
    var draws_buffer: [8]Draw = undefined;
    var draws: std.ArrayList(Draw) = .initBuffer(&draws_buffer);

    fn reset() void {
        fills.clearRetainingCapacity();
        draws.clearRetainingCapacity();
        last_message = null;
    }

    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills.appendBounded(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
    }

    fn drawText(
        _: ?*anyopaque,
        x: i32,
        y: i32,
        str: [*:0]const u8,
        fg: u32,
        bg: u32,
    ) callconv(.c) void {
        draws.appendBounded(.{ .x = x, .y = y, .text = str, .fg = fg, .bg = bg }) catch unreachable;
    }

    /// Fixed-width measurement so right alignment has something to subtract.
    fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        out_w.* = @intCast(std.mem.span(str).len * 6);
        out_h.* = 12;
    }
};

const full_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = Recorder.drawText,
    .text_size = Recorder.textSize,
};

const fill_only_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = null,
    .text_size = null,
};

const text_only_backend: abi.Paint = .{
    .user = null,
    .fill_rect = null,
    .draw_text = Recorder.drawText,
    .text_size = Recorder.textSize,
};

const bg_color: u32 = 0x00101010;
const fg_color: u32 = 0x00F0F0F0;
const fg_right_color: u32 = 0x00909090;
const rule_color: u32 = 0x00404040;

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

fn barOn(backend: ?*const abi.Paint, left: ?[*:0]const u8, right: ?[*:0]const u8, rule_h: i16) abi.StatusBar {
    return .{
        .paint = backend,
        .left = left,
        .right = right,
        .bg = bg_color,
        .fg = fg_color,
        .fg_right = fg_right_color,
        .rule = rule_color,
        .pad = 4,
        .rule_h = rule_h,
    };
}

fn render(widget: *abi.Widget, bar: *abi.StatusBar) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_status_bar_init(widget, bar));
    widget.vt.?.render.?(widget);
}

test "init refuses a null widget with the C's message" {
    Recorder.reset();
    var bar = barOn(&full_backend, "left", "right", 1);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_status_bar_init(null, &bar));
    try std.testing.expectEqualStrings("w must not be nullptr", std.mem.span(last_message.?));
}

test "init refuses a null descriptor with the C's message" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 100, .h = 20 });

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_status_bar_init(&widget, null));
    try std.testing.expectEqualStrings("bar must not be nullptr", std.mem.span(last_message.?));
}

test "init binds the vtable, the context and visibility" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 100, .h = 20 });
    var bar = barOn(&full_backend, "left", "right", 1);

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_status_bar_init(&widget, &bar));
    try std.testing.expectEqual(abi.ra8_widget_status_bar_vtable(), widget.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&bar)), widget.ctx);
    try std.testing.expect(widget.visible);
}

test "the band vtable is display only" {
    const vtable = abi.ra8_widget_status_bar_vtable();

    try std.testing.expectEqual(@as(?*const anyopaque, null), @as(?*const anyopaque, @ptrCast(vtable.measure)));
    try std.testing.expect(vtable.render != null);
    try std.testing.expectEqual(@as(?*const anyopaque, null), @as(?*const anyopaque, @ptrCast(vtable.on_input)));
    try std.testing.expectEqual(vtable, abi.ra8_widget_status_bar_vtable());
}

test "render is a no-op without a descriptor or a backend" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 100, .h = 20 });
    widget.vt = abi.ra8_widget_status_bar_vtable();

    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.items.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.items.len);

    var no_paint = barOn(null, "left", "right", 1);
    try render(&widget, &no_paint);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.items.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.items.len);
}

test "a full band fills, draws both labels, then rules the bottom edge" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 10, .y = 20, .w = 200, .h = 24 });
    var bar = barOn(&full_backend, "9:41", "88%", 2);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.items.len);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 20, .w = 200, .h = 24, .color = bg_color }, Recorder.fills.items[0]);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 42, .w = 200, .h = 2, .color = rule_color }, Recorder.fills.items[1]);

    try std.testing.expectEqual(@as(usize, 2), Recorder.draws.items.len);
    try std.testing.expectEqualStrings("9:41", std.mem.span(Recorder.draws.items[0].text));
    try std.testing.expectEqual(fg_color, Recorder.draws.items[0].fg);
    try std.testing.expectEqualStrings("88%", std.mem.span(Recorder.draws.items[1].text));
    try std.testing.expectEqual(fg_right_color, Recorder.draws.items[1].fg);
    try std.testing.expectEqual(bg_color, Recorder.draws.items[1].bg);
}

test "the right label sits right of the left one" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 200, .h = 24 });
    var bar = barOn(&full_backend, "AA", "BB", 0);

    try render(&widget, &bar);

    try std.testing.expect(Recorder.draws.items[1].x > Recorder.draws.items[0].x);
}

test "a null label is skipped and the other still draws" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 120, .h = 20 });
    var bar = barOn(&full_backend, null, "only", 0);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 1), Recorder.draws.items.len);
    try std.testing.expectEqualStrings("only", std.mem.span(Recorder.draws.items[0].text));
    try std.testing.expectEqual(fg_right_color, Recorder.draws.items[0].fg);
}

test "a backend with no draw_text still fills the band and the rule" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 120, .h = 20 });
    var bar = barOn(&fill_only_backend, "left", "right", 3);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.items.len);
    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.items.len);
    try std.testing.expectEqual(@as(i32, 3), Recorder.fills.items[1].h);
}

test "a rule_h at or below zero leaves the hairline off" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 120, .h = 20 });

    var flat = barOn(&full_backend, "a", "b", 0);
    try render(&widget, &flat);
    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.items.len);

    Recorder.reset();
    var negative = barOn(&full_backend, "a", "b", -4);
    try render(&widget, &negative);
    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.items.len);
}

test "a backend with no fill_rect draws the labels and no bands" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 120, .h = 20 });
    var bar = barOn(&text_only_backend, "left", "right", 2);

    try render(&widget, &bar);

    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.items.len);
    try std.testing.expectEqual(@as(usize, 2), Recorder.draws.items.len);
}

test "the hairline hugs the bottom edge whatever the band height" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = -5, .y = 7, .w = 64, .h = 40 });
    var bar = barOn(&fill_only_backend, null, null, 5);

    try render(&widget, &bar);

    const rule = Recorder.fills.items[1];
    try std.testing.expectEqual(@as(i32, -5), rule.x);
    try std.testing.expectEqual(@as(i32, 42), rule.y);
    try std.testing.expectEqual(@as(i32, 64), rule.w);
    try std.testing.expectEqual(@as(i32, 5), rule.h);
}

test "the descriptor matches the C layout and both thresholds are zero" {
    const ptr = @sizeOf(usize);

    try std.testing.expectEqual(@as(usize, 0), @offsetOf(abi.StatusBar, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.StatusBar, "left"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.StatusBar, "right"));
    try std.testing.expectEqual(3 * ptr, @offsetOf(abi.StatusBar, "bg"));
    try std.testing.expectEqual(3 * ptr + 4, @offsetOf(abi.StatusBar, "fg"));
    try std.testing.expectEqual(3 * ptr + 8, @offsetOf(abi.StatusBar, "fg_right"));
    try std.testing.expectEqual(3 * ptr + 12, @offsetOf(abi.StatusBar, "rule"));
    try std.testing.expectEqual(3 * ptr + 16, @offsetOf(abi.StatusBar, "pad"));
    try std.testing.expectEqual(3 * ptr + 18, @offsetOf(abi.StatusBar, "rule_h"));
    try std.testing.expectEqual(@as(i16, 0), abi.geometry.no_rule);
    try std.testing.expectEqual(@as(i16, 0), abi.geometry.no_border);
}
