//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the label membrane: the ABI layouts, the bind guards, and the
//! render's paint sequence through a recording backend. The label owns no
//! geometry of its own, so what is asserted here is the dispatch: which
//! backend calls it issues, in which order, and where it stops.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitute for the C logger the library leaves undefined; it
/// records the last guard message so each refusal can be told apart.
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
    fg: u32,
    bg: u32,
    text: [*:0]const u8,
};

/// Recording paint backend: every primitive appends to a module-level log, so
/// a render can be asserted call by call.
const Recorder = struct {
    var fills: std.BoundedArray(Fill, 8) = .{};
    var draws: std.BoundedArray(Draw, 8) = .{};
    var measured_w: i32 = 0;
    var measured_h: i32 = 0;

    fn reset() void {
        fills = .{};
        draws = .{};
        measured_w = 0;
        measured_h = 0;
        last_message = null;
    }

    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills.append(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
    }

    fn drawText(
        _: ?*anyopaque,
        x: i32,
        y: i32,
        str: [*:0]const u8,
        fg: u32,
        bg: u32,
    ) callconv(.c) void {
        draws.append(.{ .x = x, .y = y, .fg = fg, .bg = bg, .text = str }) catch unreachable;
    }

    fn textSize(_: ?*anyopaque, _: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        out_w.* = measured_w;
        out_h.* = measured_h;
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

const silent_backend: abi.Paint = .{
    .user = null,
    .fill_rect = null,
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

fn labelOn(backend: *const abi.Paint, text: ?[*:0]const u8) abi.Label {
    return .{
        .paint = backend,
        .text = text,
        .fg = 0x112233,
        .bg = 0x445566,
        .pad = 4,
        .alignment = .left,
        .reserved = 0,
    };
}

/// Bind a widget to a label and render it through the published vtable, the
/// way `ra8_widget_render_dirty` does on the board.
fn renderBound(w: *abi.Widget, label: *abi.Label) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_label_init(w, label));
    w.vt.?.render.?(w);
}

test "init binds the vtable, the context and visibility" {
    Recorder.reset();
    var w = emptyWidget();
    var label = labelOn(&full_backend, "hi");

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_label_init(&w, &label));
    try std.testing.expectEqual(abi.ra8_widget_label_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&label)), w.ctx);
    try std.testing.expect(w.visible);
    try std.testing.expectEqual(@as(?[*:0]const u8, null), last_message);
}

test "init refuses a null widget and a null descriptor by their own messages" {
    Recorder.reset();
    var label = labelOn(&full_backend, "hi");
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_label_init(null, &label));
    try std.testing.expectEqualStrings("w must not be nullptr", std.mem.span(last_message.?));

    var w = emptyWidget();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_label_init(&w, null));
    try std.testing.expectEqualStrings("label must not be nullptr", std.mem.span(last_message.?));
    try std.testing.expectEqual(@as(?*const abi.Vtable, null), w.vt);
}

test "the label vtable is display only and shared by every label" {
    const vt = abi.ra8_widget_label_vtable();
    try std.testing.expectEqual(vt, abi.ra8_widget_label_vtable());
    try std.testing.expect(vt.measure == null);
    try std.testing.expect(vt.on_input == null);
    try std.testing.expect(vt.render != null);
}

test "render fills the whole rect with bg and draws the text at the inset" {
    Recorder.reset();
    var w = emptyWidget();
    var label = labelOn(&full_backend, "hi");
    try renderBound(&w, &label);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    const fill = Recorder.fills.get(0);
    try std.testing.expectEqual(abi.Rect{ .x = 10, .y = 20, .w = 100, .h = 40 }, abi.Rect{
        .x = fill.x,
        .y = fill.y,
        .w = fill.w,
        .h = fill.h,
    });
    try std.testing.expectEqual(@as(u32, 0x445566), fill.color);

    try std.testing.expectEqual(@as(usize, 1), Recorder.draws.len);
    const draw = Recorder.draws.get(0);
    try std.testing.expectEqual(@as(i32, 14), draw.x);
    try std.testing.expectEqual(@as(i32, 24), draw.y);
    try std.testing.expectEqual(@as(u32, 0x112233), draw.fg);
    try std.testing.expectEqual(@as(u32, 0x445566), draw.bg);
    try std.testing.expectEqualStrings("hi", std.mem.span(draw.text));
}

test "a centred label is placed by the backend's measurement" {
    Recorder.reset();
    Recorder.measured_w = 40;
    Recorder.measured_h = 10;
    var w = emptyWidget();
    var label = labelOn(&full_backend, "hi");
    label.alignment = .center;
    try renderBound(&w, &label);

    const draw = Recorder.draws.get(0);
    try std.testing.expectEqual(@as(i32, 40), draw.x);
    try std.testing.expectEqual(@as(i32, 35), draw.y);
}

test "a label with no text fills the background and draws nothing" {
    Recorder.reset();
    var w = emptyWidget();
    var label = labelOn(&full_backend, null);
    try renderBound(&w, &label);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "a backend without draw_text still paints the background" {
    Recorder.reset();
    var w = emptyWidget();
    var label = labelOn(&fill_only_backend, "hi");
    try renderBound(&w, &label);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "a backend that draws nothing touches no pixels" {
    Recorder.reset();
    var w = emptyWidget();
    var label = labelOn(&silent_backend, "hi");
    try renderBound(&w, &label);

    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "render with no descriptor and with no paint backend are both no-ops" {
    Recorder.reset();
    var w = emptyWidget();
    w.vt = abi.ra8_widget_label_vtable();
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);

    var label = labelOn(&full_backend, "hi");
    label.paint = null;
    try renderBound(&w, &label);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "the mirrored C layouts are the ones the header publishes" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(3 * ptr, @sizeOf(abi.Vtable));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Widget, "rect"));
    try std.testing.expectEqual(2 * ptr + 24, @offsetOf(abi.Widget, "dirty"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Label, "fg"));
    try std.testing.expectEqual(2 * ptr + 10, @offsetOf(abi.Label, "alignment"));
}
