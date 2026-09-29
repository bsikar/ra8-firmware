//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the `ra8_gfx` lifecycle core: the pitch arithmetic, the two
//! validation paths and the two state shapes a bind and a teardown produce.

const std = @import("std");
const bind = @import("bind");
const core = bind.core;

test "packedRow multiplies the edge by the format's bytes per pixel" {
    try std.testing.expectEqual(@as(u32, 200), bind.packedRow(100, core.format.rgb565));
    try std.testing.expectEqual(@as(u32, 300), bind.packedRow(100, core.format.rgb888));
    try std.testing.expectEqual(@as(u32, 400), bind.packedRow(100, core.format.argb8888));
}

test "packedRow holds at the widest edge without truncating" {
    try std.testing.expectEqual(
        @as(u32, 4096 * 4),
        bind.packedRow(core.dim.max, core.format.argb8888),
    );
}

test "dimOk accepts the closed range and nothing outside it" {
    try std.testing.expect(bind.dimOk(core.dim.min));
    try std.testing.expect(bind.dimOk(core.dim.max));
    try std.testing.expect(bind.dimOk(320));
    try std.testing.expect(!bind.dimOk(0));
    try std.testing.expect(!bind.dimOk(core.dim.max + 1));
}

test "checkDims passes a supported format inside the range" {
    try std.testing.expectEqual(core.err.ok, bind.checkDims(320, 240, core.format.rgb565));
    try std.testing.expectEqual(core.err.ok, bind.checkDims(1, 1, core.format.argb8888));
}

test "checkDims rejects a zero or oversized edge" {
    try std.testing.expectEqual(
        core.err.invalid_arg,
        bind.checkDims(0, 240, core.format.rgb565),
    );
    try std.testing.expectEqual(
        core.err.invalid_arg,
        bind.checkDims(320, 0, core.format.rgb565),
    );
    try std.testing.expectEqual(
        core.err.invalid_arg,
        bind.checkDims(core.dim.max + 1, 240, core.format.rgb565),
    );
    try std.testing.expectEqual(
        core.err.invalid_arg,
        bind.checkDims(320, core.dim.max + 1, core.format.rgb565),
    );
}

test "checkDims rejects a format this library does not render" {
    try std.testing.expectEqual(core.err.invalid_arg, bind.checkDims(320, 240, 0));
    try std.testing.expectEqual(core.err.invalid_arg, bind.checkDims(320, 240, 1));
    try std.testing.expectEqual(core.err.invalid_arg, bind.checkDims(320, 240, 5));
}

test "checkSurface accepts a densely packed descriptor" {
    const s: bind.Surface = .{
        .pixels = null,
        .w = 100,
        .h = 50,
        .stride_bytes = 200,
        .fmt = core.format.rgb565,
    };
    try std.testing.expectEqual(core.err.ok, bind.checkSurface(s));
}

test "checkSurface honours a padded row as given" {
    const s: bind.Surface = .{
        .pixels = null,
        .w = 100,
        .h = 50,
        .stride_bytes = 256,
        .fmt = core.format.rgb565,
    };
    try std.testing.expectEqual(core.err.ok, bind.checkSurface(s));
}

test "checkSurface rejects a pitch narrower than one packed row" {
    const s: bind.Surface = .{
        .pixels = null,
        .w = 100,
        .h = 50,
        .stride_bytes = 199,
        .fmt = core.format.rgb565,
    };
    try std.testing.expectEqual(core.err.invalid_arg, bind.checkSurface(s));
}

test "checkSurface reports the dimension error before looking at the pitch" {
    const s: bind.Surface = .{
        .pixels = null,
        .w = 0,
        .h = 50,
        .stride_bytes = 0,
        .fmt = core.format.rgb565,
    };
    try std.testing.expectEqual(core.err.invalid_arg, bind.checkSurface(s));
}

test "bound populates the whole binding and defaults the clip to the buffer" {
    var pixels: [64]u8 = undefined;
    const state = bind.bound(&pixels, 8, 4, core.format.rgb565, 16);
    try std.testing.expect(state.fb != null);
    try std.testing.expectEqual(@as(u16, 8), state.width);
    try std.testing.expectEqual(@as(u16, 4), state.height);
    try std.testing.expectEqual(@as(u32, 16), state.pitch);
    try std.testing.expectEqual(core.format.rgb565, state.format);
    try std.testing.expectEqual(@as(u8, 2), state.bpp);
    try std.testing.expect(state.initialized);
    try std.testing.expectEqual(@as(i32, 0), state.clip_x0);
    try std.testing.expectEqual(@as(i32, 0), state.clip_y0);
    try std.testing.expectEqual(@as(i32, 8), state.clip_x1);
    try std.testing.expectEqual(@as(i32, 4), state.clip_y1);
}

test "bound keeps a padded pitch instead of recomputing width times bpp" {
    var pixels: [512]u8 = undefined;
    const state = bind.bound(&pixels, 8, 4, core.format.argb8888, 64);
    try std.testing.expectEqual(@as(u32, 64), state.pitch);
    try std.testing.expectEqual(@as(u8, 4), state.bpp);
}

test "bound sizes bpp from the format for every supported format" {
    var pixels: [16]u8 = undefined;
    try std.testing.expectEqual(
        @as(u8, 3),
        bind.bound(&pixels, 2, 2, core.format.rgb888, 6).bpp,
    );
    try std.testing.expectEqual(
        @as(u8, 4),
        bind.bound(&pixels, 2, 2, core.format.argb8888, 8).bpp,
    );
}

test "released drops the pointer, zeroes the geometry and empties the clip" {
    var pixels: [64]u8 = undefined;
    const state = bind.released(bind.bound(&pixels, 8, 4, core.format.argb8888, 32));
    try std.testing.expect(state.fb == null);
    try std.testing.expectEqual(@as(u16, 0), state.width);
    try std.testing.expectEqual(@as(u16, 0), state.height);
    try std.testing.expectEqual(@as(u32, 0), state.pitch);
    try std.testing.expectEqual(@as(u8, 0), state.bpp);
    try std.testing.expect(!state.initialized);
    try std.testing.expectEqual(@as(i32, 0), state.clip_x1);
    try std.testing.expectEqual(@as(i32, 0), state.clip_y1);
}

test "released leaves the format as it was, the way the C did" {
    var pixels: [64]u8 = undefined;
    const bound = bind.bound(&pixels, 8, 4, core.format.argb8888, 32);
    try std.testing.expectEqual(core.format.argb8888, bind.released(bound).format);
}

test "a packed surface binds to exactly what the positional form gives" {
    var pixels: [400]u8 = undefined;
    const positional = bind.bound(
        &pixels,
        10,
        10,
        core.format.rgb565,
        bind.packedRow(10, core.format.rgb565),
    );
    const s: bind.Surface = .{
        .pixels = &pixels,
        .w = 10,
        .h = 10,
        .stride_bytes = 20,
        .fmt = core.format.rgb565,
    };
    try std.testing.expectEqual(core.err.ok, bind.checkSurface(s));
    const surface = bind.bound(&pixels, s.w, s.h, s.fmt, s.stride_bytes);
    try std.testing.expectEqual(positional.pitch, surface.pitch);
    try std.testing.expectEqual(positional.bpp, surface.bpp);
    try std.testing.expectEqual(positional.clip_x1, surface.clip_x1);
    try std.testing.expectEqual(positional.clip_y1, surface.clip_y1);
}
