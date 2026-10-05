//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CPU paint backend for host screen previews. Pixels are stored as one byte
//! per pixel; PPM output expands each grey sample to RGB for broad viewer support.

const std = @import("std");

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

    fn glyph(ch: u8) [5]u8 {
        const upper = std.ascii.toUpper(ch);
        return switch (upper) {
            65 => .{ 14, 17, 17, 31, 17 },
            66 => .{ 30, 17, 30, 17, 30 },
            67 => .{ 14, 17, 16, 17, 14 },
            68 => .{ 30, 17, 17, 17, 30 },
            69 => .{ 31, 16, 30, 16, 31 },
            70 => .{ 31, 16, 30, 16, 16 },
            71 => .{ 14, 17, 23, 17, 15 },
            72 => .{ 17, 17, 31, 17, 17 },
            73 => .{ 14, 4, 4, 4, 14 },
            74 => .{ 7, 2, 2, 18, 12 },
            75 => .{ 17, 18, 28, 18, 17 },
            76 => .{ 16, 16, 16, 16, 31 },
            77 => .{ 17, 27, 21, 17, 17 },
            78 => .{ 17, 25, 21, 19, 17 },
            79 => .{ 14, 17, 17, 17, 14 },
            80 => .{ 30, 17, 30, 16, 16 },
            81 => .{ 14, 17, 17, 19, 15 },
            82 => .{ 30, 17, 30, 18, 17 },
            83 => .{ 15, 16, 14, 1, 30 },
            84 => .{ 31, 4, 4, 4, 4 },
            85 => .{ 17, 17, 17, 17, 14 },
            86 => .{ 17, 17, 17, 10, 4 },
            87 => .{ 17, 17, 21, 27, 17 },
            88 => .{ 17, 10, 4, 10, 17 },
            89 => .{ 17, 10, 4, 4, 4 },
            90 => .{ 31, 2, 4, 8, 31 },
            48 => .{ 14, 17, 19, 17, 14 },
            49 => .{ 4, 12, 4, 4, 14 },
            50 => .{ 14, 17, 2, 4, 31 },
            51 => .{ 30, 1, 14, 1, 30 },
            52 => .{ 18, 18, 31, 2, 2 },
            53 => .{ 31, 16, 30, 1, 30 },
            54 => .{ 14, 16, 30, 17, 14 },
            55 => .{ 31, 1, 2, 4, 4 },
            56 => .{ 14, 17, 14, 17, 14 },
            57 => .{ 14, 17, 15, 1, 14 },
            45 => .{ 0, 0, 31, 0, 0 },
            46 => .{ 0, 0, 0, 0, 4 },
            else => .{ 0, 0, 0, 0, 0 },
        };
    }
    pub fn drawText(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, fg: u32, _: u32) callconv(.c) void {
        const self: *Canvas = @ptrCast(@alignCast(user orelse return));
        const text = std.mem.span(str);
        const value = shade(fg);
        for (text, 0..) |ch, index| {
            if (ch == 32) continue;
            const columns = glyph(ch);
            for (columns, 0..) |row, cy| {
                for (0..5) |cx| {
                    if ((row & (@as(u8, 1) << @intCast(4 - cx))) != 0) {
                        self.set(x + @as(i32, @intCast(index * 6 + cx)), y + @as(i32, @intCast(cy)), value);
                    }
                }
            }
        }
    }

    pub fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        out_w.* = @intCast(std.mem.span(str).len * 6);
        out_h.* = 5;
    }
};
