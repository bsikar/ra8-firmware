//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const degrade = @import("degrade.zig");

// The ra8_jpeg software implementations currently use shared static working
// state, despite stale thread-safe declarations in the public header. This
// single-threaded application never calls the codec concurrently. See #893.
const c = @cImport({
    @cInclude("stdbool.h");
    @cInclude("ra8_jpeg_sw.h");
});

pub const CodecFailure = struct { code: u32 };
pub const DimensionsResult = union(enum) { value: degrade.Dimensions, failure: CodecFailure };
pub const ImageResult = union(enum) { value: degrade.Image, failure: CodecFailure };
pub const BytesResult = union(enum) { value: []u8, failure: CodecFailure };

fn checkedInputLength(length: usize) error{InputTooLarge}!u32 {
    return std.math.cast(u32, length) orelse error.InputTooLarge;
}

pub fn validateDecodedOutput(expected: degrade.Dimensions, width: u16, height: u16, output_len: usize) error{InvalidCodecOutput}!void {
    const pixel_count = std.math.mul(usize, width, height) catch return error.InvalidCodecOutput;
    const actual_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidCodecOutput;
    if (width == 0 or height == 0 or width != expected.width or height != expected.height or actual_len != output_len) {
        return error.InvalidCodecOutput;
    }
}

pub fn checkedDecodedLength(dims: degrade.Dimensions) error{InvalidCodecOutput}!usize {
    if (dims.width == 0 or dims.height == 0) return error.InvalidCodecOutput;
    const pixel_count = std.math.mul(usize, dims.width, dims.height) catch return error.InvalidCodecOutput;
    const output_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidCodecOutput;
    if (output_len == 0) return error.InvalidCodecOutput;
    return output_len;
}

pub fn checkedEncodedLength(capacity: usize, output_len: u32) error{InvalidCodecOutput}!usize {
    if (output_len == 0 or output_len > capacity) return error.InvalidCodecOutput;
    return output_len;
}

pub fn dimensions(bytes: []const u8) !DimensionsResult {
    var width: u16 = 0;
    var height: u16 = 0;
    const rc = c.ra8_jpeg_sw_get_dimensions(bytes.ptr, try checkedInputLength(bytes.len), &width, &height);
    if (rc != c.k_ra8_ok) return .{ .failure = .{ .code = @intCast(rc) } };
    return .{ .value = .{ .width = width, .height = height } };
}

pub fn decode(allocator: std.mem.Allocator, bytes: []const u8) !ImageResult {
    const input_len = try checkedInputLength(bytes.len);
    const dims = switch (try dimensions(bytes)) {
        .value => |value| value,
        .failure => |failure| return .{ .failure = failure },
    };
    const output_len = try checkedDecodedLength(dims);
    const c_output_len = std.math.cast(u32, output_len) orelse return error.ImageTooLarge;
    const pixels = try allocator.alloc(u8, output_len);
    errdefer allocator.free(pixels);
    var width: u16 = 0;
    var height: u16 = 0;
    const rc = c.ra8_jpeg_sw_decode(bytes.ptr, input_len, pixels.ptr, c_output_len, &width, &height);
    if (rc != c.k_ra8_ok) {
        allocator.free(pixels);
        return .{ .failure = .{ .code = @intCast(rc) } };
    }
    try validateDecodedOutput(dims, width, height, pixels.len);
    return .{ .value = .{ .width = width, .height = height, .pixels = pixels, .allocator = allocator } };
}

pub fn encode(allocator: std.mem.Allocator, image: degrade.Image) !BytesResult {
    if (image.width == 0 or image.height == 0) return error.InvalidImage;
    const pixel_count = std.math.mul(usize, image.width, image.height) catch return error.InvalidImage;
    const required_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidImage;
    if (image.pixels.len != required_len) return error.InvalidImage;
    const rgb_len = image.pixels.len;
    var capacity = try std.math.add(usize, rgb_len, 65_536);
    while (capacity <= 16 * 1024 * 1024) {
        const output = try allocator.alloc(u8, capacity);
        errdefer allocator.free(output);
        var output_len: u32 = 0;
        const rc = c.ra8_jpeg_sw_encode(image.pixels.ptr, image.width, image.height, 25, output.ptr, @intCast(output.len), &output_len);
        if (rc == c.k_ra8_ok) {
            const encoded_len = try checkedEncodedLength(output.len, output_len);
            return .{ .value = try allocator.realloc(output, encoded_len) };
        }
        allocator.free(output);
        if (rc != c.k_ra8_err_invalid_size) return .{ .failure = .{ .code = @intCast(rc) } };
        if (capacity > 8 * 1024 * 1024) break;
        capacity *= 2;
    }
    return .{ .failure = .{ .code = @intCast(c.k_ra8_err_invalid_size) } };
}
