//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the text-label leaf widget: what used to be
//! `src/ra8_widget_label.c`. It owns the extern mirrors of the widget
//! instance, its vtable and the label descriptor, the layout assertions that
//! pin them, the one immutable label vtable, and the two published entry
//! points. All the geometry it paints with already lives in the sibling paint
//! membrane, so this file is binding plus dispatch and nothing else.
//!
//! Guard order, log lines and error codes are the C's, byte for byte.

const paint_abi = @import("widget_paint_abi.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = paint_abi.Rect;
/// Alignment selector of the published ABI (`ra8_widget_align_t`).
pub const Alignment = paint_abi.Alignment;
/// Draw backend of the published ABI (`ra8_widget_paint_t`).
pub const Paint = paint_abi.Paint;

/// The `ra8_err_t` values this membrane answers with.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const null_ptr: u16 = 0x504;
};

/// A plain label paints exactly one fill, so it asks the shared box helper for
/// no frame at all. The bordered face belongs to the button, not here.
pub const geometry = struct {
    pub const no_border: i16 = 0;
};

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_label";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `RA8_CHECK_NULL_PTR(ptr, s_tag, message)`: log then answer null_ptr.
fn refuseNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return err.null_ptr;
}

/// `ra8_widget_event_t` is opaque to a label: its vtable never consumes input,
/// so the mirror only has to make the pointer the right width.
pub const Event = opaque {};

/// Behaviour table of the published ABI (`ra8_widget_vtable_t`). Every member
/// is optional because a vtable leaves out whatever its widget does not do.
pub const Vtable = extern struct {
    measure: ?*const fn (
        w: *Widget,
        avail_w: i32,
        avail_h: i32,
        out_w: *i32,
        out_h: *i32,
    ) callconv(.c) void,
    render: ?*const fn (w: *Widget) callconv(.c) void,
    on_input: ?*const fn (w: *Widget, event: *const Event) callconv(.c) bool,
};

/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = extern struct {
    vt: ?*const Vtable,
    ctx: ?*anyopaque,
    rect: Rect,
    fixed: i16,
    flex: u16,
    action_id: u16,
    refresh: u8,
    visible: bool,
    dirty: bool,
};

/// Label descriptor of the published ABI (`ra8_widget_label_t`). Caller-owned
/// plain data: a null `paint` draws nothing and a null `text` fills only.
pub const Label = extern struct {
    paint: ?*const Paint,
    text: ?[*:0]const u8,
    fg: u32,
    bg: u32,
    pad: i16,
    alignment: Alignment,
    reserved: u8,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@sizeOf(Vtable) != 3 * ptr) @compileError("ra8_widget_vtable_t size");
    if (@offsetOf(Vtable, "measure") != 0) @compileError("ra8_widget_vtable_t measure offset");
    if (@offsetOf(Vtable, "render") != ptr) @compileError("ra8_widget_vtable_t render offset");
    if (@offsetOf(Vtable, "on_input") != 2 * ptr) @compileError("ra8_widget_vtable_t on_input offset");

    if (@alignOf(Widget) != @alignOf(usize)) @compileError("ra8_widget_t alignment");
    if (@offsetOf(Widget, "vt") != 0) @compileError("ra8_widget_t vt offset");
    if (@offsetOf(Widget, "ctx") != ptr) @compileError("ra8_widget_t ctx offset");
    if (@offsetOf(Widget, "rect") != 2 * ptr) @compileError("ra8_widget_t rect offset");
    if (@offsetOf(Widget, "fixed") != 2 * ptr + 16) @compileError("ra8_widget_t fixed offset");
    if (@offsetOf(Widget, "flex") != 2 * ptr + 18) @compileError("ra8_widget_t flex offset");
    if (@offsetOf(Widget, "action_id") != 2 * ptr + 20) @compileError("ra8_widget_t action_id offset");
    if (@offsetOf(Widget, "refresh") != 2 * ptr + 22) @compileError("ra8_widget_t refresh offset");
    if (@offsetOf(Widget, "visible") != 2 * ptr + 23) @compileError("ra8_widget_t visible offset");
    if (@offsetOf(Widget, "dirty") != 2 * ptr + 24) @compileError("ra8_widget_t dirty offset");

    if (@alignOf(Label) != @alignOf(usize)) @compileError("ra8_widget_label_t alignment");
    if (@offsetOf(Label, "paint") != 0) @compileError("ra8_widget_label_t paint offset");
    if (@offsetOf(Label, "text") != ptr) @compileError("ra8_widget_label_t text offset");
    if (@offsetOf(Label, "fg") != 2 * ptr) @compileError("ra8_widget_label_t fg offset");
    if (@offsetOf(Label, "bg") != 2 * ptr + 4) @compileError("ra8_widget_label_t bg offset");
    if (@offsetOf(Label, "pad") != 2 * ptr + 8) @compileError("ra8_widget_label_t pad offset");
    if (@offsetOf(Label, "alignment") != 2 * ptr + 10) @compileError("ra8_widget_label_t align offset");
    if (@offsetOf(Label, "reserved") != 2 * ptr + 11) @compileError("ra8_widget_label_t reserved offset");
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
    const draw_text = backend.draw_text orelse return;

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(
        backend,
        &w.rect,
        text,
        label.pad,
        label.alignment,
        &pen_x,
        &pen_y,
    );
    draw_text(backend.user, pen_x, pen_y, text, label.fg, label.bg);
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
    const widget = w orelse return refuseNull("w must not be nullptr");
    const descriptor = label orelse return refuseNull("label must not be nullptr");

    widget.vt = ra8_widget_label_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
