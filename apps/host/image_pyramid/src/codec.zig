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

/// A refusal from the C JPEG codec, carrying the `ra8_status_t` code it returned.
/// A failure is a normal codec outcome (an unsupported file, a buffer that is too
/// small), distinct from a Zig error, which signals a defect or an allocation failure.
pub const CodecFailure = struct { code: u32 };
/// Either the dimensions read from a JPEG header, or the codec's refusal.
pub const DimensionsResult = union(enum) { value: degrade.Dimensions, failure: CodecFailure };
/// Either a decoded RGB image owning its pixel buffer, or the codec's refusal.
pub const ImageResult = union(enum) { value: degrade.Image, failure: CodecFailure };
/// Either an encoded JPEG buffer owned by the caller's allocator, or the codec's refusal.
pub const BytesResult = union(enum) { value: []u8, failure: CodecFailure };

fn checkedInputLength(length: usize) error{InputTooLarge}!u32 {
    return std.math.cast(u32, length) orelse error.InputTooLarge;
}

/// Checks that a decode wrote exactly the image the header promised.
///
/// The C decoder reports its own width and height alongside the buffer it filled;
/// this rejects a zero dimension, a disagreement with `expected`, or a length that
/// is not `width * height * 3`, so a short or mismatched write cannot reach the
/// rest of the pipeline. Returns `error.InvalidCodecOutput` on any of those.
pub fn validateDecodedOutput(expected: degrade.Dimensions, width: u16, height: u16, output_len: usize) error{InvalidCodecOutput}!void {
    const pixel_count = std.math.mul(usize, width, height) catch return error.InvalidCodecOutput;
    const actual_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidCodecOutput;
    if (width == 0 or height == 0 or width != expected.width or height != expected.height or actual_len != output_len) {
        return error.InvalidCodecOutput;
    }
}

/// Returns the RGB byte length `dims` decodes to, or `error.InvalidCodecOutput`.
///
/// Rejects a zero dimension and any overflow of `width * height * 3`, so the
/// caller can size its allocation without trusting the header.
pub fn checkedDecodedLength(dims: degrade.Dimensions) error{InvalidCodecOutput}!usize {
    if (dims.width == 0 or dims.height == 0) return error.InvalidCodecOutput;
    const pixel_count = std.math.mul(usize, dims.width, dims.height) catch return error.InvalidCodecOutput;
    const output_len = std.math.mul(usize, pixel_count, 3) catch return error.InvalidCodecOutput;
    if (output_len == 0) return error.InvalidCodecOutput;
    return output_len;
}

/// Returns the encoded length the C encoder reported, validated against `capacity`.
///
/// Returns `error.InvalidCodecOutput` when the encoder claims zero bytes or more
/// bytes than the buffer it was handed.
pub fn checkedEncodedLength(capacity: usize, output_len: u32) error{InvalidCodecOutput}!usize {
    if (output_len == 0 or output_len > capacity) return error.InvalidCodecOutput;
    return output_len;
}

/// Reads the pixel dimensions from a JPEG header without decoding it.
///
/// Returns the codec's refusal as a value; errors only when `bytes` is longer than
/// the C API's 32-bit length.
pub fn dimensions(bytes: []const u8) !DimensionsResult {
    var width: u16 = 0;
    var height: u16 = 0;
    const rc = c.ra8_jpeg_sw_get_dimensions(bytes.ptr, try checkedInputLength(bytes.len), &width, &height);
    if (rc != c.k_ra8_ok) return .{ .failure = .{ .code = @intCast(rc) } };
    return .{ .value = .{ .width = width, .height = height } };
}

/// Decodes a JPEG into a newly allocated RGB image owned by `allocator`.
///
/// Reads the header first to size the buffer, then validates the decoder's own
/// reported dimensions against it. The pixel buffer is freed before returning a
/// failure or an error, so the caller only ever owns a fully validated image.
/// Call `Image.deinit` on the returned value.
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

/// Encodes an RGB image to JPEG in a buffer owned by `allocator`.
///
/// Starts at the RGB length plus 64 KiB and doubles on `k_ra8_err_invalid_size`
/// until 16 MiB, since the C encoder cannot predict its own output size. The
/// returned slice is reallocated down to the encoded length. Returns
/// `error.InvalidImage` when `image` is not `width * height * 3` bytes.
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
