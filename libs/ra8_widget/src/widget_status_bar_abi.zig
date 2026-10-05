//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the status-bar leaf widget: what used to be
//! `src/ra8_widget_status_bar.c`. It owns the band descriptor, the one
//! immutable status-bar vtable, and the two published entry points. The widget
//! tree's shared types live in `widget_abi_types.zig` and the geometry in
//! `internal/paint.zig`, so this file is binding plus dispatch and nothing else.
//!
//! Guard order, log lines and error codes are the C's, byte for byte.

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
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;

/// The band's two thresholds, both zero in the C: it asks the box helper for
/// no frame, and skips the hairline at or below this thickness. The C spelled
/// the second as a one-member `enum : int16_t`.
pub const geometry = struct {
    pub const no_border: i16 = 0;
    pub const no_rule: i16 = 0;
};

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_status_bar";

/// Status-bar descriptor of the published ABI (`ra8_widget_status_bar_t`).
/// Caller-owned plain data: a null `paint` draws nothing, either label may be
/// null, and a `rule_h` at or below zero leaves the hairline off.
pub const StatusBar = extern struct {
    paint: ?*const Paint,
    left: ?[*:0]const u8,
    right: ?[*:0]const u8,
    bg: u32,
    fg: u32,
    fg_right: u32,
    rule: u32,
    pad: i16,
    rule_h: i16,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@alignOf(StatusBar) != @alignOf(usize)) @compileError("ra8_widget_status_bar_t alignment");
    if (@offsetOf(StatusBar, "paint") != 0) @compileError("ra8_widget_status_bar_t paint offset");
    if (@offsetOf(StatusBar, "left") != ptr) @compileError("ra8_widget_status_bar_t left offset");
    if (@offsetOf(StatusBar, "right") != 2 * ptr) @compileError("ra8_widget_status_bar_t right offset");
    if (@offsetOf(StatusBar, "bg") != 3 * ptr) @compileError("ra8_widget_status_bar_t bg offset");
    if (@offsetOf(StatusBar, "fg") != 3 * ptr + 4) @compileError("ra8_widget_status_bar_t fg offset");
    if (@offsetOf(StatusBar, "fg_right") != 3 * ptr + 8) @compileError("ra8_widget_status_bar_t fg_right offset");
    if (@offsetOf(StatusBar, "rule") != 3 * ptr + 12) @compileError("ra8_widget_status_bar_t rule offset");
    if (@offsetOf(StatusBar, "pad") != 3 * ptr + 16) @compileError("ra8_widget_status_bar_t pad offset");
    if (@offsetOf(StatusBar, "rule_h") != 3 * ptr + 18) @compileError("ra8_widget_status_bar_t rule_h offset");
}

/// Place one aligned label inside the band and draw it. A null text or a
/// backend with no `draw_text` is a no-op, so both labels share one path.
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
    paint_abi.priv_widget_text_pos(backend, rect, string, pad, alignment, .sans, .regular, .size_3, false, &pen_x, &pen_y);
    draw_text(backend.user, pen_x, pen_y, string, fg, bg);
}

/// Vtable `render`: fill the band, draw both labels, then the bottom hairline.
///
/// The hairline is a strip along the band's bottom edge, so it is issued
/// directly rather than through the box helper, and it is the only part that
/// needs `fill_rect` in its own right.
fn renderStatusBar(w: *Widget) callconv(.c) void {
    const bar: *const StatusBar = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = bar.paint orelse return;

    const rect = &w.rect;
    paint_abi.priv_widget_fill_box(backend, rect, bar.bg, bar.bg, geometry.no_border);
    drawAligned(backend, rect, bar.left, bar.pad, .left, bar.fg, bar.bg);
    drawAligned(backend, rect, bar.right, bar.pad, .right, bar.fg_right, bar.bg);

    if (bar.rule_h <= geometry.no_rule) return;
    const fill_rect = backend.fill_rect orelse return;

    const thickness: i32 = bar.rule_h;
    fill_rect(backend.user, rect.x, rect.y + rect.h - thickness, rect.w, thickness, bar.rule);
}

/// The single immutable vtable shared by every status bar: display only, so
/// it measures nothing and never consumes a touch.
const status_bar_vtable: Vtable = .{
    .measure = null,
    .render = renderStatusBar,
    .on_input = null,
};

/// `ra8_widget_status_bar_vtable`: the shared band vtable, in static storage.
pub export fn ra8_widget_status_bar_vtable() callconv(.c) *const Vtable {
    return &status_bar_vtable;
}

/// `ra8_widget_status_bar_init`: bind `w` to render `bar`. The caller still
/// sets `w`'s `fixed` / `flex` for its parent's layout.
pub export fn ra8_widget_status_bar_init(w: ?*Widget, bar: ?*StatusBar) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = bar orelse return types.refuseNull(tag, "bar must not be nullptr");

    widget.vt = ra8_widget_status_bar_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
