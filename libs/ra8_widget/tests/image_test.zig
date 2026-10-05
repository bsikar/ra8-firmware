//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Scaling and missing-image rendering through a recording paint backend.

const std = @import("std");
const image = @import("image");
const types = image.types;

const Fill = struct { x: i32, y: i32, w: i32, h: i32, color: u32 };
var fills: std.BoundedArray(Fill, 64) = .{};

fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
    fills.append(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
}

const backend: types.Paint = .{ .user = null, .fill_rect = fillRect, .draw_text = null, .text_size = null };
const rect: types.Rect = .{ .x = 10, .y = 20, .w = 4, .h = 4 };

fn reset() void {
    fills = .{};
}

test "fit preserves aspect ratio and centers the image" {
    reset();
    const pixels = [_]u8{ 20, 40 };
    image.render(&backend, rect, .{ .pixels = &pixels, .width = 2, .height = 1 }, .fit, .{});
    try std.testing.expectEqual(@as(usize, 5), fills.len);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 20, .w = 4, .h = 4, .color = 0xffffff }, fills.get(0));
    try std.testing.expectEqual(Fill{ .x = 10, .y = 21, .w = 2, .h = 1, .color = 0x141414 }, fills.get(1));
    try std.testing.expectEqual(Fill{ .x = 12, .y = 21, .w = 2, .h = 1, .color = 0x282828 }, fills.get(2));
    try std.testing.expectEqual(Fill{ .x = 10, .y = 22, .w = 2, .h = 1, .color = 0x141414 }, fills.get(3));
    try std.testing.expectEqual(Fill{ .x = 12, .y = 22, .w = 2, .h = 1, .color = 0x282828 }, fills.get(4));
    try std.testing.expectEqual(rect, image.damageRect(rect));
}

test "fill crops the source to cover the destination" {
    reset();
    const pixels = [_]u8{ 10, 20, 30, 40, 50, 60, 70, 80 };
    image.render(&backend, .{ .x = 0, .y = 0, .w = 2, .h = 2 }, .{ .pixels = &pixels, .width = 4, .height = 2 }, .fill, .{});
    try std.testing.expectEqual(@as(usize, 4), fills.len);
    try std.testing.expectEqual(@as(u32, 0x141414), fills.get(0).color);
    try std.testing.expectEqual(@as(u32, 0x1e1e1e), fills.get(1).color);
    try std.testing.expectEqual(@as(u32, 0x3c3c3c), fills.get(2).color);
    try std.testing.expectEqual(@as(u32, 0x464646), fills.get(3).color);
}

test "an absent image paints the framed placeholder" {
    reset();
    image.render(&backend, rect, null, .fit, .{ .fill = 255, .border = 0, .border_width = 1 });
    try std.testing.expectEqual(@as(usize, 2), fills.len);
    try std.testing.expectEqual(Fill{ .x = 10, .y = 20, .w = 4, .h = 4, .color = 0 }, fills.get(0));
    try std.testing.expectEqual(Fill{ .x = 11, .y = 21, .w = 2, .h = 2, .color = 0xffffff }, fills.get(1));
}
