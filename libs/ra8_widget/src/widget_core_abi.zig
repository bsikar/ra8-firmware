//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the flat container ops of `inc/ra8_widget.h`: the core
//! every widget in this library dispatches through. Five published entry
//! points live here, `ra8_widget_layout_stack`, `_dispatch`, `_invalidate`,
//! `_damage` and `_render_dirty`, and nothing else does.
//!
//! The ops are flat by design: they take a caller-owned widget array and never
//! own storage. Layout is delegated to `ra8_box`, which this archive links
//! against rather than reimplements, so the only geometry here is the damage
//! union and the measure clamp.

const types = @import("widget_abi_types.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = types.Widget;
/// Behaviour table of the published ABI (`ra8_widget_vtable_t`).
pub const Vtable = types.Vtable;
/// Input event of the published ABI (`ra8_widget_event_t`).
pub const Event = types.Event;
/// Event kind of the published ABI (`ra8_widget_ev_kind_t`).
pub const EventKind = types.EventKind;
/// Refresh hint of the published ABI (`ra8_widget_refresh_t`).
pub const Refresh = types.Refresh;
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;

/// Logging / check tag, the C file's `s_tag`.
const tag: [*:0]const u8 = "ra8_widget";

/// Main axis a container stacks along (`ra8_widget_axis_t`).
pub const Axis = enum(u8) {
    col = 0,
    row = 1,
};

/// The bounds this membrane clamps against, grouped rather than spelled as
/// `K_` macros at their use sites.
pub const limits = struct {
    /// Largest main-axis extent `ra8_box_t.fixed` holds; a widget that
    /// measures larger is capped here instead of wrapping the narrower field.
    pub const extent_max: i32 = 32767;
};

// ---------------------------------------------------------------------------
// ra8_box, linked from the sibling archive through its published C ABI.
// ---------------------------------------------------------------------------

/// Box-tree sentinels of `ra8_box.h` (`ra8_box_const_t`).
pub const box = struct {
    pub const none: i16 = -1;
    pub const stack_v: u8 = 0;
    pub const stack_h: u8 = 1;
    pub const leaf: u8 = 3;
};

/// One node of a box tree (`ra8_box_t`). `rect` is the layout output.
pub const Box = extern struct {
    kind: u8 = 0,
    grid_cols: u8 = 0,
    fixed: i16 = 0,
    flex: u16 = 0,
    pad: i16 = 0,
    gap: i16 = 0,
    fill: u32 = 0,
    border: u32 = 0,
    border_w: i16 = 0,
    tag: i16 = 0,
    first_child: i32 = 0,
    next: i32 = 0,
    rect: Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
};

/// Append-only builder over caller-owned node storage (`ra8_box_tree_t`).
pub const BoxTree = extern struct {
    nodes: ?[*]Box = null,
    cap: u16 = 0,
    count: u16 = 0,
};

extern fn ra8_box_tree_init(tree: *BoxTree, storage: [*]Box, cap: u16) callconv(.c) u16;
extern fn ra8_box_add(tree: *BoxTree, parent: i16, node: *const Box) callconv(.c) i16;
extern fn ra8_box_layout(tree: *BoxTree, root: i16, frame: *const Rect) callconv(.c) u16;
extern fn ra8_ui_rect_contains(r: *const Rect, px: i32, py: i32) callconv(.c) bool;

comptime {
    if (@sizeOf(Box) != 48) @compileError("ra8_box_t size");
    if (@alignOf(Box) != 4) @compileError("ra8_box_t alignment");
    if (@offsetOf(Box, "kind") != 0) @compileError("ra8_box_t kind offset");
    if (@offsetOf(Box, "grid_cols") != 1) @compileError("ra8_box_t grid_cols offset");
    if (@offsetOf(Box, "fixed") != 2) @compileError("ra8_box_t fixed offset");
    if (@offsetOf(Box, "flex") != 4) @compileError("ra8_box_t flex offset");
    if (@offsetOf(Box, "pad") != 6) @compileError("ra8_box_t pad offset");
    if (@offsetOf(Box, "gap") != 8) @compileError("ra8_box_t gap offset");
    if (@offsetOf(Box, "fill") != 12) @compileError("ra8_box_t fill offset");
    if (@offsetOf(Box, "border") != 16) @compileError("ra8_box_t border offset");
    if (@offsetOf(Box, "border_w") != 20) @compileError("ra8_box_t border_w offset");
    if (@offsetOf(Box, "tag") != 22) @compileError("ra8_box_t tag offset");
    if (@offsetOf(Box, "first_child") != 24) @compileError("ra8_box_t first_child offset");
    if (@offsetOf(Box, "next") != 28) @compileError("ra8_box_t next offset");
    if (@offsetOf(Box, "rect") != 32) @compileError("ra8_box_t rect offset");

    if (@offsetOf(BoxTree, "nodes") != 0) @compileError("ra8_box_tree_t nodes offset");
    if (@offsetOf(BoxTree, "cap") != @sizeOf(usize)) @compileError("ra8_box_tree_t cap offset");

    if (@sizeOf(Axis) != 1) @compileError("ra8_widget_axis_t width");
    if (@backingInt(Axis.col) != 0) @compileError("ra8_widget_axis_t col value");
    if (@backingInt(Axis.row) != 1) @compileError("ra8_widget_axis_t row value");
}

// ---------------------------------------------------------------------------
// Pure geometry.
// ---------------------------------------------------------------------------

/// True when a rect covers no pixels. Such a rect is the identity of
/// `rectUnion`, so a damage accumulator can start at all-zero.
pub fn rectEmpty(r: Rect) bool {
    return r.w <= 0 and r.h <= 0;
}

/// Smallest rect covering both inputs; an empty input returns the other.
pub fn rectUnion(acc: Rect, r: Rect) Rect {
    if (rectEmpty(acc)) return r;
    if (rectEmpty(r)) return acc;
    const x0 = @min(acc.x, r.x);
    const y0 = @min(acc.y, r.y);
    const x1 = @max(acc.x + acc.w, r.x + r.w);
    const y1 = @max(acc.y + acc.h, r.y + r.h);
    return .{ .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
}

/// How many of `widgets` are visible. Sizes the box scratch a stack needs.
pub fn visibleCount(widgets: []const Widget) u16 {
    var vis: u16 = 0;
    for (widgets) |wg| {
        if (wg.visible) vis += 1;
    }
    return vis;
}

/// Main-axis extent a widget's `measure` asks for, or 0 for none.
///
/// A widget carrying a flex weight, no vtable or no `measure` callback keeps
/// its flex sizing and reports nothing. One that does measure is handed the
/// frame's content box, and its answer is clamped to that box (the vtable
/// contract puts the clamp on the caller) and to `limits.extent_max`.
pub fn measuredExtent(wg: *Widget, axis: Axis, avail_w: i32, avail_h: i32) i16 {
    if (wg.flex != 0) return 0;
    const vt = wg.vt orelse return 0;
    const measure = vt.measure orelse return 0;

    var want_w: i32 = 0;
    var want_h: i32 = 0;
    measure(wg, avail_w, avail_h, &want_w, &want_h);

    const is_row = axis == .row;
    const want = if (is_row) want_w else want_h;
    const avail = if (is_row) avail_w else avail_h;
    const ext = @min(@min(want, avail), limits.extent_max);
    if (ext <= 0) return 0;
    return @intCast(ext);
}

// ---------------------------------------------------------------------------
// Published C ABI.
// ---------------------------------------------------------------------------

/// Build the box tree a stack lays out through: one container, then one leaf
/// per visible widget carrying its `fixed` / `flex`. A visible widget pinning
/// no extent is offered the measure pass, so a content-sized widget does not
/// collapse.
fn buildStackTree(
    widgets: []Widget,
    frame: *const Rect,
    axis: Axis,
    gap: i16,
    pad: i16,
    scratch: [*]Box,
    cap: u16,
    out_tree: *BoxTree,
    out_root: *i16,
) u16 {
    const vis = visibleCount(widgets);
    if (@as(u32, cap) < @as(u32, vis) + 1) return err.invalid_arg;

    const ierr = ra8_box_tree_init(out_tree, scratch, cap);
    if (ierr != err.ok) return ierr;

    const container: Box = .{
        .kind = if (axis == .row) box.stack_h else box.stack_v,
        .pad = pad,
        .gap = gap,
        .flex = 1,
        .tag = box.none,
    };
    out_root.* = ra8_box_add(out_tree, box.none, &container);
    if (out_root.* == box.none) return err.invalid_arg;

    const inset = 2 * @as(i32, pad);
    const avail_w = @max(frame.w - inset, 0);
    const avail_h = @max(frame.h - inset, 0);

    for (widgets) |*wg| {
        if (!wg.visible) continue;
        var leaf: Box = .{
            .kind = box.leaf,
            .fixed = wg.fixed,
            .flex = wg.flex,
            .tag = @bitCast(wg.action_id),
        };
        if (leaf.fixed == 0) leaf.fixed = measuredExtent(wg, axis, avail_w, avail_h);
        if (ra8_box_add(out_tree, out_root.*, &leaf) == box.none) return err.invalid_arg;
    }
    return err.ok;
}

/// `ra8_widget_layout_stack`: lay a stack of widgets out inside a frame.
pub export fn ra8_widget_layout_stack(
    widgets: ?[*]Widget,
    count: u16,
    frame: ?*const Rect,
    axis: Axis,
    gap: i16,
    pad: i16,
    box_scratch: ?[*]Box,
    box_cap: u16,
) callconv(.c) u16 {
    const wids = widgets orelse return types.refuseNull(tag, "widgets must not be nullptr");
    const frm = frame orelse return types.refuseNull(tag, "frame must not be nullptr");
    const scratch = box_scratch orelse
        return types.refuseNull(tag, "box_scratch must not be nullptr");

    const slice = wids[0..count];
    var tree: BoxTree = .{};
    var root: i16 = box.none;
    const berr = buildStackTree(slice, frm, axis, gap, pad, scratch, box_cap, &tree, &root);
    if (berr != err.ok) return berr;

    const lerr = ra8_box_layout(&tree, root, frm);
    if (lerr != err.ok) return lerr;

    // Box nodes 1..vis are the visible children, in add order.
    var box_idx: u16 = 1;
    for (slice) |*wg| {
        if (!wg.visible) continue;
        wg.rect = scratch[box_idx].rect;
        box_idx += 1;
    }
    return err.ok;
}

/// `ra8_widget_dispatch`: route one input event across a widget array.
///
/// A touch is offered only to the widget it lands inside, and that widget's
/// answer is final. A button press is offered to each visible widget in turn
/// until one consumes it.
pub export fn ra8_widget_dispatch(
    widgets: ?[*]Widget,
    count: u16,
    ev: ?*const Event,
    out_handled: ?*bool,
) callconv(.c) u16 {
    const event = ev orelse return types.refuseNull(tag, "ev must not be nullptr");
    const handled = out_handled orelse
        return types.refuseNull(tag, "out_handled must not be nullptr");
    if (count > 0 and widgets == null) return err.null_ptr;
    handled.* = false;

    const wids = widgets orelse return err.ok;
    for (wids[0..count]) |*wg| {
        if (!wg.visible) continue;
        const vt = wg.vt orelse continue;
        const on_input = vt.on_input orelse continue;

        if (event.kind == .touch) {
            if (!ra8_ui_rect_contains(&wg.rect, event.x, event.y)) continue;
            handled.* = on_input(wg, event);
            return err.ok;
        }
        if (on_input(wg, event)) {
            handled.* = true;
            return err.ok;
        }
    }
    return err.ok;
}

/// `ra8_widget_invalidate`: mark a widget dirty, folding the refresh hint
/// upward in strength so the strongest request for this frame wins.
pub export fn ra8_widget_invalidate(w: ?*Widget, refresh: Refresh) callconv(.c) u16 {
    const wg = w orelse return types.refuseNull(tag, "w must not be nullptr");
    if (refresh == .none) return err.invalid_arg;
    wg.dirty = true;
    const hint = @backingInt(refresh);
    if (hint > wg.refresh) wg.refresh = hint;
    return err.ok;
}

/// `ra8_widget_damage`: bounding rect, strongest refresh hint and count of the
/// visible dirty widgets.
pub export fn ra8_widget_damage(
    widgets: ?[*]const Widget,
    count: u16,
    out_rect: ?*Rect,
    out_hint: ?*Refresh,
    out_count: ?*u16,
) callconv(.c) u16 {
    const rect = out_rect orelse return types.refuseNull(tag, "out_rect must not be nullptr");
    const hint_out = out_hint orelse return types.refuseNull(tag, "out_hint must not be nullptr");
    const count_out = out_count orelse
        return types.refuseNull(tag, "out_count must not be nullptr");
    if (count > 0 and widgets == null) return err.null_ptr;

    var acc: Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
    var hint: u8 = @backingInt(Refresh.none);
    var dirty: u16 = 0;

    const wids: []const Widget = if (widgets) |p| p[0..count] else &.{};
    for (wids) |*wg| {
        if (!wg.visible or !wg.dirty) continue;
        acc = rectUnion(acc, wg.rect);
        if (wg.refresh > hint) hint = wg.refresh;
        dirty += 1;
    }
    rect.* = acc;
    hint_out.* = @fromBackingInt(@intCast(hint));
    count_out.* = dirty;
    return err.ok;
}

/// `ra8_widget_render_dirty`: render every visible dirty widget, then clear
/// its dirty flag and refresh hint.
pub export fn ra8_widget_render_dirty(widgets: ?[*]Widget, count: u16) callconv(.c) u16 {
    if (count > 0 and widgets == null) return err.null_ptr;

    const wids = widgets orelse return err.ok;
    for (wids[0..count]) |*wg| {
        if (!wg.visible or !wg.dirty) continue;
        if (wg.vt) |vt| {
            if (vt.render) |render| render(wg);
        }
        wg.dirty = false;
        wg.refresh = @backingInt(Refresh.none);
    }
    return err.ok;
}
