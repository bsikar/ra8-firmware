//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Desktop RGBA framebuffer backend exposed via `ra8_widget_host` (RA8FW-1016):
//! pairs `gfx.Surface` (argb8888 software rasterizer with alpha, lines, blits,
//! text, and clips) with `host_surface_paint` and byte/stride accessors for
//! SDL3 texture presentation (`SDL_PIXELFORMAT_ARGB8888` or `RGBA8888`).

const std = @import("std");
const gfx = @import("ra8_gfx");
const surface_paint = @import("host_surface_paint.zig");

/// Desktop RGBA render target owning a `gfx.Surface` and Paint binding.
pub const DesktopSurface = struct {
    surface: gfx.Surface,

    /// Allocate a `width x height` desktop RGBA surface cleared to `background`.
    pub fn init(allocator: std.mem.Allocator, width: u32, height: u32, background: u32) !DesktopSurface {
        return .{ .surface = try gfx.Surface.init(allocator, width, height, background) };
    }

    pub fn deinit(self: *DesktopSurface, allocator: std.mem.Allocator) void {
        self.surface.deinit(allocator);
        self.* = undefined;
    }

    /// Build a `Paint` table of type `Paint` bound to this desktop surface.
    pub fn paint(self: *DesktopSurface, comptime Paint: type) Paint {
        return surface_paint.table(Paint, &self.surface);
    }

    /// Declared desktop capabilities (`argb8888`, colour, alpha, line, blit, clip).
    pub fn capabilities(self: *const DesktopSurface) gfx.Capabilities {
        return self.surface.capabilities();
    }

    /// Native pixel format (`argb8888`).
    pub fn pixelFormat(self: *const DesktopSurface) gfx.PixelFormat {
        return self.surface.pixelFormat();
    }

    /// Read pixel (`x`, `y`) as 0xFFRRGGBB (`argb8888`).
    pub fn at(self: *const DesktopSurface, x: u32, y: u32) u32 {
        return self.surface.at(x, y);
    }

    /// Row stride in bytes for `SDL_UpdateTexture` (`width * 4`).
    pub fn pitchBytes(self: *const DesktopSurface) usize {
        return @as(usize, self.surface.width) * @sizeOf(u32);
    }

    /// Raw packed `argb8888` pixel buffer as host-endian bytes.
    pub fn rgbaBytes(self: *const DesktopSurface) []const u8 {
        return std.mem.sliceAsBytes(self.surface.pixels);
    }

    /// Copy pixels into `dst_rgba` as big-endian `RGBA8888` (`R, G, B, A` bytes).
    pub fn writeRgbaBytes(self: *const DesktopSurface, dst_rgba: []u8) error{BufferTooSmall}!void {
        const needed = @as(usize, self.surface.pixels.len) * 4;
        if (dst_rgba.len < needed) return error.BufferTooSmall;
        for (self.surface.pixels, 0..) |px, i| {
            const out = dst_rgba[i * 4 ..][0..4];
            out.* = .{
                @truncate(px >> 16),
                @truncate(px >> 8),
                @truncate(px),
                @truncate(px >> 24),
            };
        }
    }

    /// Encode this surface as a binary P6 PPM image for test goldens and inspection.
    pub fn ppm(self: *const DesktopSurface, allocator: std.mem.Allocator) ![]u8 {
        return self.surface.ppm(allocator);
    }
};
