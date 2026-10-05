//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the push-button leaf widget: what used to be
//! `src/ra8_widget_button.c`. The interactive counterpart to the label -- a
//! bordered face that latches on a tap -- so unlike the label this vtable
//! carries an `on_input`. The widget tree's shared types live in
//! `widget_abi_types.zig` and the geometry in `internal/paint.zig`.
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
/// Input event of the published ABI (`ra8_widget_event_t`).
pub const Event = types.Event;
/// Input event kind of the published ABI (`ra8_widget_ev_kind_t`).
pub const EventKind = types.EventKind;
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_button";

/// Button descriptor of the published ABI (`ra8_widget_button_t`).
/// Caller-owned plain data; `presses` is observable by the app.
pub const Button = extern struct {
    paint: ?*const Paint,
    text: ?[*:0]const u8,
    on_press: ?*const fn (w: *Widget) callconv(.c) void,
    fg: u32,
    face: u32,
    face_pressed: u32,
    border: u32,
    presses: u32,
    pad: i16,
    border_w: i16,
    alignment: Alignment,
    pressed: bool,
    reserved: u8,
    text_face: paint_abi.Face = .sans,
    text_weight: paint_abi.Weight = .regular,
    text_size: paint_abi.TextSize = .default,

    /// The face fill for the current latch state: the C's
    /// `internal_button_face`, kept a single decision.
    fn faceColor(button: *const Button) u32 {
        if (button.pressed) return button.face_pressed;
        return button.face;
    }
};

comptime {
    const ptr = @sizeOf(usize);

    if (@alignOf(Button) != @alignOf(usize)) @compileError("ra8_widget_button_t alignment");
    if (@offsetOf(Button, "paint") != 0) @compileError("ra8_widget_button_t paint offset");
    if (@offsetOf(Button, "text") != ptr) @compileError("ra8_widget_button_t text offset");
    if (@offsetOf(Button, "on_press") != 2 * ptr) @compileError("ra8_widget_button_t on_press offset");
    if (@offsetOf(Button, "fg") != 3 * ptr) @compileError("ra8_widget_button_t fg offset");
    if (@offsetOf(Button, "face") != 3 * ptr + 4) @compileError("ra8_widget_button_t face offset");
    if (@offsetOf(Button, "face_pressed") != 3 * ptr + 8) @compileError("ra8_widget_button_t face_pressed offset");
    if (@offsetOf(Button, "border") != 3 * ptr + 12) @compileError("ra8_widget_button_t border offset");
    if (@offsetOf(Button, "presses") != 3 * ptr + 16) @compileError("ra8_widget_button_t presses offset");
    if (@offsetOf(Button, "pad") != 3 * ptr + 20) @compileError("ra8_widget_button_t pad offset");
    if (@offsetOf(Button, "border_w") != 3 * ptr + 22) @compileError("ra8_widget_button_t border_w offset");
    if (@offsetOf(Button, "alignment") != 3 * ptr + 24) @compileError("ra8_widget_button_t align offset");
    if (@offsetOf(Button, "pressed") != 3 * ptr + 25) @compileError("ra8_widget_button_t pressed offset");
    if (@offsetOf(Button, "reserved") != 3 * ptr + 26) @compileError("ra8_widget_button_t reserved offset");
}

/// Vtable `render`: paint the bordered face for the current latch state, then
/// the label over it. The label is drawn on the face colour, not on a separate
/// background, so a pressed button's text sits on `face_pressed`.
fn renderButton(w: *Widget) callconv(.c) void {
    const button: *const Button = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = button.paint orelse return;

    const face = button.faceColor();
    paint_abi.priv_widget_fill_box(backend, &w.rect, face, button.border, button.border_w);

    const text = button.text orelse return;
    const styled = backend.draw_text_style;
    if (styled == null and backend.draw_text == null) return;

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    const size: paint_abi.TextSize = if (button.text_size == .default) .size_3 else button.text_size;
    paint_abi.priv_widget_text_pos(backend, &w.rect, text, button.pad, button.alignment, button.text_face, button.text_weight, size, styled != null, &pen_x, &pen_y);
    if (styled) |draw| draw(backend.user, pen_x, pen_y, text, @intFromEnum(button.text_face), @intFromEnum(button.text_weight), @intFromEnum(size), button.fg, face) else backend.draw_text.?(backend.user, pen_x, pen_y, text, button.fg, face);
}

/// Vtable `on_input`: latch a touch, decline everything else.
///
/// The dispatcher has already hit-tested the touch onto this button, so the
/// latch is unconditional: flip `pressed`, bump `presses`, self-invalidate
/// with the fast hint so only this rect re-flushes, then fire `on_press`. A
/// button-kind event is declined so it keeps routing to other widgets.
fn onButtonInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const button: *Button = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;

    button.pressed = !button.pressed;
    button.presses +%= 1;
    _ = types.ra8_widget_invalidate(w, .fast);

    if (button.on_press) |on_press| on_press(w);
    return true;
}

/// The single immutable vtable shared by every push button.
const button_vtable: Vtable = .{
    .measure = null,
    .render = renderButton,
    .on_input = onButtonInput,
};

/// `ra8_widget_button_vtable`: the shared button vtable, in static storage.
pub export fn ra8_widget_button_vtable() callconv(.c) *const Vtable {
    return &button_vtable;
}

/// `ra8_widget_button_init`: bind `w` to render and latch `button`.
pub export fn ra8_widget_button_init(w: ?*Widget, button: ?*Button) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = button orelse return types.refuseNull(tag, "button must not be nullptr");

    widget.vt = ra8_widget_button_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
