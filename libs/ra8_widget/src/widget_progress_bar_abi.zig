//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the progress-bar leaf widget: what used to be
//! `src/ra8_widget_progress_bar.c`. It owns the bar descriptor, the one
//! immutable bar vtable, and the two published entry points. The widget tree's
//! shared types live in `widget_abi_types.zig` and the fill-fraction maths in
//! `internal/paint.zig`, so this file is binding plus dispatch and nothing else.
//!
//! Guard order, log lines and error codes are the C's, byte for byte.

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
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;

/// Empty-bar sentinel: at or below this fill width, no fill is painted. The C
/// spelled it as a one-member `enum : int32_t`.
pub const geometry = struct {
    pub const empty: i32 = 0;
};

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_progress_bar";

/// Progress-bar descriptor of the published ABI (`ra8_widget_progress_bar_t`).
/// Caller-owned plain data: a null `paint` draws nothing, a `total` of 0 is an
/// empty bar rather than a divide, and `value` is clamped into `[0, total]`.
pub const ProgressBar = extern struct {
    paint: ?*const Paint,
    track: u32,
    fill: u32,
    value: u16,
    total: u16,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@alignOf(ProgressBar) != @alignOf(usize)) @compileError("ra8_widget_progress_bar_t alignment");
    if (@offsetOf(ProgressBar, "paint") != 0) @compileError("ra8_widget_progress_bar_t paint offset");
    if (@offsetOf(ProgressBar, "track") != ptr) @compileError("ra8_widget_progress_bar_t track offset");
    if (@offsetOf(ProgressBar, "fill") != ptr + 4) @compileError("ra8_widget_progress_bar_t fill offset");
    if (@offsetOf(ProgressBar, "value") != ptr + 8) @compileError("ra8_widget_progress_bar_t value offset");
    if (@offsetOf(ProgressBar, "total") != ptr + 10) @compileError("ra8_widget_progress_bar_t total offset");
}

/// Vtable `render`: fill the track, then the proportional fill.
///
/// The bar issues its two fills directly rather than through the box helper:
/// it has no frame, and the second fill is a partial-width overpaint the box
/// helper does not express. A missing descriptor, backend or `fill_rect` is a
/// no-op in exactly the place the C returned.
fn renderProgressBar(w: *Widget) callconv(.c) void {
    const bar: *const ProgressBar = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = bar.paint orelse return;
    const fill_rect = backend.fill_rect orelse return;

    const rect = &w.rect;
    fill_rect(backend.user, rect.x, rect.y, rect.w, rect.h, bar.track);

    const filled = paint_abi.priv_widget_fill_frac(bar.value, bar.total, rect.w);
    if (filled > geometry.empty) {
        fill_rect(backend.user, rect.x, rect.y, filled, rect.h, bar.fill);
    }
}

/// The single immutable vtable shared by every progress bar: display only, so
/// it measures nothing and never consumes a touch.
const progress_bar_vtable: Vtable = .{
    .measure = null,
    .render = renderProgressBar,
    .on_input = null,
};

/// `ra8_widget_progress_bar_vtable`: the shared bar vtable, in static storage.
pub export fn ra8_widget_progress_bar_vtable() callconv(.c) *const Vtable {
    return &progress_bar_vtable;
}

/// `ra8_widget_progress_bar_init`: bind `w` to render `bar`. The caller still
/// sets `w`'s `fixed` / `flex` for its parent's layout.
pub export fn ra8_widget_progress_bar_init(w: ?*Widget, bar: ?*ProgressBar) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = bar orelse return types.refuseNull(tag, "bar must not be nullptr");

    widget.vt = ra8_widget_progress_bar_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
