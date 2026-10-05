//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CPU renderer for decoded greyscale image content. The caller owns the pixel
//! slice for the duration of render; no image decoding or allocation occurs here.

const std = @import("std");
pub const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");

/// Scaling policy for an image placed in a destination rectangle.
pub const Scale = enum { fit, fill };

/// Decoded, row-major, 8-bit greyscale pixels.
pub const Bitmap = struct {
    pixels: []const u8,
    width: u32,
    height: u32,
};

/// Placeholder appearance used when no bitmap is available.
pub const Placeholder = struct {
    fill: u8 = 255,
    border: u8 = 96,
    border_width: i32 = 2,
};

/// Damage caused by replacing this image. The renderer may redraw all pixels
/// in this rectangle, so callers can pass it directly to the compositor.
pub fn damageRect(rect: types.Rect) types.Rect {
    return rect;
}

fn gray(pixel: u8) u32 {
    return (@as(u32, pixel) << 16) | (@as(u32, pixel) << 8) | pixel;
}

fn paintPlaceholder(backend: *const types.Paint, rect: types.Rect, placeholder: Placeholder) void {
    paint_abi.priv_widget_fill_box(backend, &rect, gray(placeholder.fill), gray(placeholder.border), @intCast(@min(placeholder.border_width, std.math.maxInt(i16))));
}

/// Paint a decoded bitmap with nearest-neighbour scaling, or a framed box when
/// the bitmap is absent or invalid. All drawing goes through the shared paint
/// backend. Equal adjacent output pixels are coalesced into horizontal fills.
pub fn render(
    backend: *const types.Paint,
    rect: types.Rect,
    bitmap: ?Bitmap,
    scale: Scale,
    placeholder: Placeholder,
) void {
    const fill_rect = backend.fill_rect orelse return;
    const image = bitmap orelse {
        paintPlaceholder(backend, rect, placeholder);
        return;
    };
    const required_len = std.math.mul(usize, image.width, image.height) catch {
        paintPlaceholder(backend, rect, placeholder);
        return;
    };
    if (rect.w <= 0 or rect.h <= 0 or image.width == 0 or image.height == 0 or image.pixels.len < required_len) {
        paintPlaceholder(backend, rect, placeholder);
        return;
    }

    var target = rect;
    var crop_x: u32 = 0;
    var crop_y: u32 = 0;
    var crop_w = image.width;
    var crop_h = image.height;

    if (scale == .fit) {
        fill_rect(backend.user, rect.x, rect.y, rect.w, rect.h, gray(placeholder.fill));
        if (@as(i64, rect.w) * image.height > @as(i64, rect.h) * image.width) {
            target.w = @intCast(@max(1, @divTrunc(@as(i64, image.width) * rect.h, image.height)));
            target.x += @divTrunc(rect.w - target.w, 2);
        } else {
            target.h = @intCast(@max(1, @divTrunc(@as(i64, image.height) * rect.w, image.width)));
            target.y += @divTrunc(rect.h - target.h, 2);
        }
    } else if (@as(i64, image.width) * rect.h > @as(i64, image.height) * rect.w) {
        crop_w = @intCast(@max(1, @divTrunc(@as(i64, image.height) * rect.w, rect.h)));
        crop_x = (image.width - crop_w) / 2;
    } else {
        crop_h = @intCast(@max(1, @divTrunc(@as(i64, image.width) * rect.h, rect.w)));
        crop_y = (image.height - crop_h) / 2;
    }

    var dy: i32 = 0;
    while (dy < target.h) : (dy += 1) {
        const sy = crop_y + @as(u32, @intCast(@as(u64, @intCast(dy)) * crop_h / @as(u64, @intCast(target.h))));
        var dx: i32 = 0;
        while (dx < target.w) {
            const sx = crop_x + @as(u32, @intCast(@as(u64, @intCast(dx)) * crop_w / @as(u64, @intCast(target.w))));
            const color = gray(image.pixels[@as(usize, sy) * image.width + sx]);
            var end = dx + 1;
            while (end < target.w) : (end += 1) {
                const next_x = crop_x + @as(u32, @intCast(@as(u64, @intCast(end)) * crop_w / @as(u64, @intCast(target.w))));
                if (gray(image.pixels[@as(usize, sy) * image.width + next_x]) != color) break;
            }
            fill_rect(backend.user, target.x + dx, target.y + dy, end - dx, 1, color);
            dx = end;
        }
    }
}
