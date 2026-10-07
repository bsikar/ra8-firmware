//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Two-line list leaf for settings and navigation screens. Every row uses one
//! fixed-height geometry helper for paint and hit routing; taps record the
//! smallest affected rect before invalidating the host widget.
const types = @import("widget_abi_types.zig");
const paint = @import("widget_paint_abi.zig");
const std = @import("std");
pub const Rect = types.Rect;
pub const Paint = types.Paint;
pub const Widget = types.Widget;
pub const Vtable = types.Vtable;
pub const Event = types.Event;
pub const err = types.err;
pub const Trailing = enum(u8) { none = 0, value = 1, chevron = 2, value_chevron = 3 };
pub const Variant = enum(u8) { standard = 0, two_buttons = 1, toggle_help = 2 };
pub const Element = enum(u8) { row = 0, button_1 = 1, button_2 = 2, toggle = 3 };
pub const Row = extern struct {
    title: ?[*:0]const u8,
    subtitle: ?[*:0]const u8,
    trailing_text: ?[*:0]const u8,
    action_id: u16,
    trailing: Trailing,
    variant: Variant = .standard,
    button_1_text: ?[*:0]const u8 = null,
    button_1_action_id: u16 = 0,
    button_2_text: ?[*:0]const u8 = null,
    button_2_action_id: u16 = 0,
    help_text: ?[*:0]const u8 = null,
    toggle_value: ?*bool = null,
};
/// Caller-owned C ABI descriptor; damage is the changed row element rect.
pub const List = extern struct {
    paint: ?*const Paint,
    rows: ?[*]const Row,
    count: u16,
    on_select: ?*const fn (w: *Widget, action_id: u16) callconv(.c) void,
    bg: u32,
    title_fg: u32,
    subtitle_fg: u32,
    trailing_fg: u32,
    divider: u32,
    row_height: i32,
    pad: i16,
    selected: u16,
    has_selection: bool,
    damage: Rect,
    text_face: paint.Face = .sans,
    text_weight: paint.Weight = .regular,
    text_size: paint.TextSize = .default,
    on_select_element: ?*const fn (w: *Widget, row: u16, element: u8, action_id: u16) callconv(.c) void = null,
    selected_element: u8 = 0,
};

pub fn rowRect(r: Rect, h: i32, i: u16) Rect {
    if (h <= 0) return .{ .x = r.x, .y = r.y, .w = r.w, .h = 0 };
    const y = r.y + @as(i32, i) * h;
    return .{ .x = r.x, .y = y, .w = r.w, .h = @max(0, @min(y + h, r.y + @max(r.h, 0)) - y) };
}
pub fn hitRow(r: Rect, h: i32, count: usize, y: i32) ?u16 {
    if (h <= 0 or r.w <= 0 or y < r.y or y >= r.y + r.h) return null;
    const i: usize = @intCast(@divTrunc(y - r.y, h));
    if (i >= count or i > std.math.maxInt(u16)) return null;
    return @intCast(i);
}
pub fn buttonRect(bounds: Rect, which: Element) Rect {
    const button_width = @divTrunc(bounds.w, 4);
    const x = if (which == .button_1) bounds.x + bounds.w - 2 * button_width else bounds.x + bounds.w - button_width;
    return .{ .x = x, .y = bounds.y, .w = button_width, .h = bounds.h };
}
pub fn toggleRect(bounds: Rect, pad: i16) Rect {
    const size: i32 = 28;
    return .{ .x = bounds.x + bounds.w - @as(i32, pad) - size, .y = bounds.y + @divTrunc(bounds.h - size, 2), .w = size, .h = size };
}
pub fn hitElement(bounds: Rect, row: Row, x: i32, y: i32) ?Element {
    if (x < bounds.x or y < bounds.y or x >= bounds.x + bounds.w or y >= bounds.y + bounds.h) return null;
    switch (row.variant) {
        .standard => return .row,
        .two_buttons => {
            const first = buttonRect(bounds, .button_1);
            const second = buttonRect(bounds, .button_2);
            if (x >= first.x and x < first.x + first.w) return .button_1;
            if (x >= second.x and x < second.x + second.w) return .button_2;
            return .row;
        },
        .toggle_help => {
            const box = toggleRect(bounds, 12);
            if (x >= box.x and x < box.x + box.w and y >= box.y and y < box.y + box.h) return .toggle;
            return .row;
        },
    }
}
fn drawText(b: *const Paint, r: Rect, s: ?[*:0]const u8, fg: u32, bg: u32, pad: i16, alignment: paint.Alignment, face: paint.Face, weight: paint.Weight, size: paint.TextSize) void {
    const value = s orelse return;
    const styled = b.draw_text_style;
    if (styled == null and b.draw_text == null) return;
    const selected_size = if (size == .default) paint.TextSize.size_3 else size;
    var x: i32 = 0;
    var y: i32 = 0;
    paint.priv_widget_text_pos(b, &r, value, pad, alignment, face, weight, selected_size, styled != null, &x, &y);
    if (styled) |draw| {
        draw(b.user, x, y, value, @backingInt(face), @backingInt(weight), @backingInt(selected_size), fg, bg);
    } else b.draw_text.?(b.user, x, y, value, fg, bg);
}
fn fill(b: *const Paint, r: Rect, color: u32) void {
    if (b.fill_rect) |draw| draw(b.user, r.x, r.y, r.w, r.h, color);
}
fn drawButtons(b: *const Paint, row: Row, bounds: Rect, list: *const List) void {
    const first = buttonRect(bounds, .button_1);
    const second = buttonRect(bounds, .button_2);
    for ([_]Rect{ first, second }, [_]?[*:0]const u8{ row.button_1_text, row.button_2_text }) |rect, text| {
        fill(b, .{ .x = rect.x, .y = rect.y + 8, .w = rect.w - 6, .h = @max(0, rect.h - 16) }, list.trailing_fg);
        fill(b, .{ .x = rect.x + 1, .y = rect.y + 9, .w = rect.w - 8, .h = @max(0, rect.h - 18) }, list.bg);
        drawText(b, rect, text, list.trailing_fg, list.bg, 4, .center, list.text_face, list.text_weight, list.text_size);
    }
}
fn drawToggle(b: *const Paint, row: Row, bounds: Rect, list: *const List) void {
    const half = @divTrunc(bounds.h, 2);
    const label_area = Rect{ .x = bounds.x, .y = bounds.y, .w = bounds.w - 52, .h = half };
    const help_area = Rect{ .x = bounds.x, .y = bounds.y + half, .w = bounds.w - 52, .h = bounds.h - half };
    drawText(b, label_area, row.title, list.title_fg, list.bg, list.pad, .left, list.text_face, list.text_weight, list.text_size);
    drawText(b, help_area, row.help_text orelse row.subtitle, list.subtitle_fg, list.bg, list.pad, .left, list.text_face, list.text_weight, list.text_size);
    const box = toggleRect(bounds, 12);
    fill(b, box, list.trailing_fg);
    fill(b, .{ .x = box.x + 2, .y = box.y + 2, .w = box.w - 4, .h = box.h - 4 }, list.bg);
    if (row.toggle_value) |value_ptr| {
        if (value_ptr.*) {
            fill(b, .{ .x = box.x + 6, .y = box.y + 14, .w = 6, .h = 5 }, list.trailing_fg);
            fill(b, .{ .x = box.x + 11, .y = box.y + 9, .w = 5, .h = 10 }, list.trailing_fg);
            fill(b, .{ .x = box.x + 16, .y = box.y + 5, .w = 6, .h = 7 }, list.trailing_fg);
        }
    }
}
fn render(w: *Widget) callconv(.c) void {
    const list: *const List = @ptrCast(@alignCast(w.ctx orelse return));
    const b = list.paint orelse return;
    const fill_rect = b.fill_rect orelse return;
    const rows = list.rows orelse return;
    for (rows[0..list.count], 0..) |row, n| {
        const i: u16 = @intCast(n);
        const bounds = rowRect(w.rect, list.row_height, i);
        if (bounds.h <= 0) break;
        fill_rect(b.user, bounds.x, bounds.y, bounds.w, bounds.h, list.bg);
        if (bounds.h > 1) fill_rect(b.user, bounds.x, bounds.y + bounds.h - 1, bounds.w, 1, list.divider);
        if (row.variant == .two_buttons) {
            const title_area = Rect{ .x = bounds.x, .y = bounds.y, .w = bounds.w - 2 * @divTrunc(bounds.w, 4), .h = bounds.h };
            drawText(b, title_area, row.title, list.title_fg, list.bg, list.pad, .left, list.text_face, list.text_weight, list.text_size);
            drawButtons(b, row, bounds, list);
            continue;
        }
        if (row.variant == .toggle_help) {
            drawToggle(b, row, bounds, list);
            continue;
        }
        const trailing_width: i32 = if (row.trailing == .none) 0 else @min(@divTrunc(bounds.w, 3), 160);
        const text_width = bounds.w - trailing_width;
        const half = @divTrunc(bounds.h, 2);
        const title_area = Rect{ .x = bounds.x, .y = bounds.y, .w = text_width, .h = half };
        const sub_area = Rect{ .x = bounds.x, .y = bounds.y + half, .w = text_width, .h = bounds.h - half };
        const trailing_area = Rect{ .x = bounds.x + text_width, .y = bounds.y, .w = trailing_width, .h = bounds.h };
        drawText(b, title_area, row.title, list.title_fg, list.bg, list.pad, .left, list.text_face, list.text_weight, list.text_size);
        drawText(b, sub_area, row.subtitle, list.subtitle_fg, list.bg, list.pad, .left, list.text_face, list.text_weight, list.text_size);
        switch (row.trailing) {
            .none => {},
            .value => drawText(b, trailing_area, row.trailing_text, list.trailing_fg, list.bg, list.pad, .right, list.text_face, list.text_weight, list.text_size),
            .chevron => drawText(b, trailing_area, ">", list.trailing_fg, list.bg, list.pad, .right, list.text_face, list.text_weight, list.text_size),
            .value_chevron => {
                const value_area = Rect{ .x = trailing_area.x, .y = trailing_area.y, .w = trailing_area.w - @divTrunc(trailing_area.w, 4), .h = trailing_area.h };
                const chevron_area = Rect{ .x = value_area.x + value_area.w, .y = trailing_area.y, .w = trailing_area.w - value_area.w, .h = trailing_area.h };
                drawText(b, value_area, row.trailing_text, list.trailing_fg, list.bg, list.pad, .right, list.text_face, list.text_weight, list.text_size);
                drawText(b, chevron_area, ">", list.trailing_fg, list.bg, 4, .right, list.text_face, list.text_weight, list.text_size);
            },
        }
    }
}
fn onInput(w: *Widget, e: *const Event) callconv(.c) bool {
    const list: *List = @ptrCast(@alignCast(w.ctx orelse return false));
    if (e.kind != .touch) return false;
    const i = hitRow(w.rect, list.row_height, list.count, e.y) orelse return false;
    const rows = list.rows orelse return false;
    const row = rows[i];
    const bounds = rowRect(w.rect, list.row_height, i);
    const element = hitElement(bounds, row, e.x, e.y) orelse return false;
    const action_id = switch (element) {
        .button_1 => row.button_1_action_id,
        .button_2 => row.button_2_action_id,
        else => row.action_id,
    };
    if (element == .toggle) {
        if (row.toggle_value) |value_ptr| value_ptr.* = !value_ptr.*;
    }
    list.selected = i;
    list.has_selection = true;
    list.selected_element = @backingInt(element);
    list.damage = switch (element) {
        .button_1, .button_2 => buttonRect(bounds, element),
        .toggle => toggleRect(bounds, 12),
        .row => bounds,
    };
    _ = types.ra8_widget_invalidate(w, .fast);
    if (list.on_select) |notify| notify(w, action_id);
    if (list.on_select_element) |notify| notify(w, i, @backingInt(element), action_id);
    return true;
}
const vtable: Vtable = .{ .measure = null, .render = render, .on_input = onInput };
pub export fn ra8_widget_list_vtable() callconv(.c) *const Vtable {
    return &vtable;
}
pub export fn ra8_widget_list_init(w: ?*Widget, list: ?*List) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull("ra8_widget_list", "w must not be nullptr");
    const descriptor = list orelse return types.refuseNull("ra8_widget_list", "list must not be nullptr");
    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
