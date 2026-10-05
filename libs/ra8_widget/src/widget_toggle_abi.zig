//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the checkbox toggle leaf widget. It paints and routes
//! through the shared widget paint backend, with all mutable state kept in the
//! caller-owned descriptor.

const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");

/// Published widget geometry, paint, instance, and event types.
pub const Rect = types.Rect;
pub const Paint = types.Paint;
pub const Widget = types.Widget;
pub const Vtable = types.Vtable;
pub const Event = types.Event;
pub const EventKind = types.EventKind;
pub const err = types.err;

/// Toggle descriptor of the published `ra8_widget_toggle_t`.
/// Caller-owned plain data; `checked` is the current state.
pub const Toggle = extern struct {
    paint: ?*const Paint,
    label: ?[*:0]const u8,
    fg: u32,
    bg: u32,
    border: u32,
    mark: u32,
    box_size: u16,
    gap: u16,
    checked: bool,
    reserved: [3]u8,
};

comptime {
    const ptr = @sizeOf(usize);
    if (@alignOf(Toggle) != @alignOf(usize)) @compileError("ra8_widget_toggle_t alignment");
    if (@sizeOf(Toggle) != 2 * ptr + 24) @compileError("ra8_widget_toggle_t size");
    if (@offsetOf(Toggle, "paint") != 0) @compileError("ra8_widget_toggle_t paint offset");
    if (@offsetOf(Toggle, "label") != ptr) @compileError("ra8_widget_toggle_t label offset");
    if (@offsetOf(Toggle, "fg") != 2 * ptr) @compileError("ra8_widget_toggle_t fg offset");
    if (@offsetOf(Toggle, "bg") != 2 * ptr + 4) @compileError("ra8_widget_toggle_t bg offset");
    if (@offsetOf(Toggle, "border") != 2 * ptr + 8) @compileError("ra8_widget_toggle_t border offset");
    if (@offsetOf(Toggle, "mark") != 2 * ptr + 12) @compileError("ra8_widget_toggle_t mark offset");
    if (@offsetOf(Toggle, "box_size") != 2 * ptr + 16) @compileError("ra8_widget_toggle_t box_size offset");
    if (@offsetOf(Toggle, "gap") != 2 * ptr + 18) @compileError("ra8_widget_toggle_t gap offset");
    if (@offsetOf(Toggle, "checked") != 2 * ptr + 20) @compileError("ra8_widget_toggle_t checked offset");
}

fn renderToggle(w: *Widget) callconv(.c) void {
    const toggle: *const Toggle = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = toggle.paint orelse return;
    const fill_rect = backend.fill_rect orelse return;
    const side = @min(@as(i32, toggle.box_size), @max(w.rect.h, 0));
    if (side == 0) return;

    const x = w.rect.x;
    const y = w.rect.y + @divTrunc(w.rect.h - side, 2);
    fill_rect(backend.user, x, y, side, side, toggle.border);
    if (side > 2) {
        fill_rect(backend.user, x + 1, y + 1, side - 2, side - 2, toggle.bg);
    }
    if (toggle.checked and side > 4) {
        const inset: i32 = @max(@divTrunc(side, 4), 1);
        fill_rect(backend.user, x + inset, y + inset, side - 2 * inset, side - 2 * inset, toggle.mark);
    }

    const label = toggle.label orelse return;
    const draw_text = backend.draw_text orelse return;
    const label_rect: Rect = .{
        .x = x + side + toggle.gap,
        .y = w.rect.y,
        .w = @max(w.rect.w - side - toggle.gap, 0),
        .h = w.rect.h,
    };
    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(backend, &label_rect, label, 0, .left, .sans, .regular, .size_3, false, &pen_x, &pen_y);
    draw_text(backend.user, pen_x, pen_y, label, toggle.fg, toggle.bg);
}

fn onToggleInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const toggle: *Toggle = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;
    toggle.checked = !toggle.checked;
    _ = types.ra8_widget_invalidate(w, .fast);
    return true;
}

const toggle_vtable: Vtable = .{ .measure = null, .render = renderToggle, .on_input = onToggleInput };

/// Return the shared vtable backing every checkbox toggle.
pub export fn ra8_widget_toggle_vtable() callconv(.c) *const Vtable {
    return &toggle_vtable;
}

/// Bind a widget instance to a caller-owned checkbox toggle descriptor.
pub export fn ra8_widget_toggle_init(w: ?*Widget, toggle: ?*Toggle) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull("ra8_widget_toggle", "w must not be nullptr");
    const descriptor = toggle orelse return types.refuseNull("ra8_widget_toggle", "toggle must not be nullptr");
    widget.vt = ra8_widget_toggle_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
