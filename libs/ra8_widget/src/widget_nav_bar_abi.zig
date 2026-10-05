//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the navigation-strip leaf widget declared in
//! `inc/ra8_widget_nav_bar.h`: `ra8_widget_nav_bar_vtable` and
//! `ra8_widget_nav_bar_init`.
//!
//! The strip is `count` equal-width cells, each a centred label and each a tap
//! target. Drawing and hit-testing both derive from one boundary helper,
//! `cellStart`, so a tap lands in the cell it looks like it lands in even on a
//! width the cell count does not divide.

const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");
const icon_atlas = @import("widget_icons.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Draw backend of the published ABI (`ra8_widget_paint_t`).
pub const Paint = types.Paint;
/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = types.Widget;
/// Behaviour table of the published ABI (`ra8_widget_vtable_t`).
pub const Vtable = types.Vtable;
/// One input event of the published ABI (`ra8_widget_event_t`).
pub const Event = types.Event;
/// E-ink-style refresh hint of the published ABI (`ra8_widget_refresh_t`).
pub const Refresh = types.Refresh;
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;
/// Optional glyph assigned to a navigation cell.
pub const Icon = icon_atlas.Icon;
/// Shared atlas painter for navigation cells and other UI widgets.
pub const icons = icon_atlas;

/// Fixed geometry of the strip and its cells.
pub const geometry = struct {
    /// A strip of this many cells draws and routes nothing.
    pub const no_cells: u16 = 0;
    /// A cell rect is already the text box, so its label gets no inset.
    pub const no_pad: i16 = 0;
    /// A strip this wide (or narrower) cannot be divided into cells.
    pub const degenerate_width: i32 = 0;
};

const tag: [*:0]const u8 = "ra8_widget_nav_bar";

/// Caller-owned navigation-strip descriptor (`ra8_widget_nav_bar_t`).
///
/// `items` is the C `const char* const*`: a bare pointer to `count` label
/// strings. It becomes a slice at the top of `render`, so the rest of this
/// file indexes a bounded thing rather than a raw pointer.
pub const NavBar = extern struct {
    paint: ?*const Paint,
    items: ?[*]const ?[*:0]const u8,
    on_select: ?*const fn (w: *Widget, index: u16) callconv(.c) void,
    bg: u32,
    fg_active: u32,
    fg_muted: u32,
    count: u16,
    active: u16,
    selected: u16,
    text_face: paint_abi.Face = .sans,
    text_weight: paint_abi.Weight = .regular,
    text_size: paint_abi.TextSize = .default,
    icons: ?[*]const Icon = null,
};

/// Offset of cell `idx`'s left edge from the strip's own left edge.
///
/// The single source of truth for where one cell ends and the next begins:
/// rounded prefix widths, so the cells tile the strip with no gap and the
/// remainder lands deterministically. `cellRect` draws from these boundaries
/// and `hitCell` routes from them, which is what keeps the two in step on a
/// width the cell count does not divide.
pub fn cellStart(width: i32, idx: u16, count: u16) i32 {
    return @divTrunc(width * @as(i32, @intCast(idx)), @as(i32, @intCast(count)));
}

/// Cell `idx` of a `count`-cell strip laid inside `strip`.
pub fn cellRect(strip: Rect, idx: u16, count: u16) Rect {
    const x0 = cellStart(strip.w, idx, count);
    const x1 = cellStart(strip.w, idx + 1, count);
    return .{ .x = strip.x + x0, .y = strip.y, .w = x1 - x0, .h = strip.h };
}

/// Cell index `px` lands on, or null for an empty strip, a degenerate width,
/// or a tap outside the strip.
///
/// Proportional division alone is not the inverse of rounded prefix widths, so
/// it serves only as a first guess: it never overshoots the cell the tap is
/// drawn inside, and advancing while the next boundary has already been passed
/// lands on that cell exactly. A cell the rounding left zero pixels wide is
/// stepped over rather than routed to.
pub fn hitCell(strip: Rect, count: u16, px: i32) ?u16 {
    if (count == geometry.no_cells) return null;
    if (strip.w <= geometry.degenerate_width) return null;
    if (px < strip.x or px >= strip.x + strip.w) return null;

    const offset = px - strip.x;
    var idx: u16 = @intCast(@divTrunc(offset * @as(i32, @intCast(count)), strip.w));
    while (idx + 1 < count and cellStart(strip.w, idx + 1, count) <= offset) idx += 1;
    return idx;
}

/// Draw a cell icon above its label when one is assigned. An item with no
/// label and no icon is a gap.
fn drawItem(backend: *const Paint, cell: Rect, label: ?[*:0]const u8, icon: Icon, fg: u32, bg: u32, face: paint_abi.Face, weight: paint_abi.Weight, size: paint_abi.TextSize) void {
    var text_rect = cell;
    if (icon != .none) {
        const icon_h = @min(@divTrunc(cell.h, 2), 24);
        icon_atlas.draw(backend, icon, .{ .x = cell.x, .y = cell.y + 2, .w = cell.w, .h = icon_h }, fg, bg);
        text_rect.y += icon_h;
        text_rect.h -= icon_h;
    }
    const text = label orelse return;
    const styled = backend.draw_text_style;
    if (styled == null and backend.draw_text == null) return;
    const selected_size = if (size == .default) paint_abi.TextSize.size_3 else size;
    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(backend, &text_rect, text, geometry.no_pad, .center, face, weight, selected_size, styled != null, &pen_x, &pen_y);
    if (styled) |draw| draw(backend.user, pen_x, pen_y, text, @intFromEnum(face), @intFromEnum(weight), @intFromEnum(selected_size), fg, bg) else backend.draw_text.?(backend.user, pen_x, pen_y, text, fg, bg);
}

/// Fill the strip, then centre each item's label in its own cell.
fn render(w: *Widget) callconv(.c) void {
    const nav: *const NavBar = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = nav.paint orelse return;

    paint_abi.priv_widget_fill_box(backend, &w.rect, nav.bg, nav.bg, geometry.no_pad);

    if (nav.count == geometry.no_cells) return;
    const items = nav.items orelse return;
    const icon_values = nav.icons;

    for (items[0..nav.count], 0..) |label, i| {
        const idx: u16 = @intCast(i);
        const fg = if (idx == nav.active) nav.fg_active else nav.fg_muted;
        const icon = if (icon_values) |values| values[i] else .none;
        drawItem(backend, cellRect(w.rect, idx, nav.count), label, icon, fg, nav.bg, nav.text_face, nav.text_weight, nav.text_size);
    }
}

/// Route a touch to the cell it hit; a tap off the strip is declined so it can
/// keep travelling.
fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const nav: *NavBar = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;

    const idx = hitCell(w.rect, nav.count, event.x) orelse return false;

    nav.selected = idx;
    _ = types.ra8_widget_invalidate(w, .fast);
    if (nav.on_select) |notify| notify(w, idx);
    return true;
}

const vtable: Vtable = .{
    .measure = null,
    .render = render,
    .on_input = onInput,
};

/// Return the one immutable vtable shared by every navigation strip.
pub export fn ra8_widget_nav_bar_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

/// Bind `w` to navigation strip `nav`: vtable, context, visible.
pub export fn ra8_widget_nav_bar_init(w: ?*Widget, nav: ?*NavBar) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = nav orelse return types.refuseNull(tag, "nav must not be nullptr");

    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

comptime {
    const ptr = @sizeOf(usize);

    if (@offsetOf(NavBar, "paint") != 0) @compileError("ra8_widget_nav_bar_t paint offset");
    if (@offsetOf(NavBar, "items") != ptr) @compileError("ra8_widget_nav_bar_t items offset");
    if (@offsetOf(NavBar, "on_select") != 2 * ptr) @compileError("ra8_widget_nav_bar_t on_select offset");
    if (@offsetOf(NavBar, "bg") != 3 * ptr) @compileError("ra8_widget_nav_bar_t bg offset");
    if (@offsetOf(NavBar, "fg_active") != 3 * ptr + 4) @compileError("ra8_widget_nav_bar_t fg_active offset");
    if (@offsetOf(NavBar, "fg_muted") != 3 * ptr + 8) @compileError("ra8_widget_nav_bar_t fg_muted offset");
    if (@offsetOf(NavBar, "count") != 3 * ptr + 12) @compileError("ra8_widget_nav_bar_t count offset");
    if (@offsetOf(NavBar, "active") != 3 * ptr + 14) @compileError("ra8_widget_nav_bar_t active offset");
    if (@offsetOf(NavBar, "selected") != 3 * ptr + 16) @compileError("ra8_widget_nav_bar_t selected offset");
    if (@offsetOf(NavBar, "text_face") != 3 * ptr + 18) @compileError("ra8_widget_nav_bar_t text_face offset");
    if (@offsetOf(NavBar, "text_size") != 3 * ptr + 20) @compileError("ra8_widget_nav_bar_t text_size offset");
    const icons_offset = ((3 * ptr + 21 + ptr - 1) / ptr) * ptr;
    if (@offsetOf(NavBar, "icons") != icons_offset) @compileError("ra8_widget_nav_bar_t icons offset");
    if (@alignOf(NavBar) != @alignOf(usize)) @compileError("ra8_widget_nav_bar_t alignment");
}
