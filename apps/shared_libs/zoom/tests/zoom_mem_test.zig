// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the in-memory gray8 source adapter (`src/zoom_mem.zig`).

const std = @import("std");
const mem = @import("mem");
const stubs = @import("zoom_stubs.zig");

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
}

const testing = std.testing;
const Err = mem.zoom.Err;
const Source = mem.zoom.Source;
const MemSrc = mem.MemSrc;

test "memSrcInit validates pointers, bounds and default stride" {
    var px = [_]u8{ 1, 2, 3, 4 };
    var ms = MemSrc{};
    try testing.expectEqual(Err.null_ptr, mem.memSrcInit(null, &px, 2, 2, 0));
    try testing.expectEqual(Err.null_ptr, mem.memSrcInit(&ms, null, 2, 2, 0));
    try testing.expectEqual(Err.invalid_arg, mem.memSrcInit(&ms, &px, 0, 2, 0));
    try testing.expectEqual(Err.invalid_arg, mem.memSrcInit(&ms, &px, mem.zoom.dim_max + 1, 2, 0));
    try testing.expectEqual(Err.invalid_arg, mem.memSrcInit(&ms, &px, 4, 2, 3));
    try testing.expectEqual(Err.ok, mem.memSrcInit(&ms, &px, 2, 2, 0));
    try testing.expectEqual(@as(u32, 2), ms.stride);
}

test "memSrcBind and memReadImpl copy strided rectangles and guard out-of-range" {
    const px = [_]u8{
        10, 11, 12, 13, 0xff, 0xff,
        20, 21, 22, 23, 0xff, 0xff,
        30, 31, 32, 33, 0xff, 0xff,
    };
    var ms = MemSrc{};
    try testing.expectEqual(Err.ok, mem.memSrcInit(&ms, &px, 4, 3, 6));
    var src = Source{};
    try testing.expectEqual(Err.ok, mem.memSrcBind(&ms, &src));
    try testing.expectEqual(@as(u32, 4), src.width);
    try testing.expectEqual(@as(u32, 3), src.height);

    var out: [8]u8 = @splat(0);
    try testing.expectEqual(Err.ok, src.read.?(src.ctx, 1, 1, 2, 2, &out, 4));
    try testing.expectEqual([8]u8{ 21, 22, 0, 0, 31, 32, 0, 0 }, out);

    try testing.expectEqual(Err.out_of_range, mem.memReadImpl(&ms, 3, 0, 2, 1, &out, 2));
    try testing.expectEqual(Err.out_of_range, mem.memReadImpl(&ms, 0, 2, 1, 2, &out, 1));
    try testing.expectEqual(Err.invalid_arg, mem.memReadImpl(&ms, 0, 0, 3, 1, &out, 2));
    try testing.expectEqual(Err.ok, mem.memReadImpl(&ms, 4, 3, 0, 0, &out, 0));
}
