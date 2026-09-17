//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const degrade = @import("image_pyramid").degrade;

test "discard columns keeps the left pixel from each horizontal pair" {
    const allocator = std.testing.allocator;
    var source_pixels = [_]u8{
        1,  2,  3,  4,  5,  6,  7,  8,  9,
        10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27,
    };
    const source = degrade.Image{ .width = 3, .height = 3, .pixels = &source_pixels, .allocator = allocator };
    var output = try degrade.discardStrips(allocator, source, .columns);
    defer output.deinit();
    try std.testing.expectEqual(@as(u16, 2), output.width);
    try std.testing.expectEqual(@as(u16, 3), output.height);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 7, 8, 9, 10, 11, 12, 16, 17, 18, 19, 20, 21, 25, 26, 27 }, output.pixels);
}

test "discard rows keeps the top pixel from each vertical pair" {
    const allocator = std.testing.allocator;
    var source_pixels = [_]u8{
        1,  2,  3,  4,  5,  6,  7,  8,  9,
        10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27,
    };
    const source = degrade.Image{ .width = 3, .height = 3, .pixels = &source_pixels, .allocator = allocator };
    var output = try degrade.discardStrips(allocator, source, .rows);
    defer output.deinit();
    try std.testing.expectEqual(@as(u16, 3), output.width);
    try std.testing.expectEqual(@as(u16, 2), output.height);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 19, 20, 21, 22, 23, 24, 25, 26, 27 }, output.pixels);
}

test "only the selected axis is reduced" {
    const columns = try degrade.nextDimensions(5, 7, .columns);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 3, .height = 7 }, columns);
    const rows = try degrade.nextDimensions(5, 7, .rows);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 5, .height = 4 }, rows);
    try std.testing.expectError(error.CannotDownsample, degrade.nextDimensions(1, 5, .columns));
    try std.testing.expectError(error.CannotDownsample, degrade.nextDimensions(5, 1, .rows));
}

test "maximum dimensions halve without overflowing" {
    const columns = try degrade.nextDimensions(65_535, 65_535, .columns);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 32_768, .height = 65_535 }, columns);
    const rows = try degrade.nextDimensions(65_535, 65_535, .rows);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 65_535, .height = 32_768 }, rows);
}

test "discard strips rejects malformed RGB images" {
    const allocator = std.testing.allocator;
    var empty = [_]u8{};
    var short = [_]u8{ 1, 2 };
    var long = [_]u8{0} ** 6;
    try std.testing.expectError(error.InvalidImage, degrade.discardStrips(allocator, .{ .width = 0, .height = 1, .pixels = &empty, .allocator = allocator }, .columns));
    try std.testing.expectError(error.InvalidImage, degrade.discardStrips(allocator, .{ .width = 1, .height = 1, .pixels = &short, .allocator = allocator }, .columns));
    try std.testing.expectError(error.InvalidImage, degrade.discardStrips(allocator, .{ .width = 1, .height = 1, .pixels = &long, .allocator = allocator }, .columns));
}

test "plan alternates columns then rows" {
    const dimensions = try degrade.plan(330, 248, 8);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 165, .height = 248 }, dimensions[0]);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 165, .height = 124 }, dimensions[1]);
    try std.testing.expectEqual(degrade.Dimensions{ .width = 83, .height = 124 }, dimensions[2]);
    _ = try degrade.plan(2, 2, 2);
    try std.testing.expectError(error.TooManyLevels, degrade.plan(2, 2, 3));
    try std.testing.expectError(error.TooManyLevels, degrade.plan(65_535, 65_535, 17));
}
