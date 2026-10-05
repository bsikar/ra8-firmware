//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Caller-owned, touch-adjustable 13-cell vertical level bar. The centre cell
//! remains marked; positive values fill above it and negative values below.

const types = @import("widget_abi_types.zig");

pub const Rect = types.Rect;
pub const Paint = types.Paint;
pub const Widget = types.Widget;
pub const Vtable = types.Vtable;
pub const Event = types.Event;
pub const Refresh = types.Refresh;
pub const err = types.err;

pub const geometry = struct {
    pub const cell_count: u8 = 13;
    pub const center: u8 = 6;
    pub const min_value: i8 = -6;
    pub const max_value: i8 = 6;
};

const tag: [*:0]const u8 = "ra8_widget_level_bar";

/// Caller-owned level bar. `damage` is the bounding rect of cells whose fill
/// state changed during the last consumed touch.
pub const LevelBar = extern struct {
    paint: ?*const Paint,
    track: u32,
    fill: u32,
    center_mark: u32,
    value: i8,
    reserved: [3]u8 = .{ 0, 0, 0 },
    damage: Rect,
};

/// Vertical tile `index`, with integer boundaries that exactly cover `rect`.
pub fn cellRect(rect: Rect, index: u8) Rect {
    if (index >= geometry.cell_count or rect.w <= 0 or rect.h <= 0)
        return .{ .x = rect.x, .y = rect.y, .w = 0, .h = 0 };
    const height: i64 = rect.h;
    const y0: i32 = @intCast(@divTrunc(height * @as(i64, index), geometry.cell_count));
    const y1: i32 = @intCast(@divTrunc(height * @as(i64, index + 1), geometry.cell_count));
    return .{ .x = rect.x, .y = rect.y + y0, .w = rect.w, .h = y1 - y0 };
}

/// Inset one pixel after each tile to keep the 13 cells visually distinct.
fn paintCellRect(rect: Rect, index: u8) Rect {
    var cell = cellRect(rect, index);
    if (index + 1 < geometry.cell_count) cell.h = @max(cell.h - 1, 0);
    return cell;
}

/// Return whether one cell is filled by `value`.
pub fn cellFilled(value: i8, index: u8) bool {
    if (value > 0) return index < geometry.center and index >= geometry.center - @as(u8, @intCast(value));
    if (value < 0) return index > geometry.center and index <= geometry.center + @as(u8, @intCast(-value));
    return false;
}

/// Bounding box of the cells whose active state changes from `old` to `new`.
pub fn changedDamage(rect: Rect, old: i8, new: i8) Rect {
    var first: ?u8 = null;
    var last: u8 = 0;
    for (0..geometry.cell_count) |raw| {
        const index: u8 = @intCast(raw);
        if (cellFilled(old, index) == cellFilled(new, index)) continue;
        if (first == null) first = index;
        last = index;
    }
    const start = first orelse return .{ .x = rect.x, .y = rect.y, .w = 0, .h = 0 };
    const top = paintCellRect(rect, start);
    const bottom = paintCellRect(rect, last);
    return .{ .x = rect.x, .y = top.y, .w = rect.w, .h = bottom.y + bottom.h - top.y };
}

fn valueAt(rect: Rect, y: i32) ?i8 {
    if (rect.w <= 0 or rect.h <= 0 or y < rect.y or y >= rect.y + rect.h) return null;
    for (0..geometry.cell_count) |raw| {
        const index: u8 = @intCast(raw);
        const cell = cellRect(rect, index);
        if (cell.h > 0 and y >= cell.y and y < cell.y + cell.h) {
            if (index < geometry.center) return @intCast(geometry.center - index);
            if (index > geometry.center) return -@as(i8, @intCast(index - geometry.center));
            return 0;
        }
    }
    return null;
}

fn render(w: *Widget) callconv(.c) void {
    const bar: *const LevelBar = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = bar.paint orelse return;
    const fill_rect = backend.fill_rect orelse return;
    for (0..geometry.cell_count) |raw| {
        const index: u8 = @intCast(raw);
        const cell = paintCellRect(w.rect, index);
        if (cell.h <= 0) continue;
        const color = if (index == geometry.center) bar.center_mark else if (cellFilled(bar.value, index)) bar.fill else bar.track;
        fill_rect(backend.user, cell.x, cell.y, cell.w, cell.h, color);
    }
}

fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    if (event.kind != .touch or event.x < w.rect.x or event.x >= w.rect.x + w.rect.w) return false;
    const bar: *LevelBar = @ptrCast(@alignCast(w.ctx orelse return false));
    const next = valueAt(w.rect, event.y) orelse return false;
    const value = @max(geometry.min_value, @min(next, geometry.max_value));
    const damage = changedDamage(w.rect, bar.value, value);
    if (bar.value == value) {
        bar.damage = damage;
        return true;
    }
    bar.value = value;
    bar.damage = damage;
    _ = types.ra8_widget_invalidate(w, Refresh.fast);
    return true;
}

const vtable: Vtable = .{ .measure = null, .render = render, .on_input = onInput };

pub export fn ra8_widget_level_bar_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

pub export fn ra8_widget_level_bar_init(w: ?*Widget, bar: ?*LevelBar) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = bar orelse return types.refuseNull(tag, "bar must not be nullptr");
    if (descriptor.value < geometry.min_value or descriptor.value > geometry.max_value) return err.invalid_arg;
    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

comptime {
    const ptr = @sizeOf(usize);
    if (@offsetOf(LevelBar, "paint") != 0) @compileError("ra8_widget_level_bar_t paint offset");
    if (@offsetOf(LevelBar, "track") != ptr) @compileError("ra8_widget_level_bar_t track offset");
    if (@offsetOf(LevelBar, "fill") != ptr + 4) @compileError("ra8_widget_level_bar_t fill offset");
    if (@offsetOf(LevelBar, "center_mark") != ptr + 8) @compileError("ra8_widget_level_bar_t center_mark offset");
    if (@offsetOf(LevelBar, "value") != ptr + 12) @compileError("ra8_widget_level_bar_t value offset");
    if (@offsetOf(LevelBar, "damage") != ptr + 16) @compileError("ra8_widget_level_bar_t damage offset");
}
