//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const app = @import("image_pyramid");
const codec = app.codec;
const degrade = app.degrade;

test "real codec round trip preserves dimensions" {
    const allocator = std.testing.allocator;
    var pixels = [_]u8{255} ** (16 * 16 * 3);
    const source = degrade.Image{ .width = 16, .height = 16, .pixels = &pixels, .allocator = allocator };
    const encoded = switch (try codec.encode(allocator, source)) {
        .value => |value| value,
        .failure => return error.TestUnexpectedResult,
    };
    defer allocator.free(encoded);
    var decoded = switch (try codec.decode(allocator, encoded)) {
        .value => |value| value,
        .failure => return error.TestUnexpectedResult,
    };
    defer decoded.deinit();
    try std.testing.expectEqual(@as(u16, 16), decoded.width);
    try std.testing.expectEqual(@as(u16, 16), decoded.height);
}

test "encoder rejects malformed RGB images before calling C" {
    const allocator = std.testing.allocator;
    var empty = [_]u8{};
    var short = [_]u8{ 1, 2 };
    var long = [_]u8{0} ** 6;
    try std.testing.expectError(error.InvalidImage, codec.encode(allocator, .{ .width = 0, .height = 1, .pixels = &empty, .allocator = allocator }));
    try std.testing.expectError(error.InvalidImage, codec.encode(allocator, .{ .width = 1, .height = 1, .pixels = &short, .allocator = allocator }));
    try std.testing.expectError(error.InvalidImage, codec.encode(allocator, .{ .width = 1, .height = 1, .pixels = &long, .allocator = allocator }));
}

test "codec input length is checked before narrowing to C width" {
    if (@sizeOf(usize) <= @sizeOf(u32)) return;
    const oversized = @as([*]const u8, @ptrFromInt(@alignOf(u8)))[0 .. @as(usize, std.math.maxInt(u32)) + 1];
    try std.testing.expectError(error.InputTooLarge, codec.dimensions(oversized));
}

test "decoder output must match its preflight dimensions and buffer" {
    const expected = degrade.Dimensions{ .width = 2, .height = 3 };
    try codec.validateDecodedOutput(expected, 2, 3, 18);
    try std.testing.expectError(error.InvalidCodecOutput, codec.validateDecodedOutput(expected, 3, 2, 18));
    try std.testing.expectError(error.InvalidCodecOutput, codec.validateDecodedOutput(expected, 2, 3, 17));
    try std.testing.expectError(error.InvalidCodecOutput, codec.validateDecodedOutput(expected, 0, 3, 0));
}

test "decoder preflight dimensions are validated before allocation" {
    try std.testing.expectEqual(@as(usize, 18), try codec.checkedDecodedLength(.{ .width = 2, .height = 3 }));
    try std.testing.expectError(error.InvalidCodecOutput, codec.checkedDecodedLength(.{ .width = 0, .height = 3 }));
    try std.testing.expectError(error.InvalidCodecOutput, codec.checkedDecodedLength(.{ .width = 2, .height = 0 }));
}

test "encoder output length must be nonzero and within capacity" {
    try std.testing.expectEqual(@as(usize, 7), try codec.checkedEncodedLength(8, 7));
    try std.testing.expectError(error.InvalidCodecOutput, codec.checkedEncodedLength(8, 0));
    try std.testing.expectError(error.InvalidCodecOutput, codec.checkedEncodedLength(8, 9));
}
