//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the container-panel membrane: the descriptor layout, the bind
//! guards, the compose cycle's order and its error forwarding, the subtree
//! repaint hint, and the input route. The flat container ops live in the
//! still-C `ra8_widget.c`, so they are substituted here by recording stubs
//! and what is checked is the glue: what the panel calls, with what, in what
//! order, and what it does with the answer.

const std = @import("std");
const abi = @import("abi");

const Op = enum { layout, damage, render_dirty, dispatch, invalidate };

/// Recorded call log plus the answers the stubs hand back.
const Flat = struct {
    var log: std.BoundedArray(Op, 32) = .{};

    var layout_result: u16 = abi.err.ok;
    var damage_result: u16 = abi.err.ok;
    var render_result: u16 = abi.err.ok;

    var layout_frame: abi.Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
    var layout_count: u16 = 0;
    var layout_axis: abi.Axis = .col;
    var layout_gap: i16 = 0;
    var layout_pad: i16 = 0;
    var layout_cap: u16 = 0;
    var layout_scratch: ?*abi.Box = null;

    var damage_count: u16 = 0;
    var render_count: u16 = 0;
    var dispatch_count: u16 = 0;
    var dispatch_handled: bool = false;

    /// When true the render stub behaves like the real one and calls each
    /// visible, dirty child's render, so a nested panel actually re-enters.
    var render_recurses: bool = false;
};

const Invalidation = struct {
    widget: *abi.Widget,
    refresh: u8,
};

var invalidations: std.BoundedArray(Invalidation, 16) = .{};
var last_message: ?[*:0]const u8 = null;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

export fn ra8_widget_invalidate(w: *abi.Widget, refresh: u8) callconv(.c) u16 {
    Flat.log.append(.invalidate) catch unreachable;
    invalidations.append(.{ .widget = w, .refresh = refresh }) catch unreachable;
    w.dirty = true;
    w.refresh = refresh;
    return abi.err.ok;
}

export fn ra8_widget_layout_stack(
    _: [*]abi.Widget,
    count: u16,
    frame: *const abi.Rect,
    axis: abi.Axis,
    gap: i16,
    pad: i16,
    box_scratch: ?*abi.Box,
    box_cap: u16,
) callconv(.c) u16 {
    Flat.log.append(.layout) catch unreachable;
    Flat.layout_count = count;
    Flat.layout_frame = frame.*;
    Flat.layout_axis = axis;
    Flat.layout_gap = gap;
    Flat.layout_pad = pad;
    Flat.layout_scratch = box_scratch;
    Flat.layout_cap = box_cap;
    return Flat.layout_result;
}

export fn ra8_widget_damage(
    _: [*]const abi.Widget,
    count: u16,
    out_rect: *abi.Rect,
    out_hint: *abi.Refresh,
    out_count: *u16,
) callconv(.c) u16 {
    Flat.log.append(.damage) catch unreachable;
    Flat.damage_count = count;
    out_rect.* = .{ .x = 1, .y = 2, .w = 3, .h = 4 };
    out_hint.* = .fast;
    out_count.* = count;
    return Flat.damage_result;
}

export fn ra8_widget_render_dirty(widgets: [*]abi.Widget, count: u16) callconv(.c) u16 {
    Flat.log.append(.render_dirty) catch unreachable;
    Flat.render_count = count;
    if (Flat.render_recurses) {
        for (widgets[0..count]) |*child| {
            if (!child.visible or !child.dirty) continue;
            child.dirty = false;
            const vt = child.vt orelse continue;
            if (vt.render) |paint| paint(child);
        }
    }
    return Flat.render_result;
}

export fn ra8_widget_dispatch(
    _: [*]abi.Widget,
    count: u16,
    _: *const abi.Event,
    out_handled: *bool,
) callconv(.c) u16 {
    Flat.log.append(.dispatch) catch unreachable;
    Flat.dispatch_count = count;
    out_handled.* = Flat.dispatch_handled;
    return abi.err.ok;
}

fn reset() void {
    Flat.log = .{};
    Flat.layout_result = abi.err.ok;
    Flat.damage_result = abi.err.ok;
    Flat.render_result = abi.err.ok;
    Flat.render_recurses = false;
    Flat.dispatch_handled = false;
    invalidations = .{};
    last_message = null;
}

/// A leaf whose render only records that it ran.
var leaf_renders: u32 = 0;

fn leafRender(_: *abi.Widget) callconv(.c) void {
    leaf_renders += 1;
}

const leaf_vtable: abi.Vtable = .{ .measure = null, .render = leafRender, .on_input = null };

fn leaf() abi.Widget {
    return .{
        .vt = &leaf_vtable,
        .ctx = null,
        .rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
        .fixed = 0,
        .flex = 1,
        .action_id = 0,
        .refresh = 0,
        .visible = true,
        .dirty = false,
    };
}

/// Scratch that is never read: the box engine is stubbed out.
var scratch: [8]usize = @splat(0);

fn scratchPtr() *abi.Box {
    return @ptrCast(&scratch);
}

fn panelOf(kids: []abi.Widget) abi.Panel {
    return .{
        .children = kids.ptr,
        .box_scratch = scratchPtr(),
        .count = @intCast(kids.len),
        .box_cap = @intCast(kids.len + 1),
        .gap = 4,
        .pad = 2,
        .axis = .row,
        .reserved = 0,
    };
}

fn bound(w: *abi.Widget, panel: *abi.Panel) !void {
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_panel_init(w, panel));
}

const screen: abi.Rect = .{ .x = 10, .y = 20, .w = 300, .h = 400 };

test "the descriptor matches the C layout" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(0, @offsetOf(abi.Panel, "children"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.Panel, "box_scratch"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Panel, "count"));
    try std.testing.expectEqual(2 * ptr + 2, @offsetOf(abi.Panel, "box_cap"));
    try std.testing.expectEqual(2 * ptr + 4, @offsetOf(abi.Panel, "gap"));
    try std.testing.expectEqual(2 * ptr + 6, @offsetOf(abi.Panel, "pad"));
    try std.testing.expectEqual(2 * ptr + 8, @offsetOf(abi.Panel, "axis"));
    try std.testing.expectEqual(2 * ptr + 9, @offsetOf(abi.Panel, "reserved"));
    try std.testing.expectEqual(@alignOf(usize), @alignOf(abi.Panel));
}

test "the axis enum matches the C values" {
    try std.testing.expectEqual(1, @sizeOf(abi.Axis));
    try std.testing.expectEqual(0, @intFromEnum(abi.Axis.col));
    try std.testing.expectEqual(1, @intFromEnum(abi.Axis.row));
}

test "a panel routes and paints but never measures" {
    const vt = abi.ra8_widget_panel_vtable();
    try std.testing.expect(vt.measure == null);
    try std.testing.expect(vt.render != null);
    try std.testing.expect(vt.on_input != null);
    try std.testing.expectEqual(vt, abi.ra8_widget_panel_vtable());
}

test "binding refuses a null widget" {
    reset();
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_panel_init(null, &panel));
    try std.testing.expect(last_message != null);
}

test "binding refuses a null descriptor" {
    reset();
    var w = leaf();
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_panel_init(&w, null));
    try std.testing.expect(last_message != null);
}

test "a panel that claims children must carry them" {
    reset();
    var w = leaf();
    var panel: abi.Panel = .{
        .children = null,
        .box_scratch = scratchPtr(),
        .count = 2,
        .box_cap = 3,
        .gap = 0,
        .pad = 0,
        .axis = .col,
        .reserved = 0,
    };
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_panel_init(&w, &panel));
}

test "scratch must hold one box per child plus the container" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);

    panel.box_cap = 2;
    var w = leaf();
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_panel_init(&w, &panel));

    panel.box_cap = 3;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_panel_init(&w, &panel));
}

test "an empty panel binds with no children and no scratch" {
    reset();
    var w = leaf();
    var panel: abi.Panel = .{
        .children = null,
        .box_scratch = null,
        .count = 0,
        .box_cap = 0,
        .gap = 0,
        .pad = 0,
        .axis = .col,
        .reserved = 0,
    };
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_panel_init(&w, &panel));
    try std.testing.expect(w.visible);
}

test "binding wires the vtable, the context and visibility" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);
    var w = leaf();
    w.visible = false;

    try bound(&w, &panel);
    try std.testing.expectEqual(abi.ra8_widget_panel_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(*anyopaque, @ptrCast(&panel)), w.ctx.?);
    try std.testing.expect(w.visible);
}

test "a refused bind leaves the widget untouched" {
    reset();
    var w = leaf();
    w.visible = false;
    var panel: abi.Panel = .{
        .children = null,
        .box_scratch = null,
        .count = 1,
        .box_cap = 0,
        .gap = 0,
        .pad = 0,
        .axis = .col,
        .reserved = 0,
    };
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_widget_panel_init(&w, &panel));
    try std.testing.expectEqual(&leaf_vtable, w.vt.?);
    try std.testing.expect(w.ctx == null);
    try std.testing.expect(!w.visible);
}

test "compose refuses every null argument" {
    reset();
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;

    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(null, &screen, &damage, &hint, &dirty),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(&w, null, &damage, &hint, &dirty),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(&w, &screen, null, &hint, &dirty),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, null, &dirty),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, null),
    );
    try std.testing.expectEqual(0, Flat.log.len);
}

test "compose refuses a widget that is not a panel" {
    reset();
    var w = leaf();
    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;

    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );
    try std.testing.expectEqual(0, Flat.log.len);
}

test "compose refuses a panel with no child array" {
    reset();
    var panel: abi.Panel = .{
        .children = null,
        .box_scratch = null,
        .count = 0,
        .box_cap = 0,
        .gap = 0,
        .pad = 0,
        .axis = .col,
        .reserved = 0,
    };
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );
}

test "compose pins the panel to the frame and lays out inside it" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );

    try std.testing.expectEqual(screen, w.rect);
    try std.testing.expectEqual(screen, Flat.layout_frame);
    try std.testing.expectEqual(2, Flat.layout_count);
    try std.testing.expectEqual(abi.Axis.row, Flat.layout_axis);
    try std.testing.expectEqual(4, Flat.layout_gap);
    try std.testing.expectEqual(2, Flat.layout_pad);
    try std.testing.expectEqual(3, Flat.layout_cap);
    try std.testing.expectEqual(scratchPtr(), Flat.layout_scratch.?);
}

test "compose lays out, then reports damage, then composites" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );

    try std.testing.expectEqualSlices(
        Op,
        &[_]Op{ .layout, .damage, .render_dirty },
        Flat.log.slice(),
    );
    try std.testing.expectEqual(abi.Refresh.fast, hint);
    try std.testing.expectEqual(2, dirty);
    try std.testing.expectEqual(@as(i32, 3), damage.w);
}

test "compose forwards a layout failure and stops there" {
    reset();
    Flat.layout_result = abi.err.invalid_arg;
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );
    try std.testing.expectEqualSlices(Op, &[_]Op{.layout}, Flat.log.slice());
}

test "compose forwards a damage failure and composites nothing" {
    reset();
    Flat.damage_result = abi.err.null_ptr;
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );
    try std.testing.expectEqualSlices(Op, &[_]Op{ .layout, .damage }, Flat.log.slice());
}

test "compose forwards a render failure" {
    reset();
    Flat.render_result = abi.err.null_ptr;
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );
    try std.testing.expectEqualSlices(
        Op,
        &[_]Op{ .layout, .damage, .render_dirty },
        Flat.log.slice(),
    );
}

test "a successful compose clears the panel's own damage" {
    reset();
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    w.dirty = true;
    w.refresh = @intFromEnum(abi.Refresh.quality);
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty),
    );
    try std.testing.expect(!w.dirty);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.none), w.refresh);
}

test "a failed compose leaves the panel dirty" {
    reset();
    Flat.render_result = abi.err.null_ptr;
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    w.dirty = true;
    w.refresh = @intFromEnum(abi.Refresh.quality);
    try bound(&w, &panel);

    var damage: abi.Rect = undefined;
    var hint: abi.Refresh = undefined;
    var dirty: u16 = undefined;
    _ = abi.ra8_widget_panel_compose(&w, &screen, &damage, &hint, &dirty);
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.quality), w.refresh);
}

test "render lays the subtree out inside the panel's own rect" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);
    w.rect = .{ .x = 5, .y = 6, .w = 70, .h = 80 };

    abi.ra8_widget_panel_vtable().render.?(&w);
    try std.testing.expectEqual(w.rect, Flat.layout_frame);
    try std.testing.expectEqual(2, Flat.render_count);
}

test "a dirty panel repaints its whole subtree with its own hint" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);
    w.refresh = @intFromEnum(abi.Refresh.fast);

    abi.ra8_widget_panel_vtable().render.?(&w);

    try std.testing.expectEqual(2, invalidations.len);
    for (invalidations.slice()) |seen| {
        try std.testing.expectEqual(@intFromEnum(abi.Refresh.fast), seen.refresh);
    }
    try std.testing.expectEqualSlices(
        Op,
        &[_]Op{ .layout, .invalidate, .invalidate, .render_dirty },
        Flat.log.slice(),
    );
}

test "a panel carrying no hint repaints at quality" {
    reset();
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);
    w.refresh = @intFromEnum(abi.Refresh.none);

    abi.ra8_widget_panel_vtable().render.?(&w);
    try std.testing.expectEqual(1, invalidations.len);
    try std.testing.expectEqual(
        @intFromEnum(abi.Refresh.quality),
        invalidations.slice()[0].refresh,
    );
}

test "render leaves an invisible child out of the repaint" {
    reset();
    var kids = [_]abi.Widget{ leaf(), leaf(), leaf() };
    kids[1].visible = false;
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    abi.ra8_widget_panel_vtable().render.?(&w);
    try std.testing.expectEqual(2, invalidations.len);
    try std.testing.expectEqual(&kids[0], invalidations.slice()[0].widget);
    try std.testing.expectEqual(&kids[2], invalidations.slice()[1].widget);
}

test "render on a widget that is not a panel does nothing" {
    reset();
    var w = leaf();
    abi.ra8_widget_panel_vtable().render.?(&w);
    try std.testing.expectEqual(0, Flat.log.len);
}

test "a layout failure during render composites nothing" {
    reset();
    Flat.layout_result = abi.err.invalid_arg;
    var kids = [_]abi.Widget{leaf()};
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    abi.ra8_widget_panel_vtable().render.?(&w);
    try std.testing.expectEqualSlices(Op, &[_]Op{.layout}, Flat.log.slice());
    try std.testing.expectEqual(0, invalidations.len);
}

test "input is offered to the children and the answer is theirs" {
    reset();
    Flat.dispatch_handled = true;
    var kids = [_]abi.Widget{ leaf(), leaf() };
    var panel = panelOf(&kids);
    var w = leaf();
    try bound(&w, &panel);

    const event: abi.Event = .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = 12, .y = 24 };
    try std.testing.expect(abi.ra8_widget_panel_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(2, Flat.dispatch_count);

    Flat.dispatch_handled = false;
    try std.testing.expect(!abi.ra8_widget_panel_vtable().on_input.?(&w, &event));
}

test "input on a widget that is not a panel is declined" {
    reset();
    var w = leaf();
    const event: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 3, .x = 0, .y = 0 };
    try std.testing.expect(!abi.ra8_widget_panel_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0, Flat.log.len);
}

test "a panel nests in a panel and the inner subtree repaints too" {
    reset();
    Flat.render_recurses = true;
    leaf_renders = 0;

    var grandchildren = [_]abi.Widget{ leaf(), leaf() };
    var inner = panelOf(&grandchildren);
    var inner_w = leaf();
    try bound(&inner_w, &inner);

    var kids = [_]abi.Widget{ leaf(), inner_w };
    var outer = panelOf(&kids);
    var outer_w = leaf();
    try bound(&outer_w, &outer);
    outer_w.refresh = @intFromEnum(abi.Refresh.quality);

    abi.ra8_widget_panel_vtable().render.?(&outer_w);

    // Outer: layout, both children invalidated, composite. The inner panel is
    // one of those children, so its own render runs inside that composite and
    // lays out and invalidates its two grandchildren before compositing them.
    try std.testing.expectEqualSlices(Op, &[_]Op{
        .layout,
        .invalidate,
        .invalidate,
        .render_dirty,
        .layout,
        .invalidate,
        .invalidate,
        .render_dirty,
    }, Flat.log.slice());
    // Three leaves painted: the outer panel's own leaf child, then the two
    // grandchildren the inner panel composites.
    try std.testing.expectEqual(3, leaf_renders);
}
