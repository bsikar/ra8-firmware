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
pub const PixelFormat = paint_abi.PixelFormat;

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
    const fill = backend.fill_rect orelse return;
    fill(backend.user, rect.x, rect.y, rect.w, rect.h, color);
}
