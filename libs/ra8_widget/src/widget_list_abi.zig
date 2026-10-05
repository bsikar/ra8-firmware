//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Two-line list leaf for settings and navigation screens. Every row uses one
//! fixed-height geometry helper for paint and hit routing; taps record the
//! affected row rect before invalidating the host widget.
const types = @import("widget_abi_types.zig");
const paint = @import("widget_paint_abi.zig");
const std = @import("std");
pub const Rect = types.Rect;
pub const Paint = types.Paint;
pub const Widget = types.Widget;
pub const Vtable = types.Vtable;
pub const Event = types.Event;
pub const err = types.err;
pub const Trailing = enum(u8) { none = 0, value = 1, chevron = 2 };
pub const Row = extern struct { title: ?[*:0]const u8, subtitle: ?[*:0]const u8, trailing_text: ?[*:0]const u8, action_id: u16, trailing: Trailing };
/// Caller-owned C ABI descriptor; damage is the row rect from the last tap.
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
fn drawText(b: *const Paint, r: Rect, s: ?[*:0]const u8, fg: u32, bg: u32, pad: i16, alignment: paint.Alignment) void {
    const value = s orelse return;
    const draw = b.draw_text orelse return;
    var x: i32 = 0;
    var y: i32 = 0;
    paint.priv_widget_text_pos(b, &r, value, pad, alignment, .sans, .regular, .size_3, false, &x, &y);
    draw(b.user, x, y, value, fg, bg);
}
fn render(w: *Widget) callconv(.c) void {
    const list: *const List = @ptrCast(@alignCast(w.ctx orelse return));
    const b = list.paint orelse return;
    const fill = b.fill_rect orelse return;
    const rows = list.rows orelse return;
    for (rows[0..list.count], 0..) |row, n| {
        const i: u16 = @intCast(n);
        const bounds = rowRect(w.rect, list.row_height, i);
        if (bounds.h <= 0) break;
        const bg = list.bg;
        fill(b.user, bounds.x, bounds.y, bounds.w, bounds.h, bg);
        if (bounds.h > 1) fill(b.user, bounds.x, bounds.y + bounds.h - 1, bounds.w, 1, list.divider);
        const trailing_width: i32 = if (row.trailing == .none) 0 else @min(@divTrunc(bounds.w, 3), 160);
        const text_width = bounds.w - trailing_width;
        const half = @divTrunc(bounds.h, 2);
        const title_area = Rect{ .x = bounds.x, .y = bounds.y, .w = text_width, .h = half };
        const sub_area = Rect{ .x = bounds.x, .y = bounds.y + half, .w = text_width, .h = bounds.h - half };
        const trailing_area = Rect{ .x = bounds.x + text_width, .y = bounds.y, .w = trailing_width, .h = bounds.h };
        drawText(b, title_area, row.title, list.title_fg, bg, list.pad, .left);
        drawText(b, sub_area, row.subtitle, list.subtitle_fg, bg, list.pad, .left);
        switch (row.trailing) {
            .none => {},
            .value => drawText(b, trailing_area, row.trailing_text, list.trailing_fg, bg, list.pad, .right),
            .chevron => drawText(b, trailing_area, ">", list.trailing_fg, bg, list.pad, .right),
        }
    }
}
fn onInput(w: *Widget, e: *const Event) callconv(.c) bool {
    const list: *List = @ptrCast(@alignCast(w.ctx orelse return false));
    if (e.kind != .touch) return false;
    const i = hitRow(w.rect, list.row_height, list.count, e.y) orelse return false;
    list.selected = i;
    list.has_selection = true;
    list.damage = rowRect(w.rect, list.row_height, i);
    _ = types.ra8_widget_invalidate(w, .fast);
    if (list.on_select) |notify| {
        const rows = list.rows orelse return true;
        notify(w, rows[i].action_id);
    }
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
