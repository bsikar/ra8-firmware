//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Page-count navigation leaf for lists and grids. The caller owns the current
//! page and item counts; this widget paints Previous, a page or item-range
//! label, and Next,
//! routes taps, and invalidates only its own rectangle after a page turn.

const std = @import("std");
const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");

pub const Rect = types.Rect;
pub const Paint = types.Paint;
pub const Widget = types.Widget;
pub const Vtable = types.Vtable;
pub const Event = types.Event;
pub const Refresh = types.Refresh;
pub const err = types.err;

/// Label shown between Previous and Next.
pub const LabelFormat = enum(u8) {
    page = 0,
    range = 1,
};

pub const geometry = struct {
    pub const no_items: u16 = 0;
    pub const no_capacity: u16 = 0;
    pub const first_page: u16 = 0;
    pub const step: u16 = 1;
    pub const divisions: i32 = 3;
    pub const no_inset: i16 = 0;
};

const tag: [*:0]const u8 = "ra8_widget_pager";
const previous_label: [*:0]const u8 = "Previous";
const next_label: [*:0]const u8 = "Next";

/// Caller-owned page state. `page` is zero-based; the displayed label is
/// one-based. A zero item count or zero capacity has zero pages.
pub const Pager = extern struct {
    paint: ?*const Paint,
    item_count: u16,
    page_capacity: u16,
    page: u16,
    label_format: LabelFormat = .page,
    bg: u32,
    fg: u32,
    fg_disabled: u32,
    text_face: paint_abi.Face = .sans,
    text_weight: paint_abi.Weight = .regular,
    text_size: paint_abi.TextSize = .default,
};

comptime {
    const ptr = @sizeOf(usize);
    if (@alignOf(Pager) != @alignOf(usize)) @compileError("ra8_widget_pager_t alignment");
    if (@offsetOf(Pager, "paint") != 0) @compileError("ra8_widget_pager_t paint offset");
    if (@offsetOf(Pager, "item_count") != ptr) @compileError("ra8_widget_pager_t item_count offset");
    if (@offsetOf(Pager, "page_capacity") != ptr + 2) @compileError("ra8_widget_pager_t page_capacity offset");
    if (@offsetOf(Pager, "page") != ptr + 4) @compileError("ra8_widget_pager_t page offset");
    if (@offsetOf(Pager, "label_format") != ptr + 6) @compileError("ra8_widget_pager_t label_format offset");
    if (@offsetOf(Pager, "bg") != ptr + 8) @compileError("ra8_widget_pager_t bg offset");
    if (@offsetOf(Pager, "fg") != ptr + 12) @compileError("ra8_widget_pager_t fg offset");
    if (@offsetOf(Pager, "fg_disabled") != ptr + 16) @compileError("ra8_widget_pager_t fg_disabled offset");
}

/// Number of pages needed to show all items, or zero when paging is undefined.
pub fn pageCount(item_count: u16, capacity: u16) u16 {
    if (item_count == geometry.no_items or capacity == geometry.no_capacity) return 0;
    return @intCast((@as(u32, item_count) + capacity - 1) / capacity);
}

/// Keep the current page within the available range. Empty content uses page 0.
pub fn clampPage(page: u16, pages: u16) u16 {
    if (pages == 0) return geometry.first_page;
    return @min(page, pages - geometry.step);
}

fn drawText(backend: *const Paint, rect: Rect, text: [*:0]const u8, fg: u32, bg: u32, face: paint_abi.Face, weight: paint_abi.Weight, size: paint_abi.TextSize) void {
    const styled = backend.draw_text_style;
    if (styled == null and backend.draw_text == null) return;
    const selected_size = if (size == .default) paint_abi.TextSize.size_3 else size;
    var x: i32 = 0;
    var y: i32 = 0;
    paint_abi.priv_widget_text_pos(backend, &rect, text, geometry.no_inset, .center, face, weight, selected_size, styled != null, &x, &y);
    if (styled) |draw| draw(backend.user, x, y, text, @backingInt(face), @backingInt(weight), @backingInt(selected_size), fg, bg) else backend.draw_text.?(backend.user, x, y, text, fg, bg);
}

/// Format into stack storage so rendering never allocates.
fn pageLabel(buffer: []u8, pager: *const Pager, page: u16, pages: u16) [*:0]const u8 {
    const label = switch (pager.label_format) {
        .page => std.fmt.bufPrintZ(buffer, "Page {d} of {d}", .{ if (pages == 0) 0 else page + 1, pages }),
        .range => rangeLabel(buffer, pager, page, pages),
    } catch return "Page 0 of 0";
    return label.ptr;
}

fn rangeLabel(buffer: []u8, pager: *const Pager, page: u16, pages: u16) ![:0]u8 {
    if (pages == 0) return std.fmt.bufPrintZ(buffer, "0 to 0 of 0", .{});
    const first = @as(u32, page) * pager.page_capacity + 1;
    const last = @min(first + pager.page_capacity - 1, pager.item_count);
    return std.fmt.bufPrintZ(buffer, "{d} to {d} of {d}", .{ first, last, pager.item_count });
}

fn render(w: *Widget) callconv(.c) void {
    const pager: *const Pager = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = pager.paint orelse return;
    paint_abi.priv_widget_fill_box(backend, &w.rect, pager.bg, pager.bg, geometry.no_inset);
    if (backend.draw_text == null and backend.draw_text_style == null) return;

    const left_edge = @divTrunc(w.rect.w, geometry.divisions);
    const right_edge = @divTrunc(w.rect.w * 2, geometry.divisions);
    const pages = pageCount(pager.item_count, pager.page_capacity);
    const page = clampPage(pager.page, pages);
    const prev_color = if (page == 0) pager.fg_disabled else pager.fg;
    const next_color = if (pages == 0 or page + 1 >= pages) pager.fg_disabled else pager.fg;
    var label: [24:0]u8 = undefined;

    drawText(backend, .{ .x = w.rect.x, .y = w.rect.y, .w = left_edge, .h = w.rect.h }, previous_label, prev_color, pager.bg, pager.text_face, pager.text_weight, pager.text_size);
    drawText(backend, .{ .x = w.rect.x + left_edge, .y = w.rect.y, .w = right_edge - left_edge, .h = w.rect.h }, pageLabel(&label, pager, page, pages), pager.fg, pager.bg, pager.text_face, pager.text_weight, pager.text_size);
    drawText(backend, .{ .x = w.rect.x + right_edge, .y = w.rect.y, .w = w.rect.w - right_edge, .h = w.rect.h }, next_label, next_color, pager.bg, pager.text_face, pager.text_weight, pager.text_size);
}

fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const pager: *Pager = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;
    if (event.x < w.rect.x or event.x >= w.rect.x + w.rect.w or event.y < w.rect.y or event.y >= w.rect.y + w.rect.h) return false;

    const pages = pageCount(pager.item_count, pager.page_capacity);
    const current = clampPage(pager.page, pages);
    const left_edge = w.rect.x + @divTrunc(w.rect.w, geometry.divisions);
    const right_edge = w.rect.x + @divTrunc(w.rect.w * 2, geometry.divisions);
    var next = current;
    if (event.x < left_edge) {
        if (current > 0) next -= geometry.step;
    } else if (event.x >= right_edge) {
        if (pages > 0 and current + geometry.step < pages) next += geometry.step;
    }
    if (next != pager.page) {
        pager.page = next;
        _ = types.ra8_widget_invalidate(w, .fast);
    }
    return true;
}

const vtable: Vtable = .{ .measure = null, .render = render, .on_input = onInput };

pub export fn ra8_widget_pager_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

pub export fn ra8_widget_pager_init(w: ?*Widget, pager: ?*Pager) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = pager orelse return types.refuseNull(tag, "pager must not be nullptr");
    descriptor.page = clampPage(descriptor.page, pageCount(descriptor.item_count, descriptor.page_capacity));
    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
