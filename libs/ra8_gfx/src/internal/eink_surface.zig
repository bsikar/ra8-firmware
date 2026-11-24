//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Chip and host e-ink panel framebuffer backend (RA8FW-1012): packed 4 bpp
//! gray4 pixels (two pixels per byte, high nibble first) with blue-noise
//! ordered dithering and optional per-panel tone calibration over caller-owned
//! memory. Performs zero heap allocations so it builds for cortex-m85
//! freestanding firmware and shares the same canvas interface as `Surface`.

const std = @import("std");
const surface = @import("surface.zig");
const blit_formats = @import("surface_blit.zig");
const dither = @import("dither.zig");
const tone = @import("tone.zig");
const text = @import("text.zig");

pub const Error = error{
    InvalidDimensions,
    InvalidStride,
    BufferTooSmall,
};

pub const Rect = surface.Rect;
pub const Capabilities = surface.Capabilities;
pub const PixelFormat = surface.PixelFormat;
pub const Format = surface.Format;

const nibble_white: u8 = 0x0F;
const opaque_alpha: u32 = 0xFF00_0000;

/// Packed 4 bpp row stride in bytes for `width` pixels.
pub fn packedRowBytes(width: u32) u32 {
    return (width + dither.dither.ppb - 1) / dither.dither.ppb;
}

/// Rec.601 luminance (0..255) for a 0xRRGGBB / 0xAARRGGBB colour.
pub fn colorLuma(color: u32) u8 {
    const r: u32 = (color >> 16) & 0xFF;
    const g: u32 = (color >> 8) & 0xFF;
    const b: u32 = color & 0xFF;
    return @truncate(((77 * r) + (150 * g) + (29 * b)) >> 8);
}

/// Caller-backed packed gray4 e-ink framebuffer with blue-noise dithering.
pub const EinkSurface = struct {
    width: u32,
    height: u32,
    stride_bytes: u32,
    pixels: []u8,
    tone_map: ?*const tone.Map = null,
    clip_stack: surface.ClipStack = .{},

    /// Bind a caller-owned packed gray4 framebuffer and clear to `background`.
    pub fn init(
        pixels: []u8,
        width: u32,
        height: u32,
        stride_bytes: u32,
        background: u32,
        tone_map: ?*const tone.Map,
    ) Error!EinkSurface {
        if (width == 0 or height == 0) return error.InvalidDimensions;
        const min_stride = packedRowBytes(width);
        const actual_stride = if (stride_bytes == 0) min_stride else stride_bytes;
        if (actual_stride < min_stride) return error.InvalidStride;
        const needed = std.math.mul(usize, actual_stride, height) catch return error.BufferTooSmall;
        if (pixels.len < needed) return error.BufferTooSmall;
        var out = EinkSurface{
            .width = width,
            .height = height,
            .stride_bytes = actual_stride,
            .pixels = pixels,
            .tone_map = tone_map,
        };
        out.fill(out.bounds(), background);
        return out;
    }

    pub fn bounds(self: *const EinkSurface) Rect {
        return .{ .x = 0, .y = 0, .w = @intCast(self.width), .h = @intCast(self.height) };
    }

    pub fn clip(self: *const EinkSurface) Rect {
        return self.clip_stack.current(self.bounds());
    }

    pub fn pushClip(self: *EinkSurface, rect: Rect) void {
        self.clip_stack.push(self.bounds(), rect);
    }

    pub fn popClip(self: *EinkSurface) void {
        self.clip_stack.pop();
    }

    /// Capabilities declared by this e-ink panel backend: gray4 + blue-noise dithering.
    pub fn capabilities(_: *const EinkSurface) Capabilities {
        return Capabilities.eink_gray4;
    }

    /// Native pixel storage format (packed 4 bpp gray4).
    pub fn pixelFormat(self: *const EinkSurface) PixelFormat {
        return self.capabilities().pixel_format;
    }

    fn writeNibble(self: *EinkSurface, x: u32, y: u32, nibble: u4) void {
        const byte_idx = @as(usize, y) * self.stride_bytes + (x / dither.dither.ppb);
        const level: u8 = nibble;
        if ((x & 1) == 0) {
            self.pixels[byte_idx] = (self.pixels[byte_idx] & 0x0F) | (level << dither.dither.nib_shift);
        } else {
            self.pixels[byte_idx] = (self.pixels[byte_idx] & 0xF0) | level;
        }
    }

    /// Read the stored 4-bit gray4 level (0..15) at (`x`, `y`).
    pub fn nibbleAt(self: *const EinkSurface, x: u32, y: u32) u4 {
        const byte_idx = @as(usize, y) * self.stride_bytes + (x / dither.dither.ppb);
        const packed_byte = self.pixels[byte_idx];
        return @truncate(if ((x & 1) == 0) packed_byte >> dither.dither.nib_shift else packed_byte);
    }

    /// Expand the stored 4-bit level at (`x`, `y`) to 8-bit luminance.
    pub fn gray8At(self: *const EinkSurface, x: u32, y: u32) u8 {
        const n: u8 = self.nibbleAt(x, y);
        return (n << dither.dither.nib_shift) | n;
    }

    /// Read (`x`, `y`) expanded to 0xFFGGGGGG for host comparison checks.
    pub fn at(self: *const EinkSurface, x: u32, y: u32) u32 {
        return opaque_alpha | (@as(u32, self.gray8At(x, y)) * 0x01_0101);
    }

    /// Dither and store an 8-bit greyscale sample at (`x`, `y`), respecting the clip stack.
    pub fn plotGray8(self: *EinkSurface, x: i32, y: i32, gray8: u8) void {
        if (!self.clip().contains(x, y)) return;
        const thr = dither.thresholdAt(x, y);
        const level: u4 = @intCast(dither.quantiseAny(self.tone_map, gray8, thr));
        self.writeNibble(@intCast(x), @intCast(y), level);
    }

    /// Convert `color` (0xRRGGBB) to Rec.601 luma, blue-noise dither, and store if clipped in.
    pub fn plot(self: *EinkSurface, x: i32, y: i32, color: u32) void {
        self.plotGray8(x, y, colorLuma(color));
    }

    /// Source-over composite `color` at `alpha` over current panel luminance and dither.
    pub fn blend(self: *EinkSurface, x: i32, y: i32, color: u32, alpha: u8) void {
        if (alpha == 0 or !self.clip().contains(x, y)) return;
        if (alpha == 0xFF) return self.plot(x, y, color);
        const px: u32 = @intCast(x);
        const py: u32 = @intCast(y);
        const top: u32 = colorLuma(color);
        const bottom: u32 = self.gray8At(px, py);
        const a: u32 = alpha;
        const mixed: u8 = @truncate((top * a + bottom * (255 - a) + 127) / 255);
        const thr = dither.thresholdAt(x, y);
        const level: u4 = @intCast(dither.quantiseAny(self.tone_map, mixed, thr));
        self.writeNibble(px, py, level);
    }

    /// Fill `rect` inside the current clip using blue-noise ordered dithering.
    pub fn fill(self: *EinkSurface, rect: Rect, color: u32) void {
        const area = self.clip().intersect(rect);
        if (area.w == 0 or area.h == 0) return;
        const luma = colorLuma(color);
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            var x = area.x;
            while (x < area.x + area.w) : (x += 1) {
                const thr = dither.thresholdAt(x, y);
                const level: u4 = @intCast(dither.quantiseAny(self.tone_map, luma, thr));
                self.writeNibble(@intCast(x), @intCast(y), level);
            }
        }
    }

    /// Fill `rect` at `alpha`; opaque alpha skips background reads and solid-dithers directly.
    pub fn fillAlpha(self: *EinkSurface, rect: Rect, color: u32, alpha: u8) void {
        if (alpha == 0xFF) return self.fill(rect, color);
        if (alpha == 0) return;
        const area = self.clip().intersect(rect);
        if (area.w == 0 or area.h == 0) return;
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            var x = area.x;
            while (x < area.x + area.w) : (x += 1) {
                self.blend(x, y, color, alpha);
            }
        }
    }

    /// One-pixel dithered line from (`x0`, `y0`) to (`x1`, `y1`).
    pub fn line(self: *EinkSurface, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) void {
        surface.walkLine(self, EinkSurface.plot, x0, y0, x1, y1, color);
    }

    /// Blit packed gray4 (high nibble first) source pixels into `rect`, clipped.
    pub fn blitGray4(self: *EinkSurface, rect: Rect, src_pixels: []const u8, src_stride: usize) void {
        const area = self.clip().intersect(rect);
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            var x = area.x;
            while (x < area.x + area.w) : (x += 1) {
                const sx: usize = @intCast(x - rect.x);
                const sy: usize = @intCast(y - rect.y);
                const idx = sy * src_stride + (sx / dither.dither.ppb);
                if (idx >= src_pixels.len) continue;
                const packed_byte = src_pixels[idx];
                const nibble: u4 = @truncate(if ((sx & 1) == 0) packed_byte >> dither.dither.nib_shift else packed_byte);
                self.writeNibble(@intCast(x), @intCast(y), nibble);
            }
        }
    }

    /// Copy `rect`'s worth of `format` pixels, rows `stride` bytes apart, dithering into gray4.
    pub fn blit(self: *EinkSurface, rect: Rect, src_pixels: []const u8, stride: usize, format: Format) void {
        const area = self.clip().intersect(rect);
        var y = area.y;
        while (y < area.y + area.h) : (y += 1) {
            var x = area.x;
            while (x < area.x + area.w) : (x += 1) {
                const sx: usize = @intCast(x - rect.x);
                const sy: usize = @intCast(y - rect.y);
                const source = blit_formats.read(src_pixels, sy * stride + sx * format.bytes(), format) orelse continue;
                self.blend(x, y, source & 0xFF_FFFF, @truncate(source >> 24));
            }
        }
    }

    fn plotCallback(user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void {
        const self: *EinkSurface = @ptrCast(@alignCast(user orelse return));
        self.plot(x, y, color);
    }

    /// Draw a sans 8x16 string dithered into the active clip stack.
    pub fn drawText(self: *EinkSurface, str: [*:0]const u8, x: i32, y: i32, fg: u32, bg: u32) void {
        text.drawSans(str, x, y, fg, bg, @ptrCast(self), plotCallback);
    }

    /// Draw text in `family` dithered into the active clip stack.
    pub fn drawTextFace(self: *EinkSurface, str: [*:0]const u8, x: i32, y: i32, family: text.Face, fg: u32, bg: u32) void {
        if (family == .serif) {
            text.drawSerif(str, x, y, fg, bg, @ptrCast(self), plotCallback);
        } else {
            text.drawSans(str, x, y, fg, bg, @ptrCast(self), plotCallback);
        }
    }

    /// Draw text with full face, weight, and size selection, dithered into the active clip.
    pub fn drawTextStyle(
        self: *EinkSurface,
        str: [*:0]const u8,
        x: i32,
        y: i32,
        family: text.Face,
        stroke: text.Weight,
        size: u8,
        fg: u32,
        bg: u32,
    ) void {
        text.drawStyle(str, x, y, family, stroke, size, fg, bg, @ptrCast(self), plotCallback);
    }

    pub fn measureText(str: [*:0]const u8, family: text.Face) text.Extent {
        return text.measure(str, family);
    }

    pub fn measureTextWeight(str: [*:0]const u8, family: text.Face, stroke: text.Weight) text.Extent {
        return text.measureWeight(str, family, stroke);
    }

    pub fn measureTextStyle(str: [*:0]const u8, family: text.Face, stroke: text.Weight, size: u8) text.Extent {
        return text.measureStyle(str, family, stroke, size);
    }

    /// Stream packed 4 bpp rows for `rect` to an IT8951-compatible panel row loader.
    /// Returns false if `rect` falls outside the panel bounds, `scratch` cannot hold
    /// one row when `rect.x` or `rect.w` requires repacking, or `write_row` declines.
    pub fn flushPackedRows(
        self: *const EinkSurface,
        rect: Rect,
        scratch: []u8,
        user: ?*anyopaque,
        write_row: *const fn (user: ?*anyopaque, x: u16, y: u16, w: u16, packed_bytes: []const u8) bool,
    ) bool {
        if (rect.w <= 0 or rect.h <= 0) return true;
        if (rect.x < 0 or rect.y < 0) return false;
        const x0: u32 = @intCast(rect.x);
        const y0: u32 = @intCast(rect.y);
        const rw: u32 = @intCast(rect.w);
        const rh: u32 = @intCast(rect.h);
        if (x0 +| rw > self.width or y0 +| rh > self.height) return false;

        const row_bytes = packedRowBytes(rw);
        const aligned_even = (x0 & 1) == 0 and (rw & 1) == 0;
        if (!aligned_even and scratch.len < row_bytes) return false;

        var r: u32 = 0;
        while (r < rh) : (r += 1) {
            const py = y0 + r;
            const packed_row = if (aligned_even) blk: {
                const start = @as(usize, py) * self.stride_bytes + (x0 / dither.dither.ppb);
                break :blk self.pixels[start .. start + row_bytes];
            } else blk: {
                var col: u32 = 0;
                while (col < rw) : (col += dither.dither.ppb) {
                    const hi: u8 = self.nibbleAt(x0 + col, py);
                    const lo: u8 = if (col + 1 < rw) self.nibbleAt(x0 + col + 1, py) else nibble_white;
                    scratch[col / dither.dither.ppb] = (hi << dither.dither.nib_shift) | lo;
                }
                break :blk scratch[0..row_bytes];
            };
            if (!write_row(user, @intCast(x0), @intCast(py), @intCast(rw), packed_row)) return false;
        }
        return true;
    }
};
