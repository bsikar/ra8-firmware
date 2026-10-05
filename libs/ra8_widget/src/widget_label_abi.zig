//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the text-label leaf widget: what used to be
//! `src/ra8_widget_label.c`. It owns the label descriptor, the one immutable
//! label vtable, and the two published entry points. The widget tree's shared
//! types live in `widget_abi_types.zig` and the geometry in `internal/paint.zig`,
//! so this file is binding plus dispatch and nothing else.
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

/// A plain label paints exactly one fill, so it asks the shared box helper for
/// no frame at all. The bordered face belongs to the button, not here.
pub const geometry = struct {
    pub const no_border: i16 = 0;
};

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_label";

/// Label descriptor of the published ABI (`ra8_widget_label_t`). Caller-owned
/// plain data: a null `paint` draws nothing and a null `text` fills only.
pub const Label = extern struct {
    paint: ?*const Paint,
    text: ?[*:0]const u8,
    fg: u32,
    bg: u32,
    pad: i16,
    alignment: Alignment,
    face: paint_abi.Face,
    weight: paint_abi.Weight = .regular,
    size: paint_abi.TextSize = .default,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@alignOf(Label) != @alignOf(usize)) @compileError("ra8_widget_label_t alignment");
    if (@offsetOf(Label, "paint") != 0) @compileError("ra8_widget_label_t paint offset");
    if (@offsetOf(Label, "text") != ptr) @compileError("ra8_widget_label_t text offset");
    if (@offsetOf(Label, "fg") != 2 * ptr) @compileError("ra8_widget_label_t fg offset");
    if (@offsetOf(Label, "bg") != 2 * ptr + 4) @compileError("ra8_widget_label_t bg offset");
    if (@offsetOf(Label, "pad") != 2 * ptr + 8) @compileError("ra8_widget_label_t pad offset");
    if (@offsetOf(Label, "alignment") != 2 * ptr + 10) @compileError("ra8_widget_label_t align offset");
    if (@offsetOf(Label, "face") != 2 * ptr + 11) @compileError("ra8_widget_label_t face offset");
    if (@offsetOf(Label, "weight") != 2 * ptr + 12) @compileError("ra8_widget_label_t weight offset");
    if (@offsetOf(Label, "size") != 2 * ptr + 13) @compileError("ra8_widget_label_t size offset");
}

/// Vtable `render`: fill the background, then draw the aligned text.
///
/// Every guard is a single condition, so a label with no descriptor, no paint
/// backend, no text or no `draw_text` is a no-op in exactly the place the C
/// returned, and the background fill still lands whenever there is a backend.
fn renderLabel(w: *Widget) callconv(.c) void {
    const label: *const Label = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = label.paint orelse return;

    paint_abi.priv_widget_fill_box(backend, &w.rect, label.bg, label.bg, geometry.no_border);

    const text = label.text orelse return;
    const weight_draw = backend.draw_text_style;
    const styled_draw = backend.draw_text_face;
    const legacy_draw = backend.draw_text;
    if (weight_draw == null and styled_draw == null and legacy_draw == null) return;

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    const size = normalizedSize(label.size);
    paint_abi.priv_widget_text_pos(
        backend,
        &w.rect,
        text,
        label.pad,
        label.alignment,
        label.face,
        label.weight,
        size,
        weight_draw != null or styled_draw != null,
        &pen_x,
        &pen_y,
    );
    if (weight_draw) |draw| {
        draw(backend.user, pen_x, pen_y, text, @intFromEnum(label.face), @intFromEnum(label.weight), @intFromEnum(size), label.fg, label.bg);
    } else if (styled_draw) |draw| {
        draw(backend.user, pen_x, pen_y, text, @intFromEnum(label.face), label.fg, label.bg);
    } else if (legacy_draw) |draw| {
        draw(backend.user, pen_x, pen_y, text, label.fg, label.bg);
    }
}

fn normalizedSize(size: paint_abi.TextSize) paint_abi.TextSize {
    return switch (@intFromEnum(size)) {
        0 => .size_3,
        1 => .size_1,
        2 => .size_2,
        3 => .size_3,
        4 => .size_4,
        5 => .size_5,
        else => .size_3,
    };
}

/// The single immutable vtable shared by every text label: display only, so
/// it measures nothing and never consumes a touch.
const label_vtable: Vtable = .{
    .measure = null,
    .render = renderLabel,
    .on_input = null,
};

/// `ra8_widget_label_vtable`: the shared label vtable, in static storage.
pub export fn ra8_widget_label_vtable() callconv(.c) *const Vtable {
    return &label_vtable;
}

/// `ra8_widget_label_init`: bind `w` to render `label`. The caller still sets
/// `w`'s `fixed` / `flex` for its parent's layout.
pub export fn ra8_widget_label_init(w: ?*Widget, label: ?*Label) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = label orelse return types.refuseNull(tag, "label must not be nullptr");

    widget.vt = ra8_widget_label_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
