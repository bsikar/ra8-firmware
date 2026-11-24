// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for SVG scanning and colour primitives (`src/reflow_svg.zig`).

const std = @import("std");
const svg = @import("reflow").svg;

const testing = std.testing;
const SvgGrads = svg.SvgGrads;
const SvgGrad = svg.SvgGrad;
const SvgXform = svg.SvgXform;

test "whitespace and fold" {
    try testing.expect(svg.priv_ra8_svgp_ws(' '));
    try testing.expect(svg.priv_ra8_svgp_ws('\t'));
    try testing.expect(svg.priv_ra8_svgp_ws('\n'));
    try testing.expect(svg.priv_ra8_svgp_ws('\r'));
    try testing.expect(svg.priv_ra8_svgp_ws(0x0C));
    try testing.expect(!svg.priv_ra8_svgp_ws('x'));
    try testing.expect(!svg.priv_ra8_svgp_ws(0));
    try testing.expectEqual(@as(u8, 'a'), svg.priv_ra8_svgp_lc('A'));
    try testing.expectEqual(@as(u8, 'z'), svg.priv_ra8_svgp_lc('Z'));
    try testing.expectEqual(@as(u8, 'm'), svg.priv_ra8_svgp_lc('m'));
    try testing.expectEqual(@as(u8, '5'), svg.priv_ra8_svgp_lc('5'));
}

test "starts_ci and find_ci" {
    const s = "ReCT FiLL";
    try testing.expect(svg.priv_ra8_svgp_starts_ci(s, s.len, 0, "rect"));
    try testing.expect(!svg.priv_ra8_svgp_starts_ci(s, s.len, 0, "circle"));
    try testing.expect(!svg.priv_ra8_svgp_starts_ci(s, s.len, 6, "filler"));
    try testing.expectEqual(@as(usize, 5), svg.priv_ra8_svgp_find_ci(s, s.len, 0, "fill"));
    try testing.expectEqual(@as(usize, s.len), svg.priv_ra8_svgp_find_ci(s, s.len, 0, "nope"));
    try testing.expectEqual(@as(usize, s.len), svg.priv_ra8_svgp_find_ci(s, s.len, 0, ""));
}

test "num parses signs, separators and truncates fractions" {
    var i: usize = 0;
    const s = " , -3.75 42+5";
    try testing.expectEqual(@as(i32, -3), svg.priv_ra8_svgp_num(s, s.len, &i));
    try testing.expectEqual(@as(i32, 42), svg.priv_ra8_svgp_num(s, s.len, &i));
    try testing.expectEqual(@as(i32, 5), svg.priv_ra8_svgp_num(s, s.len, &i));
    try testing.expectEqual(@as(i32, 0), svg.priv_ra8_svgp_num(s, s.len, &i));
    try testing.expectEqual(s.len, i);
    var j: usize = 0;
    try testing.expectEqual(@as(i32, 0), svg.priv_ra8_svgp_num("x", 1, &j));
}

test "attr finds quoted values on word boundaries" {
    var off: usize = 0;
    var vl: usize = 0;
    var off2: usize = 0;
    var vl2: usize = 0;
    const tag = "<rect x=\"10\" fill = ' #0f0 ' width='6'>";
    try testing.expect(svg.priv_ra8_svgp_attr(tag, tag.len, "fill", &off, &vl));
    try testing.expectEqualStrings(" #0f0 ", tag[off .. off + vl]);
    try testing.expect(svg.priv_ra8_svgp_attr(tag, tag.len, "x", &off, &vl));
    try testing.expectEqualStrings("10", tag[off .. off + vl]);
    try testing.expect(!svg.priv_ra8_svgp_attr(tag, tag.len, "filla", &off, &vl));
    try testing.expect(!svg.priv_ra8_svgp_attr(tag, tag.len, "fi", &off, &vl));
    try testing.expect(!svg.priv_ra8_svgp_attr(tag, tag.len, "stroke", &off, &vl));
    const bad = "<rect fill=\"open";
    try testing.expect(svg.priv_ra8_svgp_attr(bad, bad.len, "fill", &off, &vl));
    try testing.expectEqualStrings("open", bad[off .. off + vl]);
    try testing.expect(!svg.priv_ra8_svgp_attr(tag, tag.len, "stroke", &off2, &vl2));
    try testing.expectEqual(@as(usize, 0), off2);
    try testing.expectEqual(@as(usize, 0), vl2);
}

test "attr_num falls back to the default" {
    const tag = "<rect x=\"-7.9\" y=\"3\">";
    try testing.expectEqual(@as(i32, -7), svg.priv_ra8_svgp_attr_num(tag, tag.len, "x", 99));
    try testing.expectEqual(@as(i32, 3), svg.priv_ra8_svgp_attr_num(tag, tag.len, "y", 99));
    try testing.expectEqual(@as(i32, 99), svg.priv_ra8_svgp_attr_num(tag, tag.len, "width", 99));
}

test "hex colours" {
    const six = "00FF7f";
    try testing.expectEqual(@as(u32, 0x00FF7F), svg.internal_hex_color(six, six.len));
    const three = "f0a";
    try testing.expectEqual(@as(u32, 0xFF00AA), svg.internal_hex_color(three, three.len));
    try testing.expectEqual(svg.k_svg_no_paint, svg.internal_hex_color("00ff", 4));
    try testing.expectEqual(svg.k_svg_no_paint, svg.internal_hex_color("00fg", 4));
    try testing.expectEqual(svg.k_svg_no_paint, svg.internal_hex_color("zzz", 3));
}

test "named colours and paint" {
    try testing.expectEqual(@as(u32, 0x000000), svg.internal_named_color("BLACK", 5));
    try testing.expectEqual(@as(u32, 0x808080), svg.internal_named_color("Grey", 4));
    try testing.expectEqual(@as(u32, 0xFFA500), svg.internal_named_color("orange", 6));
    try testing.expectEqual(svg.k_svg_no_paint, svg.internal_named_color("none", 4));
    try testing.expectEqual(svg.k_svg_no_paint, svg.internal_named_color("chartreuse", 10));
    try testing.expectEqual(@as(u32, 0x123456), svg.priv_ra8_svgp_paint("#123456", 7));
    try testing.expectEqual(@as(u32, 0x00FFFF), svg.priv_ra8_svgp_paint("#0fF", 4));
    try testing.expectEqual(@as(u32, 0xFF0000), svg.priv_ra8_svgp_paint("red", 3));
    try testing.expectEqual(svg.k_svg_no_paint, svg.priv_ra8_svgp_paint("none", 4));
    try testing.expectEqual(svg.k_svg_no_paint, svg.priv_ra8_svgp_paint("", 0));
}

test "attr_paint falls back to the default" {
    const tag = "<path fill=\"#A1b2C3\" stroke=\"none\">";
    try testing.expectEqual(@as(u32, 0xA1B2C3), svg.priv_ra8_svgp_attr_paint(tag, tag.len, "fill", svg.k_svg_no_paint));
    try testing.expectEqual(svg.k_svg_no_paint, svg.priv_ra8_svgp_attr_paint(tag, tag.len, "stroke", svg.k_svg_def_fill));
    try testing.expectEqual(@as(u32, 0x123456), svg.priv_ra8_svgp_attr_paint(tag, tag.len, "d", 0x123456));
}

test "match_grad matches by exact id" {
    var grads = SvgGrads{ .n = 2, .g = undefined };
    grads.g[0] = SvgGrad{ .kind = svg.k_svg_grad_linear, .nstops = 1, .id = undefined, .x1 = 0, .y1 = 0, .x2 = 1, .y2 = 0, .stops = undefined };
    grads.g[1] = SvgGrad{ .kind = svg.k_svg_grad_radial, .nstops = 2, .id = undefined, .x1 = 0.5, .y1 = 0.5, .x2 = 1, .y2 = 0, .stops = undefined };
    @memcpy(grads.g[0].id[0..2], "g1");
    grads.g[0].id[2] = 0;
    @memcpy(grads.g[1].id[0..2], "g2");
    grads.g[1].id[2] = 0;

    try testing.expectEqual(@as(i32, 1), svg.priv_match_grad(&grads, "url(#g2)", 8));
    try testing.expectEqual(@as(i32, 0), svg.priv_match_grad(&grads, "URL(#g1)", 8));
    try testing.expectEqual(@as(i32, -1), svg.priv_match_grad(&grads, "url(#nope)", 10));
    try testing.expectEqual(@as(i32, -1), svg.priv_match_grad(&grads, "#g1", 3));
    try testing.expectEqual(@as(i32, -1), svg.priv_match_grad(null, "url(#g1)", 8));
    try testing.expectEqual(@as(i32, -1), svg.priv_match_grad(&grads, "url(#G2)", 8));
}

test "resolve_fill routes solid, gradient and default" {
    var grads = SvgGrads{ .n = 1, .g = undefined };
    grads.g[0] = SvgGrad{ .kind = svg.k_svg_grad_linear, .nstops = 1, .id = undefined, .x1 = 0, .y1 = 0, .x2 = 1, .y2 = 0, .stops = undefined };
    @memcpy(grads.g[0].id[0..2], "gg");
    grads.g[0].id[2] = 0;
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1,
        .ub = 0,
        .uc = 0,
        .ud = 1,
        .ue = 0,
        .uf = 0,
        .grads = &grads,
    };
    var gi: i32 = 99;

    const solid = "<rect fill=\"#102030\">";
    try testing.expectEqual(@as(u32, 0x102030), svg.priv_ra8_svgp_resolve_fill(solid, solid.len, &t, svg.k_svg_def_fill, &gi));
    try testing.expectEqual(@as(i32, -1), gi);

    const grad = "<rect fill=\"url(#gg)\">";
    try testing.expectEqual(svg.k_svg_no_paint, svg.priv_ra8_svgp_resolve_fill(grad, grad.len, &t, svg.k_svg_def_fill, &gi));
    try testing.expectEqual(@as(i32, 0), gi);

    const grad_miss = "<rect fill=\"url(#zz)\">";
    try testing.expectEqual(svg.k_svg_no_paint, svg.priv_ra8_svgp_resolve_fill(grad_miss, grad_miss.len, &t, svg.k_svg_def_fill, &gi));
    try testing.expectEqual(@as(i32, -1), gi);

    const plain = "<rect width=\"4\">";
    try testing.expectEqual(svg.k_svg_def_fill, svg.priv_ra8_svgp_resolve_fill(plain, plain.len, &t, svg.k_svg_def_fill, &gi));
    try testing.expectEqual(svg.k_svg_no_paint, svg.priv_ra8_svgp_resolve_fill(grad, grad.len, null, svg.k_svg_def_fill, &gi));
    try testing.expectEqual(@as(i32, -1), gi);
}
