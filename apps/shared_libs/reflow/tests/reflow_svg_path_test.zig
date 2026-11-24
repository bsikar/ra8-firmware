// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the SVG `<path d="...">` parser and drawer (`src/reflow_svg_path.zig`).

const std = @import("std");
const path = @import("reflow").svg_path;
const seam = @import("reflow_render_test_seam.zig");

const testing = std.testing;
const svg = path.svg;
const shape = path.shape;

test "cmd argc table" {
    try testing.expectEqual(@as(i32, 2), path.internal_cmd_argc('m'));
    try testing.expectEqual(@as(i32, 2), path.internal_cmd_argc('l'));
    try testing.expectEqual(@as(i32, 2), path.internal_cmd_argc('t'));
    try testing.expectEqual(@as(i32, 1), path.internal_cmd_argc('h'));
    try testing.expectEqual(@as(i32, 1), path.internal_cmd_argc('v'));
    try testing.expectEqual(@as(i32, 6), path.internal_cmd_argc('c'));
    try testing.expectEqual(@as(i32, 4), path.internal_cmd_argc('s'));
    try testing.expectEqual(@as(i32, 4), path.internal_cmd_argc('q'));
    try testing.expectEqual(@as(i32, 7), path.internal_cmd_argc('a'));
    try testing.expectEqual(@as(i32, 0), path.internal_cmd_argc('z'));
    try testing.expectEqual(@as(i32, -1), path.internal_cmd_argc('w'));
}

test "path step h v and endpoint pair" {
    var cx: i32 = 0;
    var cy: i32 = 0;
    const args_h = [_]i32{8};
    path.internal_path_step('h', false, &args_h, 1, &cx, &cy);
    try testing.expectEqual(@as(i32, 8), cx);
    try testing.expectEqual(@as(i32, 0), cy);
    const args_v = [_]i32{5};
    path.internal_path_step('v', true, &args_v, 1, &cx, &cy);
    try testing.expectEqual(@as(i32, 8), cx);
    try testing.expectEqual(@as(i32, 5), cy);
    const args_pt = [_]i32{ 3, 4 };
    path.internal_path_step('l', true, &args_pt, 2, &cx, &cy);
    try testing.expectEqual(@as(i32, 11), cx);
    try testing.expectEqual(@as(i32, 9), cy);
    path.internal_path_step('l', false, &args_pt, 2, &cx, &cy);
    try testing.expectEqual(@as(i32, 3), cx);
    try testing.expectEqual(@as(i32, 4), cy);
}

test "bezier cubic and quad sample curves" {
    try testing.expectEqual(@as(f32, 0.0), path.internal_bezier1(0.0, 0, 0, 0, 1));
    try testing.expectEqual(@as(f32, 1.0), path.internal_bezier1(1.0, 0, 0, 0, 1));
    try testing.expectEqual(@as(f32, 0.125), path.internal_bezier1(0.5, 0, 0, 0, 1));
    try testing.expectEqual(@as(f32, 0.75), path.internal_bezier1(0.5, 0, 1, 1, 0));
    try testing.expectEqual(@as(f32, 1.0), path.internal_bezier_q1(0.5, 0, 1, 2));
}

test "parse m l and z exact" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 L 10,2 L 10,10 Z";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 2), xs[0]);
    try testing.expectEqual(@as(i32, 2), ys[0]);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 2), ys[1]);
    try testing.expectEqual(@as(i32, 10), xs[2]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "implicit repeats after m are line-tos" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 10,2 10,10";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "relative commands move and line" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "m 2,2 l 8,0 0,8";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 2), xs[0]);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "h and v shorthand" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 H 10 V 10";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 2), ys[1]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "cubic flatten hits the exact midpoint" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 C 4,0 8,10 12,10";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, @intCast(1 + path.k_svg_curve_seg)), n);
    try testing.expectEqual(@as(i32, 6), xs[6]);
    try testing.expectEqual(@as(i32, 5), ys[6]);
    try testing.expectEqual(@as(i32, 12), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 10), ys[@intCast(n - 1)]);
}

test "smooth cubic s reflects control" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 C 2,2 4,4 6,6 S 8,8 10,10";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, @intCast(1 + 2 * path.k_svg_curve_seg)), n);
    try testing.expectEqual(@as(i32, 10), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 10), ys[@intCast(n - 1)]);
}

test "smooth quad t reflects control" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 Q 4,4 8,8 T 16,16";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, @intCast(1 + 2 * path.k_svg_curve_seg)), n);
    try testing.expectEqual(@as(i32, 16), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 16), ys[@intCast(n - 1)]);
}

test "arc delegates to the shared flatten" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 A 5 5 0 0 1 10,0";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expect(n > 1);
    try testing.expectEqual(@as(i32, 0), xs[0]);
    try testing.expectEqual(@as(i32, 0), ys[0]);
    try testing.expectEqual(@as(i32, 10), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 0), ys[@intCast(n - 1)]);
}

test "malformed z repeat cannot spin" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const n1 = path.internal_parse_path("M 2,2 L 10,2 Z", &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 2), n1);
    const n2 = path.internal_parse_path("Z2", &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 0), n2);
}

test "unknown letter stops parsing" {
    const t = seam.identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 X 5";
    const n = path.internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 1), n);
    try testing.expectEqual(@as(i32, 2), xs[0]);
}

test "draw path gate fill none and missing d" {
    const t = seam.identityXform();
    const no_fill = "<path d=\"M 2,2 L 10,10\" fill=\"none\"/>";
    seam.shapeCallsReset();
    path.priv_ra8_svgp_draw_path(no_fill, no_fill.len, &t);
    try testing.expectEqual(@as(usize, 0), seam.calls().len);
    const no_d = "<path fill=\"#FF0000\"/>";
    path.priv_ra8_svgp_draw_path(no_d, no_d.len, &t);
    try testing.expectEqual(@as(usize, 0), seam.calls().len);
}

test "draw path solid fill spans" {
    seam.shapeCallsReset();
    const t = seam.identityXform();
    const s = "<path d=\"M 2,2 L 10,2 L 10,10 L 2,10\" fill=\"#FF0000\"/>";
    path.priv_ra8_svgp_draw_path(s, s.len, &t);
    const cs = seam.calls();
    try testing.expect(cs.len >= 8);
    try testing.expectEqual(@as(u8, 1), cs[0].kind);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 10), cs[0].c);
    try testing.expectEqual(@as(i32, 2), cs[0].d);
    try testing.expectEqual(@as(u32, 0xFF0000), cs[0].color);
}

test "draw path gradient fills per pixel" {
    seam.shapeCallsReset();
    var t = seam.identityXform();
    var grads = svg.SvgGrads{ .n = 1, .g = @splat(seam.gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF)) };
    grads.g[0].id[0] = 'g';
    t.grads = &grads;
    const s = "<path d=\"M 2,2 L 10,2 L 10,10 L 2,10\" fill=\"url(#g)\"/>";
    path.priv_ra8_svgp_draw_path(s, s.len, &t);
    const cs = seam.calls();
    try testing.expect(cs.len > 0);
    try testing.expectEqual(@as(u8, 4), cs[0].kind);
}
