// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Test-only render seam and shared SVG test helpers for `apps/shared_libs/reflow`.

const reflow = @import("reflow");
const svg = reflow.svg;

const k_ra8_ok: c_int = 0;
const k_ra8_err_not_supported: c_int = 0x107;

export fn ra8_gfx_pixel(x: c_int, y: c_int, color: u32) callconv(.c) c_int {
    shapeRecord(4, x, y, 0, 0, color, 0);
    return k_ra8_ok;
}

export fn ra8_glyph_atlas_init(atlas: *anyopaque, cfg: *const anyopaque) callconv(.c) c_int {
    _ = atlas;
    _ = cfg;
    return k_ra8_err_not_supported;
}

export fn ra8_glyph_atlas_get(atlas: *anyopaque, key: *const anyopaque, out_glyph: *anyopaque) callconv(.c) c_int {
    _ = atlas;
    _ = key;
    _ = out_glyph;
    return k_ra8_err_not_supported;
}

export fn ra8_glyph_atlas_put(atlas: *anyopaque, bitmap: [*]const u8) callconv(.c) c_int {
    _ = atlas;
    _ = bitmap;
    return k_ra8_ok;
}

/// One recorded `ra8_gfx` draw call (`kind`: 1=line, 2=rect, 3=circle, 4=pixel).
pub const ShapeCall = struct {
    kind: u8,
    a: i32,
    b: i32,
    c: i32,
    d: i32,
    color: u32,
    filled: u8,
};

pub var shape_calls: [512]ShapeCall = @splat(.{ .kind = 0, .a = 0, .b = 0, .c = 0, .d = 0, .color = 0, .filled = 0 });
pub var shape_call_n: usize = 0;

/// Empty the recorded draw-call buffer before a draw assertion.
pub fn shapeCallsReset() void {
    shape_call_n = 0;
}

/// Slice of draw calls recorded since the last `shapeCallsReset()`.
pub fn calls() []ShapeCall {
    return shape_calls[0..shape_call_n];
}

/// Identity 64x64 `SvgXform` fixture shared across SVG unit test modules.
pub fn identityXform() svg.SvgXform {
    return .{
        .bx = 0,
        .by = 0,
        .bw = 64,
        .bh = 64,
        .vx = 0,
        .vy = 0,
        .vw = 64,
        .vh = 64,
        .ua = 1.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 1.0,
        .ue = 0.0,
        .uf = 0.0,
        .grads = null,
    };
}

/// Linear two-stop gradient fixture shared across SVG shape and path tests.
pub fn gradLinear(off0: f32, col0: u32, off1: f32, col1: u32) svg.SvgGrad {
    var g = svg.SvgGrad{
        .kind = svg.k_svg_grad_linear,
        .nstops = 2,
        .id = @splat(0),
        .x1 = 0.0,
        .y1 = 0.0,
        .x2 = 1.0,
        .y2 = 0.0,
        .stops = @splat(.{ .off = 0, .col = 0 }),
    };
    g.stops[0] = .{ .off = off0, .col = col0 };
    g.stops[1] = .{ .off = off1, .col = col1 };
    return g;
}

fn shapeRecord(kind: u8, a: i32, b: i32, c: i32, d: i32, color: u32, filled: u8) void {
    if (shape_call_n < shape_calls.len) {
        shape_calls[shape_call_n] = .{ .kind = kind, .a = a, .b = b, .c = c, .d = d, .color = color, .filled = filled };
        shape_call_n += 1;
    }
}

export fn ra8_gfx_line(x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) u16 {
    shapeRecord(1, x0, y0, x1, y1, color, 0);
    return 0;
}

export fn ra8_gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u32, filled: u8) callconv(.c) u16 {
    shapeRecord(2, x, y, w, h, color, filled);
    return 0;
}

export fn ra8_gfx_circle(cx: i32, cy: i32, r: i32, color: u32, filled: u8) callconv(.c) u16 {
    shapeRecord(3, cx, cy, r, 0, color, filled);
    return 0;
}
