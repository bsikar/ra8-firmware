//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host RGBA backend (RA8FW-950): an argb8888 surface that desktop GUI apps
//! draw widgets into, with solid and blended fills, lines, glyph coverage,
//! unscaled blits and a clip stack. SDL or any other window only presents
//! its pixels. Colours come in as 0xRRGGBB the way the widget Paint table
//! passes them; every stored pixel is opaque (alpha byte 0xFF), and a
//! blended draw composites source-over onto what is already there.

const std = @import("std");
const blit_formats = @import("surface_blit.zig");

pub const Format = blit_formats.Format;

/// A rectangle in surface pixels.
pub const Rect = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,

    /// The overlap of two rectangles; zero-sized when they do not meet.
    pub fn intersect(a: Rect, b: Rect) Rect {
        const x0 = @max(a.x, b.x);
        const y0 = @max(a.y, b.y);
        const x1 = @min(a.x +| a.w, b.x +| b.w);
        const y1 = @min(a.y +| a.h, b.y +| b.h);
        return .{ .x = x0, .y = y0, .w = @max(x1 - x0, 0), .h = @max(y1 - y0, 0) };
    }

    pub fn contains(self: Rect, x: i32, y: i32) bool {
        return x >= self.x and y >= self.y and x < self.x +| self.w and y < self.y +| self.h;
    }
};

/// How deep pushClip nests before further pushes only narrow the top.
pub const clip_depth = 16;

const opaque_alpha: u32 = 0xFF00_0000;

pub const Surface = struct {
    width: u32,
    height: u32,
    /// Row-major argb8888, `width * height` long.
    pixels: []u32,
    clips: [clip_depth]Rect = undefined,
    depth: u8 = 0,
    /// Pushes past clip_depth, so the matching pops stay balanced.
    overflow: u32 = 0,

    pub fn init(allocator: std.mem.Allocator, width: u32, height: u32, background: u32) !Surface {
        if (width == 0 or height == 0) return error.InvalidDimensions;
        const pixels = try allocator.alloc(u32, try std.math.mul(usize, width, height));
        @memset(pixels, opaque_alpha | (background & 0xFF_FFFF));
        return .{ .width = width, .height = height, .pixels = pixels };
    }

    pub fn deinit(self: *Surface, allocator: std.mem.Allocator) void {
        allocator.free(self.pixels);
        self.* = undefined;
    }

    /// The whole surface.
    pub fn bounds(self: *const Surface) Rect {
        return .{ .x = 0, .y = 0, .w = @intCast(self.width), .h = @intCast(self.height) };
    }

    /// Where draws land now: the innermost clip, inside the surface.
    pub fn clip(self: *const Surface) Rect {
        return if (self.depth == 0) self.bounds() else self.clips[self.depth - 1];
    }

    /// Clip later draws to `rect` inside the current clip.
    pub fn pushClip(self: *Surface, rect: Rect) void {
        const next = self.clip().intersect(rect);
        if (self.depth == clip_depth) {
            self.overflow += 1;
            self.clips[clip_depth - 1] = next;
            return;
        }
        self.clips[self.depth] = next;
        self.depth += 1;
    }

    /// Restore the clip from before the matching pushClip. An overflowed
    /// push leaves the narrowed top in place.
    pub fn popClip(self: *Surface) void {
        if (self.overflow > 0) {
            self.overflow -= 1;
            return;
        }
        if (self.depth > 0) self.depth -= 1;
    }

    /// The pixel at (`x`, `y`) as argb8888.
    pub fn at(self: *const Surface, x: u32, y: u32) u32 {
        return self.pixels[@as(usize, y) * self.width + x];
    }

    fn index(self: *const Surface, x: i32, y: i32) usize {
        return @as(usize, @intCast(y)) * self.width + @as(usize, @intCast(x));
    }

    /// Paint one pixel solid, if it is inside the clip.
    pub fn plot(self: *Surface, x: i32, y: i32, color: u32) void {
        if (!self.clip().contains(x, y)) return;
        self.pixels[self.index(x, y)] = opaque_alpha | (color & 0xFF_FFFF);
    }

    /// Composite `color` at `alpha` (0 to 255) over one pixel, if inside
    /// the clip. Glyph coverage goes through here.
    pub fn blend(self: *Surface, x: i32, y: i32, color: u32, alpha: u8) void {
        if (alpha == 0 or !self.clip().contains(x, y)) return;
        const slot = &self.pixels[self.index(x, y)];
        slot.* = over(slot.*, color, alpha);
    }

    /// Fill `rect` solid inside the clip.
    pub fn fill(self: *Surface, rect: Rect, color: u32) void {
        const area = self.clip().intersect(rect);
        if (area.w == 0 or area.h == 0) return;
        const value = opaque_alpha | (color & 0xFF_FFFF);
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            const start = self.index(area.x, y);
            @memset(self.pixels[start .. start + @as(usize, @intCast(area.w))], value);
        }
    }

    /// Fill `rect` with `color` at `alpha`, source-over, inside the clip.
    pub fn fillAlpha(self: *Surface, rect: Rect, color: u32, alpha: u8) void {
        if (alpha == 0xFF) return self.fill(rect, color);
        if (alpha == 0) return;
        const area = self.clip().intersect(rect);
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            const start = self.index(area.x, y);
            for (self.pixels[start .. start + @as(usize, @intCast(area.w))]) |*slot| slot.* = over(slot.*, color, alpha);
        }
    }

    /// A one-pixel line from (`x0`, `y0`) to (`x1`, `y1`), both ends drawn.
    pub fn line(self: *Surface, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) void {
        const dx: i64 = @intCast(@abs(@as(i64, x1) - x0));
        const dy: i64 = -@as(i64, @intCast(@abs(@as(i64, y1) - y0)));
        const sx: i32 = if (x0 < x1) 1 else -1;
        const sy: i32 = if (y0 < y1) 1 else -1;
        var err = dx + dy;
        var x = x0;
        var y = y0;
        while (true) {
            self.plot(x, y, color);
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

    /// Copy `rect`'s worth of `format` pixels, rows `stride` bytes apart,
    /// to `rect` unscaled. argb8888 sources composite by their own alpha;
    /// the others are opaque.
    pub fn blit(self: *Surface, rect: Rect, pixels: []const u8, stride: usize, format: Format) void {
        const area = self.clip().intersect(rect);
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            var x = area.x;
            while (x < area.x + area.w) : (x += 1) {
                const sx: usize = @intCast(x - rect.x);
                const sy: usize = @intCast(y - rect.y);
                const source = blit_formats.read(pixels, sy * stride + sx * format.bytes(), format) orelse continue;
                const slot = &self.pixels[self.index(x, y)];
                slot.* = over(slot.*, source & 0xFF_FFFF, @truncate(source >> 24));
            }
        }
    }

    /// The surface as a binary PPM (P6), alpha dropped, for goldens.
    pub fn ppm(self: *const Surface, allocator: std.mem.Allocator) ![]u8 {
        var header_buffer: [32]u8 = undefined;
        const header = try std.fmt.bufPrint(&header_buffer, "P6\n{d} {d}\n255\n", .{ self.width, self.height });
        const result = try allocator.alloc(u8, header.len + self.pixels.len * 3);
        @memcpy(result[0..header.len], header);
        for (self.pixels, 0..) |pixel, i| {
            const out = result[header.len + i * 3 ..][0..3];
            out.* = .{ @truncate(pixel >> 16), @truncate(pixel >> 8), @truncate(pixel) };
        }
        return result;
    }
};

/// `color` at `alpha` composited source-over onto opaque `under`.
pub fn over(under: u32, color: u32, alpha: u8) u32 {
    if (alpha == 0xFF) return opaque_alpha | (color & 0xFF_FFFF);
    const a: u32 = alpha;
    var result: u32 = opaque_alpha;
    inline for (.{ 16, 8, 0 }) |shift| {
        const top = (color >> shift) & 0xFF;
        const bottom = (under >> shift) & 0xFF;
        const mixed = (top * a + bottom * (255 - a) + 127) / 255;
        result |= mixed << shift;
    }
    return result;
}
