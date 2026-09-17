//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");

/// An RGB image, three bytes per pixel, owning its pixel buffer.
pub const Image = struct {
    width: u16,
    height: u16,
    pixels: []u8,
    allocator: std.mem.Allocator,

    /// Frees the pixel buffer and poisons the image.
    pub fn deinit(self: *Image) void {
        self.allocator.free(self.pixels);
        self.* = undefined;
    }
};

/// A pixel size in the pyramid.
pub const Dimensions = struct { width: u16, height: u16 };

/// Which axis a downsampling step halves.
pub const Axis = enum { columns, rows };

fn validateImage(image: Image) error{InvalidImage}!void {
    if (image.width == 0 or image.height == 0) return error.InvalidImage;
    const pixel_count = std.math.mul(usize, image.width, image.height) catch return error.InvalidImage;
    const expected_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidImage;
    if (image.pixels.len != expected_len) return error.InvalidImage;
}

/// Returns the dimensions after halving `axis`, rounding up.
///
/// Returns `error.CannotDownsample` when the chosen axis is already one pixel.
pub fn nextDimensions(width: u16, height: u16, axis: Axis) error{CannotDownsample}!Dimensions {
    return switch (axis) {
        .columns => if (width == 1) error.CannotDownsample else .{
            .width = width / 2 + width % 2,
            .height = height,
        },
        .rows => if (height == 1) error.CannotDownsample else .{
            .width = width,
            .height = height / 2 + height % 2,
        },
    };
}

/// Returns the axis level `level` halves: columns on odd levels, rows on even.
///
/// Alternating keeps the aspect ratio close to the source rather than squashing
/// one axis away first.
pub fn axisForLevel(level: usize) Axis {
    return if (level % 2 == 1) .columns else .rows;
}

/// Computes the dimensions of every pyramid level up to `levels`, alternating axes.
///
/// Returns a fixed 16-entry array of which only the first `levels` are meaningful.
/// Returns `error.TooManyLevels` when `levels` exceeds 16 or when an axis runs out
/// of pixels before the last level.
pub fn plan(width: u16, height: u16, levels: u8) error{TooManyLevels}![16]Dimensions {
    if (levels > 16) return error.TooManyLevels;
    var result: [16]Dimensions = undefined;
    var current = Dimensions{ .width = width, .height = height };
    for (0..levels) |index| {
        current = nextDimensions(current.width, current.height, axisForLevel(index + 1)) catch return error.TooManyLevels;
        result[index] = current;
    }
    return result;
}

/// Halves `source` along `axis` by keeping every other column or row.
///
/// Point sampling, not averaging: the device-side degradation this mirrors drops
/// strips rather than filtering, so the host tool has to produce the same pixels.
/// The returned image owns a fresh buffer from `allocator`.
pub fn discardStrips(allocator: std.mem.Allocator, source: Image, axis: Axis) !Image {
    try validateImage(source);
    const dims = try nextDimensions(source.width, source.height, axis);
    const output_len = try std.math.mul(usize, try std.math.mul(usize, dims.width, dims.height), 3);
    const pixels = try allocator.alloc(u8, output_len);
    errdefer allocator.free(pixels);

    var dst_index: usize = 0;
    for (0..dims.height) |dst_y| {
        const src_y = if (axis == .rows) dst_y * 2 else dst_y;
        for (0..dims.width) |dst_x| {
            const src_x = if (axis == .columns) dst_x * 2 else dst_x;
            const src_index = (src_y * source.width + src_x) * 3;
            @memcpy(pixels[dst_index .. dst_index + 3], source.pixels[src_index .. src_index + 3]);
            dst_index += 3;
        }
    }
    return .{ .width = dims.width, .height = dims.height, .pixels = pixels, .allocator = allocator };
}
