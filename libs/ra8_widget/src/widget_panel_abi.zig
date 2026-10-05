//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the container panel declared in `inc/ra8_widget.h`:
//! `ra8_widget_panel_vtable`, `ra8_widget_panel_init` and
//! `ra8_widget_panel_compose`.
//!
//! A panel is the widget that makes the widget array a tree: it is itself a
//! `ra8_widget_t` whose `ctx` is a child array, so a panel nests in a panel.
//! The compositing itself stays with the flat ops in the still-C
//! `src/ra8_widget.c`; this file is only the tree-recursion glue, and it
//! carries no framebuffer dependency.
//!
//! The child array crosses the ABI as the C pair (pointer, count) and becomes
//! a Zig slice at the top of every entry point, so nothing below indexes a
//! raw pointer. That is the shape every container in this library uses.

const types = @import("widget_abi_types.zig");
const builtin = @import("builtin");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = types.Widget;
/// Behaviour table of the published ABI (`ra8_widget_vtable_t`).
pub const Vtable = types.Vtable;
/// One input event of the published ABI (`ra8_widget_event_t`).
pub const Event = types.Event;
/// E-ink-style refresh hint of the published ABI (`ra8_widget_refresh_t`).
pub const Refresh = types.Refresh;
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;

const DebugWidget = if (builtin.mode == .Debug) @import("debug").SnapshotWidget else struct {};
const DebugChildrenFn = *const fn (*DebugWidget) ?[]DebugWidget;
const debug_channel = if (builtin.mode == .Debug) @import("debug") else struct {
    pub fn publish(_: *Widget, _: DebugChildrenFn) void {}
};

/// The `ra8_box_t` layout scratch node. A panel only forwards the array to
/// the box engine, so its contents stay opaque here.
pub const Box = opaque {};

/// Main axis a container stacks its children along (`ra8_widget_axis_t`).
pub const Axis = enum(u8) {
    col = 0,
    row = 1,
};

/// Fixed rules of a panel's descriptor.
pub const layout = struct {
    /// A panel with this many children validates nothing on bind.
    pub const no_children: u16 = 0;
    /// The box engine needs one scratch node per child plus the container.
    pub const container_nodes: u32 = 1;
};

const tag: [*:0]const u8 = "ra8_widget_panel";

/// Caller-owned container descriptor (`ra8_widget_panel_t`).
pub const Panel = extern struct {
    children: ?[*]Widget,
    box_scratch: ?*Box,
    count: u16,
    box_cap: u16,
    gap: i16,
    pad: i16,
    axis: Axis,
    reserved: u8,
    paint: ?*const types.Paint = null,
    bg: u32 = 0,
};

/// The flat container ops of the still-C `src/ra8_widget.c`. A panel is the
/// recursion around them, never a reimplementation of them.
pub extern fn ra8_widget_layout_stack(
    widgets: [*]Widget,
    count: u16,
    frame: *const Rect,
    axis: Axis,
    gap: i16,
    pad: i16,
    box_scratch: ?*Box,
    box_cap: u16,
) callconv(.c) u16;
pub extern fn ra8_widget_dispatch(
    widgets: [*]Widget,
    count: u16,
    event: *const Event,
    out_handled: *bool,
) callconv(.c) u16;
pub extern fn ra8_widget_damage(
    widgets: [*]const Widget,
    count: u16,
    out_rect: *Rect,
    out_hint: *Refresh,
    out_count: *u16,
) callconv(.c) u16;
pub extern fn ra8_widget_render_dirty(widgets: [*]Widget, count: u16) callconv(.c) u16;

/// Paint the panel face through its optional caller-owned backend.
fn fillBackground(rect: Rect, panel: *const Panel) void {
    const backend = panel.paint orelse return;
    const fill = backend.fill_rect orelse return;
    fill(backend.user, rect.x, rect.y, rect.w, rect.h, panel.bg);
}

/// True when a compose will redraw every visible child.
fn allVisibleChildrenDirty(kids: []const Widget, dirty: u16) bool {
    if (dirty == 0) return false;
    var visible: u16 = 0;
    for (kids) |child| {
        if (child.visible) visible += 1;
    }
    return dirty == visible;
}

/// The panel's children as a slice, or null when the widget is not a panel.
///
/// This is the one place the C `(children, count)` pair is turned into a
/// bounded thing; every entry point below starts here.
pub fn children(w: *Widget) ?[]Widget {
    const panel: *Panel = @ptrCast(@alignCast(w.ctx orelse return null));
    const base = panel.children orelse return null;
    return base[0..panel.count];
}

/// Return children only when the instance is a panel; leaf contexts have other
/// layouts and must never be interpreted as a `Panel`.
fn debugChildren(raw: *DebugWidget) ?[]DebugWidget {
    const w: *Widget = @ptrCast(@alignCast(raw));
    if (w.vt != &vtable) return null;
    const kids = children(w) orelse return null;
    const base: [*]DebugWidget = @ptrCast(@alignCast(kids.ptr));
    return base[0..kids.len];
}

/// Lay a panel's children out inside `rect`. Shared by `render` and
/// `compose` so the stack-layout call is written once.
fn layoutInto(panel: *const Panel, kids: []Widget, rect: *const Rect) u16 {
    return ra8_widget_layout_stack(
        kids.ptr,
        @intCast(kids.len),
        rect,
        panel.axis,
        panel.gap,
        panel.pad,
        panel.box_scratch,
        panel.box_cap,
    );
}

/// The hint a dirty panel repaints its subtree with: its own, or quality when
/// it carries none.
fn subtreeHint(w: *const Widget) Refresh {
    if (w.refresh == @intFromEnum(Refresh.none)) return .quality;
    return @enumFromInt(w.refresh);
}

/// Vtable render: lay the subtree out, mark every visible child dirty with
/// the panel's hint, then composite. A child that is itself a panel re-enters
/// here, so the depth is the caller's static tree.
fn render(w: *Widget) callconv(.c) void {
    const kids = children(w) orelse return;
    const panel: *Panel = @ptrCast(@alignCast(w.ctx.?));
    if (layoutInto(panel, kids, &w.rect) != err.ok) return;

    fillBackground(w.rect, panel);
    const hint = subtreeHint(w);
    for (kids) |*child| {
        if (child.visible) _ = types.ra8_widget_invalidate(child, hint);
    }
    _ = ra8_widget_render_dirty(kids.ptr, @intCast(kids.len));
}

/// Vtable input: offer the event to the children, and report whether one of
/// them consumed it.
fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const kids = children(w) orelse return false;

    var handled = false;
    _ = ra8_widget_dispatch(kids.ptr, @intCast(kids.len), event, &handled);
    return handled;
}

const vtable: Vtable = .{
    .measure = null,
    .render = render,
    .on_input = onInput,
};

/// Return the one immutable vtable shared by every container panel.
pub export fn ra8_widget_panel_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

/// Bind `w` to container `panel`: vtable, context, visible.
///
/// A panel that claims children has to carry them, and has to carry scratch
/// for one box per child plus the container.
pub export fn ra8_widget_panel_init(w: ?*Widget, panel: ?*Panel) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = panel orelse return types.refuseNull(tag, "panel must not be nullptr");

    if (descriptor.count > layout.no_children) {
        if (descriptor.children == null) return err.invalid_arg;
        const needed = @as(u32, descriptor.count) + layout.container_nodes;
        if (@as(u32, descriptor.box_cap) < needed) return err.invalid_arg;
    }

    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

/// Run one top-level compose cycle: pin the panel to `frame`, lay its
/// children out, report the minimal flush, composite the dirty ones.
pub export fn ra8_widget_panel_compose(
    panel_w: ?*Widget,
    frame: ?*const Rect,
    out_damage: ?*Rect,
    out_hint: ?*Refresh,
    out_dirty: ?*u16,
) callconv(.c) u16 {
    const widget = panel_w orelse return types.refuseNull(tag, "panel_w must not be nullptr");
    const rect = frame orelse return types.refuseNull(tag, "frame must not be nullptr");
    const damage = out_damage orelse return types.refuseNull(tag, "out_damage must not be nullptr");
    const hint = out_hint orelse return types.refuseNull(tag, "out_hint must not be nullptr");
    const dirty = out_dirty orelse return types.refuseNull(tag, "out_dirty must not be nullptr");

    const kids = children(widget) orelse return err.invalid_arg;
    const panel: *Panel = @ptrCast(@alignCast(widget.ctx.?));

    widget.rect = rect.*;
    const laid = layoutInto(panel, kids, &widget.rect);
    if (laid != err.ok) return laid;

    const damaged = ra8_widget_damage(kids.ptr, @intCast(kids.len), damage, hint, dirty);
    if (damaged != err.ok) return damaged;
    if (allVisibleChildrenDirty(kids, dirty.*)) {
        fillBackground(widget.rect, panel);
        damage.* = widget.rect;
    }

    const rendered = ra8_widget_render_dirty(kids.ptr, @intCast(kids.len));
    if (rendered != err.ok) return rendered;

    widget.dirty = false;
    widget.refresh = @intFromEnum(Refresh.none);
    if (builtin.mode == .Debug) {
        debug_channel.publish(@ptrCast(widget), debugChildren);
    }
    return err.ok;
}

comptime {
    const ptr = @sizeOf(usize);

    if (@offsetOf(Panel, "children") != 0) @compileError("ra8_widget_panel_t children offset");
    if (@offsetOf(Panel, "box_scratch") != ptr) @compileError("ra8_widget_panel_t box_scratch offset");
    if (@offsetOf(Panel, "count") != 2 * ptr) @compileError("ra8_widget_panel_t count offset");
    if (@offsetOf(Panel, "box_cap") != 2 * ptr + 2) @compileError("ra8_widget_panel_t box_cap offset");
    if (@offsetOf(Panel, "gap") != 2 * ptr + 4) @compileError("ra8_widget_panel_t gap offset");
    if (@offsetOf(Panel, "pad") != 2 * ptr + 6) @compileError("ra8_widget_panel_t pad offset");
    if (@offsetOf(Panel, "axis") != 2 * ptr + 8) @compileError("ra8_widget_panel_t axis offset");
    if (@offsetOf(Panel, "reserved") != 2 * ptr + 9) @compileError("ra8_widget_panel_t reserved offset");
    const paint_offset = ((2 * ptr + 10 + ptr - 1) / ptr) * ptr;
    if (@offsetOf(Panel, "paint") != paint_offset) @compileError("ra8_widget_panel_t paint offset");
    if (@offsetOf(Panel, "bg") != paint_offset + ptr) @compileError("ra8_widget_panel_t bg offset");
    if (@alignOf(Panel) != @alignOf(usize)) @compileError("ra8_widget_panel_t alignment");

    if (@sizeOf(Axis) != 1) @compileError("ra8_widget_axis_t width");
    if (@intFromEnum(Axis.col) != 0) @compileError("ra8_widget_axis_t col value");
    if (@intFromEnum(Axis.row) != 1) @compileError("ra8_widget_axis_t row value");
}

comptime {
    if (builtin.mode == .Debug) {
        if (@sizeOf(DebugWidget) != @sizeOf(Widget)) @compileError("debug widget size mismatch");
        if (@offsetOf(DebugWidget, "rect") != @offsetOf(Widget, "rect")) @compileError("debug widget rect offset");
        if (@offsetOf(DebugWidget, "visible") != @offsetOf(Widget, "visible")) @compileError("debug widget visible offset");
    }
}
