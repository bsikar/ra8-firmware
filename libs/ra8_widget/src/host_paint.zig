//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CPU paint backend for host screen previews. Pixels are stored as one byte
//! per pixel; PPM output expands each grey sample to RGB for broad viewer support.

const std = @import("std");
const text = @import("text");

pub const Canvas = struct {
    width: usize,
    height: usize,
    pixels: []u8,

    pub fn init(allocator: std.mem.Allocator, width: usize, height: usize, background: u8) !Canvas {
        if (width == 0 or height == 0) return error.InvalidDimensions;
        const pixels = try allocator.alloc(u8, try std.math.mul(usize, width, height));
        @memset(pixels, background);
        return .{ .width = width, .height = height, .pixels = pixels };
    }

    pub fn deinit(self: *Canvas, allocator: std.mem.Allocator) void {
        allocator.free(self.pixels);
        self.* = undefined;
    }

    pub fn ppm(self: Canvas, allocator: std.mem.Allocator) ![]u8 {
        const header = try std.fmt.allocPrint(allocator, "P6\n{d} {d}\n255\n", .{ self.width, self.height });
        defer allocator.free(header);
        const rgb_len = try std.math.mul(usize, self.pixels.len, 3);
        const result = try allocator.alloc(u8, header.len + rgb_len);
        @memcpy(result[0..header.len], header);
        var i: usize = 0;
        while (i < self.pixels.len) : (i += 1) {
            const offset = header.len + i * 3;
            result[offset] = self.pixels[i];
            result[offset + 1] = self.pixels[i];
            result[offset + 2] = self.pixels[i];
        }
        return result;
    }

    fn shade(color: u32) u8 {
        const r: u32 = (color >> 16) & 0xff;
        const g: u32 = (color >> 8) & 0xff;
        const b: u32 = color & 0xff;
        return @intCast((r * 77 + g * 150 + b * 29) >> 8);
    }

    fn set(self: *Canvas, x: i32, y: i32, value: u8) void {
        if (x < 0 or y < 0) return;
        const px: usize = @intCast(x);
        const py: usize = @intCast(y);
        if (px >= self.width or py >= self.height) return;
        self.pixels[py * self.width + px] = value;
    }

    pub fn fillRect(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        const self: *Canvas = @ptrCast(@alignCast(user orelse return));
        if (w <= 0 or h <= 0) return;
        const x0 = @max(x, 0);
        const y0 = @max(y, 0);
        const x1 = @min(x +| w, @as(i32, @intCast(self.width)));
        const y1 = @min(y +| h, @as(i32, @intCast(self.height)));
        if (x0 >= x1 or y0 >= y1) return;
        const value = shade(color);
        var row: i32 = y0;
        while (row < y1) : (row += 1) {
            const start: usize = @intCast(row * @as(i32, @intCast(self.width)) + x0);
            @memset(self.pixels[start .. start + @as(usize, @intCast(x1 - x0))], value);
        }
    }

    fn putPixel(user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void {
        const self: *Canvas = @ptrCast(@alignCast(user orelse return));
        self.set(x, y, shade(color));
    }

    pub fn drawText(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void {
        text.drawSans(str, x, y, fg, bg, user, putPixel);
    }

    pub fn drawTextFace(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, face: u8, fg: u32, bg: u32) callconv(.c) void {
        const family: text.Face = if (face == 1) .serif else .sans;
        if (family == .serif) text.drawSerif(str, x, y, fg, bg, user, putPixel) else text.drawSans(str, x, y, fg, bg, user, putPixel);
    }

    pub fn drawTextStyle(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, face: u8, weight: u8, size: u8, fg: u32, bg: u32) callconv(.c) void {
        const family: text.Face = if (face == 1) .serif else .sans;
        const stroke: text.Weight = if (weight == 1) .bold else .regular;
        text.drawScaledWeight(str, x, y, family, stroke, size, fg, bg, user, putPixel);
    }

    pub fn textSizeFace(_: ?*anyopaque, str: [*:0]const u8, face: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        const extent = text.measure(str, if (face == 1) .serif else .sans);
        out_w.* = @intCast(extent.width);
        out_h.* = @intCast(extent.height);
    }

    pub fn textSizeStyle(_: ?*anyopaque, str: [*:0]const u8, face: u8, weight: u8, size: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        const extent = text.measureScaledWeight(str, if (face == 1) .serif else .sans, if (weight == 1) .bold else .regular, size);
        out_w.* = @intCast(extent.width);
        out_h.* = @intCast(extent.height);
    }

    pub fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        const extent = text.measure(str, .sans);
        out_w.* = @intCast(extent.width);
        out_h.* = @intCast(extent.height);
    }
};
