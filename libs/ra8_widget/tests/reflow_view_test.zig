//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the reflow-view membrane: the paging maths on its own, then the
//! render and input paths against a recording substitute for the reflow-engine
//! seam, so what is under test is the routing rather than any pixel work.

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

var fills: u32 = 0;
var last_fill_rect: abi.Rect = undefined;
var last_fill_color: u32 = 0;

var page_paints: u32 = 0;
var last_paint_page: u16 = 0xFFFF;
var last_body: abi.Rect = undefined;

var link_calls: u32 = 0;
var last_link_page: u16 = 0xFFFF;
var last_link_x: i32 = 0;
var last_link_y: i32 = 0;
var link_answer: bool = false;
var link_dest: u16 = 0;

fn reset() void {
    last_message = null;
    invalidations = 0;
    last_refresh = 0xFF;
    fills = 0;
    last_fill_color = 0;
    page_paints = 0;
    last_paint_page = 0xFFFF;
    link_calls = 0;
    last_link_page = 0xFFFF;
    last_link_x = 0;
    last_link_y = 0;
    link_answer = false;
    link_dest = 0;
}

fn fillRect(user: ?*anyopaque, x: i32, y: i32, wid: i32, hei: i32, color: u32) callconv(.c) void {
    _ = user;
    fills += 1;
    last_fill_rect = .{ .x = x, .y = y, .w = wid, .h = hei };
    last_fill_color = color;
}

const backend: abi.Paint = .{
    .user = null,
    .fill_rect = fillRect,
    .draw_text = null,
    .text_size = null,
};

fn paintPage(user: ?*anyopaque, page: u16, body: *const abi.Rect) callconv(.c) void {
    _ = user;
    page_paints += 1;
    last_paint_page = page;
    last_body = body.*;
}

fn followLink(user: ?*anyopaque, page: u16, x: i32, y: i32, out_page: *u16) callconv(.c) bool {
    _ = user;
    link_calls += 1;
    last_link_page = page;
    last_link_x = x;
    last_link_y = y;
    if (!link_answer) return false;
    out_page.* = link_dest;
    return true;
}

var full_ops: abi.Ops = .{
    .user = null,
    .render_page = paintPage,
    .follow_link = followLink,
};

fn viewWith(ops: ?*const abi.Ops, page: u16, count: u16) abi.ReflowView {
    return .{
        .paint = &backend,
        .ops = ops,
        .bg = 0x00FFFFFF,
        .page = page,
        .page_count = count,
        .margin_x = 24,
        .margin_y = 8,
    };
}

const band: abi.Rect = .{ .x = 10, .y = 100, .w = 600, .h = 800 };

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

fn touchAt(x: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = 400 };
}

test "the descriptors mirror ra8_widget_reflow_view_t and its ops seam" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(0, @offsetOf(abi.Ops, "user"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.Ops, "render_page"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Ops, "follow_link"));

    try std.testing.expectEqual(0, @offsetOf(abi.ReflowView, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.ReflowView, "ops"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.ReflowView, "bg"));
    try std.testing.expectEqual(2 * ptr + 4, @offsetOf(abi.ReflowView, "page"));
    try std.testing.expectEqual(2 * ptr + 6, @offsetOf(abi.ReflowView, "page_count"));
    try std.testing.expectEqual(2 * ptr + 8, @offsetOf(abi.ReflowView, "margin_x"));
    try std.testing.expectEqual(2 * ptr + 10, @offsetOf(abi.ReflowView, "margin_y"));
}

test "the body rect insets the widget rect on both sides of both axes" {
    const body = abi.bodyRect(band, 24, 8);
    try std.testing.expectEqual(34, body.x);
    try std.testing.expectEqual(108, body.y);
    try std.testing.expectEqual(600 - 48, body.w);
    try std.testing.expectEqual(800 - 16, body.h);
}

test "zero margins leave the body equal to the widget rect" {
    const body = abi.bodyRect(band, 0, 0);
    try std.testing.expectEqual(band.x, body.x);
    try std.testing.expectEqual(band.y, body.y);
    try std.testing.expectEqual(band.w, body.w);
    try std.testing.expectEqual(band.h, body.h);
}

test "a page in range passes through the clamp untouched" {
    try std.testing.expectEqual(0, abi.clampPage(0, 12));
    try std.testing.expectEqual(7, abi.clampPage(7, 12));
    try std.testing.expectEqual(11, abi.clampPage(11, 12));
}

test "a page past the end clamps to the last page" {
    try std.testing.expectEqual(11, abi.clampPage(12, 12));
    try std.testing.expectEqual(11, abi.clampPage(0xFFFF, 12));
    try std.testing.expectEqual(0, abi.clampPage(5, 1));
}

test "an empty book clamps every page to the first" {
    try std.testing.expectEqual(0, abi.clampPage(0, 0));
    try std.testing.expectEqual(0, abi.clampPage(9, 0));
}

test "the tap midpoint splits the widget rect down the middle" {
    try std.testing.expectEqual(310, abi.tapMidpoint(band));
    try std.testing.expectEqual(0, abi.tapMidpoint(.{ .x = 0, .y = 0, .w = 1, .h = 10 }));
}

test "a tap on the right half steps forward, a tap on the left steps back" {
    var view = viewWith(&full_ops, 4, 12);
    try std.testing.expect(abi.turnPage(&view, 400, 310));
    try std.testing.expectEqual(5, view.page);
    try std.testing.expect(abi.turnPage(&view, 100, 310));
    try std.testing.expectEqual(4, view.page);
}

test "the midpoint itself steps forward" {
    var view = viewWith(&full_ops, 0, 12);
    try std.testing.expect(abi.turnPage(&view, 310, 310));
    try std.testing.expectEqual(1, view.page);
}

test "both ends of the book are walls rather than wraps" {
    var first = viewWith(&full_ops, 0, 12);
    try std.testing.expect(!abi.turnPage(&first, 100, 310));
    try std.testing.expectEqual(0, first.page);

    var last = viewWith(&full_ops, 11, 12);
    try std.testing.expect(!abi.turnPage(&last, 400, 310));
    try std.testing.expectEqual(11, last.page);
}

test "a one-page book cannot turn in either direction" {
    var view = viewWith(&full_ops, 0, 1);
    try std.testing.expect(!abi.turnPage(&view, 100, 310));
    try std.testing.expect(!abi.turnPage(&view, 400, 310));
    try std.testing.expectEqual(0, view.page);
}

test "an empty book cannot turn forward" {
    var view = viewWith(&full_ops, 0, 0);
    try std.testing.expect(!abi.turnPage(&view, 400, 310));
    try std.testing.expectEqual(0, view.page);
}

test "init binds the vtable, the context and visibility" {
    reset();
    var view = viewWith(&full_ops, 0, 12);
    var w = widgetAt(band);

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_reflow_view_init(&w, &view));
    try std.testing.expectEqual(abi.ra8_widget_reflow_view_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(*anyopaque, @ptrCast(&view)), w.ctx.?);
    try std.testing.expect(w.visible);
}

test "init refuses a null widget or a null descriptor" {
    var view = viewWith(&full_ops, 0, 12);
    var w = widgetAt(band);
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_reflow_view_init(null, &view));
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_reflow_view_init(&w, null));
    try std.testing.expect(last_message != null);
}

test "the vtable renders and routes but does not measure" {
    const vt = abi.ra8_widget_reflow_view_vtable();
    try std.testing.expectEqual(null, vt.measure);
    try std.testing.expect(vt.render != null);
    try std.testing.expect(vt.on_input != null);
    try std.testing.expectEqual(vt, abi.ra8_widget_reflow_view_vtable());
}

test "render clears the widget rect and paints the page inside the margins" {
    reset();
    var view = viewWith(&full_ops, 3, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    w.vt.?.render.?(&w);

    try std.testing.expectEqual(1, fills);
    try std.testing.expectEqual(band.x, last_fill_rect.x);
    try std.testing.expectEqual(band.w, last_fill_rect.w);
    try std.testing.expectEqual(view.bg, last_fill_color);

    try std.testing.expectEqual(1, page_paints);
    try std.testing.expectEqual(3, last_paint_page);
    try std.testing.expectEqual(abi.bodyRect(band, 24, 8).x, last_body.x);
    try std.testing.expectEqual(abi.bodyRect(band, 24, 8).w, last_body.w);
}

test "render with no paint backend still paints the page" {
    reset();
    var view = viewWith(&full_ops, 2, 12);
    view.paint = null;
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    w.vt.?.render.?(&w);

    try std.testing.expectEqual(0, fills);
    try std.testing.expectEqual(1, page_paints);
    try std.testing.expectEqual(2, last_paint_page);
}

test "an inert seam clears the body and paints nothing" {
    reset();
    var no_ops = viewWith(null, 2, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &no_ops);
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(1, fills);
    try std.testing.expectEqual(0, page_paints);

    reset();
    var blind: abi.Ops = .{ .user = null, .render_page = null, .follow_link = followLink };
    var no_painter = viewWith(&blind, 2, 12);
    var w2 = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w2, &no_painter);
    w2.vt.?.render.?(&w2);
    try std.testing.expectEqual(1, fills);
    try std.testing.expectEqual(0, page_paints);
}

test "render and input on a widget with no context do nothing" {
    reset();
    var w = widgetAt(band);
    const vt = abi.ra8_widget_reflow_view_vtable();
    vt.render.?(&w);
    try std.testing.expectEqual(0, fills);
    try std.testing.expectEqual(0, page_paints);

    const tap = touchAt(400);
    try std.testing.expect(!vt.on_input.?(&w, &tap));
}

test "the link seam sees the tap and the page it was made on" {
    reset();
    var view = viewWith(&full_ops, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const tap = touchAt(400);
    try std.testing.expect(w.vt.?.on_input.?(&w, &tap));

    try std.testing.expectEqual(1, link_calls);
    try std.testing.expectEqual(4, last_link_page);
    try std.testing.expectEqual(400, last_link_x);
    try std.testing.expectEqual(400, last_link_y);
}

test "a followed link adopts its destination page and skips the page turn" {
    reset();
    link_answer = true;
    link_dest = 9;
    var view = viewWith(&full_ops, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    // a right-half tap, which would otherwise step 4 -> 5
    const tap = touchAt(400);
    try std.testing.expect(w.vt.?.on_input.?(&w, &tap));
    try std.testing.expectEqual(9, view.page);
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.quality), last_refresh);
}

test "a link destination past the end of the book is clamped, not adopted" {
    reset();
    link_answer = true;
    link_dest = 99;
    var view = viewWith(&full_ops, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const tap = touchAt(400);
    try std.testing.expect(w.vt.?.on_input.?(&w, &tap));
    try std.testing.expectEqual(11, view.page);
}

test "a declined link falls through to the page turn" {
    reset();
    link_answer = false;
    var view = viewWith(&full_ops, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const forward = touchAt(400);
    try std.testing.expect(w.vt.?.on_input.?(&w, &forward));
    try std.testing.expectEqual(5, view.page);
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.quality), last_refresh);
}

test "a view with no link callback turns the page directly" {
    reset();
    var turner: abi.Ops = .{ .user = null, .render_page = paintPage, .follow_link = null };
    var view = viewWith(&turner, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const back = touchAt(100);
    try std.testing.expect(w.vt.?.on_input.?(&w, &back));
    try std.testing.expectEqual(0, link_calls);
    try std.testing.expectEqual(3, view.page);
}

test "a view with no seam at all still turns the page" {
    reset();
    var view = viewWith(null, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const forward = touchAt(400);
    try std.testing.expect(w.vt.?.on_input.?(&w, &forward));
    try std.testing.expectEqual(5, view.page);
}

test "a tap at a boundary is consumed but leaves the view clean" {
    reset();
    link_answer = false;
    var view = viewWith(&full_ops, 0, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const back = touchAt(100);
    try std.testing.expect(w.vt.?.on_input.?(&w, &back));
    try std.testing.expectEqual(0, view.page);
    try std.testing.expect(!w.dirty);
    try std.testing.expectEqual(0, invalidations);
}

test "a button event is declined without reaching the seam" {
    reset();
    var view = viewWith(&full_ops, 4, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    const press: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 3, .x = 0, .y = 0 };
    try std.testing.expect(!w.vt.?.on_input.?(&w, &press));
    try std.testing.expectEqual(0, link_calls);
    try std.testing.expectEqual(4, view.page);
}

test "taps walk the book end to end and stop at each wall" {
    reset();
    link_answer = false;
    var view = viewWith(&full_ops, 0, 4);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    for (0..6) |_| _ = w.vt.?.on_input.?(&w, &touchAt(400));
    try std.testing.expectEqual(3, view.page);

    for (0..6) |_| _ = w.vt.?.on_input.?(&w, &touchAt(100));
    try std.testing.expectEqual(0, view.page);
}

test "the page the seam paints follows the page the taps left behind" {
    reset();
    link_answer = false;
    var view = viewWith(&full_ops, 0, 12);
    var w = widgetAt(band);
    _ = abi.ra8_widget_reflow_view_init(&w, &view);

    _ = w.vt.?.on_input.?(&w, &touchAt(400));
    _ = w.vt.?.on_input.?(&w, &touchAt(400));
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(2, last_paint_page);
}
