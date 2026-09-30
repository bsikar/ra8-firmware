//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the module-private paint helpers declared in
//! `src/ra8_widget_internal.h`. The geometry lives in `internal/paint.zig`;
//! this file owns only the extern mirrors of the C types, the layout
//! assertions that pin them, and the callback dispatch the sibling widget
//! translation units link against.

const std = @import("std");
const paint = @import("internal/paint.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = paint.Rect;
/// Alignment selector of the published ABI (`ra8_widget_align_t`).
pub const Alignment = paint.Alignment;

/// Draw backend of the published ABI (`ra8_widget_paint_t`). Every member is
/// optional because the C struct is zero-initialised by callers that bind only
/// the primitives they need.
pub const Paint = extern struct {
    user: ?*anyopaque,
    fill_rect: ?*const fn (
        user: ?*anyopaque,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        color: u32,
    ) callconv(.c) void,
    draw_text: ?*const fn (
        user: ?*anyopaque,
        x: i32,
        y: i32,
        str: [*:0]const u8,
        fg: u32,
        bg: u32,
    ) callconv(.c) void,
    text_size: ?*const fn (
        user: ?*anyopaque,
        str: [*:0]const u8,
        out_w: *i32,
        out_h: *i32,
    ) callconv(.c) void,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@sizeOf(Rect) != 16) @compileError("ra8_ui_rect_t size");
    if (@alignOf(Rect) != 4) @compileError("ra8_ui_rect_t alignment");
    if (@offsetOf(Rect, "x") != 0) @compileError("ra8_ui_rect_t x offset");
    if (@offsetOf(Rect, "y") != 4) @compileError("ra8_ui_rect_t y offset");
    if (@offsetOf(Rect, "w") != 8) @compileError("ra8_ui_rect_t w offset");
    if (@offsetOf(Rect, "h") != 12) @compileError("ra8_ui_rect_t h offset");

    if (@sizeOf(Paint) != 4 * ptr) @compileError("ra8_widget_paint_t size");
    if (@alignOf(Paint) != @alignOf(usize)) @compileError("ra8_widget_paint_t alignment");
    if (@offsetOf(Paint, "user") != 0) @compileError("ra8_widget_paint_t user offset");
    if (@offsetOf(Paint, "fill_rect") != ptr) @compileError("ra8_widget_paint_t fill_rect offset");
    if (@offsetOf(Paint, "draw_text") != 2 * ptr) @compileError("ra8_widget_paint_t draw_text offset");
    if (@offsetOf(Paint, "text_size") != 3 * ptr) @compileError("ra8_widget_paint_t text_size offset");

    if (@sizeOf(Alignment) != 1) @compileError("ra8_widget_align_t width");
    if (@intFromEnum(Alignment.left) != 0) @compileError("ra8_widget_align_t left value");
    if (@intFromEnum(Alignment.center) != 1) @compileError("ra8_widget_align_t center value");
    if (@intFromEnum(Alignment.right) != 2) @compileError("ra8_widget_align_t right value");
}

/// Resolve the pen position for `text` drawn inside `rect`.
///
/// Writes the inner-inset position first, so an unmeasurable string (left
/// alignment, or a backend with no `text_size`) still lands at the inset. When
/// the backend can measure, the glyph cell is centred vertically and the pen x
/// follows `alignment`.
pub export fn priv_widget_text_pos(
    backend: *const Paint,
    rect: *const Rect,
    text: [*:0]const u8,
    pad: i16,
    alignment: Alignment,
    out_x: *i32,
    out_y: *i32,
) callconv(.c) void {
    const inset = paint.insetPen(rect.*, pad);
    out_x.* = inset.x;
    out_y.* = inset.y;

    if (alignment == .left) return;
    const measure = backend.text_size orelse return;

    var text_w: i32 = 0;
    var text_h: i32 = 0;
    measure(backend.user, text, &text_w, &text_h);

    const pen = paint.measuredPen(rect.*, pad, alignment, text_w, text_h);
    out_x.* = pen.x;
    out_y.* = pen.y;
}

/// Fill `rect` with `fill`, framed by `border` when `border_w` is positive.
/// A backend with no `fill_rect` paints nothing.
pub export fn priv_widget_fill_box(
    backend: *const Paint,
    rect: *const Rect,
    fill: u32,
    border: u32,
    border_w: i16,
) callconv(.c) void {
    const fill_rect = backend.fill_rect orelse return;

    if (border_w <= 0) {
        fill_rect(backend.user, rect.x, rect.y, rect.w, rect.h, fill);
        return;
    }

    const edge: i32 = border_w;
    fill_rect(backend.user, rect.x, rect.y, rect.w, rect.h, border);
    fill_rect(
        backend.user,
        rect.x + edge,
        rect.y + edge,
        rect.w - (edge + edge),
        rect.h - (edge + edge),
        fill,
    );
}

/// Filled width in pixels for a `value / total` bar spanning `width`.
pub export fn priv_widget_fill_frac(value: u32, total: u32, width: i32) callconv(.c) i32 {
    return paint.fillFrac(value, total, width);
}
