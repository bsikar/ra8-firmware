//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the one-of-N segmented-control leaf widget. Labels and
//! selection remain caller-owned, bounded by the descriptor's byte-sized count.

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

/// Segmented-control descriptor of the published `ra8_widget_segmented_t`.
/// `labels` has `count` entries and `selected` is always less than `count`.
pub const Segmented = extern struct {
    paint: ?*const Paint,
    labels: ?[*]const [*:0]const u8,
    fg: u32,
    selected_fg: u32,
    bg: u32,
    selected_bg: u32,
    border: u32,
    count: u8,
    selected: u8,
    pad: u16,
};

comptime {
    const ptr = @sizeOf(usize);
    if (@alignOf(Segmented) != @alignOf(usize)) @compileError("ra8_widget_segmented_t alignment");
    if (@sizeOf(Segmented) != 2 * ptr + 24) @compileError("ra8_widget_segmented_t size");
    if (@offsetOf(Segmented, "paint") != 0) @compileError("ra8_widget_segmented_t paint offset");
    if (@offsetOf(Segmented, "labels") != ptr) @compileError("ra8_widget_segmented_t labels offset");
    if (@offsetOf(Segmented, "fg") != 2 * ptr) @compileError("ra8_widget_segmented_t fg offset");
    if (@offsetOf(Segmented, "selected_fg") != 2 * ptr + 4) @compileError("ra8_widget_segmented_t selected_fg offset");
    if (@offsetOf(Segmented, "bg") != 2 * ptr + 8) @compileError("ra8_widget_segmented_t bg offset");
    if (@offsetOf(Segmented, "selected_bg") != 2 * ptr + 12) @compileError("ra8_widget_segmented_t selected_bg offset");
    if (@offsetOf(Segmented, "border") != 2 * ptr + 16) @compileError("ra8_widget_segmented_t border offset");
    if (@offsetOf(Segmented, "count") != 2 * ptr + 20) @compileError("ra8_widget_segmented_t count offset");
    if (@offsetOf(Segmented, "selected") != 2 * ptr + 21) @compileError("ra8_widget_segmented_t selected offset");
    if (@offsetOf(Segmented, "pad") != 2 * ptr + 22) @compileError("ra8_widget_segmented_t pad offset");
}

/// Width of segment `index`, distributing remainder pixels to leading segments.
pub fn segmentWidth(rect_width: i32, count: u8, index: u8) i32 {
    if (rect_width <= 0 or count == 0 or index >= count) return 0;
    const count_i32: i32 = count;
    const base = @divTrunc(rect_width, count_i32);
    const remainder = @rem(rect_width, count_i32);
    return base + @intFromBool(index < remainder);
}

fn segmentRect(w: *const Widget, control: *const Segmented, index: u8) Rect {
    var x = w.rect.x;
    for (0..index) |prior| {
        x += segmentWidth(w.rect.w, control.count, @intCast(prior));
    }
    return .{ .x = x, .y = w.rect.y, .w = segmentWidth(w.rect.w, control.count, index), .h = w.rect.h };
}

fn renderSegmented(w: *Widget) callconv(.c) void {
    const control: *const Segmented = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = control.paint orelse return;
    const fill_rect = backend.fill_rect orelse return;
    const labels = control.labels orelse return;
    if (control.count == 0 or control.selected >= control.count) return;

    for (0..control.count) |i| {
        const index: u8 = @intCast(i);
        const rect = segmentRect(w, control, index);
        const selected = index == control.selected;
        const bg = if (selected) control.selected_bg else control.bg;
        fill_rect(backend.user, rect.x, rect.y, rect.w, rect.h, control.border);
        const content: Rect = .{
            .x = rect.x + 1,
            .y = rect.y + 1,
            .w = @max(rect.w - 2, 0),
            .h = @max(rect.h - 2, 0),
        };
        if (content.w > 0 and content.h > 0) {
            fill_rect(backend.user, content.x, content.y, content.w, content.h, bg);
            if (backend.draw_text) |draw_text| {
                var pen_x: i32 = 0;
                var pen_y: i32 = 0;
                const pad: i16 = @intCast(@min(control.pad, @as(u16, 32767)));
                paint_abi.priv_widget_text_pos(backend, &content, labels[index], pad, .center, .sans, .regular, .size_3, false, &pen_x, &pen_y);
                draw_text(backend.user, pen_x, pen_y, labels[index], if (selected) control.selected_fg else control.fg, bg);
            }
        }
    }
}

fn onSegmentedInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const control: *Segmented = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch or control.count == 0 or control.selected >= control.count or w.rect.w <= 0) return false;
    if (control.labels == null) return false;

    const relative = @as(i64, event.x) - w.rect.x;
    if (relative < 0 or relative >= w.rect.w) return false;
    const index: u8 = @intCast(@divTrunc(relative * control.count, w.rect.w));
    if (index == control.selected) return true;

    control.selected = index;
    _ = types.ra8_widget_invalidate(w, .fast);
    return true;
}

const segmented_vtable: Vtable = .{ .measure = null, .render = renderSegmented, .on_input = onSegmentedInput };

/// Return the shared vtable backing every segmented control.
pub export fn ra8_widget_segmented_vtable() callconv(.c) *const Vtable {
    return &segmented_vtable;
}

/// Bind a widget instance to a caller-owned segmented-control descriptor.
pub export fn ra8_widget_segmented_init(w: ?*Widget, control: ?*Segmented) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull("ra8_widget_segmented", "w must not be nullptr");
    const descriptor = control orelse return types.refuseNull("ra8_widget_segmented", "control must not be nullptr");
    if (descriptor.labels == null or descriptor.count == 0 or descriptor.selected >= descriptor.count) return err.invalid_arg;
    widget.vt = ra8_widget_segmented_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
