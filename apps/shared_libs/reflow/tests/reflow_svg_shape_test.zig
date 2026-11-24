// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for SVG shape rasterisers and gradients (`src/reflow_svg_shape.zig`).

const std = @import("std");
const shape = @import("reflow").svg_shape;
const seam = @import("reflow_render_test_seam.zig");

const testing = std.testing;
const svg = shape.svg;
const xf = shape.xf;
const SvgGrad = svg.SvgGrad;

test "col lerp endpoints and mid" {
    try testing.expectEqual(@as(u32, 0x000000), shape.internal_col_lerp(0x000000, 0xFFFFFF, 0.0));
    try testing.expectEqual(@as(u32, 0xFFFFFF), shape.internal_col_lerp(0x000000, 0xFFFFFF, 1.0));
    try testing.expectEqual(@as(u32, 0x7F7F7F), shape.internal_col_lerp(0x000000, 0xFFFFFF, 0.5));
    try testing.expectEqual(@as(u32, 0xFF0000), shape.internal_col_lerp(0xFF0000, 0xFF0000, 0.7));
}

test "grad eval clamps and interpolates" {
    var empty = SvgGrad{
        .kind = svg.k_svg_grad_linear,
        .nstops = 0,
        .id = @splat(0),
        .x1 = 0,
        .y1 = 0,
        .x2 = 0,
        .y2 = 0,
        .stops = @splat(.{ .off = 0, .col = 0 }),
    };
    try testing.expectEqual(svg.k_svg_def_fill, shape.internal_grad_eval(&empty, 0.5, 0.5));

    const g = seam.gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF);
    try testing.expectEqual(@as(u32, 0x000000), shape.internal_grad_eval(&g, 0.0, 0.5));
    try testing.expectEqual(@as(u32, 0xFFFFFF), shape.internal_grad_eval(&g, 1.0, 0.5));
    try testing.expectEqual(@as(u32, 0x000000), shape.internal_grad_eval(&g, -0.5, 0.5));
    try testing.expectEqual(@as(u32, 0xFFFFFF), shape.internal_grad_eval(&g, 1.5, 0.5));
    try testing.expectEqual(@as(u32, 0x7F7F7F), shape.internal_grad_eval(&g, 0.5, 0.5));

    var rad = seam.gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF);
    rad.kind = svg.k_svg_grad_radial;
    rad.x1 = 0.5;
    rad.y1 = 0.5;
    rad.x2 = 0.5;
    try testing.expectEqual(@as(u32, 0x000000), shape.internal_grad_eval(&rad, 0.5, 0.5));
    try testing.expectEqual(@as(u32, 0xFFFFFF), shape.internal_grad_eval(&rad, 1.0, 0.5));
}

test "scanline x crossings and sort" {
    const xs = [_]i32{ 0, 8, 8, 0 };
    const ys = [_]i32{ 0, 0, 8, 8 };
    var xint: [shape.k_svg_poly_max]i32 = @splat(0);
    const m = shape.internal_scanline_x(&xs, &ys, 4, &xint);
    try testing.expectEqual(@as(i32, 2), m);
    shape.internal_sort_i32(xint[0..@intCast(m)]);
    try testing.expectEqual(@as(i32, 0), xint[0]);
    try testing.expectEqual(@as(i32, 8), xint[1]);
    try testing.expectEqual(@as(i32, 0), shape.internal_scanline_x(&xs, &ys, -1, &xint));
    try testing.expectEqual(@as(i32, 0), shape.internal_scanline_x(&xs, &ys, 8, &xint));
}

test "fill poly emits one span per row" {
    seam.shapeCallsReset();
    const xs = [_]i32{ 2, 10, 10, 2 };
    const ys = [_]i32{ 2, 2, 10, 10 };
    shape.priv_ra8_svgp_fill_poly(&xs, &ys, 4, 0x00FF00);

    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 8), cs.len);
    try testing.expectEqual(@as(u8, 1), cs[0].kind);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 10), cs[0].c);
    try testing.expectEqual(@as(i32, 2), cs[0].d);
    try testing.expectEqual(@as(u32, 0x00FF00), cs[0].color);
    try testing.expectEqual(@as(i32, 2), cs[7].a);
    try testing.expectEqual(@as(i32, 9), cs[7].b);
    try testing.expectEqual(@as(i32, 10), cs[7].c);
    try testing.expectEqual(@as(i32, 9), cs[7].d);

    seam.shapeCallsReset();
    const two_x = [_]i32{ 2, 10, 10 };
    const two_y = [_]i32{ 2, 2, 10 };
    shape.priv_ra8_svgp_fill_poly(&two_x, &two_y, 2, 0x00FF00);
    try testing.expectEqual(@as(usize, 0), seam.calls().len);
}

test "fill poly grad evaluates per pixel" {
    seam.shapeCallsReset();
    const xs = [_]i32{ 0, 10, 10, 0 };
    const ys = [_]i32{ 0, 0, 10, 10 };
    const g = seam.gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF);
    shape.priv_ra8_svgp_fill_poly_grad(&xs, &ys, 4, &g);

    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 110), cs.len);
    try testing.expectEqual(@as(u8, 4), cs[0].kind);
    try testing.expectEqual(@as(i32, 0), cs[0].a);
    try testing.expectEqual(@as(i32, 0), cs[0].b);
    try testing.expectEqual(@as(u32, 0x000000), cs[0].color);
    const mid = cs[5 * 11 + 5];
    try testing.expectEqual(@as(i32, 5), mid.a);
    try testing.expectEqual(@as(i32, 5), mid.b);
    try testing.expectEqual(@as(u32, 0x7F7F7F), mid.color);
    try testing.expectEqual(@as(u32, 0xFFFFFF), cs[5 * 11 + 10].color);
}

test "parse points maps and bounds" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const v = "0,0 4, 0 4,4";
    const n = shape.internal_parse_points(v, v.len, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 0), xs[0]);
    try testing.expectEqual(@as(i32, 4), xs[1]);
    try testing.expectEqual(@as(i32, 4), ys[2]);

    const long = try testing.allocator.alloc(u8, (shape.k_svg_poly_max + 8) * 8);
    defer testing.allocator.free(long);
    for (0..shape.k_svg_poly_max + 8) |i| {
        _ = try std.fmt.bufPrint(long[i * 8 ..][0..8], "{d},{d} ", .{ i, i });
    }
    const capped = shape.internal_parse_points(long.ptr, long.len, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, @intCast(shape.k_svg_poly_max)), capped);
}

test "draw rect fast path and gate" {
    seam.shapeCallsReset();
    const tag = "<rect x=\"2\" y=\"2\" width=\"4\" height=\"4\" fill=\"#FF0000\"/>";
    shape.priv_ra8_svgp_draw_rect(tag.ptr, tag.len, &seam.identityXform());
    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 1), cs.len);
    try testing.expectEqual(@as(u8, 2), cs[0].kind);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 4), cs[0].c);
    try testing.expectEqual(@as(i32, 4), cs[0].d);
    try testing.expectEqual(@as(u32, 0xFF0000), cs[0].color);
    try testing.expectEqual(@as(u8, 1), cs[0].filled);

    seam.shapeCallsReset();
    const none = "<rect x=\"30\" y=\"30\" width=\"4\" height=\"4\" fill=\"none\"/>";
    shape.priv_ra8_svgp_draw_rect(none.ptr, none.len, &seam.identityXform());
    try testing.expectEqual(@as(usize, 0), seam.calls().len);
}

test "draw rect maps under shear" {
    seam.shapeCallsReset();
    var t2 = seam.identityXform();
    const xsh = "transform=\"skewX(45)\"";
    xf.priv_ra8_svgp_apply_xform(&t2, xsh.ptr, xsh.len);
    const tag = "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"#00FF00\"/>";
    shape.priv_ra8_svgp_draw_rect(tag.ptr, tag.len, &t2);
    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 4), cs.len);
    try testing.expectEqual(@as(i32, 0), cs[0].a);
    try testing.expectEqual(@as(i32, 0), cs[0].b);
    try testing.expectEqual(@as(i32, 4), cs[0].c);
    try testing.expectEqual(@as(i32, 0), cs[0].d);
    try testing.expectEqual(@as(i32, 3), cs[3].a);
    try testing.expectEqual(@as(i32, 3), cs[3].b);
    try testing.expectEqual(@as(i32, 7), cs[3].c);
    try testing.expectEqual(@as(i32, 3), cs[3].d);
}

test "draw circle fast path and ngon" {
    seam.shapeCallsReset();
    const tag = "<circle cx=\"8\" cy=\"8\" r=\"4\" fill=\"#FF0000\"/>";
    shape.priv_ra8_svgp_draw_circle(tag.ptr, tag.len, &seam.identityXform());
    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 1), cs.len);
    try testing.expectEqual(@as(u8, 3), cs[0].kind);
    try testing.expectEqual(@as(i32, 8), cs[0].a);
    try testing.expectEqual(@as(i32, 8), cs[0].b);
    try testing.expectEqual(@as(i32, 4), cs[0].c);
    try testing.expectEqual(@as(u32, 0xFF0000), cs[0].color);
    try testing.expectEqual(@as(u8, 1), cs[0].filled);

    seam.shapeCallsReset();
    var t = seam.identityXform();
    const xsh = "transform=\"rotate(45)\"";
    xf.priv_ra8_svgp_apply_xform(&t, xsh.ptr, xsh.len);
    shape.priv_ra8_svgp_draw_circle(tag.ptr, tag.len, &t);
    try testing.expect(seam.calls().len > 0);
    for (seam.calls()) |c| {
        try testing.expectEqual(@as(u8, 1), c.kind);
    }
}

test "draw line uses stroke" {
    seam.shapeCallsReset();
    const tag = "<line x1=\"2\" y1=\"2\" x2=\"10\" y2=\"2\" stroke=\"#FF0000\"/>";
    shape.priv_ra8_svgp_draw_line(tag.ptr, tag.len, &seam.identityXform());
    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 1), cs.len);
    try testing.expectEqual(@as(u8, 1), cs[0].kind);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 10), cs[0].c);
    try testing.expectEqual(@as(i32, 2), cs[0].d);

    seam.shapeCallsReset();
    const bare = "<line x1=\"2\" y1=\"20\" x2=\"10\" y2=\"20\"/>";
    shape.priv_ra8_svgp_draw_line(bare.ptr, bare.len, &seam.identityXform());
    try testing.expectEqual(@as(usize, 0), seam.calls().len);
}

test "draw polygon and polyline" {
    seam.shapeCallsReset();
    const poly = "<polygon fill=\"#00FF00\" points=\"2,2 10,2 10,10 2,10\"/>";
    shape.priv_ra8_svgp_draw_polygon(poly.ptr, poly.len, &seam.identityXform());
    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 8), cs.len);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 10), cs[0].c);
    try testing.expectEqual(@as(i32, 2), cs[0].d);

    seam.shapeCallsReset();
    const bare = "<polygon fill=\"#00FF00\"/>";
    shape.priv_ra8_svgp_draw_polygon(bare.ptr, bare.len, &seam.identityXform());
    try testing.expectEqual(@as(usize, 0), seam.calls().len);

    seam.shapeCallsReset();
    const line = "<polyline stroke=\"#FF0000\" points=\"2,30 10,30 10,40\"/>";
    shape.priv_ra8_svgp_draw_polyline(line.ptr, line.len, &seam.identityXform());
    const cs2 = seam.calls();
    try testing.expectEqual(@as(usize, 2), cs2.len);
    try testing.expectEqual(@as(i32, 10), cs2[1].a);
    try testing.expectEqual(@as(i32, 30), cs2[1].b);
    try testing.expectEqual(@as(i32, 10), cs2[1].c);
    try testing.expectEqual(@as(i32, 40), cs2[1].d);
}

test "arc flatten degenerate and quarter" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);

    var n: i32 = 0;
    const args0 = [_]i32{ 0, 0, 0, 0, 1, 0, 0 };
    shape.priv_ra8_svgp_flatten_arc(&t, .{ .x = 0, .y = 0 }, &args0, .{ .x = 10, .y = 0 }, &xs, &ys, &n);
    try testing.expectEqual(@as(i32, 1), n);
    try testing.expectEqual(@as(i32, 10), xs[0]);

    n = 0;
    const args = [_]i32{ 5, 5, 0, 0, 1, 10, 0 };
    shape.priv_ra8_svgp_flatten_arc(&t, .{ .x = 0, .y = 0 }, &args, .{ .x = 10, .y = 0 }, &xs, &ys, &n);
    try testing.expect(n > 1);
    try testing.expect(n <= @as(i32, @intCast(shape.k_svg_poly_max)));
    try testing.expectEqual(@as(i32, 10), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 0), ys[@intCast(n - 1)]);
    try testing.expect(ys[@intCast(@divTrunc(n, 2))] < 0);
}

test "arc center solves the quarter circle" {
    const a = shape.internal_arc_center(.{ .x = 0, .y = 0 }, .{ .x = 10, .y = 0 }, 5, 5, 0, false, true);
    try testing.expect(a.ok);
    try testing.expectEqual(@as(f32, 5.0), a.cx);
    try testing.expectEqual(@as(f32, 0.0), a.cy);

    const d = shape.internal_arc_center(.{ .x = 3, .y = 3 }, .{ .x = 3, .y = 3 }, 5, 5, 0, false, true);
    try testing.expect(!d.ok);
}

test "arc segs bounded" {
    try testing.expectEqual(@as(i32, 1), shape.internal_arc_segs(0.0));
    try testing.expectEqual(@as(i32, 2), shape.internal_arc_segs(0.4));
    try testing.expectEqual(shape.k_svg_arc_seg_max, shape.internal_arc_segs(100.0));
    try testing.expectEqual(shape.k_svg_arc_seg_max, shape.internal_arc_segs(-100.0));
}
