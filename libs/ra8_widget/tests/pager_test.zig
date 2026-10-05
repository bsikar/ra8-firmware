//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for pager counts, bounded navigation, paint output, and the
//! damage rectangle produced by a page turn.

const std = @import("std");
const abi = @import("abi");

var last_message: ?[*:0]const u8 = null;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

export fn ra8_widget_invalidate(widget: *abi.Widget, refresh: u8) callconv(.c) u16 {
    widget.dirty = true;
    if (refresh > widget.refresh) widget.refresh = refresh;
    return abi.err.ok;
}

const Draw = struct {
    x: i32,
    y: i32,
    fg: u32,
    bg: u32,
    text: [24:0]u8,
};

const Recorder = struct {
    var draws: std.BoundedArray(Draw, 4) = .{};

    fn reset() void {
        draws = .{};
        last_message = null;
    }

    fn drawText(_: ?*anyopaque, x: i32, y: i32, text: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void {
        var draw: Draw = .{ .x = x, .y = y, .fg = fg, .bg = bg, .text = undefined };
        const source = std.mem.span(text);
        @memcpy(draw.text[0..source.len], source);
        draw.text[source.len] = 0;
        draws.append(draw) catch unreachable;
    }
};

const bg_color: u32 = 0x00FFFFFF;
const fg_color: u32 = 0x00101010;
const disabled_color: u32 = 0x00808080;

const paint: abi.Paint = .{
    .user = null,
    .fill_rect = null,
    .draw_text = Recorder.drawText,
    .text_size = null,
};

fn pagerOf(item_count: u16, capacity: u16, page: u16) abi.Pager {
    return .{
        .paint = &paint,
        .item_count = item_count,
        .page_capacity = capacity,
        .page = page,
        .label_format = .page,
        .bg = bg_color,
        .fg = fg_color,
        .fg_disabled = disabled_color,
    };
}

fn widgetAt() abi.Widget {
    return .{
        .vt = null,
        .ctx = null,
        .rect = .{ .x = 24, .y = 900, .w = 300, .h = 48 },
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = false,
        .dirty = false,
    };
}

fn touch(x: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = 920 };
}

fn pageDamage(widget: *abi.Widget) struct { rect: abi.Rect, hint: abi.Refresh, count: u16 } {
    return .{
        .rect = widget.rect,
        .hint = @enumFromInt(widget.refresh),
        .count = if (widget.visible and widget.dirty) 1 else 0,
    };
}

test "page counts round up and empty or zero-capacity inputs have no pages" {
    try std.testing.expectEqual(@as(u16, 0), abi.pageCount(0, 10));
    try std.testing.expectEqual(@as(u16, 0), abi.pageCount(12, 0));
    try std.testing.expectEqual(@as(u16, 1), abi.pageCount(1, 10));
    try std.testing.expectEqual(@as(u16, 3), abi.pageCount(21, 10));
    try std.testing.expectEqual(@as(u16, 65535), abi.pageCount(65535, 1));
}

test "init clamps an out-of-range page and binds the widget" {
    var pager = pagerOf(21, 10, 8);
    var widget = widgetAt();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_pager_init(&widget, &pager));
    try std.testing.expectEqual(@as(u16, 2), pager.page);
    try std.testing.expectEqual(abi.ra8_widget_pager_vtable(), widget.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&pager)), widget.ctx);
    try std.testing.expect(widget.visible);
}

test "a next tap turns one page and reports its exact fast damage rect" {
    var pager = pagerOf(21, 10, 0);
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);

    try std.testing.expect(abi.ra8_widget_pager_vtable().on_input.?(&widget, &touch(300)));
    try std.testing.expectEqual(@as(u16, 1), pager.page);
    const damage = pageDamage(&widget);
    try std.testing.expectEqual(widget.rect, damage.rect);
    try std.testing.expectEqual(abi.Refresh.fast, damage.hint);
    try std.testing.expectEqual(@as(u16, 1), damage.count);
}

test "previous and next stop at their respective edges without reporting damage" {
    var pager = pagerOf(21, 10, 0);
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    try std.testing.expect(abi.ra8_widget_pager_vtable().on_input.?(&widget, &touch(24)));
    try std.testing.expectEqual(@as(u16, 0), pager.page);
    try std.testing.expect(!widget.dirty);

    pager.page = 2;
    widget.dirty = false;
    widget.refresh = 0;
    try std.testing.expect(abi.ra8_widget_pager_vtable().on_input.?(&widget, &touch(323)));
    try std.testing.expectEqual(@as(u16, 2), pager.page);
    try std.testing.expect(!widget.dirty);
}

test "a previous tap from the last page steps back and preserves bounds" {
    var pager = pagerOf(21, 10, 2);
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    try std.testing.expect(abi.ra8_widget_pager_vtable().on_input.?(&widget, &touch(24)));
    try std.testing.expectEqual(@as(u16, 1), pager.page);
    try std.testing.expect(widget.dirty);
}

test "render labels the page and dims unavailable directions" {
    Recorder.reset();
    var pager = pagerOf(21, 10, 0);
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    abi.ra8_widget_pager_vtable().render.?(&widget);

    try std.testing.expectEqual(@as(usize, 3), Recorder.draws.len);
    try std.testing.expectEqualStrings("Previous", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[0].text))));
    try std.testing.expectEqual(disabled_color, Recorder.draws.buffer[0].fg);
    try std.testing.expectEqualStrings("Page 1 of 3", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[1].text))));
    try std.testing.expectEqualStrings("Next", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[2].text))));
    try std.testing.expectEqual(fg_color, Recorder.draws.buffer[2].fg);
}

test "range format reports first, middle, and partial last item ranges" {
    const cases = [_]struct { count: u16, capacity: u16, page: u16, expected: []const u8 }{
        .{ .count = 14, .capacity = 8, .page = 0, .expected = "1 to 8 of 14" },
        .{ .count = 42, .capacity = 10, .page = 2, .expected = "21 to 30 of 42" },
        .{ .count = 14, .capacity = 8, .page = 1, .expected = "9 to 14 of 14" },
    };
    for (cases) |case| {
        Recorder.reset();
        var pager = pagerOf(case.count, case.capacity, case.page);
        pager.label_format = .range;
        var widget = widgetAt();
        _ = abi.ra8_widget_pager_init(&widget, &pager);
        abi.ra8_widget_pager_vtable().render.?(&widget);
        try std.testing.expectEqualStrings(case.expected, std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[1].text))));
    }
}

test "range format with no pages displays a zero range" {
    Recorder.reset();
    var pager = pagerOf(14, 0, 0);
    pager.label_format = .range;
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    abi.ra8_widget_pager_vtable().render.?(&widget);
    try std.testing.expectEqualStrings("0 to 0 of 0", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[1].text))));
}

test "zero-initialized label format preserves the page label" {
    Recorder.reset();
    var pager = std.mem.zeroes(abi.Pager);
    pager.paint = &paint;
    pager.item_count = 21;
    pager.page_capacity = 10;
    pager.bg = bg_color;
    pager.fg = fg_color;
    pager.fg_disabled = disabled_color;
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    abi.ra8_widget_pager_vtable().render.?(&widget);
    try std.testing.expectEqualStrings("Page 1 of 3", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[1].text))));
}

test "empty content displays zero of zero and disables both directions" {
    Recorder.reset();
    var pager = pagerOf(0, 10, 0);
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    abi.ra8_widget_pager_vtable().render.?(&widget);

    try std.testing.expectEqualStrings("Previous", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[0].text))));
    try std.testing.expectEqual(disabled_color, Recorder.draws.buffer[0].fg);
    try std.testing.expectEqualStrings("Page 0 of 0", std.mem.span(@as([*:0]const u8, @ptrCast(&Recorder.draws.buffer[1].text))));
    try std.testing.expectEqual(disabled_color, Recorder.draws.buffer[2].fg);
}

test "non-touch events and touches outside the widget are declined" {
    var pager = pagerOf(21, 10, 1);
    var widget = widgetAt();
    _ = abi.ra8_widget_pager_init(&widget, &pager);
    const button: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 1, .x = 323, .y = 920 };
    const outside: abi.Event = .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 323, .y = 949 };
    try std.testing.expect(!abi.ra8_widget_pager_vtable().on_input.?(&widget, &button));
    try std.testing.expect(!abi.ra8_widget_pager_vtable().on_input.?(&widget, &outside));
    try std.testing.expectEqual(@as(u16, 1), pager.page);
    try std.testing.expect(!widget.dirty);
}
