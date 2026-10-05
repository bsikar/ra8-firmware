//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the toolbar leaf widget: what used to be
//! `src/ra8_widget_toolbar.c`. It owns the band descriptor, the one immutable
//! toolbar vtable, and the two published entry points. The widget tree's
//! shared types live in `widget_abi_types.zig` and the geometry in
//! `internal/paint.zig`, so this file is binding plus dispatch and nothing
//! else.
//!
//! The search-field rect is computed once by `fieldRect` and shared between
//! render and input, so the drawn box and the tap target stay the same
//! rectangle. Guard order, log lines and error codes are the C's, byte for
//! byte.

const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Alignment selector of the published ABI (`ra8_widget_align_t`).
pub const Alignment = types.Alignment;
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

/// The band's two geometry floors, both zero in the C: it asks the box helper
/// for no frame around the band itself, and a field squeezed past the chip
/// collapses to this width rather than going negative.
pub const geometry = struct {
    pub const no_border: i16 = 0;
    pub const collapsed_field: i32 = 0;
};

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_toolbar";

/// `ra8_ui_rect_contains` from `libs/ra8_ui`: the hit test the C toolbar used,
/// so the field's tap target keeps one definition across the tree.
extern fn ra8_ui_rect_contains(r: *const Rect, px: i32, py: i32) callconv(.c) bool;

/// Toolbar descriptor of the published ABI (`ra8_widget_toolbar_t`).
/// Caller-owned plain data: a null `paint` draws nothing, either string may be
/// null, and `on_search` is optional.
pub const Toolbar = extern struct {
    paint: ?*const Paint,
    hint: ?[*:0]const u8,
    count: ?[*:0]const u8,
    on_search: ?*const fn (w: *Widget) callconv(.c) void,
    bg: u32,
    field: u32,
    border: u32,
    hint_fg: u32,
    count_fg: u32,
    searches: u32,
    pad: i16,
    border_w: i16,
    count_w: i16,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@alignOf(Toolbar) != @alignOf(usize)) @compileError("ra8_widget_toolbar_t alignment");
    if (@offsetOf(Toolbar, "paint") != 0) @compileError("ra8_widget_toolbar_t paint offset");
    if (@offsetOf(Toolbar, "hint") != ptr) @compileError("ra8_widget_toolbar_t hint offset");
    if (@offsetOf(Toolbar, "count") != 2 * ptr) @compileError("ra8_widget_toolbar_t count offset");
    if (@offsetOf(Toolbar, "on_search") != 3 * ptr) @compileError("ra8_widget_toolbar_t on_search offset");
    if (@offsetOf(Toolbar, "bg") != 4 * ptr) @compileError("ra8_widget_toolbar_t bg offset");
    if (@offsetOf(Toolbar, "field") != 4 * ptr + 4) @compileError("ra8_widget_toolbar_t field offset");
    if (@offsetOf(Toolbar, "border") != 4 * ptr + 8) @compileError("ra8_widget_toolbar_t border offset");
    if (@offsetOf(Toolbar, "hint_fg") != 4 * ptr + 12) @compileError("ra8_widget_toolbar_t hint_fg offset");
    if (@offsetOf(Toolbar, "count_fg") != 4 * ptr + 16) @compileError("ra8_widget_toolbar_t count_fg offset");
    if (@offsetOf(Toolbar, "searches") != 4 * ptr + 20) @compileError("ra8_widget_toolbar_t searches offset");
    if (@offsetOf(Toolbar, "pad") != 4 * ptr + 24) @compileError("ra8_widget_toolbar_t pad offset");
    if (@offsetOf(Toolbar, "border_w") != 4 * ptr + 26) @compileError("ra8_widget_toolbar_t border_w offset");
    if (@offsetOf(Toolbar, "count_w") != 4 * ptr + 28) @compileError("ra8_widget_toolbar_t count_w offset");
}

/// The search field inside a toolbar band: inset by `pad` on every side, with
/// the count chip's fixed width plus a gutter reserved on the right. Pure, and
/// the one place the field's geometry is decided.
pub fn fieldRect(bar: *const Toolbar, band: *const Rect) Rect {
    const pad: i32 = bar.pad;
    const chip: i32 = bar.count_w;
    const width = band.w - (pad + pad) - chip - pad;

    return .{
        .x = band.x + pad,
        .y = band.y + pad,
        .w = if (width > geometry.collapsed_field) width else geometry.collapsed_field,
        .h = band.h - (pad + pad),
    };
}

/// Place one aligned string inside `rect` and draw it. A null string is a
/// no-op, so the hint and the chip share one path.
fn drawAligned(
    backend: *const Paint,
    rect: *const Rect,
    text: ?[*:0]const u8,
    pad: i16,
    alignment: Alignment,
    fg: u32,
    bg: u32,
) void {
    const string = text orelse return;
    const draw_text = backend.draw_text orelse return;

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(backend, rect, string, pad, alignment, .sans, .regular, false, &pen_x, &pen_y);
    draw_text(backend.user, pen_x, pen_y, string, fg, bg);
}

/// Vtable `render`: band fill, bordered search field, hint, then count chip.
///
/// The hint is placed inside the field and the chip inside the whole band, and
/// each is drawn on the colour it sits on, so they read as one strip.
fn renderToolbar(w: *Widget) callconv(.c) void {
    const bar: *const Toolbar = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = bar.paint orelse return;

    const band = &w.rect;
    const field = fieldRect(bar, band);
    paint_abi.priv_widget_fill_box(backend, band, bar.bg, bar.bg, geometry.no_border);
    paint_abi.priv_widget_fill_box(backend, &field, bar.field, bar.border, bar.border_w);

    if (backend.draw_text == null) return;
    drawAligned(backend, &field, bar.hint, bar.pad, .left, bar.hint_fg, bar.field);
    drawAligned(backend, band, bar.count, bar.pad, .right, bar.count_fg, bar.bg);
}

/// Vtable `on_input`: latch a touch that lands inside the search field.
///
/// The tap target is the drawn field, not the band: a touch on the count chip
/// or a button event is declined so it keeps routing.
fn onInputToolbar(w: *Widget, event: *const Event) callconv(.c) bool {
    const bar: *Toolbar = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;

    const field = fieldRect(bar, &w.rect);
    if (!ra8_ui_rect_contains(&field, event.x, event.y)) return false;

    bar.searches +%= 1;
    _ = types.ra8_widget_invalidate(w, .fast);
    if (bar.on_search) |notify| notify(w);
    return true;
}

/// The single immutable vtable shared by every toolbar: it renders and routes,
/// and leaves sizing to its parent.
const toolbar_vtable: Vtable = .{
    .measure = null,
    .render = renderToolbar,
    .on_input = onInputToolbar,
};

/// `ra8_widget_toolbar_vtable`: the shared toolbar vtable, in static storage.
pub export fn ra8_widget_toolbar_vtable() callconv(.c) *const Vtable {
    return &toolbar_vtable;
}

/// `ra8_widget_toolbar_init`: bind `w` to render and route `bar`. The caller
/// still sets `w`'s `fixed` / `flex` for its parent's layout.
pub export fn ra8_widget_toolbar_init(w: ?*Widget, bar: ?*Toolbar) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = bar orelse return types.refuseNull(tag, "bar must not be nullptr");

    widget.vt = ra8_widget_toolbar_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
