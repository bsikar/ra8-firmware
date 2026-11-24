// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for SVG viewBox/transform affine mapping (`src/reflow_svg_xform.zig`).

const std = @import("std");
const xf = @import("reflow").svg_xform;

const SvgXform = xf.svg.SvgXform;
const SvgUtf = xf.SvgUtf;

fn expectClose(actual: f32, want: f32, tol: f32) !void {
    try std.testing.expectApproxEqAbs(want, actual, tol);
}

test "numf parses integers, signs, and fractions" {
    const s = "12.5, -3";
    var i: usize = 0;
    try expectClose(xf.priv_ra8_svgp_numf(s, s.len, &i), 12.5, 1e-6);
    try std.testing.expectEqual(@as(usize, 4), i);
    try expectClose(xf.priv_ra8_svgp_numf(s, s.len, &i), -3.0, 1e-6);
    try std.testing.expectEqual(s.len, i);
    const s2 = "-";
    var j: usize = 0;
    try expectClose(xf.priv_ra8_svgp_numf(s2, s2.len, &j), 0.0, 1e-6);
    try std.testing.expectEqual(@as(usize, 1), j);
    const s3 = ".75";
    var k: usize = 0;
    try expectClose(xf.priv_ra8_svgp_numf(s3, s3.len, &k), 0.75, 1e-6);
    try std.testing.expectEqual(@as(usize, 3), k);
}

test "axis-aligned map matches the fast-path mappers" {
    var t = SvgXform{
        .bx = 10,
        .by = 20,
        .bw = 400,
        .bh = 200,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 50,
        .ua = 2.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 3.0,
        .ue = 5.0,
        .uf = -1.0,
        .grads = null,
    };
    try std.testing.expect(!xf.priv_ra8_svgp_has_rot(&t));
    try std.testing.expectEqual(@as(i32, 10 + ((2 * 7 + 5) * 4)), xf.priv_ra8_svgp_mx(&t, 7));
    try std.testing.expectEqual(@as(i32, 20 + ((3 * 9 - 1) * 4)), xf.priv_ra8_svgp_my(&t, 9));
    try std.testing.expectEqual(@as(i32, (6 * 2) * 4), xf.priv_ra8_svgp_sx(&t, 6));
    var fx: i32 = 0;
    var fy: i32 = 0;
    xf.priv_ra8_svgp_map_point(&t, 7, 9, &fx, &fy);
    try std.testing.expectEqual(xf.priv_ra8_svgp_mx(&t, 7), fx);
    try std.testing.expectEqual(xf.priv_ra8_svgp_my(&t, 9), fy);
}

test "map point handles rotation and the centred rotate keeps the centre" {
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 1.0,
        .ue = 0.0,
        .uf = 0.0,
        .grads = null,
    };
    const src = "transform=\"rotate(90 50 50)\"";
    xf.priv_ra8_svgp_apply_xform(&t, src, src.len);
    try std.testing.expect(xf.priv_ra8_svgp_has_rot(&t));
    var fx: i32 = 0;
    var fy: i32 = 0;
    xf.priv_ra8_svgp_map_point(&t, 50, 50, &fx, &fy);
    try expectClose(@as(f32, @floatFromInt(fx)), 50.0, 1.0);
    try expectClose(@as(f32, @floatFromInt(fy)), 50.0, 1.0);
    xf.priv_ra8_svgp_map_point(&t, 60, 50, &fx, &fy);
    try expectClose(@as(f32, @floatFromInt(fx)), 50.0, 1.0);
    try expectClose(@as(f32, @floatFromInt(fy)), 60.0, 1.0);
}

test "translate and scale compose left to right" {
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 1.0,
        .ue = 0.0,
        .uf = 0.0,
        .grads = null,
    };
    const src = "transform=\"translate(10 20) scale(2)\"";
    xf.priv_ra8_svgp_apply_xform(&t, src, src.len);
    var fx: i32 = 0;
    var fy: i32 = 0;
    xf.priv_ra8_svgp_map_point(&t, 3, 4, &fx, &fy);
    try std.testing.expectEqual(@as(i32, 16), fx);
    try std.testing.expectEqual(@as(i32, 28), fy);
    try std.testing.expectEqual(@as(f32, 2.0), t.ua);
    try std.testing.expectEqual(@as(f32, 2.0), t.ud);
    try expectClose(t.ue, 10.0, 1e-6);
    try expectClose(t.uf, 20.0, 1e-6);
}

test "scale with one argument is uniform" {
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 1.0,
        .ue = 0.0,
        .uf = 0.0,
        .grads = null,
    };
    const src = "transform=\"scale(3)\"";
    xf.priv_ra8_svgp_apply_xform(&t, src, src.len);
    try std.testing.expectEqual(@as(f32, 3.0), t.ua);
    try std.testing.expectEqual(@as(f32, 3.0), t.ud);
}

test "skew terms and matrix copy through" {
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 1.0,
        .ue = 0.0,
        .uf = 0.0,
        .grads = null,
    };
    const src = "transform=\"matrix(1 0 0 1 5 6)\"";
    xf.priv_ra8_svgp_apply_xform(&t, src, src.len);
    try expectClose(t.ue, 5.0, 1e-6);
    try expectClose(t.uf, 6.0, 1e-6);
    try std.testing.expect(!xf.priv_ra8_svgp_has_rot(&t));
    const src2 = "transform=\"skewx(45)\"";
    xf.priv_ra8_svgp_apply_xform(&t, src2, src2.len);
    try std.testing.expect(xf.priv_ra8_svgp_has_rot(&t));
    try expectClose(t.uc, 1.0, 1e-5);
}

test "unknown functions are skipped whole" {
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1.0,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 1.0,
        .ue = 0.0,
        .uf = 0.0,
        .grads = null,
    };
    const src = "transform=\"bogus(1 2 3) translate(4 0)\"";
    xf.priv_ra8_svgp_apply_xform(&t, src, src.len);
    try expectClose(t.ue, 4.0, 1e-6);
    try expectClose(t.ub, 0.0, 1e-6);
}

test "apply_xform without a transform attribute is a no-op" {
    var t = SvgXform{
        .bx = 0,
        .by = 0,
        .bw = 100,
        .bh = 100,
        .vx = 0,
        .vy = 0,
        .vw = 100,
        .vh = 100,
        .ua = 1.5,
        .ub = 0.0,
        .uc = 0.0,
        .ud = 2.5,
        .ue = 7.0,
        .uf = 8.0,
        .grads = null,
    };
    const src = "<rect x=\"1\" y=\"2\"/>";
    xf.priv_ra8_svgp_apply_xform(&t, src, src.len);
    try std.testing.expectEqual(@as(f32, 1.5), t.ua);
    try std.testing.expectEqual(@as(f32, 7.0), t.ue);
    try std.testing.expectEqual(@as(f32, 8.0), t.uf);
}

test "affine composition composes as a(b(p))" {
    const a = SvgUtf{ .a = 2.0, .b = 0.0, .c = 0.0, .d = 2.0, .e = 1.0, .f = 1.0 };
    const b = SvgUtf{ .a = 1.0, .b = 0.0, .c = 0.0, .d = 1.0, .e = 3.0, .f = 4.0 };
    const r = xf.internal_utf_compose(a, b);
    try expectClose(r.e, 7.0, 1e-6);
    try expectClose(r.f, 9.0, 1e-6);
    try expectClose(r.a, 2.0, 1e-6);
}
