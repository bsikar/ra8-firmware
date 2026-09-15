//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");

pub const Image = struct {
    width: u16,
    height: u16,
    pixels: []u8,
    allocator: std.mem.Allocator,

    pub fn deinit(self: *Image) void {
        self.allocator.free(self.pixels);
        self.* = undefined;
    }
};

pub const Dimensions = struct { width: u16, height: u16 };

fn validateImage(image: Image) error{InvalidImage}!void {
    if (image.width == 0 or image.height == 0) return error.InvalidImage;
    const pixel_count = std.math.mul(usize, image.width, image.height) catch return error.InvalidImage;
    const expected_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidImage;
    if (image.pixels.len != expected_len) return error.InvalidImage;
}

pub fn nextDimensions(width: u16, height: u16) error{CannotDownsample}!Dimensions {
    if (width == 1 and height == 1) return error.CannotDownsample;
    return .{
        .width = @max(1, width / 2 + width % 2),
        .height = @max(1, height / 2 + height % 2),
    };
}

pub fn plan(width: u16, height: u16, levels: u8) error{TooManyLevels}![16]Dimensions {
    if (levels > 16) return error.TooManyLevels;
    var result: [16]Dimensions = undefined;
    var current = Dimensions{ .width = width, .height = height };
    for (0..levels) |index| {
        current = nextDimensions(current.width, current.height) catch return error.TooManyLevels;
        result[index] = current;
    }
    return result;
}

pub fn discardStrips(allocator: std.mem.Allocator, source: Image) !Image {
    try validateImage(source);
    const dims = try nextDimensions(source.width, source.height);
    const output_len = try std.math.mul(usize, try std.math.mul(usize, dims.width, dims.height), 3);
    const pixels = try allocator.alloc(u8, output_len);
    errdefer allocator.free(pixels);

    var dst_index: usize = 0;
    for (0..dims.height) |dst_y| {
        const src_y = dst_y * 2;
        for (0..dims.width) |dst_x| {
            const src_x = dst_x * 2;
            const src_index = (src_y * source.width + src_x) * 3;
            @memcpy(pixels[dst_index .. dst_index + 3], source.pixels[src_index .. src_index + 3]);
            dst_index += 3;
        }
    }
    return .{ .width = dims.width, .height = dims.height, .pixels = pixels, .allocator = allocator };
}
