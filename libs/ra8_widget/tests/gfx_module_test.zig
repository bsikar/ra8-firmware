//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8FW-949: host code outside libs/ra8_gfx imports the ra8_gfx module
//! through its public root and measures and renders a string with the
//! panel's fonts, the way the emulator and host GUI apps will.

const std = @import("std");
const gfx = @import("ra8_gfx");

const width = 64;
const height = 16;

const Grid = struct {
    pixels: [width * height]u32 = @splat(0),

    fn put(user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void {
        const self: *Grid = @ptrCast(@alignCast(user.?));
        if (x < 0 or y < 0 or x >= width or y >= height) return;
        self.pixels[@as(usize, @intCast(y)) * width + @as(usize, @intCast(x))] = color;
    }

    fn count(self: *const Grid, color: u32) usize {
        var n: usize = 0;
        for (self.pixels) |pixel| n += @intFromBool(pixel == color);
        return n;
    }
};

test "the sans face measures eight pixels a character and one cell high" {
    const extent = gfx.text.measure("RA8", gfx.Face.sans);
    try std.testing.expectEqual(@as(u32, 3 * gfx.text.sans.width), extent.width);
    try std.testing.expectEqual(@as(u32, gfx.text.sans.height), extent.height);
}

test "drawing a run through the module paints ink inside its measured box and nowhere else" {
    var grid = Grid{};
    const ink: u32 = 0xFFFFFF;
    const paper: u32 = 0x000001;
    gfx.text.drawSans("RA8", 0, 0, ink, paper, &grid, Grid.put);
    const extent = gfx.text.measure("RA8", .sans);
    try std.testing.expect(grid.count(ink) > 0);
    try std.testing.expect(grid.count(paper) > 0);
    for (grid.pixels, 0..) |pixel, index| {
        if (pixel == 0) continue;
        try std.testing.expect(index % width < extent.width);
        try std.testing.expect(index / width < extent.height);
    }
}

test "the serif reading face measures a wider run as wider" {
    const short = gfx.text.measure("i", .serif);
    const long = gfx.text.measure("iii", .serif);
    try std.testing.expect(short.width > 0 and short.height > 0);
    try std.testing.expect(long.width > short.width);
}
