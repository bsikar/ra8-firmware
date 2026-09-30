//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host suite for the flat container ops. `ra8_box` and `ra8_ui` are sibling
//! libraries with their own suites, so they are substituted here: the box
//! stand-in records exactly which nodes the core asked for and hands back
//! recognisable rects, which is what lets these tests pin the core's own
//! behaviour (tree shape, measure pass, copy-back order) rather than
//! re-testing someone else's layout maths.

const std = @import("std");
const abi = @import("abi");

// ---------------------------------------------------------------------------
// Substitutes for the sibling archives.
// ---------------------------------------------------------------------------

var last_log: ?[*:0]const u8 = null;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_log = message;
}

export fn ra8_ui_rect_contains(r: *const abi.Rect, px: i32, py: i32) callconv(.c) bool {
    return px >= r.x and px < r.x + r.w and py >= r.y and py < r.y + r.h;
}

var init_calls: u32 = 0;
var layout_calls: u32 = 0;
var layout_root: i16 = -1;
var layout_frame: abi.Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
var init_fails: bool = false;
var layout_fails: bool = false;
var add_fails_at: ?u16 = null;

export fn ra8_box_tree_init(tree: *abi.BoxTree, storage: [*]abi.Box, cap: u16) callconv(.c) u16 {
    init_calls += 1;
    if (init_fails) return abi.err.invalid_arg;
    tree.* = .{ .nodes = storage, .cap = cap, .count = 0 };
    return abi.err.ok;
}

export fn ra8_box_add(tree: *abi.BoxTree, parent: i16, node: *const abi.Box) callconv(.c) i16 {
    if (add_fails_at) |n| {
        if (tree.count == n) return -1;
    }
    if (tree.count >= tree.cap) return -1;
    const idx = tree.count;
    tree.nodes.?[idx] = node.*;
    tree.nodes.?[idx].first_child = parent;
    tree.count += 1;
    return @intCast(idx);
}

export fn ra8_box_layout(tree: *abi.BoxTree, root: i16, frame: *const abi.Rect) callconv(.c) u16 {
    layout_calls += 1;
    layout_root = root;
    layout_frame = frame.*;
    if (layout_fails) return abi.err.invalid_arg;
    // Recognisable output: node i lands at x = 1000 + i so a copy-back that
    // takes the wrong node is visible in the assertion.
    var i: u16 = 0;
    while (i < tree.count) : (i += 1) {
        tree.nodes.?[i].rect = .{ .x = 1000 + @as(i32, i), .y = 7, .w = 20, .h = 30 };
    }
    return abi.err.ok;
}

fn resetSubstitutes() void {
    last_log = null;
    init_calls = 0;
    layout_calls = 0;
    layout_root = -1;
    layout_frame = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
    init_fails = false;
    layout_fails = false;
    add_fails_at = null;
}

// ---------------------------------------------------------------------------
// Widget fixtures.
// ---------------------------------------------------------------------------

var renders: u32 = 0;
var rendered: [8]u16 = @splat(0);
var inputs: u32 = 0;
var consume: bool = false;
var measure_calls: u32 = 0;
var measure_avail_w: i32 = 0;
var measure_avail_h: i32 = 0;
var measure_answer_w: i32 = 0;
var measure_answer_h: i32 = 0;

fn onRender(w: *abi.Widget) callconv(.c) void {
    if (renders < rendered.len) rendered[renders] = w.action_id;
    renders += 1;
}

fn onInput(_: *abi.Widget, _: *const abi.Event) callconv(.c) bool {
    inputs += 1;
    return consume;
}

fn onMeasure(_: *abi.Widget, avail_w: i32, avail_h: i32, out_w: *i32, out_h: *i32) callconv(.c) void {
    measure_calls += 1;
    measure_avail_w = avail_w;
    measure_avail_h = avail_h;
    out_w.* = measure_answer_w;
    out_h.* = measure_answer_h;
}

const full_vt: abi.Vtable = .{ .measure = onMeasure, .render = onRender, .on_input = onInput };
const render_only_vt: abi.Vtable = .{ .measure = null, .render = onRender, .on_input = null };
const input_only_vt: abi.Vtable = .{ .measure = null, .render = null, .on_input = onInput };
const empty_vt: abi.Vtable = .{ .measure = null, .render = null, .on_input = null };

fn widgetOf(vt: ?*const abi.Vtable, action_id: u16) abi.Widget {
    return .{
        .vt = vt,
        .ctx = null,
        .rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 },
        .fixed = 0,
        .flex = 1,
        .action_id = action_id,
        .refresh = 0,
        .visible = true,
        .dirty = false,
    };
}

fn reset() void {
    resetSubstitutes();
    renders = 0;
    rendered = @splat(0);
    inputs = 0;
    consume = false;
    measure_calls = 0;
    measure_avail_w = 0;
    measure_avail_h = 0;
    measure_answer_w = 0;
    measure_answer_h = 0;
}

// ---------------------------------------------------------------------------
// rectUnion / rectEmpty.
// ---------------------------------------------------------------------------

test "a rect with no width and no height is the union identity" {
    const empty: abi.Rect = .{ .x = 5, .y = 5, .w = 0, .h = 0 };
    const r: abi.Rect = .{ .x = 1, .y = 2, .w = 3, .h = 4 };
    try std.testing.expect(abi.rectEmpty(empty));
    try std.testing.expectEqual(r, abi.rectUnion(empty, r));
    try std.testing.expectEqual(r, abi.rectUnion(r, empty));
}

test "a rect with one positive dimension is not empty" {
    try std.testing.expect(!abi.rectEmpty(.{ .x = 0, .y = 0, .w = 4, .h = 0 }));
    try std.testing.expect(!abi.rectEmpty(.{ .x = 0, .y = 0, .w = 0, .h = 4 }));
    try std.testing.expect(abi.rectEmpty(.{ .x = 0, .y = 0, .w = -2, .h = -2 }));
}

test "the union of two rects covers both corners" {
    const a: abi.Rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };
    const b: abi.Rect = .{ .x = 20, .y = 5, .w = 5, .h = 20 };
    try std.testing.expectEqual(
        abi.Rect{ .x = 0, .y = 0, .w = 25, .h = 25 },
        abi.rectUnion(a, b),
    );
}

test "a contained rect does not grow the union" {
    const outer: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };
    const inner: abi.Rect = .{ .x = 10, .y = 10, .w = 5, .h = 5 };
    try std.testing.expectEqual(outer, abi.rectUnion(outer, inner));
    try std.testing.expectEqual(outer, abi.rectUnion(inner, outer));
}

test "the union folds negative origins" {
    const a: abi.Rect = .{ .x = -30, .y = -10, .w = 5, .h = 5 };
    const b: abi.Rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };
    try std.testing.expectEqual(
        abi.Rect{ .x = -30, .y = -10, .w = 40, .h = 20 },
        abi.rectUnion(a, b),
    );
}

test "visibleCount counts only the visible widgets" {
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    try std.testing.expectEqual(3, abi.visibleCount(&ws));
    ws[1].visible = false;
    try std.testing.expectEqual(2, abi.visibleCount(&ws));
    for (&ws) |*w| w.visible = false;
    try std.testing.expectEqual(0, abi.visibleCount(&ws));
    try std.testing.expectEqual(0, abi.visibleCount(&[_]abi.Widget{}));
}

// ---------------------------------------------------------------------------
// measuredExtent.
// ---------------------------------------------------------------------------

test "a flexing widget is never measured" {
    reset();
    var w = widgetOf(&full_vt, 0);
    w.flex = 3;
    measure_answer_h = 40;
    try std.testing.expectEqual(0, abi.measuredExtent(&w, .col, 100, 100));
    try std.testing.expectEqual(0, measure_calls);
}

test "a widget with no vtable or no measure reports nothing" {
    reset();
    var bare = widgetOf(null, 0);
    bare.flex = 0;
    try std.testing.expectEqual(0, abi.measuredExtent(&bare, .col, 100, 100));

    var no_measure = widgetOf(&render_only_vt, 0);
    no_measure.flex = 0;
    try std.testing.expectEqual(0, abi.measuredExtent(&no_measure, .col, 100, 100));
    try std.testing.expectEqual(0, measure_calls);
}

test "the measured extent follows the stack axis" {
    reset();
    var w = widgetOf(&full_vt, 0);
    w.flex = 0;
    measure_answer_w = 30;
    measure_answer_h = 40;
    try std.testing.expectEqual(30, abi.measuredExtent(&w, .row, 100, 100));
    try std.testing.expectEqual(40, abi.measuredExtent(&w, .col, 100, 100));
}

test "a measured extent is clamped to the space it was offered" {
    reset();
    var w = widgetOf(&full_vt, 0);
    w.flex = 0;
    measure_answer_h = 5000;
    try std.testing.expectEqual(60, abi.measuredExtent(&w, .col, 100, 60));
    try std.testing.expectEqual(100, measure_avail_w);
    try std.testing.expectEqual(60, measure_avail_h);
}

test "an extent beyond the fixed field's range is capped, not wrapped" {
    reset();
    var w = widgetOf(&full_vt, 0);
    w.flex = 0;
    measure_answer_h = 70000;
    // The clamp against the available box would let 70000 through if the box
    // is larger, so the second clamp is the one under test here.
    try std.testing.expectEqual(32767, abi.measuredExtent(&w, .col, 100, 100000));
}

test "a widget that asks for nothing usable keeps its flex sizing" {
    reset();
    var w = widgetOf(&full_vt, 0);
    w.flex = 0;
    measure_answer_h = 0;
    try std.testing.expectEqual(0, abi.measuredExtent(&w, .col, 100, 100));
    measure_answer_h = -20;
    try std.testing.expectEqual(0, abi.measuredExtent(&w, .col, 100, 100));
}

// ---------------------------------------------------------------------------
// layout_stack.
// ---------------------------------------------------------------------------

test "layout_stack refuses a null widget array, frame or scratch" {
    reset();
    var ws = [_]abi.Widget{widgetOf(null, 0)};
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_layout_stack(null, 1, &frame, .col, 0, 0, &scratch, 4),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_layout_stack(&ws, 1, null, .col, 0, 0, &scratch, 4),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, null, 4),
    );
    try std.testing.expectEqual(0, init_calls);
}

test "layout_stack copies each visible widget's rect back in add order" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    var scratch: [8]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_layout_stack(&ws, 3, &frame, .col, 4, 2, &scratch, 8),
    );
    // Node 0 is the container, so the children are nodes 1, 2, 3.
    try std.testing.expectEqual(1001, ws[0].rect.x);
    try std.testing.expectEqual(1002, ws[1].rect.x);
    try std.testing.expectEqual(1003, ws[2].rect.x);
    try std.testing.expectEqual(1, layout_calls);
    try std.testing.expectEqual(0, layout_root);
    try std.testing.expectEqual(frame, layout_frame);
}

test "an invisible widget gets no leaf and keeps its own rect" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    ws[1].visible = false;
    ws[1].rect = .{ .x = -1, .y = -1, .w = -1, .h = -1 };
    var scratch: [8]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_layout_stack(&ws, 3, &frame, .col, 0, 0, &scratch, 8),
    );
    try std.testing.expectEqual(1001, ws[0].rect.x);
    try std.testing.expectEqual(-1, ws[1].rect.x);
    try std.testing.expectEqual(1002, ws[2].rect.x);
}

test "the container carries the axis, gap and padding it was given" {
    reset();
    var ws = [_]abi.Widget{widgetOf(null, 0)};
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    _ = abi.ra8_widget_layout_stack(&ws, 1, &frame, .row, 6, 3, &scratch, 4);
    try std.testing.expectEqual(abi.box.stack_h, scratch[0].kind);
    try std.testing.expectEqual(6, scratch[0].gap);
    try std.testing.expectEqual(3, scratch[0].pad);
    try std.testing.expectEqual(1, scratch[0].flex);
    try std.testing.expectEqual(abi.box.none, scratch[0].tag);

    _ = abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, &scratch, 4);
    try std.testing.expectEqual(abi.box.stack_v, scratch[0].kind);
}

test "a leaf carries the widget's fixed extent, flex weight and action id" {
    reset();
    var ws = [_]abi.Widget{widgetOf(null, 77)};
    ws[0].fixed = 24;
    ws[0].flex = 5;
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    _ = abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, &scratch, 4);
    try std.testing.expectEqual(abi.box.leaf, scratch[1].kind);
    try std.testing.expectEqual(24, scratch[1].fixed);
    try std.testing.expectEqual(5, scratch[1].flex);
    try std.testing.expectEqual(77, scratch[1].tag);
}

test "a widget pinning no extent is measured against the frame's content box" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&full_vt, 0)};
    ws[0].fixed = 0;
    ws[0].flex = 0;
    measure_answer_h = 18;
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 60 };

    _ = abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 5, &scratch, 4);
    try std.testing.expectEqual(1, measure_calls);
    // The content box is the frame inset by the padding on both sides.
    try std.testing.expectEqual(90, measure_avail_w);
    try std.testing.expectEqual(50, measure_avail_h);
    try std.testing.expectEqual(18, scratch[1].fixed);
}

test "a widget that already pins an extent is not measured" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&full_vt, 0)};
    ws[0].fixed = 12;
    ws[0].flex = 0;
    measure_answer_h = 99;
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    _ = abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, &scratch, 4);
    try std.testing.expectEqual(0, measure_calls);
    try std.testing.expectEqual(12, scratch[1].fixed);
}

test "padding wider than the frame offers a content box of zero, not a negative one" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&full_vt, 0)};
    ws[0].fixed = 0;
    ws[0].flex = 0;
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };

    _ = abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 40, &scratch, 4);
    try std.testing.expectEqual(0, measure_avail_w);
    try std.testing.expectEqual(0, measure_avail_h);
}

test "a scratch too small for the container plus its leaves is refused early" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    var scratch: [3]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_layout_stack(&ws, 3, &frame, .col, 0, 0, &scratch, 3),
    );
    try std.testing.expectEqual(0, init_calls);
    try std.testing.expectEqual(0, layout_calls);
}

test "three widgets with one hidden fit a scratch sized for two plus the container" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    ws[2].visible = false;
    var scratch: [3]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_layout_stack(&ws, 3, &frame, .col, 0, 0, &scratch, 3),
    );
}

test "a failure inside the box tree is forwarded, not swallowed" {
    reset();
    var ws = [_]abi.Widget{widgetOf(null, 0)};
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    init_fails = true;
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, &scratch, 4),
    );
    try std.testing.expectEqual(0, layout_calls);

    reset();
    layout_fails = true;
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, &scratch, 4),
    );
}

test "a refused container add stops the layout" {
    reset();
    var ws = [_]abi.Widget{widgetOf(null, 0)};
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    add_fails_at = 0;
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_layout_stack(&ws, 1, &frame, .col, 0, 0, &scratch, 4),
    );
    try std.testing.expectEqual(0, layout_calls);
}

test "a refused leaf add stops the layout" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1) };
    var scratch: [4]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    add_fails_at = 2;
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_layout_stack(&ws, 2, &frame, .col, 0, 0, &scratch, 4),
    );
    try std.testing.expectEqual(0, layout_calls);
}

test "a stack of no widgets still lays its container out" {
    reset();
    var ws = [_]abi.Widget{};
    var scratch: [2]abi.Box = @splat(.{});
    const frame: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };

    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_layout_stack(&ws, 0, &frame, .col, 0, 0, &scratch, 2),
    );
    try std.testing.expectEqual(1, layout_calls);
}

// ---------------------------------------------------------------------------
// dispatch.
// ---------------------------------------------------------------------------

fn touchAt(x: i32, y: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = y };
}

fn buttonPress(id: u16) abi.Event {
    return .{ .kind = .button, .reserved = 0, .button_id = id, .x = 0, .y = 0 };
}

test "dispatch refuses a null event or a null out flag" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&input_only_vt, 0)};
    var handled = false;
    const ev = touchAt(1, 1);

    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_dispatch(&ws, 1, null, &handled),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_dispatch(&ws, 1, &ev, null),
    );
}

test "dispatch refuses a null array only when it is asked to walk one" {
    reset();
    var handled = true;
    const ev = touchAt(1, 1);

    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_dispatch(null, 3, &ev, &handled),
    );
    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_dispatch(null, 0, &ev, &handled),
    );
    try std.testing.expect(!handled);
}

test "a touch is offered only to the widget it lands inside" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(&input_only_vt, 0), widgetOf(&input_only_vt, 1) };
    ws[0].rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };
    ws[1].rect = .{ .x = 20, .y = 0, .w = 10, .h = 10 };
    consume = true;

    var handled = false;
    const ev = touchAt(22, 3);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 2, &ev, &handled));
    try std.testing.expect(handled);
    try std.testing.expectEqual(1, inputs);
}

test "a touch inside a widget that declines it stops there" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(&input_only_vt, 0), widgetOf(&input_only_vt, 1) };
    ws[0].rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };
    ws[1].rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };
    consume = false;

    var handled = true;
    const ev = touchAt(5, 5);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 2, &ev, &handled));
    try std.testing.expect(!handled);
    // The second overlapping widget is never offered the touch.
    try std.testing.expectEqual(1, inputs);
}

test "a touch outside every widget is handled by nobody" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&input_only_vt, 0)};
    ws[0].rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };

    var handled = true;
    const ev = touchAt(50, 50);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 1, &ev, &handled));
    try std.testing.expect(!handled);
    try std.testing.expectEqual(0, inputs);
}

test "a button press is offered to each visible widget until one consumes it" {
    reset();
    var ws = [_]abi.Widget{
        widgetOf(&input_only_vt, 0),
        widgetOf(&input_only_vt, 1),
        widgetOf(&input_only_vt, 2),
    };
    consume = false;

    var handled = true;
    const ev = buttonPress(9);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 3, &ev, &handled));
    try std.testing.expect(!handled);
    try std.testing.expectEqual(3, inputs);

    reset();
    consume = true;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 3, &ev, &handled));
    try std.testing.expect(handled);
    try std.testing.expectEqual(1, inputs);
}

test "a button press ignores where the widget is drawn" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&input_only_vt, 0)};
    ws[0].rect = .{ .x = 500, .y = 500, .w = 1, .h = 1 };
    consume = true;

    var handled = false;
    const ev = buttonPress(3);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 1, &ev, &handled));
    try std.testing.expect(handled);
}

test "hidden widgets and widgets with no input callback are skipped" {
    reset();
    var ws = [_]abi.Widget{
        widgetOf(&input_only_vt, 0),
        widgetOf(&render_only_vt, 1),
        widgetOf(null, 2),
        widgetOf(&empty_vt, 3),
    };
    ws[0].visible = false;
    consume = true;

    var handled = true;
    const ev = buttonPress(1);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_dispatch(&ws, 4, &ev, &handled));
    try std.testing.expect(!handled);
    try std.testing.expectEqual(0, inputs);
}

// ---------------------------------------------------------------------------
// invalidate.
// ---------------------------------------------------------------------------

test "invalidate refuses a null widget" {
    reset();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_invalidate(null, .fast));
    try std.testing.expect(last_log != null);
}

test "invalidate refuses the none hint and leaves the widget alone" {
    reset();
    var w = widgetOf(null, 0);
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_invalidate(&w, .none));
    try std.testing.expect(!w.dirty);
    try std.testing.expectEqual(0, w.refresh);
}

test "invalidate marks the widget dirty and records the hint" {
    reset();
    var w = widgetOf(null, 0);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_invalidate(&w, .fast));
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.fast), w.refresh);
}

test "a weaker hint never downgrades one already asked for" {
    reset();
    var w = widgetOf(null, 0);
    _ = abi.ra8_widget_invalidate(&w, .quality);
    _ = abi.ra8_widget_invalidate(&w, .fast);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.quality), w.refresh);
}

test "a stronger hint upgrades the one already asked for" {
    reset();
    var w = widgetOf(null, 0);
    _ = abi.ra8_widget_invalidate(&w, .fast);
    _ = abi.ra8_widget_invalidate(&w, .quality);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.quality), w.refresh);
}

// ---------------------------------------------------------------------------
// damage.
// ---------------------------------------------------------------------------

test "damage refuses any null out parameter" {
    reset();
    var ws = [_]abi.Widget{widgetOf(null, 0)};
    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;

    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_damage(&ws, 1, null, &hint, &count),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_damage(&ws, 1, &rect, null, &count),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_damage(&ws, 1, &rect, &hint, null),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_damage(null, 2, &rect, &hint, &count),
    );
}

test "no dirty widget reports an empty rect, no hint and a zero count" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1) };
    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;

    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_damage(&ws, 2, &rect, &hint, &count),
    );
    try std.testing.expectEqual(abi.Rect{ .x = 0, .y = 0, .w = 0, .h = 0 }, rect);
    try std.testing.expectEqual(abi.Refresh.none, hint);
    try std.testing.expectEqual(0, count);
}

test "damage is the bounding rect of the dirty widgets only" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    ws[0].rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };
    ws[1].rect = .{ .x = 100, .y = 100, .w = 10, .h = 10 };
    ws[2].rect = .{ .x = 40, .y = 40, .w = 10, .h = 10 };
    ws[0].dirty = true;
    ws[2].dirty = true;

    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;
    _ = abi.ra8_widget_damage(&ws, 3, &rect, &hint, &count);
    try std.testing.expectEqual(abi.Rect{ .x = 0, .y = 0, .w = 50, .h = 50 }, rect);
    try std.testing.expectEqual(2, count);
}

test "a hidden widget contributes no damage even when it is dirty" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1) };
    ws[0].rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };
    ws[1].rect = .{ .x = 900, .y = 900, .w = 10, .h = 10 };
    ws[0].dirty = true;
    ws[1].dirty = true;
    ws[1].visible = false;

    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;
    _ = abi.ra8_widget_damage(&ws, 2, &rect, &hint, &count);
    try std.testing.expectEqual(abi.Rect{ .x = 0, .y = 0, .w = 10, .h = 10 }, rect);
    try std.testing.expectEqual(1, count);
}

test "the reported hint is the strongest any dirty widget asked for" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1), widgetOf(null, 2) };
    for (&ws) |*w| w.dirty = true;
    ws[0].refresh = @intFromEnum(abi.Refresh.fast);
    ws[1].refresh = @intFromEnum(abi.Refresh.quality);
    ws[2].refresh = @intFromEnum(abi.Refresh.fast);

    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;
    _ = abi.ra8_widget_damage(&ws, 3, &rect, &hint, &count);
    try std.testing.expectEqual(abi.Refresh.quality, hint);
    try std.testing.expectEqual(3, count);
}

test "a clean widget's hint is ignored however strong it is" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 0), widgetOf(null, 1) };
    ws[0].dirty = true;
    ws[0].refresh = @intFromEnum(abi.Refresh.fast);
    ws[1].dirty = false;
    ws[1].refresh = @intFromEnum(abi.Refresh.quality);

    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;
    _ = abi.ra8_widget_damage(&ws, 2, &rect, &hint, &count);
    try std.testing.expectEqual(abi.Refresh.fast, hint);
}

// ---------------------------------------------------------------------------
// render_dirty.
// ---------------------------------------------------------------------------

test "render_dirty refuses a null array only when it is asked to walk one" {
    reset();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_render_dirty(null, 4));
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_render_dirty(null, 0));
}

test "render_dirty renders the dirty widgets and clears them" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(&render_only_vt, 10), widgetOf(&render_only_vt, 11) };
    ws[0].dirty = true;
    ws[0].refresh = @intFromEnum(abi.Refresh.quality);

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_render_dirty(&ws, 2));
    try std.testing.expectEqual(1, renders);
    try std.testing.expectEqual(10, rendered[0]);
    try std.testing.expect(!ws[0].dirty);
    try std.testing.expectEqual(0, ws[0].refresh);
}

test "render_dirty renders in array order" {
    reset();
    var ws = [_]abi.Widget{
        widgetOf(&render_only_vt, 10),
        widgetOf(&render_only_vt, 11),
        widgetOf(&render_only_vt, 12),
    };
    for (&ws) |*w| w.dirty = true;

    _ = abi.ra8_widget_render_dirty(&ws, 3);
    try std.testing.expectEqual(3, renders);
    try std.testing.expectEqual(10, rendered[0]);
    try std.testing.expectEqual(11, rendered[1]);
    try std.testing.expectEqual(12, rendered[2]);
}

test "a hidden dirty widget is not rendered and stays dirty" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&render_only_vt, 10)};
    ws[0].dirty = true;
    ws[0].visible = false;

    _ = abi.ra8_widget_render_dirty(&ws, 1);
    try std.testing.expectEqual(0, renders);
    try std.testing.expect(ws[0].dirty);
}

test "a dirty widget with no render callback is still cleared" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(null, 10), widgetOf(&input_only_vt, 11) };
    for (&ws) |*w| {
        w.dirty = true;
        w.refresh = @intFromEnum(abi.Refresh.fast);
    }

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_render_dirty(&ws, 2));
    try std.testing.expectEqual(0, renders);
    for (&ws) |*w| {
        try std.testing.expect(!w.dirty);
        try std.testing.expectEqual(0, w.refresh);
    }
}

test "a clean widget is never rendered" {
    reset();
    var ws = [_]abi.Widget{widgetOf(&render_only_vt, 10)};
    _ = abi.ra8_widget_render_dirty(&ws, 1);
    try std.testing.expectEqual(0, renders);
}

test "damage then render_dirty leaves nothing dirty behind" {
    reset();
    var ws = [_]abi.Widget{ widgetOf(&render_only_vt, 10), widgetOf(&render_only_vt, 11) };
    ws[0].rect = .{ .x = 0, .y = 0, .w = 10, .h = 10 };
    ws[1].rect = .{ .x = 30, .y = 0, .w = 10, .h = 10 };
    _ = abi.ra8_widget_invalidate(&ws[0], .fast);
    _ = abi.ra8_widget_invalidate(&ws[1], .quality);

    var rect: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var count: u16 = undefined;
    _ = abi.ra8_widget_damage(&ws, 2, &rect, &hint, &count);
    try std.testing.expectEqual(abi.Rect{ .x = 0, .y = 0, .w = 40, .h = 10 }, rect);
    try std.testing.expectEqual(abi.Refresh.quality, hint);
    try std.testing.expectEqual(2, count);

    _ = abi.ra8_widget_render_dirty(&ws, 2);
    _ = abi.ra8_widget_damage(&ws, 2, &rect, &hint, &count);
    try std.testing.expectEqual(0, count);
    try std.testing.expectEqual(abi.Refresh.none, hint);
}
