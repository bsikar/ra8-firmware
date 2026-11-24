// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! In-memory 8-bit grayscale source adapter for the zoom viewport.
//!
//! Binds a caller-owned gray8 pixel buffer (`zoom_mem_src_t`) onto `zoom_source_t`
//! so decoded EPUB raster figures can be panned, zoomed, and blue-noise dithered
//! by `zoom_view_t` without heap allocations or flash paging.

const std = @import("std");
const zoom = @import("zoom.zig");

const Err = zoom.Err;
const Source = zoom.Source;

/// Caller-owned strided 8-bit grayscale pixel plane (`zoom_mem_src_t`).
pub const MemSrc = extern struct {
    pixels: ?[*]const u8 = null,
    width: u32 = 0,
    height: u32 = 0,
    stride: u32 = 0,
};

fn memSrcValid(src: *const MemSrc) bool {
    return zoom.dimOk(src.width) and zoom.dimOk(src.height) and (src.stride >= src.width);
}

/// Validate and populate a caller-owned `MemSrc` descriptor (`zoom_mem_src_init`).
pub fn memSrcInit(out: ?*MemSrc, pixels: ?[*]const u8, width: u32, height: u32, stride: u32) Err {
    if (out == null) {
        zoom.logErr("mem src out must not be nullptr");
        return .null_ptr;
    }
    if (pixels == null) {
        zoom.logErr("mem src pixels must not be nullptr");
        return .null_ptr;
    }
    if (!zoom.dimOk(width) or !zoom.dimOk(height)) {
        zoom.logErr("mem src extent out of range");
        return .invalid_arg;
    }
    const eff_stride: u32 = if (stride == 0) width else stride;
    if (eff_stride < width) {
        zoom.logErr("mem src stride narrower than width");
        return .invalid_arg;
    }
    out.?.* = .{
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = eff_stride,
    };
    return .ok;
}

/// Bind a `MemSrc` onto a `zoom_source_t` (`zoom_mem_src_bind`).
pub fn memSrcBind(src: ?*const MemSrc, out: ?*Source) Err {
    if (src == null) {
        zoom.logErr("mem src must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        zoom.logErr("source out must not be nullptr");
        return .null_ptr;
    }
    if (src.?.pixels == null) {
        zoom.logErr("mem src pixels must not be nullptr");
        return .null_ptr;
    }
    if (!memSrcValid(src.?)) {
        zoom.logErr("mem src invalid extent or stride");
        return .invalid_arg;
    }
    return zoom.sourceInit(out, memReadAdapter, @constCast(src.?), src.?.width, src.?.height);
}

fn memReadAdapter(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    return memReadImpl(ctx, x, y, w, h, out, out_stride);
}

/// Read a clipped sub-rectangle out of a bound `MemSrc` (`zoom_mem_read`).
pub fn memReadImpl(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) Err {
    if (ctx == null) {
        zoom.logErr("mem read ctx must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        zoom.logErr("mem read out must not be nullptr");
        return .null_ptr;
    }
    const src: *const MemSrc = @ptrCast(@alignCast(ctx.?));
    if (src.pixels == null) {
        zoom.logErr("mem src pixels must not be nullptr");
        return .null_ptr;
    }
    if (!memSrcValid(src)) {
        return .invalid_arg;
    }
    if ((w > 0) and (out_stride < w)) {
        return .invalid_arg;
    }
    const end_x: u64 = @as(u64, x) + @as(u64, w);
    const end_y: u64 = @as(u64, y) + @as(u64, h);
    if ((end_x > src.width) or (end_y > src.height)) {
        return .out_of_range;
    }
    if (w == 0 or h == 0) {
        return .ok;
    }
    const px = src.pixels.?;
    const dst = out.?;
    const copy_w: usize = @intCast(w);
    const src_stride: usize = @intCast(src.stride);
    const dst_stride: usize = @intCast(out_stride);
    var row: u32 = 0;
    while (row < h) : (row += 1) {
        const s_off: usize = (@as(usize, @intCast(y + row)) * src_stride) + @as(usize, @intCast(x));
        const d_off: usize = @as(usize, @intCast(row)) * dst_stride;
        @memcpy(dst[d_off .. d_off + copy_w], px[s_off .. s_off + copy_w]);
    }
    return .ok;
}

const testing = std.testing;

test "memSrcInit validates pointers, bounds and default stride" {
    var px = [_]u8{ 1, 2, 3, 4 };
    var ms = MemSrc{};
    try testing.expectEqual(Err.null_ptr, memSrcInit(null, &px, 2, 2, 0));
    try testing.expectEqual(Err.null_ptr, memSrcInit(&ms, null, 2, 2, 0));
    try testing.expectEqual(Err.invalid_arg, memSrcInit(&ms, &px, 0, 2, 0));
    try testing.expectEqual(Err.invalid_arg, memSrcInit(&ms, &px, zoom.dim_max + 1, 2, 0));
    try testing.expectEqual(Err.invalid_arg, memSrcInit(&ms, &px, 4, 2, 3));
    try testing.expectEqual(Err.ok, memSrcInit(&ms, &px, 2, 2, 0));
    try testing.expectEqual(@as(u32, 2), ms.stride);
}

test "memSrcBind and memReadImpl copy strided rectangles and guard out-of-range" {
    // 4x3 logical image packed in stride 6 (2 bytes of row padding).
    const px = [_]u8{
        10, 11, 12, 13, 0xff, 0xff,
        20, 21, 22, 23, 0xff, 0xff,
        30, 31, 32, 33, 0xff, 0xff,
    };
    var ms = MemSrc{};
    try testing.expectEqual(Err.ok, memSrcInit(&ms, &px, 4, 3, 6));
    var src = Source{};
    try testing.expectEqual(Err.ok, memSrcBind(&ms, &src));
    try testing.expectEqual(@as(u32, 4), src.width);
    try testing.expectEqual(@as(u32, 3), src.height);

    var out: [8]u8 = @splat(0);
    try testing.expectEqual(Err.ok, src.read.?(src.ctx, 1, 1, 2, 2, &out, 4));
    try testing.expectEqual([8]u8{ 21, 22, 0, 0, 31, 32, 0, 0 }, out);

    try testing.expectEqual(Err.out_of_range, memReadImpl(&ms, 3, 0, 2, 1, &out, 2));
    try testing.expectEqual(Err.out_of_range, memReadImpl(&ms, 0, 2, 1, 2, &out, 1));
    try testing.expectEqual(Err.invalid_arg, memReadImpl(&ms, 0, 0, 3, 1, &out, 2));
    try testing.expectEqual(Err.ok, memReadImpl(&ms, 4, 3, 0, 0, &out, 0));
}
