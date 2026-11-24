//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Dispatch for the canvas ops RA8FW-951 added to the Paint table: lines,
//! unscaled blits, a clip stack and blended fills. Each callback is optional,
//! so a panel backend that binds only fill_rect keeps working: a missing line
//! falls back to fill_rect spans, a missing clip is a no-op, and a missing
//! blended fill paints solid. A missing blit is reported to the caller, which
//! keeps its own fill_rect path.

const std = @import("std");
const paint_abi = @import("widget_paint_abi.zig");

pub const Paint = paint_abi.Paint;
pub const Rect = paint_abi.Rect;
pub const Alignment = paint_abi.Alignment;
pub const PixelFormat = paint_abi.PixelFormat;
pub const CanvasCaps = paint_abi.CanvasCaps;
pub const Face = paint_abi.Face;
pub const Weight = paint_abi.Weight;
pub const TextSize = paint_abi.TextSize;

/// Normalize `.default` (0) and any unknown raw ABI byte to `.size_3`.
pub fn normalizeTextSize(size: TextSize) TextSize {
    return switch (@backingInt(size)) {
        1 => .size_1,
        2 => .size_2,
        3 => .size_3,
        4 => .size_4,
        5 => .size_5,
        6 => .body_38,
        7 => .title_68,
        8 => .clock_120,
        9 => .ui_26,
        10 => .ui_30,
        else => .size_3,
    };
}

/// Report whether `backend` binds at least one text drawing callback.
pub fn hasText(backend: *const Paint) bool {
    return backend.draw_text_style != null or backend.draw_text_face != null or backend.draw_text != null;
}

/// Report whether `backend` binds at least one text measurement callback.
pub fn hasMeasure(backend: *const Paint) bool {
    return backend.text_size_style != null or backend.text_size_face != null or backend.text_size != null;
}

/// Query native capabilities advertised by `backend`, falling back to
/// conservative feature detection when `backend.caps` is left null.
pub fn capabilities(backend: *const Paint) CanvasCaps {
    if (backend.caps) |c| return c.*;
    return .{
        .pixel_format = .gray8,
        .color = 0,
        .alpha = @intFromBool(backend.fill_rect_alpha != null),
        .gray4 = 0,
        .dither = 0,
        .line = @intFromBool(backend.draw_line != null),
        .blit = @intFromBool(backend.blit != null),
        .clip = @intFromBool(backend.push_clip != null and backend.pop_clip != null),
    };
}

/// Fill an axis-aligned rectangle when `fill_rect` is bound.
pub fn fillRect(backend: *const Paint, rect: Rect, color: u32) void {
    const fill = backend.fill_rect orelse return;
    fill(backend.user, rect.x, rect.y, rect.w, rect.h, color);
}

/// Fill `rect` with `fill` and an optional `border` frame of width `border_w`.
pub fn fillBox(backend: *const Paint, rect: Rect, fill: u32, border: u32, border_w: i16) void {
    paint_abi.priv_widget_fill_box(backend, &rect, fill, border, border_w);
}

/// Resolve aligned pen coordinates inside `rect` for `str`.
pub fn textPen(
    backend: *const Paint,
    rect: Rect,
    str: [*:0]const u8,
    pad: i16,
    alignment: Alignment,
    family: Face,
    stroke: Weight,
    size: TextSize,
) struct { x: i32, y: i32 } {
    const selected_size = normalizeTextSize(size);
    const styled = backend.draw_text_style != null or backend.draw_text_face != null;
    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(backend, &rect, str, pad, alignment, family, stroke, selected_size, styled, &pen_x, &pen_y);
    return .{ .x = pen_x, .y = pen_y };
}

/// Align and draw `str` inside `rect` using the highest-fidelity bound text callback.
pub fn drawTextInRect(
    backend: *const Paint,
    rect: Rect,
    str: [*:0]const u8,
    pad: i16,
    alignment: Alignment,
    family: Face,
    stroke: Weight,
    size: TextSize,
    fg: u32,
    bg: u32,
) void {
    if (!hasText(backend)) return;
    const selected_size = normalizeTextSize(size);
    const pen = textPen(backend, rect, str, pad, alignment, family, stroke, selected_size);
    drawTextStyle(backend, pen.x, pen.y, str, family, stroke, selected_size, fg, bg);
}

/// Draw a NUL-terminated string in the default UI style.
pub fn drawText(backend: *const Paint, x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32) void {
    const draw = backend.draw_text orelse return;
    draw(backend.user, x, y, str, fg, bg);
}

/// Draw text selecting family, falling back to `draw_text` when unbound.
pub fn drawTextFace(backend: *const Paint, x: i32, y: i32, str: [*:0]const u8, family: Face, fg: u32, bg: u32) void {
    if (backend.draw_text_face) |draw| return draw(backend.user, x, y, str, @backingInt(family), fg, bg);
    drawText(backend, x, y, str, fg, bg);
}

/// Draw text with full style, falling back through face then basic `draw_text`.
pub fn drawTextStyle(
    backend: *const Paint,
    x: i32,
    y: i32,
    str: [*:0]const u8,
    family: Face,
    stroke: Weight,
    size: TextSize,
    fg: u32,
    bg: u32,
) void {
    if (backend.draw_text_style) |draw| {
        return draw(backend.user, x, y, str, @backingInt(family), @backingInt(stroke), @backingInt(size), fg, bg);
    }
    drawTextFace(backend, x, y, str, family, fg, bg);
}

/// Measure text in the default UI style; returns zero extent when unmeasurable.
pub fn measureText(backend: *const Paint, str: [*:0]const u8) struct { w: i32, h: i32 } {
    var w: i32 = 0;
    var h: i32 = 0;
    if (backend.text_size) |measure| measure(backend.user, str, &w, &h);
    return .{ .w = w, .h = h };
}

/// Measure text with full style, falling back through face then `text_size`.
pub fn measureTextStyle(
    backend: *const Paint,
    str: [*:0]const u8,
    family: Face,
    stroke: Weight,
    size: TextSize,
) struct { w: i32, h: i32 } {
    var w: i32 = 0;
    var h: i32 = 0;
    if (backend.text_size_style) |measure| {
        measure(backend.user, str, @backingInt(family), @backingInt(stroke), @backingInt(size), &w, &h);
    } else if (backend.text_size_face) |measure| {
        measure(backend.user, str, @backingInt(family), &w, &h);
    } else if (backend.text_size) |measure| {
        measure(backend.user, str, &w, &h);
    }
    return .{ .w = w, .h = h };
}

/// Draw a line. Straight runs fall back to one fill_rect; a slope falls back
/// to one-pixel fills along the Bresenham walk.
pub fn line(backend: *const Paint, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) void {
    if (backend.draw_line) |draw| return draw(backend.user, x0, y0, x1, y1, color);
    const fill = backend.fill_rect orelse return;
    if (y0 == y1) return fill(backend.user, @min(x0, x1), y0, span(x0, x1), 1, color);
    if (x0 == x1) return fill(backend.user, x0, @min(y0, y1), 1, span(y0, y1), color);
    walk(backend, fill, x0, y0, x1, y1, color);
}

fn span(a: i32, b: i32) i32 {
    return @intCast(@abs(@as(i64, b) - a) + 1);
}

fn walk(backend: *const Paint, fill: anytype, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) void {
    const dx: i64 = @intCast(@abs(@as(i64, x1) - x0));
    const dy: i64 = -@as(i64, @intCast(@abs(@as(i64, y1) - y0)));
    const sx: i32 = if (x0 < x1) 1 else -1;
    const sy: i32 = if (y0 < y1) 1 else -1;
    var err = dx + dy;
    var x = x0;
    var y = y0;
    while (true) {
        fill(backend.user, x, y, 1, 1, color);
        if (x == x1 and y == y1) return;
        const twice = 2 * err;
        if (twice >= dy) {
            err += dy;
            x += sx;
        }
        if (twice <= dx) {
            err += dx;
            y += sy;
        }
    }
}

/// Hand `rect`'s worth of pixels to the backend unscaled. Returns false when
/// the backend has no blit, so the caller paints them its own way.
pub fn blit(backend: *const Paint, rect: Rect, pixels: [*]const u8, stride: i32, format: PixelFormat) bool {
    const copy = backend.blit orelse return false;
    copy(backend.user, rect.x, rect.y, rect.w, rect.h, pixels, stride, @backingInt(format));
    return true;
}

/// Clip later draws to `rect`, until the matching popClip.
pub fn pushClip(backend: *const Paint, rect: Rect) void {
    if (backend.push_clip) |push| push(backend.user, rect.x, rect.y, rect.w, rect.h);
}

/// Restore the clip that was current before the matching pushClip.
pub fn popClip(backend: *const Paint) void {
    if (backend.pop_clip) |pop| pop(backend.user);
}

/// Fill `rect` with `color` at `alpha` (0 transparent, 255 opaque). A backend
/// with no blended fill gets a solid fill for any visible alpha.
pub fn fillAlpha(backend: *const Paint, rect: Rect, color: u32, alpha: u8) void {
    if (alpha == 0) return;
    if (alpha != std.math.maxInt(u8)) {
        if (backend.fill_rect_alpha) |blend| return blend(backend.user, rect.x, rect.y, rect.w, rect.h, color, alpha);
    }
    fillRect(backend, rect, color);
}
