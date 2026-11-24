// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the strip composite pipeline (`src/zoom_render.zig`).

const std = @import("std");
const render = @import("render");
const stubs = @import("zoom_stubs.zig");

const D = stubs.DitherBlitStubs(render);

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
    _ = D;
}

const zoom = render.zoom;
const Err = zoom.Err;
const View = zoom.View;
const ToneMap = render.ToneMap;

test "renderReady rejects each broken state in the C order" {
    var v = View{};
    try std.testing.expectEqual(Err.null_ptr, render.renderReady(null));
    try std.testing.expectEqual(Err.null_ptr, render.renderReady(&v));
    v.src.read = D.testReadFill;
    try std.testing.expectEqual(Err.invalid_state, render.renderReady(&v));
    v.active = true;
    try std.testing.expectEqual(Err.invalid_state, render.renderReady(&v));
    v.strip_rows = 2;
    try std.testing.expectEqual(Err.ok, render.renderReady(&v));
}

test "expandRow samples source columns and leaves letterbox alone" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = D.testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .row_cap = 4 },
        .scale = 2,
        .anchor_x = 0,
        .active = true,
        .strip_rows = 2,
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var drow: [8]u8 = undefined;
    var i: usize = 0;
    while (i < drow.len) : (i += 1) {
        drow[i] = 0xEE;
    }
    for (&row, 0..) |*rp, ri| rp.* = @intCast(ri);
    render.expandRow(&v, &ax, &drow);
    for (drow, 0..) |b, d| {
        try std.testing.expectEqual(@as(u8, @intCast(d / 2)), b);
    }
    const ax_lb = zoom.axis(-4, 8, 2, 4);
    v.anchor_x = -4;
    v.scratch.row.?[@intCast(ax_lb.s0)] = 9;
    var drow2: [8]u8 = undefined;
    i = 0;
    while (i < drow2.len) : (i += 1) {
        drow2[i] = 0xEE;
    }
    render.expandRow(&v, &ax_lb, &drow2);
    try std.testing.expectEqual(@as(u8, 0xEE), drow2[0]);
    try std.testing.expectEqual(@as(u8, 0xEE), drow2[3]);
    try std.testing.expectEqual(@as(u8, 9), drow2[4]);
}

test "fillRow faults only on a new source row and resets on failure" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = D.testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .row_cap = 4 },
        .scale = 2,
        .anchor_x = 0,
        .active = true,
        .strip_rows = 2,
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var drow: [8]u8 = undefined;
    var cached: i32 = -1;
    try std.testing.expectEqual(Err.ok, render.fillRow(&v, &ax, 0, &drow, &cached));
    try std.testing.expectEqual(@as(i32, 0), cached);
    try std.testing.expectEqual(Err.ok, render.fillRow(&v, &ax, 1, &drow, &cached));
    try std.testing.expectEqual(Err.ok, render.fillRow(&v, &ax, -1, &drow, &cached));
    try std.testing.expectEqual(@as(i32, 0), cached);
    try std.testing.expectEqual(Err.ok, render.fillRow(&v, &ax, 8, &drow, &cached));
    try std.testing.expectEqual(@as(i32, 0), cached);
    D.read_fail = true;
    defer D.read_fail = false;
    try std.testing.expectEqual(Err.no_mem, render.fillRow(&v, &ax, 2, &drow, &cached));
    try std.testing.expectEqual(@as(i32, -1), cached);
}

test "fillStrip fills letterbox first and composites covered rows" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    var strip: [16]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = D.testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .strip = strip[0..].ptr, .row_cap = 4, .strip_cap = 16 },
        .dst = .{ .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 0,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var cached: i32 = -1;
    try std.testing.expectEqual(Err.ok, render.fillStrip(&v, &ax, 0, 2, &cached));
    var c: usize = 0;
    while (c < 8) : (c += 1) {
        try std.testing.expectEqual(@as(u8, @intCast(c / 2)), strip[c]);
        try std.testing.expectEqual(@as(u8, @intCast(c / 2)), strip[8 + c]);
    }
    var img2: [64]u8 = undefined;
    for (&img2) |*b| b.* = 0xAA;
    var row2: [4]u8 = undefined;
    var strip2: [16]u8 = undefined;
    var v2 = View{
        .src = .{ .read = D.testReadFill, .ctx = @ptrCast(&img2), .width = 4, .height = 4 },
        .scratch = .{ .row = row2[0..].ptr, .strip = strip2[0..].ptr, .row_cap = 4, .strip_cap = 16 },
        .dst = .{ .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 100,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
    };
    var cached2: i32 = -1;
    try std.testing.expectEqual(Err.ok, render.fillStrip(&v2, &zoom.axis(100, 8, 2, 4), 0, 2, &cached2));
    for (strip2) |b| try std.testing.expectEqual(zoom.bg_gray, b);
    try std.testing.expectEqual(@as(i32, -1), cached2);
}

test "emitStrip dithers at plane phase and blits at native scale" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    var strip: [16]u8 = undefined;
    var packed_buf: [8]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var tone = ToneMap{ .level = undefined, .up_threshold = undefined };
    var v = View{
        .src = .{ .read = D.testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .strip = strip[0..].ptr, .packed_buf = packed_buf[0..].ptr, .row_cap = 4, .strip_cap = 16, .packed_cap = 8 },
        .dst = .{ .x = 3, .y = 4, .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 0,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
        .tone = @ptrCast(&tone),
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var cached: i32 = -1;
    D.dither_calls = 0;
    D.blit_calls = 0;
    try std.testing.expectEqual(Err.ok, render.emitStrip(&v, &ax, 6, 2, &cached));
    try std.testing.expectEqual(@as(u32, 1), D.dither_calls);
    try std.testing.expectEqual(@as(i32, 8), D.dither_args.w);
    try std.testing.expectEqual(@as(i32, 2), D.dither_args.h);
    try std.testing.expectEqual(@as(i32, 0), D.dither_args.origin_x);
    try std.testing.expectEqual(@as(i32, 6), D.dither_args.origin_y);
    try std.testing.expectEqual(@as(u32, 1), D.blit_calls);
    try std.testing.expectEqual(@as(i32, 8), D.blit_args.sw);
    try std.testing.expectEqual(@as(i32, 2), D.blit_args.sh);
    try std.testing.expectEqual(@as(i32, 1), D.blit_args.zoom);
    try std.testing.expectEqual(@as(i32, 3), D.blit_args.dst_x);
    try std.testing.expectEqual(@as(i32, 10), D.blit_args.dst_y);
    D.blit_err = .hw_error;
    defer D.blit_err = .ok;
    try std.testing.expectEqual(Err.hw_error, render.emitStrip(&v, &ax, 6, 2, &cached));
}

test "viewRender strips the viewport and stops on a failing strip" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    var strip: [16]u8 = undefined;
    var packed_buf: [8]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = D.testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .strip = strip[0..].ptr, .packed_buf = packed_buf[0..].ptr, .row_cap = 4, .strip_cap = 16, .packed_cap = 8 },
        .dst = .{ .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 0,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
    };
    D.dither_calls = 0;
    D.blit_calls = 0;
    try std.testing.expectEqual(Err.ok, render.viewRender(&v));
    try std.testing.expectEqual(@as(u32, 4), D.blit_calls);
    try std.testing.expectEqual(@as(u32, 4), D.dither_calls);
    try std.testing.expectEqual(@as(i32, 6), D.blit_args.dst_y);
    D.dither_calls = 0;
    D.blit_calls = 0;
    D.read_fail = true;
    defer D.read_fail = false;
    try std.testing.expectEqual(Err.no_mem, render.viewRender(&v));
    try std.testing.expectEqual(@as(u32, 0), D.blit_calls);
    try std.testing.expectEqual(Err.null_ptr, render.viewRender(null));
    v.active = false;
    try std.testing.expectEqual(Err.invalid_state, render.viewRender(&v));
}
