//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the reflowed-reading-body leaf widget declared in
//! `inc/ra8_widget_reflow_view.h`: `ra8_widget_reflow_view_vtable` and
//! `ra8_widget_reflow_view_init`.
//!
//! The view owns page state, body margins and tap routing; the reflow engine
//! itself is reached through the injected `ra8_widget_reflow_ops_t` seam, which
//! is what keeps this archive free of `ra8_reflow` and `ra8_gfx`. So the paging
//! decisions here are testable on the host and the pixel work stays in the
//! app-bound callbacks.

const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Draw backend of the published ABI (`ra8_widget_paint_t`).
pub const Paint = types.Paint;
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

/// Fixed paging and body geometry.
pub const paging = struct {
    /// First page index, and the page an empty book clamps to.
    pub const first: u16 = 0;
    /// One page forward or back.
    pub const step: u16 = 1;
    /// A book of this many pages has nothing to turn to.
    pub const empty: u16 = 0;
    /// A body rect is the page's own box, so its clear gets no inset.
    pub const no_inset: i16 = 0;
    /// The tap side splits the body down the middle.
    pub const halves: i32 = 2;
};

const tag: [*:0]const u8 = "ra8_widget_reflow_view";

/// Injected reflow-engine seam (`ra8_widget_reflow_ops_t`).
///
/// `follow_link` reports whether it followed a link and, when it did, writes
/// the destination page through `out_page`.
pub const Ops = extern struct {
    user: ?*anyopaque,
    render_page: ?*const fn (user: ?*anyopaque, page: u16, body: *const Rect) callconv(.c) void,
    follow_link: ?*const fn (
        user: ?*anyopaque,
        page: u16,
        x: i32,
        y: i32,
        out_page: *u16,
    ) callconv(.c) bool,
};

/// Caller-owned reflow-view descriptor (`ra8_widget_reflow_view_t`).
pub const ReflowView = extern struct {
    paint: ?*const Paint,
    ops: ?*const Ops,
    bg: u32,
    page: u16,
    page_count: u16,
    margin_x: i16,
    margin_y: i16,
};

/// The widget rect inset by the view's margins: where the page paints.
pub fn bodyRect(rect: Rect, margin_x: i16, margin_y: i16) Rect {
    const mx: i32 = margin_x;
    const my: i32 = margin_y;
    return .{ .x = rect.x + mx, .y = rect.y + my, .w = rect.w - 2 * mx, .h = rect.h - 2 * my };
}

/// `page` held inside `[0, count - 1]`; an empty book clamps to the first page.
///
/// The seam hands back link destinations from the engine's own page numbering,
/// so a stale or out-of-range one is clamped rather than adopted.
pub fn clampPage(page: u16, count: u16) u16 {
    if (count == paging.empty) return paging.first;
    return @min(page, count - paging.step);
}

/// Midpoint of `rect`: taps left of it step back, taps on or right of it step
/// forward.
pub fn tapMidpoint(rect: Rect) i32 {
    return rect.x + @divTrunc(rect.w, paging.halves);
}

/// Step `view` one page by which side of `mid` the tap at `px` landed on.
/// Returns whether the page actually moved; both ends are walls, not wraps.
pub fn turnPage(view: *ReflowView, px: i32, mid: i32) bool {
    if (px < mid) {
        if (view.page == paging.first) return false;
        view.page -= paging.step;
        return true;
    }
    if (view.page + paging.step >= view.page_count) return false;
    view.page += paging.step;
    return true;
}

/// Offer the tap to the link seam. Returns whether a link was followed, in
/// which case the clamped destination is adopted and the view is invalidated.
fn followLink(w: *Widget, view: *ReflowView, event: *const Event) bool {
    const ops = view.ops orelse return false;
    const follow = ops.follow_link orelse return false;

    var dest: u16 = view.page;
    if (!follow(ops.user, view.page, event.x, event.y, &dest)) return false;

    view.page = clampPage(dest, view.page_count);
    _ = types.ra8_widget_invalidate(w, .quality);
    return true;
}

/// Clear the widget rect, then ask the seam to paint the current page inside
/// the body rect.
fn render(w: *Widget) callconv(.c) void {
    const view: *const ReflowView = @ptrCast(@alignCast(w.ctx orelse return));

    if (view.paint) |backend| {
        paint_abi.priv_widget_fill_box(backend, &w.rect, view.bg, view.bg, paging.no_inset);
    }

    const ops = view.ops orelse return;
    const paint_page = ops.render_page orelse return;
    const body = bodyRect(w.rect, view.margin_x, view.margin_y);
    paint_page(ops.user, view.page, &body);
}

/// Follow a link if the tap is on one, otherwise turn the page. Every body
/// touch is consumed, including a tap at a boundary that changes nothing.
fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const view: *ReflowView = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;

    if (followLink(w, view, event)) return true;

    if (turnPage(view, event.x, tapMidpoint(w.rect))) {
        _ = types.ra8_widget_invalidate(w, .quality);
    }
    return true;
}

const vtable: Vtable = .{
    .measure = null,
    .render = render,
    .on_input = onInput,
};

/// Return the one immutable vtable shared by every reflow view.
pub export fn ra8_widget_reflow_view_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

/// Bind `w` to reflow view `view`: vtable, context, visible.
pub export fn ra8_widget_reflow_view_init(w: ?*Widget, view: ?*ReflowView) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = view orelse return types.refuseNull(tag, "view must not be nullptr");

    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

comptime {
    const ptr = @sizeOf(usize);

    if (@offsetOf(Ops, "user") != 0) @compileError("ra8_widget_reflow_ops_t user offset");
    if (@offsetOf(Ops, "render_page") != ptr) @compileError("ra8_widget_reflow_ops_t render_page offset");
    if (@offsetOf(Ops, "follow_link") != 2 * ptr) @compileError("ra8_widget_reflow_ops_t follow_link offset");
    if (@sizeOf(Ops) != 3 * ptr) @compileError("ra8_widget_reflow_ops_t size");

    if (@offsetOf(ReflowView, "paint") != 0) @compileError("ra8_widget_reflow_view_t paint offset");
    if (@offsetOf(ReflowView, "ops") != ptr) @compileError("ra8_widget_reflow_view_t ops offset");
    if (@offsetOf(ReflowView, "bg") != 2 * ptr) @compileError("ra8_widget_reflow_view_t bg offset");
    if (@offsetOf(ReflowView, "page") != 2 * ptr + 4) @compileError("ra8_widget_reflow_view_t page offset");
    if (@offsetOf(ReflowView, "page_count") != 2 * ptr + 6) @compileError("ra8_widget_reflow_view_t page_count offset");
    if (@offsetOf(ReflowView, "margin_x") != 2 * ptr + 8) @compileError("ra8_widget_reflow_view_t margin_x offset");
    if (@offsetOf(ReflowView, "margin_y") != 2 * ptr + 10) @compileError("ra8_widget_reflow_view_t margin_y offset");
    if (@alignOf(ReflowView) != @alignOf(usize)) @compileError("ra8_widget_reflow_view_t alignment");
}
