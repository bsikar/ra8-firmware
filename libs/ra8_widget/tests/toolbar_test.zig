//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the toolbar membrane: the descriptor layout, the bind guards, the
//! paint order across band, field, hint and count chip, and the tap target.
//! The field rect is the thing both halves share, so it is checked directly
//! and then again through a touch that has to land in the drawn box.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitutes for the three C symbols the library leaves
/// undefined: the logger, `ra8_widget_invalidate` from the still-C
/// `ra8_widget.c`, and `ra8_ui_rect_contains` from `libs/ra8_ui`.
var last_message: ?[*:0]const u8 = null;
var invalidations: u32 = 0;
var last_refresh: u8 = 0xFF;
var contains_calls: u32 = 0;

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

export fn ra8_ui_rect_contains(r: *const abi.Rect, px: i32, py: i32) callconv(.c) bool {
    contains_calls += 1;
    return px >= r.x and px < r.x + r.w and py >= r.y and py < r.y + r.h;
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
    var fills: std.BoundedArray(Fill, 8) = .{};
    var draws: std.BoundedArray(Draw, 8) = .{};

    fn reset() void {
        fills = .{};
        draws = .{};
        last_message = null;
        invalidations = 0;
        last_refresh = 0xFF;
        contains_calls = 0;
        searches_seen = 0;
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
        draws.append(.{ .x = x, .y = y, .text = str, .fg = fg, .bg = bg }) catch unreachable;
    }

    /// Fixed-width measurement so right alignment has something to subtract.
    fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        out_w.* = @intCast(std.mem.span(str).len * 6);
        out_h.* = 12;
    }
};

/// Observed by the `on_search` notification below.
var searches_seen: u32 = 0;

fn noteSearch(w: *abi.Widget) callconv(.c) void {
    const bar: *const abi.Toolbar = @ptrCast(@alignCast(w.ctx.?));
    searches_seen = bar.searches;
}

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

const bg_color: u32 = 0x00FFFFFF;
const field_color: u32 = 0x00F0F0F0;
const border_color: u32 = 0x00C0C0C0;
const hint_color: u32 = 0x00909090;
const count_color: u32 = 0x00707070;

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

fn toolbarOn(
    backend: ?*const abi.Paint,
    hint: ?[*:0]const u8,
    count: ?[*:0]const u8,
    on_search: ?*const fn (w: *abi.Widget) callconv(.c) void,
) abi.Toolbar {
    return .{
        .paint = backend,
        .hint = hint,
        .count = count,
        .on_search = on_search,
        .bg = bg_color,
        .field = field_color,
        .border = border_color,
        .hint_fg = hint_color,
        .count_fg = count_color,
        .searches = 0,
        .pad = 8,
        .border_w = 1,
        .count_w = 72,
    };
}

fn bind(widget: *abi.Widget, bar: *abi.Toolbar) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_toolbar_init(widget, bar));
}

fn touchAt(x: i32, y: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = y };
}

test "init refuses a null widget with the C's message" {
    Recorder.reset();
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_toolbar_init(null, &bar));
    try std.testing.expectEqualStrings("w must not be nullptr", std.mem.span(last_message.?));
}

test "init refuses a null descriptor with the C's message" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_toolbar_init(&widget, null));
    try std.testing.expectEqualStrings("bar must not be nullptr", std.mem.span(last_message.?));
}

test "init binds the vtable, the context and visibility" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);

    try bind(&widget, &bar);
    try std.testing.expectEqual(abi.ra8_widget_toolbar_vtable(), widget.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&bar)), widget.ctx);
    try std.testing.expect(widget.visible);
}

test "the toolbar vtable renders and routes but never measures" {
    const vtable = abi.ra8_widget_toolbar_vtable();

    try std.testing.expectEqual(@as(?*const anyopaque, null), @as(?*const anyopaque, @ptrCast(vtable.measure)));
    try std.testing.expect(vtable.render != null);
    try std.testing.expect(vtable.on_input != null);
    try std.testing.expectEqual(vtable, abi.ra8_widget_toolbar_vtable());
}

test "the field is inset by pad and reserves the chip on the right" {
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);
    const band: abi.Rect = .{ .x = 10, .y = 20, .w = 320, .h = 48 };

    const field = abi.fieldRect(&bar, &band);

    try std.testing.expectEqual(@as(i32, 18), field.x);
    try std.testing.expectEqual(@as(i32, 28), field.y);
    try std.testing.expectEqual(@as(i32, 320 - 8 - 8 - 72 - 8), field.w);
    try std.testing.expectEqual(@as(i32, 32), field.h);
}

test "a band too narrow for the chip collapses the field instead of going negative" {
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);
    const band: abi.Rect = .{ .x = 0, .y = 0, .w = 40, .h = 48 };

    const field = abi.fieldRect(&bar, &band);

    try std.testing.expectEqual(@as(i32, 0), field.w);
    try std.testing.expectEqual(abi.geometry.collapsed_field, field.w);
}

test "render is a no-op without a descriptor or a backend" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    widget.vt = abi.ra8_widget_toolbar_vtable();

    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);

    var no_paint = toolbarOn(null, "Search", "12 books", null);
    try bind(&widget, &no_paint);
    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 0), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "a full toolbar fills the band, frames the field, then draws hint and chip" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);

    try bind(&widget, &bar);
    widget.vt.?.render.?(&widget);

    // Band fill, then the framed field: border underneath, fill inset by border_w.
    try std.testing.expectEqual(@as(usize, 3), Recorder.fills.len);
    try std.testing.expectEqual(Fill{ .x = 0, .y = 0, .w = 320, .h = 48, .color = bg_color }, Recorder.fills.buffer[0]);
    try std.testing.expectEqual(border_color, Recorder.fills.buffer[1].color);
    try std.testing.expectEqual(field_color, Recorder.fills.buffer[2].color);
    try std.testing.expectEqual(Recorder.fills.buffer[1].x + 1, Recorder.fills.buffer[2].x);

    try std.testing.expectEqual(@as(usize, 2), Recorder.draws.len);
    try std.testing.expectEqualStrings("Search", std.mem.span(Recorder.draws.buffer[0].text));
    try std.testing.expectEqual(hint_color, Recorder.draws.buffer[0].fg);
    try std.testing.expectEqual(field_color, Recorder.draws.buffer[0].bg);
    try std.testing.expectEqualStrings("12 books", std.mem.span(Recorder.draws.buffer[1].text));
    try std.testing.expectEqual(count_color, Recorder.draws.buffer[1].fg);
    try std.testing.expectEqual(bg_color, Recorder.draws.buffer[1].bg);
    try std.testing.expect(Recorder.draws.buffer[1].x > Recorder.draws.buffer[0].x);
}

test "either string may be null and the other still draws" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });

    var no_hint = toolbarOn(&full_backend, null, "12 books", null);
    try bind(&widget, &no_hint);
    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 1), Recorder.draws.len);
    try std.testing.expectEqualStrings("12 books", std.mem.span(Recorder.draws.buffer[0].text));

    Recorder.reset();
    var no_count = toolbarOn(&full_backend, "Search", null, null);
    try bind(&widget, &no_count);
    widget.vt.?.render.?(&widget);
    try std.testing.expectEqual(@as(usize, 1), Recorder.draws.len);
    try std.testing.expectEqualStrings("Search", std.mem.span(Recorder.draws.buffer[0].text));
}

test "a backend with no draw_text still paints the band and the field" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&fill_only_backend, "Search", "12 books", null);

    try bind(&widget, &bar);
    widget.vt.?.render.?(&widget);

    try std.testing.expectEqual(@as(usize, 3), Recorder.fills.len);
    try std.testing.expectEqual(@as(usize, 0), Recorder.draws.len);
}

test "a touch inside the field latches, invalidates fast and notifies" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", noteSearch);
    try bind(&widget, &bar);

    const event = touchAt(40, 24);
    try std.testing.expect(widget.vt.?.on_input.?(&widget, &event));

    try std.testing.expectEqual(@as(u32, 1), bar.searches);
    try std.testing.expectEqual(@as(u32, 1), invalidations);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.fast), last_refresh);
    try std.testing.expect(widget.dirty);
    // The callback sees the already-incremented counter, as in the C.
    try std.testing.expectEqual(@as(u32, 1), searches_seen);
}

test "a touch on the count chip is declined and changes nothing" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", noteSearch);
    try bind(&widget, &bar);

    const event = touchAt(300, 24);
    try std.testing.expect(!widget.vt.?.on_input.?(&widget, &event));

    try std.testing.expectEqual(@as(u32, 0), bar.searches);
    try std.testing.expectEqual(@as(u32, 0), invalidations);
    try std.testing.expect(!widget.dirty);
    try std.testing.expectEqual(@as(u32, 1), contains_calls);
}

test "a button event is declined before the hit test runs" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);
    try bind(&widget, &bar);

    const event: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 3, .x = 40, .y = 24 };
    try std.testing.expect(!widget.vt.?.on_input.?(&widget, &event));

    try std.testing.expectEqual(@as(u32, 0), contains_calls);
    try std.testing.expectEqual(@as(u32, 0), bar.searches);
}

test "input without a descriptor is declined" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    widget.vt = abi.ra8_widget_toolbar_vtable();

    const event = touchAt(40, 24);
    try std.testing.expect(!widget.vt.?.on_input.?(&widget, &event));
    try std.testing.expectEqual(@as(u32, 0), contains_calls);
}

test "a latch with no callback still counts and invalidates" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);
    try bind(&widget, &bar);

    const event = touchAt(40, 24);
    try std.testing.expect(widget.vt.?.on_input.?(&widget, &event));
    try std.testing.expect(widget.vt.?.on_input.?(&widget, &event));

    try std.testing.expectEqual(@as(u32, 2), bar.searches);
    try std.testing.expectEqual(@as(u32, 2), invalidations);
}

test "the counter wraps rather than trapping, as the C's uint32_t does" {
    Recorder.reset();
    var widget = widgetAt(.{ .x = 0, .y = 0, .w = 320, .h = 48 });
    var bar = toolbarOn(&full_backend, "Search", "12 books", null);
    bar.searches = std.math.maxInt(u32);
    try bind(&widget, &bar);

    const event = touchAt(40, 24);
    try std.testing.expect(widget.vt.?.on_input.?(&widget, &event));
    try std.testing.expectEqual(@as(u32, 0), bar.searches);
}

test "the descriptor matches the C layout" {
    const ptr = @sizeOf(usize);

    try std.testing.expectEqual(@as(usize, 0), @offsetOf(abi.Toolbar, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.Toolbar, "hint"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Toolbar, "count"));
    try std.testing.expectEqual(3 * ptr, @offsetOf(abi.Toolbar, "on_search"));
    try std.testing.expectEqual(4 * ptr, @offsetOf(abi.Toolbar, "bg"));
    try std.testing.expectEqual(4 * ptr + 4, @offsetOf(abi.Toolbar, "field"));
    try std.testing.expectEqual(4 * ptr + 8, @offsetOf(abi.Toolbar, "border"));
    try std.testing.expectEqual(4 * ptr + 12, @offsetOf(abi.Toolbar, "hint_fg"));
    try std.testing.expectEqual(4 * ptr + 16, @offsetOf(abi.Toolbar, "count_fg"));
    try std.testing.expectEqual(4 * ptr + 20, @offsetOf(abi.Toolbar, "searches"));
    try std.testing.expectEqual(4 * ptr + 24, @offsetOf(abi.Toolbar, "pad"));
    try std.testing.expectEqual(4 * ptr + 26, @offsetOf(abi.Toolbar, "border_w"));
    try std.testing.expectEqual(4 * ptr + 28, @offsetOf(abi.Toolbar, "count_w"));
    try std.testing.expectEqual(@as(i16, 0), abi.geometry.no_border);
}
