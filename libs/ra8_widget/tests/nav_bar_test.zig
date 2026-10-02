//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the nav-strip membrane: the descriptor layout, the bind guards,
//! the cell tiling, the active vs muted colour, and the tap routing. The cell
//! maths and the hit maths have to be exact inverses, so they are checked
//! directly and then again through a touch that must land in the drawn cell.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitutes for the two C symbols the library leaves undefined:
/// the logger and `ra8_widget_invalidate` from the still-C `ra8_widget.c`.
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
    text: [*:0]const u8,
    fg: u32,
    bg: u32,
};

/// Recording paint backend.
const Recorder = struct {
    var fills: std.BoundedArray(Fill, 8) = .{};
    var draws: std.BoundedArray(Draw, 8) = .{};

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

    /// Fixed-width measurement so centring has something to halve.
    fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        out_w.* = @intCast(std.mem.span(str).len * 6);
        out_h.* = 12;
    }
};

var selections: std.BoundedArray(u16, 32) = .{};

fn noteSelect(_: *abi.Widget, index: u16) callconv(.c) void {
    selections.append(index) catch unreachable;
}

fn reset() void {
    Recorder.fills = .{};
    Recorder.draws = .{};
    selections = .{};
    last_message = null;
    invalidations = 0;
    last_refresh = 0xFF;
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

const bg_color: u32 = 0x00101010;
const active_color: u32 = 0x00FFFFFF;
const muted_color: u32 = 0x00808080;

const labels = [_]?[*:0]const u8{ "library", "read", "search", "settings" };

fn navWith(paint: ?*const abi.Paint, items: ?[*]const ?[*:0]const u8, count: u16) abi.NavBar {
    return .{
        .paint = paint,
        .items = items,
        .on_select = noteSelect,
        .bg = bg_color,
        .fg_active = active_color,
        .fg_muted = muted_color,
        .count = count,
        .active = 1,
        .selected = 0xFFFF,
    };
}

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

/// 100 wide over 4 cells divides evenly; 101 does not, which is the case the
/// rounded prefix widths exist for.
const strip: abi.Rect = .{ .x = 10, .y = 300, .w = 100, .h = 40 };
const odd_strip: abi.Rect = .{ .x = 0, .y = 0, .w = 101, .h = 40 };

fn touchAt(x: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = 310 };
}

test "the descriptor mirrors ra8_widget_nav_bar_t" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(0, @offsetOf(abi.NavBar, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.NavBar, "items"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.NavBar, "on_select"));
    try std.testing.expectEqual(3 * ptr, @offsetOf(abi.NavBar, "bg"));
    try std.testing.expectEqual(3 * ptr + 12, @offsetOf(abi.NavBar, "count"));
    try std.testing.expectEqual(3 * ptr + 14, @offsetOf(abi.NavBar, "active"));
    try std.testing.expectEqual(3 * ptr + 16, @offsetOf(abi.NavBar, "selected"));
}

test "cells tile the strip with no gap and no overlap" {
    var x = strip.x;
    for (0..4) |i| {
        const cell = abi.cellRect(strip, @intCast(i), 4);
        try std.testing.expectEqual(x, cell.x);
        try std.testing.expectEqual(25, cell.w);
        try std.testing.expectEqual(strip.y, cell.y);
        try std.testing.expectEqual(strip.h, cell.h);
        x += cell.w;
    }
    try std.testing.expectEqual(strip.x + strip.w, x);
}

test "cellStart is the boundary both halves are built from" {
    // cellRect's edges ARE cellStart's values, which is the property the hit
    // maths leans on; a second expression here would defeat the point.
    for (0..5) |i| {
        const idx: u16 = @intCast(i);
        try std.testing.expectEqual(abi.cellStart(odd_strip.w, idx, 4), abi.cellRect(odd_strip, idx, 4).x - odd_strip.x);
    }
    try std.testing.expectEqual(0, abi.cellStart(odd_strip.w, 0, 4));
    try std.testing.expectEqual(odd_strip.w, abi.cellStart(odd_strip.w, 4, 4));
}

test "an indivisible width absorbs the remainder without a gap" {
    var x = odd_strip.x;
    var widths: [4]i32 = undefined;
    for (0..4) |i| {
        const cell = abi.cellRect(odd_strip, @intCast(i), 4);
        try std.testing.expectEqual(x, cell.x);
        widths[i] = cell.w;
        x += cell.w;
    }
    // 101 over 4: three 25s and one 26, in some order, tiling exactly.
    try std.testing.expectEqual(odd_strip.x + odd_strip.w, x);
    try std.testing.expectEqual(101, widths[0] + widths[1] + widths[2] + widths[3]);
}

/// Every pixel column of every cell routes to the cell it is drawn in.
fn expectHitsMatchCells(rect: abi.Rect, count: u16) !void {
    for (0..count) |i| {
        const idx: u16 = @intCast(i);
        const cell = abi.cellRect(rect, idx, count);
        if (cell.w == 0) continue; // rounded out of existence; nothing to hit
        var px = cell.x;
        while (px < cell.x + cell.w) : (px += 1) {
            try std.testing.expectEqual(idx, abi.hitCell(rect, count, px).?);
        }
    }
}

test "on a divisible width every cell's first and last pixel route to it" {
    for (0..4) |i| {
        const idx: u16 = @intCast(i);
        const cell = abi.cellRect(strip, idx, 4);
        try std.testing.expectEqual(idx, abi.hitCell(strip, 4, cell.x).?);
        try std.testing.expectEqual(idx, abi.hitCell(strip, 4, cell.x + cell.w - 1).?);
    }
    try expectHitsMatchCells(strip, 4);
}

test "an indivisible width routes each cell's first pixel to that same cell" {
    // The C this was ported from computed cell rects one way (`w * i /
    // count`) and hit-tested another (`(px - x) * count / w`), two expressions
    // that only coincide when `count` divides `w`: on a 101-wide strip cell 1
    // was drawn from x = 25 but a tap at x = 25 activated cell 0. Both halves
    // now derive from `cellStart`, so the leading edge belongs to its own cell.
    for (0..4) |i| {
        const idx: u16 = @intCast(i);
        const cell = abi.cellRect(odd_strip, idx, 4);
        try std.testing.expectEqual(idx, abi.hitCell(odd_strip, 4, cell.x).?);
        try std.testing.expectEqual(idx, abi.hitCell(odd_strip, 4, cell.x + cell.w - 1).?);
    }
    try expectHitsMatchCells(odd_strip, 4);
}

test "widths that divide and widths that do not all route pixel for pixel" {
    // The remainder lands in a different place for each of these, so every one
    // is a distinct rounding pattern rather than the same case restated.
    for ([_]i32{ 1, 2, 3, 7, 37, 99, 100, 101, 102, 103, 240, 241 }) |w| {
        for ([_]u16{ 1, 2, 3, 4, 5, 8 }) |count| {
            try expectHitsMatchCells(.{ .x = 17, .y = 0, .w = w, .h = 40 }, count);
        }
    }
}

test "a cell the rounding left zero pixels wide is never routed to" {
    // 3 pixels over 4 cells: boundaries 0, 0, 1, 2, so cell 0 is empty and the
    // strip's first pixel belongs to cell 1. Every tap still lands on a cell
    // that has pixels, and the strip stays fully covered.
    const thin: abi.Rect = .{ .x = 0, .y = 0, .w = 3, .h = 40 };
    try std.testing.expectEqual(0, abi.cellRect(thin, 0, 4).w);
    try std.testing.expectEqual(1, abi.hitCell(thin, 4, 0).?);
    try std.testing.expectEqual(2, abi.hitCell(thin, 4, 1).?);
    try std.testing.expectEqual(3, abi.hitCell(thin, 4, 2).?);
    try expectHitsMatchCells(thin, 4);
}

test "a tap off the strip, an empty strip and a degenerate width all miss" {
    try std.testing.expectEqual(null, abi.hitCell(strip, 4, strip.x - 1));
    try std.testing.expectEqual(null, abi.hitCell(strip, 4, strip.x + strip.w));
    try std.testing.expectEqual(null, abi.hitCell(strip, 0, strip.x + 1));
    try std.testing.expectEqual(null, abi.hitCell(.{ .x = 0, .y = 0, .w = 0, .h = 40 }, 4, 0));
    try std.testing.expectEqual(null, abi.hitCell(.{ .x = 0, .y = 0, .w = -8, .h = 40 }, 4, 0));
}

test "the last pixel of the strip stays in the last cell" {
    try std.testing.expectEqual(3, abi.hitCell(strip, 4, strip.x + strip.w - 1).?);
    try std.testing.expectEqual(3, abi.hitCell(odd_strip, 4, odd_strip.w - 1).?);
}

test "init binds the vtable, the context and visibility" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_nav_bar_init(&w, &nav));
    try std.testing.expectEqual(abi.ra8_widget_nav_bar_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(*anyopaque, @ptrCast(&nav)), w.ctx.?);
    try std.testing.expect(w.visible);
}

test "init refuses a null widget or descriptor and leaves nothing bound" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_nav_bar_init(null, &nav));
    try std.testing.expect(last_message != null);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_nav_bar_init(&w, null));
    try std.testing.expectEqual(null, w.vt);
    try std.testing.expect(!w.visible);
}

test "the vtable measures nothing and is shared by every strip" {
    const vt = abi.ra8_widget_nav_bar_vtable();
    try std.testing.expectEqual(null, vt.measure);
    try std.testing.expect(vt.render != null);
    try std.testing.expect(vt.on_input != null);
    try std.testing.expectEqual(vt, abi.ra8_widget_nav_bar_vtable());
}

test "render fills the strip once and centres one label per cell" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);

    try std.testing.expectEqual(1, Recorder.fills.len);
    try std.testing.expectEqual(bg_color, Recorder.fills.get(0).color);
    try std.testing.expectEqual(strip.w, Recorder.fills.get(0).w);
    try std.testing.expectEqual(4, Recorder.draws.len);

    // "read" is 4 glyphs at 6px, centred in cell 1 (x = 35, w = 25).
    try std.testing.expectEqualStrings("read", std.mem.span(Recorder.draws.get(1).text));
    try std.testing.expectEqual(35 + @divTrunc(25 - 24, 2), Recorder.draws.get(1).x);
    try std.testing.expectEqual(strip.y + @divTrunc(strip.h - 12, 2), Recorder.draws.get(1).y);
    try std.testing.expectEqual(bg_color, Recorder.draws.get(1).bg);
}

test "only the active cell gets the active colour" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);

    try std.testing.expectEqual(muted_color, Recorder.draws.get(0).fg);
    try std.testing.expectEqual(active_color, Recorder.draws.get(1).fg);
    try std.testing.expectEqual(muted_color, Recorder.draws.get(2).fg);
    try std.testing.expectEqual(muted_color, Recorder.draws.get(3).fg);
}

test "an out-of-range active index simply mutes every cell" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    nav.active = 9;
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);

    for (0..4) |i| {
        try std.testing.expectEqual(muted_color, Recorder.draws.get(i).fg);
    }
}

test "a null label is a gap, not a crash" {
    reset();
    const gapped = [_]?[*:0]const u8{ "one", null, "three" };
    var nav = navWith(&full_backend, &gapped, gapped.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);

    try std.testing.expectEqual(2, Recorder.draws.len);
    try std.testing.expectEqualStrings("one", std.mem.span(Recorder.draws.get(0).text));
    try std.testing.expectEqualStrings("three", std.mem.span(Recorder.draws.get(1).text));
}

test "an empty strip, a null item array and no draw_text all stop after the fill" {
    reset();
    var empty = navWith(&full_backend, &labels, 0);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &empty);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);
    try std.testing.expectEqual(1, Recorder.fills.len);
    try std.testing.expectEqual(0, Recorder.draws.len);

    reset();
    var no_items = navWith(&full_backend, null, 4);
    var w2 = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w2, &no_items);
    abi.ra8_widget_nav_bar_vtable().render.?(&w2);
    try std.testing.expectEqual(1, Recorder.fills.len);
    try std.testing.expectEqual(0, Recorder.draws.len);

    reset();
    var no_text = navWith(&fill_only_backend, &labels, labels.len);
    var w3 = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w3, &no_text);
    abi.ra8_widget_nav_bar_vtable().render.?(&w3);
    try std.testing.expectEqual(1, Recorder.fills.len);
    try std.testing.expectEqual(0, Recorder.draws.len);
}

test "render touches nothing without a paint backend" {
    reset();
    var nav = navWith(null, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);
    try std.testing.expectEqual(0, Recorder.fills.len);
}

test "a tap records the cell, invalidates fast and notifies once" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);

    // x = 62 is 52 into a 100-wide strip: cell 2.
    const event = touchAt(62);
    try std.testing.expect(abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(2, nav.selected);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.fast), last_refresh);
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(1, selections.len);
    try std.testing.expectEqual(2, selections.get(0));
}

test "a tap anywhere inside a drawn cell selects that cell" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);

    for (0..4) |i| {
        const idx: u16 = @intCast(i);
        const cell = abi.cellRect(strip, idx, 4);
        for ([_]i32{ cell.x, cell.x + @divTrunc(cell.w, 2), cell.x + cell.w - 1 }) |px| {
            const event = touchAt(px);
            try std.testing.expect(abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
            try std.testing.expectEqual(idx, nav.selected);
        }
    }
}

test "a label wider than its cell overflows without moving the tap target" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);
    abi.ra8_widget_nav_bar_vtable().render.?(&w);

    // "settings" is 48px of text in a 25px cell, so centring puts the pen left
    // of the cell it belongs to. The cell itself is unchanged, and that is
    // what the tap follows.
    const cell = abi.cellRect(strip, 3, 4);
    try std.testing.expect(Recorder.draws.get(3).x < cell.x);
    const event = touchAt(cell.x);
    try std.testing.expect(abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(3, nav.selected);
}

test "a tap off the strip is declined and changes nothing" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);

    const event = touchAt(strip.x + strip.w);
    try std.testing.expect(!abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0xFFFF, nav.selected);
    try std.testing.expectEqual(0, invalidations);
    try std.testing.expectEqual(0, selections.len);
}

test "an empty strip declines the touch outright" {
    reset();
    var nav = navWith(&full_backend, &labels, 0);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);

    const event = touchAt(strip.x + 1);
    try std.testing.expect(!abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0, invalidations);
}

test "a button event is declined so it keeps travelling" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);

    const event: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 2, .x = 20, .y = 310 };
    try std.testing.expect(!abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0xFFFF, nav.selected);
    try std.testing.expectEqual(0, invalidations);
}

test "a tap with no callback bound still records and invalidates" {
    reset();
    var nav = navWith(&full_backend, &labels, labels.len);
    nav.on_select = null;
    var w = widgetAt(strip);
    _ = abi.ra8_widget_nav_bar_init(&w, &nav);

    const event = touchAt(strip.x + 1);
    try std.testing.expect(abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0, nav.selected);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(0, selections.len);
}

test "a widget with no context declines input and renders nothing" {
    reset();
    var w = widgetAt(strip);
    const event = touchAt(strip.x + 1);
    try std.testing.expect(!abi.ra8_widget_nav_bar_vtable().on_input.?(&w, &event));
    abi.ra8_widget_nav_bar_vtable().render.?(&w);
    try std.testing.expectEqual(0, Recorder.fills.len);
}
