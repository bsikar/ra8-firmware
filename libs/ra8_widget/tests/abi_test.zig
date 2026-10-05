//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the C ABI membrane: the exported private paint helpers, driven
//! through a recording backend in the same shape the host C suites bind.

const std = @import("std");
const abi = @import("abi");

const Recorder = struct {
    var fills: [4]Fill = undefined;
    var fill_count: usize = 0;
    var measure_calls: usize = 0;
    var text_w: i32 = 16;
    var text_h: i32 = 12;

    const Fill = struct { x: i32, y: i32, w: i32, h: i32, color: u32 };

    fn reset() void {
        fill_count = 0;
        measure_calls = 0;
        text_w = 16;
        text_h = 12;
    }

    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills[fill_count] = .{ .x = x, .y = y, .w = w, .h = h, .color = color };
        fill_count += 1;
    }

    fn textSize(_: ?*anyopaque, _: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        measure_calls += 1;
        out_w.* = text_w;
        out_h.* = text_h;
    }
};

const rect: abi.Rect = .{ .x = 10, .y = 20, .w = 100, .h = 40 };
const text: [*:0]const u8 = "hello";

fn measuringBackend() abi.Paint {
    return .{
        .user = null,
        .fill_rect = Recorder.fillRect,
        .draw_text = null,
        .text_size = Recorder.textSize,
    };
}

test "text pos places an unmeasurable string at the inner inset" {
    Recorder.reset();
    var backend = measuringBackend();
    backend.text_size = null;

    var x: i32 = -1;
    var y: i32 = -1;
    abi.priv_widget_text_pos(&backend, &rect, text, 3, .center, .sans, .regular, false, &x, &y);

    try std.testing.expectEqual(@as(i32, 13), x);
    try std.testing.expectEqual(@as(i32, 23), y);
    try std.testing.expectEqual(@as(usize, 0), Recorder.measure_calls);
}

test "text pos never measures a left-aligned string" {
    Recorder.reset();
    const backend = measuringBackend();

    var x: i32 = -1;
    var y: i32 = -1;
    abi.priv_widget_text_pos(&backend, &rect, text, 3, .left, .sans, .regular, false, &x, &y);

    try std.testing.expectEqual(@as(i32, 13), x);
    try std.testing.expectEqual(@as(i32, 23), y);
    try std.testing.expectEqual(@as(usize, 0), Recorder.measure_calls);
}

test "text pos centres a measured string on both axes" {
    Recorder.reset();
    const backend = measuringBackend();

    var x: i32 = -1;
    var y: i32 = -1;
    abi.priv_widget_text_pos(&backend, &rect, text, 3, .center, .sans, .regular, false, &x, &y);

    try std.testing.expectEqual(@as(i32, 10 + (100 - 16) / 2), x);
    try std.testing.expectEqual(@as(i32, 20 + (40 - 12) / 2), y);
    try std.testing.expectEqual(@as(usize, 1), Recorder.measure_calls);
}

test "text pos right-aligns a measured string against the inset" {
    Recorder.reset();
    const backend = measuringBackend();

    var x: i32 = -1;
    var y: i32 = -1;
    abi.priv_widget_text_pos(&backend, &rect, text, 3, .right, .sans, .regular, false, &x, &y);

    try std.testing.expectEqual(@as(i32, (10 + 100) - 3 - 16), x);
    try std.testing.expectEqual(@as(i32, 20 + (40 - 12) / 2), y);
}

test "fill box with no fill_rect paints nothing" {
    Recorder.reset();
    var backend = measuringBackend();
    backend.fill_rect = null;

    abi.priv_widget_fill_box(&backend, &rect, 0x111111, 0x222222, 2);

    try std.testing.expectEqual(@as(usize, 0), Recorder.fill_count);
}

test "fill box without a border is one fill of the whole rect" {
    Recorder.reset();
    const backend = measuringBackend();

    abi.priv_widget_fill_box(&backend, &rect, 0x111111, 0x222222, 0);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fill_count);
    try std.testing.expectEqual(@as(u32, 0x111111), Recorder.fills[0].color);
    try std.testing.expectEqual(@as(i32, 10), Recorder.fills[0].x);
    try std.testing.expectEqual(@as(i32, 100), Recorder.fills[0].w);
}

test "fill box with a border frames the face in two fills" {
    Recorder.reset();
    const backend = measuringBackend();

    abi.priv_widget_fill_box(&backend, &rect, 0x111111, 0x222222, 2);

    try std.testing.expectEqual(@as(usize, 2), Recorder.fill_count);
    try std.testing.expectEqual(@as(u32, 0x222222), Recorder.fills[0].color);
    try std.testing.expectEqual(@as(i32, 100), Recorder.fills[0].w);
    try std.testing.expectEqual(@as(u32, 0x111111), Recorder.fills[1].color);
    try std.testing.expectEqual(@as(i32, 12), Recorder.fills[1].x);
    try std.testing.expectEqual(@as(i32, 22), Recorder.fills[1].y);
    try std.testing.expectEqual(@as(i32, 96), Recorder.fills[1].w);
    try std.testing.expectEqual(@as(i32, 36), Recorder.fills[1].h);
}

test "fill box treats a negative border like no border" {
    Recorder.reset();
    const backend = measuringBackend();

    abi.priv_widget_fill_box(&backend, &rect, 0x111111, 0x222222, -1);

    try std.testing.expectEqual(@as(usize, 1), Recorder.fill_count);
    try std.testing.expectEqual(@as(u32, 0x111111), Recorder.fills[0].color);
}

test "fill frac is reachable through the C entry point" {
    try std.testing.expectEqual(@as(i32, 0), abi.priv_widget_fill_frac(5, 0, 100));
    try std.testing.expectEqual(@as(i32, 50), abi.priv_widget_fill_frac(5, 10, 100));
    try std.testing.expectEqual(@as(i32, 100), abi.priv_widget_fill_frac(20, 10, 100));
}
