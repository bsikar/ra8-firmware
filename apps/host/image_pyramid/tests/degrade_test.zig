//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const degrade = @import("image_pyramid").degrade;

test "discard strips keeps top-left pixel from each two-by-two cell" {
    const allocator = std.testing.allocator;
    var source_pixels = [_]u8{
        1,  2,  3,  4,  5,  6,  7,  8,  9,
        10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27,
    };
    const source = degrade.Image{ .width = 3, .height = 3, .pixels = &source_pixels, .allocator = allocator };
    var output = try degrade.discardStrips(allocator, source);
    defer output.deinit();
    try std.testing.expectEqual(@as(u16, 2), output.width);
    try std.testing.expectEqual(@as(u16, 2), output.height);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 7, 8, 9, 19, 20, 21, 25, 26, 27 }, output.pixels);
}

test "one-pixel axis survives until one-by-one" {
    const dims = try degrade.nextDimensions(1, 5);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 1, .height = 3 }, dims);
    try std.testing.expectError(error.CannotDownsample, degrade.nextDimensions(1, 1));
}

test "maximum dimensions halve without overflowing" {
    const dims = try degrade.nextDimensions(65_535, 65_535);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 32_768, .height = 32_768 }, dims);
}

test "discard strips rejects malformed RGB images" {
    const allocator = std.testing.allocator;
    var empty = [_]u8{};
    var short = [_]u8{ 1, 2 };
    var long = [_]u8{0} ** 6;
    try std.testing.expectError(error.InvalidImage, degrade.discardStrips(allocator, .{ .width = 0, .height = 1, .pixels = &empty, .allocator = allocator }));
    try std.testing.expectError(error.InvalidImage, degrade.discardStrips(allocator, .{ .width = 1, .height = 1, .pixels = &short, .allocator = allocator }));
    try std.testing.expectError(error.InvalidImage, degrade.discardStrips(allocator, .{ .width = 1, .height = 1, .pixels = &long, .allocator = allocator }));
}

test "plan rejects levels beyond one-by-one" {
    _ = try degrade.plan(330, 248, 8);
    try std.testing.expectError(error.TooManyLevels, degrade.plan(2, 2, 2));
    try std.testing.expectError(error.TooManyLevels, degrade.plan(65_535, 65_535, 17));
}
