//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the button membrane: the descriptor layout, the bind guards, the
//! face/label paint for both latch states, and the input latch. The button
//! owns no geometry of its own, so what is asserted here is the dispatch and
//! the state the latch mutates.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitutes for the two C symbols the library leaves undefined:
/// the logger, and `ra8_widget_invalidate` from the still-C `ra8_widget.c`.
var last_message: ?[*:0]const u8 = null;
var invalidations: u32 = 0;
var last_refresh: u8 = 0xFF;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

export fn ra8_widget_invalidate(w: *abi.Widget, refresh: u8) callconv(.c) u16 {
    invalidations += 1;
    last_refresh = refresh;
    w.dirty = true;
    w.refresh = refresh;
    return abi.err.ok;
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
    fg: u32,
    bg: u32,
};

/// Recording paint backend: every primitive appends to a module-level log.
const Recorder = struct {
    var fills: std.BoundedArray(Fill, 8) = .{};
    var draws: std.BoundedArray(Draw, 8) = .{};

    fn reset() void {
        fills = .{};
        draws = .{};
        last_message = null;
        invalidations = 0;
        last_refresh = 0xFF;
        presses_seen = 0;
    }

    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills.append(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
    }

    fn drawText(
        _: ?*anyopaque,
        x: i32,
        y: i32,
        _: [*:0]const u8,
        fg: u32,
        bg: u32,
    ) callconv(.c) void {
        draws.append(.{ .x = x, .y = y, .fg = fg, .bg = bg }) catch unreachable;
    }
};

/// Observed by the `on_press` hook so the callback order can be asserted.
var presses_seen: u32 = 0;

fn onPress(w: *abi.Widget) callconv(.c) void {
    const button: *const abi.Button = @ptrCast(@alignCast(w.ctx.?));
    presses_seen = button.presses;
}

const full_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = Recorder.drawText,
    .text_size = null,
};

const fill_only_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = null,
    .text_size = null,
};

fn emptyWidget() abi.Widget {
    return .{
        .vt = null,
        .ctx = null,
        .rect = .{ .x = 10, .y = 20, .w = 100, .h = 40 },
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = false,
        .dirty = false,
    };
}

fn buttonOn(backend: *const abi.Paint, text: ?[*:0]const u8) abi.Button {
    return .{
        .paint = backend,
        .text = text,
        .on_press = null,
        .fg = 0x111111,
        .face = 0x222222,
        .face_pressed = 0x333333,
        .border = 0x444444,
        .presses = 0,
        .pad = 4,
        .border_w = 2,
        .alignment = .left,
        .pressed = false,
        .reserved = 0,
    };
}

fn touch() abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 15, .y = 25 };
}

fn buttonEvent() abi.Event {
    return .{ .kind = .button, .reserved = 0, .button_id = 7, .x = 0, .y = 0 };
}

fn bind(w: *abi.Widget, button: *abi.Button) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_button_init(w, button));
}

test "init binds the vtable, the context and visibility" {
    Recorder.reset();
    var w = emptyWidget();
    var button = buttonOn(&full_backend, "ok");

    try bind(&w, &button);
    try std.testing.expectEqual(abi.ra8_widget_button_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&button)), w.ctx);
    try std.testing.expect(w.visible);
    try std.testing.expectEqual(@as(?[*:0]const u8, null), last_message);
}

test "init refuses a null widget and a null descriptor by their own messages" {
    Recorder.reset();
    var button = buttonOn(&full_backend, "ok");
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_button_init(null, &button));
    try std.testing.expectEqualStrings("w must not be nullptr", std.mem.span(last_message.?));

    var w = emptyWidget();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_button_init(&w, null));
    try std.testing.expectEqualStrings("button must not be nullptr", std.mem.span(last_message.?));
    try std.testing.expectEqual(@as(?*const abi.Vtable, null), w.vt);
}

test "the button vtable latches input and is shared by every button" {
    const vt = abi.ra8_widget_button_vtable();
    try std.testing.expectEqual(vt, abi.ra8_widget_button_vtable());
    try std.testing.expect(vt.measure == null);
    try std.testing.expect(vt.render != null);
    try std.testing.expect(vt.on_input != null);
}

test "a released button paints border then face, and the label over the face" {
    Recorder.reset();
    var w = emptyWidget();
    var button = buttonOn(&full_backend, "ok");
    try bind(&w, &button);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.len);
    const frame = Recorder.fills.get(0);
    try std.testing.expectEqual(@as(u32, 0x444444), frame.color);
    try std.testing.expectEqual(@as(i32, 100), frame.w);
    const face = Recorder.fills.get(1);
    try std.testing.expectEqual(@as(u32, 0x222222), face.color);
    try std.testing.expectEqual(@as(i32, 12), face.x);
    try std.testing.expectEqual(@as(i32, 96), face.w);

    const draw = Recorder.draws.get(0);
    try std.testing.expectEqual(@as(i32, 14), draw.x);
    try std.testing.expectEqual(@as(u32, 0x111111), draw.fg);
    try std.testing.expectEqual(@as(u32, 0x222222), draw.bg);
}

test "a pressed button paints the pressed face and draws the label on it" {
    Recorder.reset();
    var w = emptyWidget();
    var button = buttonOn(&full_backend, "ok");
    button.pressed = true;
    try bind(&w, &button);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(@as(u32, 0x333333), Recorder.fills.get(1).color);
    try std.testing.expectEqual(@as(u32, 0x333333), Recorder.draws.get(0).bg);
}

test "a borderless button paints one fill" {
    Recorder.reset();
    var w = emptyWidget();
    var button = buttonOn(&full_backend, "ok");
    button.border_w = 0;
    try bind(&w, &button);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    try std.testing.expectEqual(@as(u32, 0x222222), Recorder.fills.get(0).color);
}

test "render stops at no descriptor, no backend, no text and no draw_text" {
    Recorder.reset();
    var w = emptyWidget();
    w.vt = abi.ra8_widget_button_vtable();
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);

    var unpainted = buttonOn(&full_backend, "ok");
    unpainted.paint = null;
    try bind(&w, &unpainted);
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);

    var textless = buttonOn(&full_backend, null);
    try bind(&w, &textless);
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);

    Recorder.reset();
    var mute = buttonOn(&fill_only_backend, "ok");
    try bind(&w, &mute);
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, 2), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "a touch latches: pressed flips, presses grows, the rect self-invalidates fast" {
    Recorder.reset();
    var w = emptyWidget();
    var button = buttonOn(&full_backend, "ok");
    button.on_press = onPress;
    try bind(&w, &button);

    const event = touch();
    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(button.pressed);
    try std.testing.expectEqual(@as(u32, 1), button.presses);
    try std.testing.expectEqual(@as(u32, 1), invalidations);
    try std.testing.expectEqual(@as(u8, 1), last_refresh);
    try std.testing.expect(w.dirty);

    // The callback sees the already-bumped counter, as it did in the C.
    try std.testing.expectEqual(@as(u32, 1), presses_seen);

    try std.testing.expect(w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(!button.pressed);
    try std.testing.expectEqual(@as(u32, 2), button.presses);
    try std.testing.expectEqual(@as(u32, 2), invalidations);
}

test "a button event is declined and changes nothing" {
    Recorder.reset();
    var w = emptyWidget();
    var button = buttonOn(&full_backend, "ok");
    try bind(&w, &button);

    const event = buttonEvent();
    try std.testing.expect(!w.vt.?.on_input.?(&w, &event));
    try std.testing.expect(!button.pressed);
    try std.testing.expectEqual(@as(u32, 0), button.presses);
    try std.testing.expectEqual(@as(u32, 0), invalidations);
}

test "input on a widget with no descriptor is declined" {
    Recorder.reset();
    var w = emptyWidget();
    w.vt = abi.ra8_widget_button_vtable();
    const event = touch();
    try std.testing.expect(!w.vt.?.on_input.?(&w, &event));
    try std.testing.expectEqual(@as(u32, 0), invalidations);
}

test "the mirrored descriptor layout is the one the header publishes" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Button, "on_press"));
    try std.testing.expectEqual(3 * ptr + 16, @offsetOf(abi.Button, "presses"));
    try std.testing.expectEqual(3 * ptr + 22, @offsetOf(abi.Button, "border_w"));
    try std.testing.expectEqual(3 * ptr + 25, @offsetOf(abi.Button, "pressed"));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(abi.Event));
}
