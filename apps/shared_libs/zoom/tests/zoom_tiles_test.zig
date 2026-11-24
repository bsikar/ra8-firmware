// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the tiled-atlas zoom source adapter (`src/zoom_tiles.zig`).

const std = @import("std");
const tiles = @import("tiles");
const stubs = @import("zoom_stubs.zig");

const T = stubs.TileStubs(tiles);

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
    _ = T;
}

const zoom = tiles.zoom;
const Err = zoom.Err;
const Source = zoom.Source;
const View = zoom.View;
const TileSrc = tiles.TileSrc;

test "srcInit rejects null binding and null cache" {
    T.resetFixture();
    var ts = std.mem.zeroes(TileSrc);
    try std.testing.expectEqual(Err.null_ptr, tiles.tileSrcInit(null, T.testCache(), 1, 1, 1, 1, 1));
    try std.testing.expectEqual(Err.null_ptr, tiles.tileSrcInit(&ts, null, 1, 1, 1, 1, 1));
}

test "srcInit rejects a zero extent and zero tile edges" {
    T.resetFixture();
    var ts = std.mem.zeroes(TileSrc);
    try std.testing.expectEqual(Err.invalid_arg, tiles.tileSrcInit(&ts, T.testCache(), 1, 0, 90, 32, 32));
    try std.testing.expectEqual(Err.invalid_arg, tiles.tileSrcInit(&ts, T.testCache(), 1, 100, 0, 32, 32));
    try std.testing.expectEqual(Err.invalid_arg, tiles.tileSrcInit(&ts, T.testCache(), 1, 100, 90, 0, 32));
    try std.testing.expectEqual(Err.invalid_arg, tiles.tileSrcInit(&ts, T.testCache(), 1, 100, 90, 32, 0));
}

test "srcInit rejects a derived grid beyond 16-bit indices" {
    T.resetFixture();
    var ts = std.mem.zeroes(TileSrc);
    try std.testing.expectEqual(Err.invalid_size, tiles.tileSrcInit(&ts, T.testCache(), 1, 70000, 1, 1, 1));
}

test "srcInit derives the tile grid from the extent" {
    T.resetFixture();
    var ts = std.mem.zeroes(TileSrc);
    try std.testing.expectEqual(Err.ok, tiles.tileSrcInit(&ts, T.testCache(), 42, 100, 90, 32, 32));
    try std.testing.expectEqual(@as(u32, 42), ts.image_id);
    try std.testing.expectEqual(@as(u16, 4), ts.tile_cols);
    try std.testing.expectEqual(@as(u16, 3), ts.tile_rows);
}

test "bind rejects nulls and an uninitialised binding" {
    T.resetFixture();
    var ts = std.mem.zeroes(TileSrc);
    var src = Source{};
    try std.testing.expectEqual(Err.null_ptr, tiles.tileSrcBind(null, &src));
    try std.testing.expectEqual(Err.null_ptr, tiles.tileSrcBind(&ts, null));
    try std.testing.expectEqual(Err.null_ptr, tiles.tileSrcBind(&ts, &src));
    try std.testing.expectEqual(Err.ok, tiles.tileSrcInit(&ts, T.testCache(), 42, 100, 90, 32, 32));
    try std.testing.expectEqual(Err.ok, tiles.tileSrcBind(&ts, &src));
    try std.testing.expect(src.read != null);
    try std.testing.expectEqual(@as(u32, 100), src.width);
    try std.testing.expectEqual(@as(u32, 90), src.height);
    try std.testing.expect(src.ctx == @as(?*anyopaque, @ptrCast(&ts)));
}

test "read rejects a null context and an uninitialised binding" {
    T.resetFixture();
    var out: [100]u8 = undefined;
    try std.testing.expectEqual(Err.null_ptr, tiles.tileRead(null, 0, 0, 1, 1, &out, 100));
    var ts = std.mem.zeroes(TileSrc);
    try std.testing.expectEqual(Err.null_ptr, tiles.tileRead(@ptrCast(&ts), 0, 0, 1, 1, &out, 100));
}

test "read rejects an empty rect, a narrow stride, and an out-of-range rect" {
    T.resetFixture();
    var ts = T.boundFixture();
    var out: [100 * 90]u8 = undefined;
    try std.testing.expectEqual(Err.invalid_arg, tiles.tileRead(&ts, 0, 0, 0, 10, &out, 100));
    try std.testing.expectEqual(Err.invalid_arg, tiles.tileRead(&ts, 0, 0, 10, 10, &out, 9));
    try std.testing.expectEqual(Err.out_of_range, tiles.tileRead(&ts, 0, 0, 101, 10, &out, 101));
    try std.testing.expectEqual(Err.out_of_range, tiles.tileRead(&ts, 0, 0, 10, 91, &out, 100));
}

test "read assembles the covering tiles' overlaps" {
    T.resetFixture();
    var ts = T.boundFixture();
    var out: [100 * 90]u8 = @splat(0xAA);
    try std.testing.expectEqual(Err.ok, tiles.tileRead(&ts, 0, 0, 100, 90, &out, 100));
    var sy: usize = 0;
    while (sy < 90) : (sy += 1) {
        var sx: usize = 0;
        while (sx < 100) : (sx += 1) {
            try std.testing.expectEqual(T.expectedPixel(sx, sy), out[sy * 100 + sx]);
        }
    }
}

test "read releases every tile it acquires" {
    T.resetFixture();
    var ts = T.boundFixture();
    var out: [100 * 90]u8 = @splat(0);
    try std.testing.expectEqual(Err.ok, tiles.tileRead(&ts, 10, 5, 80, 60, &out, 100));
    try std.testing.expectEqual(@as(u32, 9), T.get_calls);
    try std.testing.expectEqual(@as(u32, 9), T.put_calls);
}

test "read propagates a failed tile fetch without pinning a cell" {
    T.resetFixture();
    T.get_err = .no_mem;
    var ts = T.boundFixture();
    var out: [100 * 90]u8 = @splat(0);
    try std.testing.expectEqual(Err.no_mem, tiles.tileRead(&ts, 10, 5, 80, 60, &out, 100));
    try std.testing.expectEqual(@as(u32, 1), T.get_calls);
    try std.testing.expectEqual(@as(u32, 0), T.put_calls);
}

test "read rejects a tile decoded to an unexpected extent and still releases it" {
    T.resetFixture();
    T.mismatch = true;
    var ts = T.boundFixture();
    var out: [100 * 90]u8 = @splat(0);
    try std.testing.expectEqual(Err.invalid_size, tiles.tileRead(&ts, 10, 5, 80, 60, &out, 100));
    try std.testing.expectEqual(@as(u32, 1), T.get_calls);
    try std.testing.expectEqual(@as(u32, 1), T.put_calls);
}

test "prefetch rejects nulls and an uninitialised binding" {
    T.resetFixture();
    var ts = std.mem.zeroes(TileSrc);
    var warmed: u16 = 0;
    var v = View{ .active = true };
    try std.testing.expectEqual(Err.null_ptr, tiles.tilesPrefetch(null, &v, .left, 4, &warmed));
    try std.testing.expectEqual(Err.null_ptr, tiles.tilesPrefetch(&ts, null, .left, 4, &warmed));
    try std.testing.expectEqual(Err.null_ptr, tiles.tilesPrefetch(&ts, &v, .left, 4, &warmed));
}

test "prefetch forwards the visible window and the pan direction" {
    T.resetFixture();
    var ts = T.boundFixture();
    var img: [4]u8 = undefined;
    var v = View{
        .src = .{ .read = null, .ctx = @ptrCast(&img), .width = 100, .height = 90 },
        .dst = .{ .w = 64, .h = 48 },
        .anchor_x = 0,
        .anchor_y = 0,
        .scale = 1,
        .active = true,
    };
    var warmed: u16 = 0;
    T.warmed = 7;
    try std.testing.expectEqual(Err.ok, tiles.tilesPrefetch(&ts, &v, .down, 4, &warmed));
    const ax = zoom.axis(0, 64, 1, 100);
    const ay = zoom.axis(0, 48, 1, 90);
    try std.testing.expectEqual(@as(u32, 42), T.pf_req.image_id);
    try std.testing.expectEqual(@as(u16, @intCast(ax.s0 / 32)), T.pf_req.view.tx0);
    try std.testing.expectEqual(@as(u16, @intCast((ax.s0 + ax.count - 1) / 32)), T.pf_req.view.tx1);
    try std.testing.expectEqual(@as(u16, @intCast(ay.s0 / 32)), T.pf_req.view.ty0);
    try std.testing.expectEqual(@as(u16, @intCast((ay.s0 + ay.count - 1) / 32)), T.pf_req.view.ty1);
    try std.testing.expectEqual(@as(u16, 4), T.pf_req.tile_cols);
    try std.testing.expectEqual(@as(u16, 3), T.pf_req.tile_rows);
    try std.testing.expectEqual(tiles.native_level, T.pf_req.zoom);
    try std.testing.expectEqual(@as(u16, 4), T.pf_req.max_tiles);
    try std.testing.expectEqual(@as(u8, @backingInt(zoom.Pan.down)), T.pf_req.dir);
    try std.testing.expectEqual(@as(u16, 7), warmed);
}

test "prefetch propagates a closed view" {
    T.resetFixture();
    var ts = T.boundFixture();
    var v = View{ .active = false };
    var warmed: u16 = 0;
    try std.testing.expectEqual(Err.invalid_state, tiles.tilesPrefetch(&ts, &v, .left, 4, &warmed));
}

test "prefetch propagates the cache's prefetch error" {
    T.resetFixture();
    T.pf_err = .no_mem;
    var ts = T.boundFixture();
    var v = View{
        .src = .{ .read = null, .ctx = null, .width = 100, .height = 90 },
        .dst = .{ .w = 64, .h = 48 },
        .scale = 1,
        .active = true,
    };
    var warmed: u16 = 0;
    try std.testing.expectEqual(Err.no_mem, tiles.tilesPrefetch(&ts, &v, .right, 4, &warmed));
}
