//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8FW-951: the line, blit, clip and alpha ops on a recording fake Paint,
//! both with the callbacks bound and with the null defaults a panel backend
//! leaves, where each op falls back or reports that it did nothing.

const std = @import("std");
const ops = @import("ops");

const Call = union(enum) {
    fill: [5]i64,
    line: [5]i64,
    blit: [7]i64,
    push: [4]i64,
    pop,
    alpha: [6]i64,
};

var calls_buffer: [64]Call = undefined;
var calls: std.ArrayList(Call) = .initBuffer(&calls_buffer);

fn record(call: Call) void {
    calls.appendBounded(call) catch unreachable;
}

fn fill(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
    record(.{ .fill = .{ x, y, w, h, color } });
}
fn line(_: ?*anyopaque, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) void {
    record(.{ .line = .{ x0, y0, x1, y1, color } });
}
fn blit(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, pixels: [*]const u8, stride: i32, format: u8) callconv(.c) void {
    record(.{ .blit = .{ x, y, w, h, pixels[0], stride, format } });
}
fn push(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32) callconv(.c) void {
    record(.{ .push = .{ x, y, w, h } });
}
fn pop(_: ?*anyopaque) callconv(.c) void {
    record(.pop);
}
fn alpha(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32, a: u8) callconv(.c) void {
    record(.{ .alpha = .{ x, y, w, h, color, a } });
}

const panel: ops.Paint = .{ .user = null, .fill_rect = fill, .draw_text = null, .text_size = null };
const desktop: ops.Paint = .{
    .user = null,
    .fill_rect = fill,
    .draw_text = null,
    .text_size = null,
    .draw_line = line,
    .blit = blit,
    .push_clip = push,
    .pop_clip = pop,
    .fill_rect_alpha = alpha,
};
const box: ops.Rect = .{ .x = 2, .y = 3, .w = 4, .h = 5 };

fn reset() void {
    calls.clearRetainingCapacity();
}

test "a bound draw_line gets the endpoints as given" {
    reset();
    ops.line(&desktop, 9, 1, 2, 7, 0xABCDEF);
    try std.testing.expectEqualSlices(Call, &.{.{ .line = .{ 9, 1, 2, 7, 0xABCDEF } }}, calls.items);
}

test "without draw_line a straight run is one fill_rect either way round" {
    reset();
    ops.line(&panel, 8, 4, 3, 4, 1);
    ops.line(&panel, 5, 9, 5, 6, 2);
    try std.testing.expectEqualSlices(Call, &.{ .{ .fill = .{ 3, 4, 6, 1, 1 } }, .{ .fill = .{ 5, 6, 1, 4, 2 } } }, calls.items);
}

test "without draw_line a slope steps one pixel at a time from end to end" {
    reset();
    ops.line(&panel, 0, 0, 3, 3, 7);
    try std.testing.expectEqual(@as(usize, 4), calls.items.len);
    try std.testing.expectEqual(Call{ .fill = .{ 0, 0, 1, 1, 7 } }, calls.items[0]);
    try std.testing.expectEqual(Call{ .fill = .{ 3, 3, 1, 1, 7 } }, calls.items[3]);
}

test "blit reaches a bound backend and reports false to a panel one" {
    reset();
    const pixels = [_]u8{ 0x11, 0x22 };
    try std.testing.expect(ops.blit(&desktop, box, &pixels, 2, .argb8888));
    try std.testing.expect(!ops.blit(&panel, box, &pixels, 2, .gray8));
    try std.testing.expectEqualSlices(Call, &.{.{ .blit = .{ 2, 3, 4, 5, 0x11, 2, 4 } }}, calls.items);
}

test "clip push and pop pass through, and are no-ops when unbound" {
    reset();
    ops.pushClip(&desktop, box);
    ops.popClip(&desktop);
    ops.pushClip(&panel, box);
    ops.popClip(&panel);
    try std.testing.expectEqualSlices(Call, &.{ .{ .push = .{ 2, 3, 4, 5 } }, .pop }, calls.items);
}

test "alpha zero paints nothing, partial alpha blends, opaque is a plain fill" {
    reset();
    ops.fillAlpha(&desktop, box, 9, 0);
    ops.fillAlpha(&desktop, box, 9, 128);
    ops.fillAlpha(&desktop, box, 9, 255);
    try std.testing.expectEqualSlices(Call, &.{ .{ .alpha = .{ 2, 3, 4, 5, 9, 128 } }, .{ .fill = .{ 2, 3, 4, 5, 9 } } }, calls.items);
}

test "without a blended fill any visible alpha paints solid" {
    reset();
    ops.fillAlpha(&panel, box, 9, 64);
    try std.testing.expectEqualSlices(Call, &.{.{ .fill = .{ 2, 3, 4, 5, 9 } }}, calls.items);
}
