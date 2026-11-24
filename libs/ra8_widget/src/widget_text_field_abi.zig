//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Single-line editable text field with a caller-owned fixed buffer. Keyboard
//! input arrives through `applyKey`, keeping layout and drawing independent
//! from the keyboard engine.

const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");
const ops = @import("paint_ops.zig");

pub const Rect = types.Rect;
pub const Widget = types.Widget;
pub const Event = types.Event;
pub const KeyAction = types.KeyAction;
pub const err = types.err;

const tag: [*:0]const u8 = "ra8_widget_text_field";

/// Caller-owned descriptor. `capacity` includes room for the trailing NUL.
pub const TextField = extern struct {
    paint: ?*const paint_abi.Paint,
    buffer: ?[*]u8,
    capacity: u16,
    len: u16,
    placeholder: ?[*:0]const u8,
    fg: u32,
    bg: u32,
    caret: u32,
    pad: i16,
    face: paint_abi.Face,
    weight: paint_abi.Weight = .regular,
    size: paint_abi.TextSize = .default,
    focused: bool = false,
    submitted: bool = false,
    damage: Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
    on_submit: ?*const fn (w: *Widget) callconv(.c) void = null,
};

const vtable: types.Vtable = .{ .measure = null, .render = render, .on_input = onInput };

pub export fn ra8_widget_text_field_vtable() callconv(.c) *const types.Vtable {
    return &vtable;
}

pub export fn ra8_widget_text_field_init(w: ?*Widget, field: ?*TextField) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = field orelse return types.refuseNull(tag, "field must not be nullptr");
    if (descriptor.buffer == null or descriptor.capacity == 0 or descriptor.len >= descriptor.capacity) return err.invalid_arg;
    descriptor.buffer.?[descriptor.len] = 0;
    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

fn text(field: *const TextField) [*:0]const u8 {
    return @ptrCast(field.buffer.?);
}

fn render(w: *Widget) callconv(.c) void {
    const field: *TextField = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = field.paint orelse return;
    ops.fillBox(backend, w.rect, field.bg, field.bg, 0);

    const showing_placeholder = field.len == 0 and field.placeholder != null;
    const value = if (showing_placeholder) field.placeholder.? else text(field);
    const size = ops.normalizeTextSize(field.size);
    const pen = ops.textPen(backend, w.rect, value, field.pad, .left, field.face, field.weight, size);
    const text_x = pen.x + @as(i32, if (showing_placeholder and field.focused) 2 else 0);
    ops.drawTextStyle(backend, text_x, pen.y, value, field.face, field.weight, size, field.fg, field.bg);
    if (!field.focused) return;

    const ext = ops.measureTextStyle(backend, text(field), field.face, field.weight, size);
    ops.fillRect(backend, .{ .x = pen.x + ext.w, .y = pen.y, .w = 2, .h = @max(ext.h, 1) }, field.caret);
}

fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    if (event.kind != .touch) return false;
    const field: *TextField = @ptrCast(@alignCast(w.ctx orelse return false));
    field.focused = true;
    field.damage = textRect(w.rect, field.pad);
    _ = types.ra8_widget_invalidate(w, .fast);
    return true;
}

/// Deliver a keyboard action to a focused field. Returns true when the value
/// or submit state changed and writes the smallest text-area damage rectangle.
pub fn applyKey(widget: *Widget, action: KeyAction, glyph: u8) bool {
    const field: *TextField = @ptrCast(@alignCast(widget.ctx orelse return false));
    if (!field.focused) return false;
    const buffer = field.buffer orelse return false;
    switch (action) {
        .character, .space => {
            if (field.len + 1 >= field.capacity) return false;
            if (action == .character and glyph == 0) return false;
            buffer[field.len] = if (action == .space) 32 else glyph;
            field.len += 1;
            buffer[field.len] = 0;
        },
        .backspace => {
            if (field.len == 0) return false;
            field.len -= 1;
            buffer[field.len] = 0;
        },
        .enter => {
            field.submitted = true;
            if (field.on_submit) |submit| submit(widget);
            return true;
        },
        .other => return false,
    }
    field.damage = textRect(widget.rect, field.pad);
    _ = types.ra8_widget_invalidate(widget, .fast);
    return true;
}

fn textRect(rect: Rect, pad: i16) Rect {
    const inset = @max(@as(i32, pad), 0);
    return .{ .x = rect.x + inset, .y = rect.y, .w = @max(rect.w - inset * 2, 0), .h = rect.h };
}
