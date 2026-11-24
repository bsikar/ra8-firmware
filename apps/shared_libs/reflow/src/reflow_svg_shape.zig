//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SVG shape rasterisers: rect / circle / line / polygon + fill + arc
//! (RA8FW-1235). Zig port of src/reflow_svg_shape.c, which this module
//! replaces and deletes: the `<rect>`, `<circle>`, `<line>`, `<polygon>`, and
//! `<polyline>` rasterisers, the even-odd scanline polygon fill (solid and
//! per-pixel gradient), the gradient colour evaluator, and the elliptical-arc
//! flatten (shared with the `<path>` `A` command). Axis-aligned shapes keep
//! the ra8_gfx fast path; rotated / sheared shapes are mapped through the
//! affine and scanline-filled. No DOM, no heap. The scan core comes from
//! reflow_svg.zig (RA8FW-1233) and the transform map from
//! reflow_svg_xform.zig (RA8FW-1234); both keep their C-linkage exports, and
//! the ra8_gfx entry points stay extern for the CMake library build.

const std = @import("std");
const svg = @import("reflow_svg.zig");
const xf = @import("reflow_svg_xform.zig");

pub const SvgXform = xf.SvgXform;
pub const SvgGrad = svg.SvgGrad;

// ---- Extern seam: the ra8_gfx entry points ----

extern fn ra8_gfx_pixel(x: i32, y: i32, color: u32) callconv(.c) u16;
extern fn ra8_gfx_line(x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) u16;
extern fn ra8_gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u32, filled: u8) callconv(.c) u16;
extern fn ra8_gfx_circle(cx: i32, cy: i32, r: i32, color: u32, filled: u8) callconv(.c) u16;

// ---- Shared constants (reflow_svg_internal.h enum priv_svg_consts_t) ----

/// Max points / scanline crossings.
pub const k_svg_poly_max: usize = 64;
/// Min points for a fillable polygon.
pub const k_svg_poly_min: i32 = 3;
/// Max args of a path command (`A`).
pub const k_svg_path_args: usize = 7;
/// `A` arg indices: rx, ry, rot, large, sweep, ex, ey.
pub const k_svg_arc_rx: usize = 0;
pub const k_svg_arc_ry: usize = 1;
pub const k_svg_arc_rot: usize = 2;
pub const k_svg_arc_large: usize = 3;
pub const k_svg_arc_sweep: usize = 4;
pub const k_svg_arc_ex: usize = 5;
pub const k_svg_arc_ey: usize = 6;
/// Vertices of the rect quad.
pub const k_svg_rect_pts: usize = 4;
/// Vertices of the circle N-gon approximation.
pub const k_svg_circle_seg: usize = 24;
/// Max segments when flattening an arc.
pub const k_svg_arc_seg_max: i32 = 24;
/// Colour channel mask (0x00RRGGBB).
pub const k_svg_chan_mask: u32 = 0xFF;
/// Red channel shift.
pub const k_svg_sh_r: u32 = 16;

/// 2*Pi, for wrapping an arc's signed sweep into range.
const s_svg_2pi: f32 = 6.28318531;

// ---- Shared geometry types (reflow_svg_internal.h) ----

/// A 2-D point in SVG user space (an x/y pair).
pub const SvgPt = extern struct {
    x: i32,
    y: i32,
};

// ===========================================================================
// Basic shapes (rect / circle / line)
// ===========================================================================

/// `priv_ra8_svgp_draw_rect`: draw one SVG 'rect' element using its fill
/// colour or gradient. Axis-aligned solid rects use the ra8_gfx_rect
/// fast path; under a rotating/shearing transform the 4 corners are mapped
/// through the full affine and the resulting quad is scanline-filled.
/// Returns without drawing when both gi < 0 and fill == k_svg_no_paint.
pub export fn priv_ra8_svgp_draw_rect(s: [*]const u8, len: usize, t: *const SvgXform) callconv(.c) void {
    var gi: i32 = -1;
    const fill = svg.priv_ra8_svgp_resolve_fill(s, len, t, svg.k_svg_def_fill, &gi);
    if ((gi < 0) and (fill == svg.k_svg_no_paint)) {
        return;
    }
    const rx = svg.priv_ra8_svgp_attr_num(s, len, "x", 0);
    const ry = svg.priv_ra8_svgp_attr_num(s, len, "y", 0);
    const rw = svg.priv_ra8_svgp_attr_num(s, len, "width", 0);
    const rh = svg.priv_ra8_svgp_attr_num(s, len, "height", 0);
    if ((gi < 0) and !xf.priv_ra8_svgp_has_rot(t)) {
        _ = ra8_gfx_rect(
            xf.priv_ra8_svgp_mx(t, rx),
            xf.priv_ra8_svgp_my(t, ry),
            xf.priv_ra8_svgp_sx(t, rw),
            xf.priv_ra8_svgp_sx(t, rh),
            fill,
            1,
        );
        return;
    }
    var xs: [k_svg_rect_pts]i32 = @splat(0);
    var ys: [k_svg_rect_pts]i32 = @splat(0);
    xf.priv_ra8_svgp_map_point(t, rx, ry, &xs[0], &ys[0]);
    xf.priv_ra8_svgp_map_point(t, rx + rw, ry, &xs[1], &ys[1]);
    xf.priv_ra8_svgp_map_point(t, rx + rw, ry + rh, &xs[2], &ys[2]);
    xf.priv_ra8_svgp_map_point(t, rx, ry + rh, &xs[3], &ys[3]);
    if (gi >= 0) {
        priv_ra8_svgp_fill_poly_grad(&xs, &ys, k_svg_rect_pts, &t.grads.?.g[@intCast(gi)]);
    } else {
        priv_ra8_svgp_fill_poly(&xs, &ys, k_svg_rect_pts, fill);
    }
}

/// `priv_ra8_svgp_draw_circle`: draw one SVG 'circle' element using its fill
/// colour or gradient. Axis-aligned solid circles use the ra8_gfx_circle
/// fast path; under a rotating/shearing transform the circle is approximated
/// by a k_svg_circle_seg N-gon mapped through the full affine, then
/// scanline-filled.
pub export fn priv_ra8_svgp_draw_circle(s: [*]const u8, len: usize, t: *const SvgXform) callconv(.c) void {
    var gi: i32 = -1;
    const fill = svg.priv_ra8_svgp_resolve_fill(s, len, t, svg.k_svg_def_fill, &gi);
    if ((gi < 0) and (fill == svg.k_svg_no_paint)) {
        return;
    }
    const cx = svg.priv_ra8_svgp_attr_num(s, len, "cx", 0);
    const cy = svg.priv_ra8_svgp_attr_num(s, len, "cy", 0);
    const r = svg.priv_ra8_svgp_attr_num(s, len, "r", 0);
    if ((gi < 0) and !xf.priv_ra8_svgp_has_rot(t)) {
        _ = ra8_gfx_circle(
            xf.priv_ra8_svgp_mx(t, cx),
            xf.priv_ra8_svgp_my(t, cy),
            xf.priv_ra8_svgp_sx(t, r),
            fill,
            1,
        );
        return;
    }
    var xs: [k_svg_circle_seg]i32 = @splat(0);
    var ys: [k_svg_circle_seg]i32 = @splat(0);
    // Bounded: k_svg_circle_seg vertices around the circle.
    var k: i32 = 0;
    while (k < @as(i32, @intCast(k_svg_circle_seg))) : (k += 1) {
        const ang = (s_svg_2pi * @as(f32, @floatFromInt(k))) / @as(f32, @floatFromInt(k_svg_circle_seg));
        const dx = @as(f32, @floatFromInt(r)) * std.math.cos(ang);
        const dy = @as(f32, @floatFromInt(r)) * std.math.sin(ang);
        xf.priv_ra8_svgp_map_point(t, cx + @as(i32, @intFromFloat(dx)), cy + @as(i32, @intFromFloat(dy)), &xs[@intCast(k)], &ys[@intCast(k)]);
    }
    if (gi >= 0) {
        priv_ra8_svgp_fill_poly_grad(&xs, &ys, k_svg_circle_seg, &t.grads.?.g[@intCast(gi)]);
    } else {
        priv_ra8_svgp_fill_poly(&xs, &ys, k_svg_circle_seg, fill);
    }
}

/// `priv_ra8_svgp_draw_line`: draw one SVG 'line' element using its stroke
/// colour. Both endpoints are mapped through the full affine and the result
/// is forwarded to ra8_gfx_line. Returns without drawing when 'stroke' is
/// absent or parses to k_svg_no_paint. 'fill' is ignored for 'line' elements
/// (SVG specification).
pub export fn priv_ra8_svgp_draw_line(s: [*]const u8, len: usize, t: *const SvgXform) callconv(.c) void {
    const stroke = svg.priv_ra8_svgp_attr_paint(s, len, "stroke", svg.k_svg_no_paint);
    if (stroke == svg.k_svg_no_paint) {
        return;
    }
    const x1 = svg.priv_ra8_svgp_attr_num(s, len, "x1", 0);
    const y1 = svg.priv_ra8_svgp_attr_num(s, len, "y1", 0);
    const x2 = svg.priv_ra8_svgp_attr_num(s, len, "x2", 0);
    const y2 = svg.priv_ra8_svgp_attr_num(s, len, "y2", 0);
    var fx1: i32 = 0;
    var fy1: i32 = 0;
    var fx2: i32 = 0;
    var fy2: i32 = 0;
    xf.priv_ra8_svgp_map_point(t, x1, y1, &fx1, &fy1);
    xf.priv_ra8_svgp_map_point(t, x2, y2, &fx2, &fy2);
    _ = ra8_gfx_line(fx1, fy1, fx2, fy2, stroke);
}

// ===========================================================================
// Polygon / polyline (a `points` list + a scanline polygon fill)
// ===========================================================================

/// `internal_parse_points`: parse a 'points' attribute value into
/// framebuffer-space vertices. Scans v[0..vlen) for whitespace/comma
/// separated x,y integer pairs via priv_ra8_svgp_num, maps each pair through
/// the full affine, and stores the result in xs[n] / ys[n]. Terminates when
/// k_svg_poly_max vertices have been collected or the end of the span is
/// reached. Returns the count of vertex pairs filled.
fn internal_parse_points(v: [*]const u8, vlen: usize, t: *const SvgXform, xs: *[k_svg_poly_max]i32, ys: *[k_svg_poly_max]i32) i32 {
    var k: usize = 0;
    var n: i32 = 0;
    // Bounded: <= k_svg_poly_max pairs; k advances past each number.
    while (n < @as(i32, @intCast(k_svg_poly_max))) {
        while ((k < vlen) and (svg.priv_ra8_svgp_ws(v[k]) or (v[k] == ','))) {
            k += 1;
        }
        if (k >= vlen) {
            break;
        }
        const ux = svg.priv_ra8_svgp_num(v, vlen, &k);
        const uy = svg.priv_ra8_svgp_num(v, vlen, &k);
        xf.priv_ra8_svgp_map_point(t, ux, uy, &xs[@intCast(n)], &ys[@intCast(n)]);
        n += 1;
    }
    return n;
}

/// `internal_sort_i32`: sort m int32 values in a into ascending order using
/// insertion sort. O(m^2), efficient for the small bounded arrays (m is
/// bounded by k_svg_poly_max scanline crossings per row). In place.
fn internal_sort_i32(a: []i32) void {
    // Bounded: a.len <= k_svg_poly_max.
    var i: usize = 1;
    while (i < a.len) : (i += 1) {
        const key = a[i];
        var j: isize = @as(isize, @intCast(i)) - 1;
        while ((j >= 0) and (a[@intCast(j)] > key)) : (j -= 1) {
            a[@intCast(j + 1)] = a[@intCast(j)];
        }
        a[@intCast(j + 1)] = key;
    }
}

/// `internal_scanline_x`: collect framebuffer X coordinates where polygon
/// edges cross scanline y. Iterates over all n edges; for each edge from
/// vertex i to vertex (i+1)%n, checks whether scanline y passes through it
/// (half-open interval [y0,y1) or [y1,y0)). For crossing edges, computes the
/// X intercept via linear interpolation with 64-bit arithmetic. Stops early
/// if k_svg_poly_max crossings are found. Returns the crossing count.
fn internal_scanline_x(xs: []const i32, ys: []const i32, y: i32, xint: *[k_svg_poly_max]i32) i32 {
    const n: usize = xs.len;
    var m: i32 = 0;
    // Bounded: one test per polygon edge (n edges); n is capped at
    // k_svg_poly_max by the point parser, so the crossing count m can never
    // reach k_svg_poly_max before i exhausts n.
    var i: usize = 0;
    while ((i < n) and (m < @as(i32, @intCast(k_svg_poly_max)))) : (i += 1) {
        const j = (i + 1) % n;
        const y0 = ys[i];
        const y1 = ys[j];
        if (((y0 <= y) and (y < y1)) or ((y1 <= y) and (y < y0))) {
            const x0 = xs[i];
            const x1 = xs[j];
            const y_delta = @as(i64, y) - @as(i64, y0);
            const x_delta = @as(i64, x1) - @as(i64, x0);
            const divisor = @as(i64, y1) - @as(i64, y0);
            const offset = @divTrunc(y_delta * x_delta, divisor);
            xint[@intCast(m)] = x0 + @as(i32, @intCast(offset));
            m += 1;
        }
    }
    return m;
}

/// `priv_ra8_svgp_fill_poly`: even-odd scanline fill of polygon (xs,ys)[0..n)
/// with a solid colour. Computes the bounding Y range of the polygon, then
/// for each scanline Y in [ymin, ymax] collects intercepts, sorts them, and
/// fills pairs of spans with ra8_gfx_line. Returns immediately for polygons
/// with fewer than k_svg_poly_min vertices.
pub export fn priv_ra8_svgp_fill_poly(xs: [*]const i32, ys: [*]const i32, n: i32, color: u32) callconv(.c) void {
    if (n < k_svg_poly_min) {
        return;
    }
    var ymin = ys[0];
    var ymax = ys[0];
    var i: usize = 1;
    while (i < @as(usize, @intCast(n))) : (i += 1) {
        ymin = if (ys[i] < ymin) ys[i] else ymin;
        ymax = if (ys[i] > ymax) ys[i] else ymax;
    }
    // Bounded: ymax - ymin <= the framebuffer-box height.
    var y = ymin;
    while (y <= ymax) : (y += 1) {
        var xint: [k_svg_poly_max]i32 = @splat(0);
        const m = internal_scanline_x(xs[0..@intCast(n)], ys[0..@intCast(n)], y, &xint);
        internal_sort_i32(xint[0..@intCast(m)]);
        var k: i32 = 0;
        while ((k + 1) < m) : (k += 2) {
            _ = ra8_gfx_line(xint[@intCast(k)], y, xint[@intCast(k + 1)], y, color);
        }
    }
}

/// `internal_col_lerp`: linear-interpolate two 0x00RRGGBB colours by factor
/// f in [0, 1]. Extracts the R, G, and B channels, interpolates each
/// independently as (channel_a + (channel_b - channel_a) * f), and recombines
/// into a 0x00RRGGBB result. f is not clamped here; the gradient evaluator
/// provides a value in [0,1].
fn internal_col_lerp(a: u32, b: u32, f: f32) u32 {
    const ra_bits = (a >> k_svg_sh_r) & k_svg_chan_mask;
    const ga_bits = (a >> svg.k_svg_hex_chan) & k_svg_chan_mask;
    const ba_bits = a & k_svg_chan_mask;
    const rb_bits = (b >> k_svg_sh_r) & k_svg_chan_mask;
    const gb_bits = (b >> svg.k_svg_hex_chan) & k_svg_chan_mask;
    const bb_bits = b & k_svg_chan_mask;
    const ra: i32 = @intCast(ra_bits);
    const ga: i32 = @intCast(ga_bits);
    const ba: i32 = @intCast(ba_bits);
    const rb: i32 = @intCast(rb_bits);
    const gb: i32 = @intCast(gb_bits);
    const bb: i32 = @intCast(bb_bits);
    const rd = rb - ra;
    const gd = gb - ga;
    const bd = bb - ba;
    const r = ra + @as(i32, @intFromFloat(@as(f32, @floatFromInt(rd)) * f));
    const g = ga + @as(i32, @intFromFloat(@as(f32, @floatFromInt(gd)) * f));
    const bl = ba + @as(i32, @intFromFloat(@as(f32, @floatFromInt(bd)) * f));
    return (@as(u32, @bitCast(r)) << k_svg_sh_r) | (@as(u32, @bitCast(g)) << svg.k_svg_hex_chan) |
        @as(u32, @bitCast(bl));
}

/// `internal_grad_eval`: evaluate gradient g at bbox-relative point
/// (px, py). For a linear gradient the scalar parameter is the projection of
/// (px - x1, py - y1) onto the vector (x2 - x1, y2 - y1) normalised by the
/// squared length of that vector. For a radial gradient the parameter is the
/// Euclidean distance from (x1, y1) divided by radius x2. The parameter is
/// compared against stop offsets; values before the first stop return the
/// first stop colour, values after the last return the last stop colour, and
/// values between two stops are linearly interpolated by internal_col_lerp.
/// Returns the default fill colour when g.nstops is zero.
fn internal_grad_eval(g: *const SvgGrad, px: f32, py: f32) u32 {
    if (g.nstops == 0) {
        return svg.k_svg_def_fill;
    }
    var p: f32 = 0.0;
    if (g.kind == svg.k_svg_grad_radial) {
        const dx = px - g.x1;
        const dy = py - g.y1;
        const rr = if (g.x2 > 0.0) g.x2 else 1.0;
        p = @sqrt((dx * dx) + (dy * dy)) / rr;
    } else {
        const vx = g.x2 - g.x1;
        const vy = g.y2 - g.y1;
        const len2 = (vx * vx) + (vy * vy);
        p = if (len2 > 0.0) (((px - g.x1) * vx) + ((py - g.y1) * vy)) / len2 else 0.0;
    }
    if (p <= g.stops[0].off) {
        return g.stops[0].col;
    }
    const last: usize = @as(usize, g.nstops) - 1;
    if (p >= g.stops[last].off) {
        return g.stops[last].col;
    }
    // Bounded: at most nstops-1 stop brackets.
    var i: usize = 0;
    while (i < last) : (i += 1) {
        const o0 = g.stops[i].off;
        const o1 = g.stops[i + 1].off;
        if ((p >= o0) and (p <= o1)) {
            const f = if (o1 > o0) ((p - o0) / (o1 - o0)) else 0.0;
            return internal_col_lerp(g.stops[i].col, g.stops[i + 1].col, f);
        }
    }
    return g.stops[last].col;
}

/// `priv_ra8_svgp_fill_poly_grad`: per-pixel gradient scanline fill of
/// polygon (xs,ys)[0..n). Computes the bounding box of the polygon, then for
/// each scanline Y in [ymin, ymax] collects edge intercepts and sorts them.
/// For each filled span, iterates over pixels and evaluates the gradient at
/// the bounding-box-relative position ((x-xmin)/bw, (y-ymin)/bh). Each pixel
/// is written individually with ra8_gfx_pixel. Returns immediately when
/// n < k_svg_poly_min.
pub export fn priv_ra8_svgp_fill_poly_grad(xs: [*]const i32, ys: [*]const i32, n: i32, g: *const SvgGrad) callconv(.c) void {
    if (n < k_svg_poly_min) {
        return;
    }
    var xmin = xs[0];
    var xmax = xs[0];
    var ymin = ys[0];
    var ymax = ys[0];
    var i: usize = 1;
    while (i < @as(usize, @intCast(n))) : (i += 1) {
        xmin = if (xs[i] < xmin) xs[i] else xmin;
        xmax = if (xs[i] > xmax) xs[i] else xmax;
        ymin = if (ys[i] < ymin) ys[i] else ymin;
        ymax = if (ys[i] > ymax) ys[i] else ymax;
    }
    const bw = if (xmax > xmin) @as(f32, @floatFromInt(@as(i64, xmax) - @as(i64, xmin))) else 1.0;
    const bh = if (ymax > ymin) @as(f32, @floatFromInt(@as(i64, ymax) - @as(i64, ymin))) else 1.0;
    // Bounded: ymax - ymin <= the framebuffer-box height.
    var y = ymin;
    while (y <= ymax) : (y += 1) {
        var xint: [k_svg_poly_max]i32 = @splat(0);
        const m = internal_scanline_x(xs[0..@intCast(n)], ys[0..@intCast(n)], y, &xint);
        internal_sort_i32(xint[0..@intCast(m)]);
        const py = @as(f32, @floatFromInt(@as(i64, y) - @as(i64, ymin))) / bh;
        var k: i32 = 0;
        while ((k + 1) < m) : (k += 2) {
            // Bounded: xint[k+1] - xint[k] <= the framebuffer-box width.
            var x = xint[@intCast(k)];
            while (x <= xint[@intCast(k + 1)]) : (x += 1) {
                const px = @as(f32, @floatFromInt(@as(i64, x) - @as(i64, xmin))) / bw;
                _ = ra8_gfx_pixel(x, y, internal_grad_eval(g, px, py));
            }
        }
    }
}

/// `priv_ra8_svgp_draw_polygon`: draw one SVG 'polygon' element as a filled
/// polygon. Resolves the fill (solid or gradient), reads the 'points'
/// attribute, parses vertices into framebuffer space, and fills the
/// resulting polygon. Returns without drawing when the fill is absent/none
/// or the 'points' attribute is not present.
pub export fn priv_ra8_svgp_draw_polygon(s: [*]const u8, len: usize, t: *const SvgXform) callconv(.c) void {
    var gi: i32 = -1;
    const fill = svg.priv_ra8_svgp_resolve_fill(s, len, t, svg.k_svg_def_fill, &gi);
    var off: usize = 0;
    var vl: usize = 0;
    if (((gi < 0) and (fill == svg.k_svg_no_paint)) or
        !svg.priv_ra8_svgp_attr(s, len, "points", &off, &vl))
    {
        return;
    }
    var xs: [k_svg_poly_max]i32 = @splat(0);
    var ys: [k_svg_poly_max]i32 = @splat(0);
    const n = internal_parse_points(s + off, vl, t, &xs, &ys);
    if (gi >= 0) {
        priv_ra8_svgp_fill_poly_grad(&xs, &ys, n, &t.grads.?.g[@intCast(gi)]);
    } else {
        priv_ra8_svgp_fill_poly(&xs, &ys, n, fill);
    }
}

/// `priv_ra8_svgp_draw_polyline`: draw one SVG 'polyline' element as
/// connected stroke line segments. Reads the 'stroke' paint and the 'points'
/// attribute, parses vertex pairs into framebuffer space, and connects
/// adjacent vertices with ra8_gfx_line. Returns without drawing when the
/// stroke colour is absent/none or the 'points' attribute is not present.
pub export fn priv_ra8_svgp_draw_polyline(s: [*]const u8, len: usize, t: *const SvgXform) callconv(.c) void {
    const stroke = svg.priv_ra8_svgp_attr_paint(s, len, "stroke", svg.k_svg_no_paint);
    var off: usize = 0;
    var vl: usize = 0;
    if ((stroke == svg.k_svg_no_paint) or !svg.priv_ra8_svgp_attr(s, len, "points", &off, &vl)) {
        return;
    }
    var xs: [k_svg_poly_max]i32 = @splat(0);
    var ys: [k_svg_poly_max]i32 = @splat(0);
    const n = internal_parse_points(s + off, vl, t, &xs, &ys);
    // Bounded: n - 1 segments.
    var i: i32 = 0;
    while ((i + 1) < n) : (i += 1) {
        _ = ra8_gfx_line(xs[@intCast(i)], ys[@intCast(i)], xs[@intCast(i + 1)], ys[@intCast(i + 1)], stroke);
    }
}

// ===========================================================================
// Elliptical arc flatten (shared with the <path> A command)
// ===========================================================================

/// `internal_absf`: the absolute value of a float without using libm fabsf.
fn internal_absf(v: f32) f32 {
    return if (v < 0.0) -v else v;
}

/// Centre-parametrisation of an SVG elliptical arc: the result of
/// internal_arc_center -- the centre, the (radius-corrected) semi-axes, the
/// x-axis rotation (as cos/sin), and the start angle + signed sweep, ready to
/// sample. ok is false for a degenerate arc (zero radius or coincident
/// endpoints), in which case the caller draws a straight line.
const SvgArc = struct {
    cx: f32,
    cy: f32,
    rx: f32,
    ry: f32,
    cos_phi: f32,
    sin_phi: f32,
    t1: f32,
    dt: f32,
    ok: bool,
};

/// `internal_arc_solve`: finish an arc's centre + start/sweep angles from the
/// rotated half-chord (x1p, y1p), the (radius-corrected) semi-axes
/// (rx, ry), and the endpoint midpoint (mx, my) (F.6.5.2 / F.6.5.5-6). a's
/// cos_phi / sin_phi must already be set by the caller. The signed sweep
/// a.dt is adjusted so its direction is consistent with the sweep
/// (positive-angle) and large (large-arc) flags.
fn internal_arc_solve(a: *SvgArc, x1p: f32, y1p: f32, rx: f32, ry: f32, mx: f32, my: f32, large: bool, sweep: bool) void {
    const rx2 = rx * rx;
    const ry2 = ry * ry;
    const num = (rx2 * ry2) - (rx2 * y1p * y1p) - (ry2 * x1p * x1p);
    const den = (rx2 * y1p * y1p) + (ry2 * x1p * x1p);
    var co: f32 = @sqrt(if (num > 0.0) (num / den) else 0.0);
    if (large == sweep) {
        co = -co;
    }
    const cxp = co * ((rx * y1p) / ry);
    const cyp = -co * ((ry * x1p) / rx);
    a.cx = (a.cos_phi * cxp) - (a.sin_phi * cyp) + mx;
    a.cy = (a.sin_phi * cxp) + (a.cos_phi * cyp) + my;
    a.t1 = std.math.atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    var dt = std.math.atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx) - a.t1;
    if (!sweep and (dt > 0.0)) {
        dt -= s_svg_2pi;
    } else if (sweep and (dt < 0.0)) {
        dt += s_svg_2pi;
    } else {
        // The sweep already has the requested direction.
    }
    a.dt = dt;
    a.rx = rx;
    a.ry = ry;
    a.ok = true;
}

/// `internal_arc_center`: convert an SVG endpoint-parametrised arc to centre
/// parametrisation (SVG 1.1 implementation notes F.6.5): out-of-range radii
/// are scaled up (F.6.6.2), the centre is solved via internal_arc_solve
/// (F.6.5.2), and the start angle plus signed sweep are derived honouring
/// the large-arc and sweep flags. Pure; no MMIO, no heap.
fn internal_arc_center(p0: SvgPt, p_end: SvgPt, rx_in: i32, ry_in: i32, rot_deg: i32, large: bool, sweep: bool) SvgArc {
    const k_pi: f32 = 3.14159265;
    const k_deg_half: f32 = 180.0;
    const k_half: f32 = 0.5;
    var a = SvgArc{ .cx = 0, .cy = 0, .rx = 0, .ry = 0, .cos_phi = 0, .sin_phi = 0, .t1 = 0, .dt = 0, .ok = false };
    var rx = internal_absf(@as(f32, @floatFromInt(rx_in)));
    var ry = internal_absf(@as(f32, @floatFromInt(ry_in)));
    if ((rx == 0.0) or (ry == 0.0) or ((p0.x == p_end.x) and (p0.y == p_end.y))) {
        a.ok = false;
        return a;
    }
    const phi = @as(f32, @floatFromInt(rot_deg)) * (k_pi / k_deg_half);
    a.cos_phi = std.math.cos(phi);
    a.sin_phi = std.math.sin(phi);
    const dx = @as(f32, @floatFromInt(@as(i64, p0.x) - @as(i64, p_end.x))) * k_half;
    const dy = @as(f32, @floatFromInt(@as(i64, p0.y) - @as(i64, p_end.y))) * k_half;
    const x1p = (a.cos_phi * dx) + (a.sin_phi * dy);
    const y1p = -(a.sin_phi * dx) + (a.cos_phi * dy);
    const lam = ((x1p * x1p) / (rx * rx)) + ((y1p * y1p) / (ry * ry));
    if (lam > 1.0) {
        const s = @sqrt(lam);
        rx *= s;
        ry *= s;
    }
    const mx = @as(f32, @floatFromInt(@as(i64, p0.x) + @as(i64, p_end.x))) * k_half;
    const my = @as(f32, @floatFromInt(@as(i64, p0.y) + @as(i64, p_end.y))) * k_half;
    internal_arc_solve(&a, x1p, y1p, rx, ry, mx, my, large, sweep);
    return a;
}

/// `internal_arc_segs`: compute a bounded segment count for an arc of signed
/// sweep dt. Divides the absolute value of dt by the target
/// radians-per-segment arc step (~pi/8) and adds 1 to ensure at least one
/// segment. Clamped to k_svg_arc_seg_max so the vertex count stays bounded.
fn internal_arc_segs(dt: f32) i32 {
    const k_arc_step: f32 = 0.39269908;
    var segs = @as(i32, @intFromFloat(internal_absf(dt) / k_arc_step)) + 1;
    if (segs > k_svg_arc_seg_max) {
        segs = k_svg_arc_seg_max;
    }
    return segs;
}

/// `priv_ra8_svgp_flatten_arc`: flatten an SVG elliptical arc into the
/// polygon vertex array. Converts the endpoint-parametrised arc (P0 to
/// p_end, with radii, rotation, and flags from args) to centre form via
/// internal_arc_center. When the arc is degenerate (ok == false), appends a
/// single line-to p_end. Otherwise samples the arc at
/// internal_arc_segs(a.dt) parameter values using the ellipse equation in
/// the rotated frame and appends each mapped framebuffer point. Bounded by
/// k_svg_poly_max.
pub export fn priv_ra8_svgp_flatten_arc(
    t: *const SvgXform,
    p0: SvgPt,
    args: [*]const i32,
    p_end: SvgPt,
    xs: [*]i32,
    ys: [*]i32,
    n: *i32,
) callconv(.c) void {
    const a = internal_arc_center(
        p0,
        p_end,
        args[k_svg_arc_rx],
        args[k_svg_arc_ry],
        args[k_svg_arc_rot],
        args[k_svg_arc_large] != 0,
        args[k_svg_arc_sweep] != 0,
    );
    if (!a.ok) {
        if (n.* < @as(i32, @intCast(k_svg_poly_max))) {
            xf.priv_ra8_svgp_map_point(t, p_end.x, p_end.y, &xs[@intCast(n.*)], &ys[@intCast(n.*)]);
            n.* += 1;
        }
        return;
    }
    const segs = internal_arc_segs(a.dt);
    // Bounded: segs (<= k_svg_arc_seg_max) samples, capped by k_svg_poly_max.
    var j: i32 = 1;
    while ((j <= segs) and (n.* < @as(i32, @intCast(k_svg_poly_max)))) : (j += 1) {
        const th = a.t1 + (a.dt * (@as(f32, @floatFromInt(j)) / @as(f32, @floatFromInt(segs))));
        const ex = a.rx * std.math.cos(th);
        const ey = a.ry * std.math.sin(th);
        const px = a.cx + (a.cos_phi * ex) - (a.sin_phi * ey);
        const py = a.cy + (a.sin_phi * ex) + (a.cos_phi * ey);
        xf.priv_ra8_svgp_map_point(t, @as(i32, @intFromFloat(px)), @as(i32, @intFromFloat(py)), &xs[@intCast(n.*)], &ys[@intCast(n.*)]);
        n.* += 1;
    }
}

// ===========================================================================
// Tests
// ===========================================================================

const testing = std.testing;
const seam = @import("reflow_render_test_seam.zig");

/// The recorded ra8_gfx draw calls since the last reset.
fn calls() []seam.ShapeCall {
    return seam.shape_calls[0..seam.shape_call_n];
}

/// An identity user-to-framebuffer map on a 64x64 box (scale 1, offset 0).
fn identityXform() SvgXform {
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

test "col lerp endpoints and mid" {
    try testing.expectEqual(@as(u32, 0x000000), internal_col_lerp(0x000000, 0xFFFFFF, 0.0));
    try testing.expectEqual(@as(u32, 0xFFFFFF), internal_col_lerp(0x000000, 0xFFFFFF, 1.0));
    // Halfway between black and white: every channel rounds down to 127.
    try testing.expectEqual(@as(u32, 0x7F7F7F), internal_col_lerp(0x000000, 0xFFFFFF, 0.5));
    // A uniform colour stays uniform.
    try testing.expectEqual(@as(u32, 0xFF0000), internal_col_lerp(0xFF0000, 0xFF0000, 0.7));
}

fn gradLinear(off0: f32, col0: u32, off1: f32, col1: u32) SvgGrad {
    var g = SvgGrad{
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

test "grad eval clamps and interpolates" {
    // No stops: the default fill.
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
    try testing.expectEqual(svg.k_svg_def_fill, internal_grad_eval(&empty, 0.5, 0.5));

    const g = gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF);
    try testing.expectEqual(@as(u32, 0x000000), internal_grad_eval(&g, 0.0, 0.5));
    try testing.expectEqual(@as(u32, 0xFFFFFF), internal_grad_eval(&g, 1.0, 0.5));
    // Before the first stop and after the last clamp to the ends.
    try testing.expectEqual(@as(u32, 0x000000), internal_grad_eval(&g, -0.5, 0.5));
    try testing.expectEqual(@as(u32, 0xFFFFFF), internal_grad_eval(&g, 1.5, 0.5));
    // Midway is the half blend.
    try testing.expectEqual(@as(u32, 0x7F7F7F), internal_grad_eval(&g, 0.5, 0.5));

    // Radial: distance from (x1, y1) over radius x2.
    var rad = gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF);
    rad.kind = svg.k_svg_grad_radial;
    rad.x1 = 0.5;
    rad.y1 = 0.5;
    rad.x2 = 0.5;
    try testing.expectEqual(@as(u32, 0x000000), internal_grad_eval(&rad, 0.5, 0.5));
    try testing.expectEqual(@as(u32, 0xFFFFFF), internal_grad_eval(&rad, 1.0, 0.5));
}

test "scanline x crossings and sort" {
    // An axis-aligned quad (0,0), (8,0), (8,8), (0,8): scanline y = 4
    // crosses the two vertical edges at x = 0 and x = 8.
    const xs = [_]i32{ 0, 8, 8, 0 };
    const ys = [_]i32{ 0, 0, 8, 8 };
    var xint: [k_svg_poly_max]i32 = @splat(0);
    const m = internal_scanline_x(&xs, &ys, 4, &xint);
    try testing.expectEqual(@as(i32, 2), m);
    internal_sort_i32(xint[0..@intCast(m)]);
    try testing.expectEqual(@as(i32, 0), xint[0]);
    try testing.expectEqual(@as(i32, 8), xint[1]);
    // Above and on the bottom edge: no crossings (half-open rule).
    try testing.expectEqual(@as(i32, 0), internal_scanline_x(&xs, &ys, -1, &xint));
    try testing.expectEqual(@as(i32, 0), internal_scanline_x(&xs, &ys, 8, &xint));
}

test "fill poly emits one span per row" {
    seam.shapeCallsReset();
    const xs = [_]i32{ 2, 10, 10, 2 };
    const ys = [_]i32{ 2, 2, 10, 10 };
    priv_ra8_svgp_fill_poly(&xs, &ys, 4, 0x00FF00);

    // Rows y = 2..9 each fill one span (2, y) to (10, y); the bottom edge
    // row is excluded by the half-open rule.
    const cs = calls();
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

    // Too few vertices: nothing drawn.
    seam.shapeCallsReset();
    const two_x = [_]i32{ 2, 10, 10 };
    const two_y = [_]i32{ 2, 2, 10 };
    priv_ra8_svgp_fill_poly(&two_x, &two_y, 2, 0x00FF00);
    try testing.expectEqual(@as(usize, 0), calls().len);
}

test "fill poly grad evaluates per pixel" {
    seam.shapeCallsReset();
    const xs = [_]i32{ 0, 10, 10, 0 };
    const ys = [_]i32{ 0, 0, 10, 10 };
    const g = gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF);
    priv_ra8_svgp_fill_poly_grad(&xs, &ys, 4, &g);

    // Rows y = 0..9, each spanning x = 0..10 inclusive: 110 pixel writes.
    const cs = calls();
    try testing.expectEqual(@as(usize, 110), cs.len);
    try testing.expectEqual(@as(u8, 4), cs[0].kind);
    try testing.expectEqual(@as(i32, 0), cs[0].a);
    try testing.expectEqual(@as(i32, 0), cs[0].b);
    try testing.expectEqual(@as(u32, 0x000000), cs[0].color);
    // The gradient is bbox-relative and linear in x: the midpoint pixel is
    // the half blend.
    const mid = cs[5 * 11 + 5];
    try testing.expectEqual(@as(i32, 5), mid.a);
    try testing.expectEqual(@as(i32, 5), mid.b);
    try testing.expectEqual(@as(u32, 0x7F7F7F), mid.color);
    // The right edge is white.
    try testing.expectEqual(@as(u32, 0xFFFFFF), cs[5 * 11 + 10].color);
}

test "parse points maps and bounds" {
    const t = identityXform();
    var xs: [k_svg_poly_max]i32 = @splat(0);
    var ys: [k_svg_poly_max]i32 = @splat(0);
    const v = "0,0 4, 0 4,4";
    const n = internal_parse_points(v, v.len, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 0), xs[0]);
    try testing.expectEqual(@as(i32, 4), xs[1]);
    try testing.expectEqual(@as(i32, 4), ys[2]);

    // Cap: more pairs than k_svg_poly_max stops at the cap.
    const long = try testing.allocator.alloc(u8, (k_svg_poly_max + 8) * 8);
    defer testing.allocator.free(long);
    for (0..k_svg_poly_max + 8) |i| {
        _ = try std.fmt.bufPrint(long[i * 8 ..][0..8], "{d},{d} ", .{ i, i });
    }
    const capped = internal_parse_points(long.ptr, long.len, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, @intCast(k_svg_poly_max)), capped);
}

test "draw rect fast path and gate" {
    seam.shapeCallsReset();
    const tag = "<rect x=\"2\" y=\"2\" width=\"4\" height=\"4\" fill=\"#FF0000\"/>";
    priv_ra8_svgp_draw_rect(tag.ptr, tag.len, &identityXform());
    const cs = calls();
    try testing.expectEqual(@as(usize, 1), cs.len);
    try testing.expectEqual(@as(u8, 2), cs[0].kind);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 4), cs[0].c);
    try testing.expectEqual(@as(i32, 4), cs[0].d);
    try testing.expectEqual(@as(u32, 0xFF0000), cs[0].color);
    try testing.expectEqual(@as(u8, 1), cs[0].filled);

    // fill="none": the gate returns without drawing.
    seam.shapeCallsReset();
    const none = "<rect x=\"30\" y=\"30\" width=\"4\" height=\"4\" fill=\"none\"/>";
    priv_ra8_svgp_draw_rect(none.ptr, none.len, &identityXform());
    try testing.expectEqual(@as(usize, 0), calls().len);
}

test "draw rect maps under shear" {
    seam.shapeCallsReset();
    // skewX(45): (x, y) -> (x + y, y). The quad (0..4, 0..4) becomes a
    // parallelogram; the scanline path fills rows y = 0..3, each shifted
    // right by its y.
    var t2 = identityXform();
    const xsh = "transform=\"skewX(45)\"";
    xf.priv_ra8_svgp_apply_xform(&t2, xsh.ptr, xsh.len);
    const tag = "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\" fill=\"#00FF00\"/>";
    priv_ra8_svgp_draw_rect(tag.ptr, tag.len, &t2);
    const cs = calls();
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
    priv_ra8_svgp_draw_circle(tag.ptr, tag.len, &identityXform());
    const cs = calls();
    try testing.expectEqual(@as(usize, 1), cs.len);
    try testing.expectEqual(@as(u8, 3), cs[0].kind);
    try testing.expectEqual(@as(i32, 8), cs[0].a);
    try testing.expectEqual(@as(i32, 8), cs[0].b);
    try testing.expectEqual(@as(i32, 4), cs[0].c);
    try testing.expectEqual(@as(u32, 0xFF0000), cs[0].color);
    try testing.expectEqual(@as(u8, 1), cs[0].filled);

    // Under a rotation the N-gon path fills with spans; the fill still
    // happens and stays bounded.
    seam.shapeCallsReset();
    var t = identityXform();
    const xsh = "transform=\"rotate(45)\"";
    xf.priv_ra8_svgp_apply_xform(&t, xsh.ptr, xsh.len);
    priv_ra8_svgp_draw_circle(tag.ptr, tag.len, &t);
    try testing.expect(calls().len > 0);
    for (calls()) |c| {
        try testing.expectEqual(@as(u8, 1), c.kind);
    }
}

test "draw line uses stroke" {
    seam.shapeCallsReset();
    const tag = "<line x1=\"2\" y1=\"2\" x2=\"10\" y2=\"2\" stroke=\"#FF0000\"/>";
    priv_ra8_svgp_draw_line(tag.ptr, tag.len, &identityXform());
    const cs = calls();
    try testing.expectEqual(@as(usize, 1), cs.len);
    try testing.expectEqual(@as(u8, 1), cs[0].kind);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 10), cs[0].c);
    try testing.expectEqual(@as(i32, 2), cs[0].d);

    // No stroke: nothing drawn.
    seam.shapeCallsReset();
    const bare = "<line x1=\"2\" y1=\"20\" x2=\"10\" y2=\"20\"/>";
    priv_ra8_svgp_draw_line(bare.ptr, bare.len, &identityXform());
    try testing.expectEqual(@as(usize, 0), calls().len);
}

test "draw polygon and polyline" {
    seam.shapeCallsReset();
    const poly = "<polygon fill=\"#00FF00\" points=\"2,2 10,2 10,10 2,10\"/>";
    priv_ra8_svgp_draw_polygon(poly.ptr, poly.len, &identityXform());
    const cs = calls();
    try testing.expectEqual(@as(usize, 8), cs.len);
    try testing.expectEqual(@as(i32, 2), cs[0].a);
    try testing.expectEqual(@as(i32, 2), cs[0].b);
    try testing.expectEqual(@as(i32, 10), cs[0].c);
    try testing.expectEqual(@as(i32, 2), cs[0].d);

    // Missing points: nothing drawn.
    seam.shapeCallsReset();
    const bare = "<polygon fill=\"#00FF00\"/>";
    priv_ra8_svgp_draw_polygon(bare.ptr, bare.len, &identityXform());
    try testing.expectEqual(@as(usize, 0), calls().len);

    seam.shapeCallsReset();
    const line = "<polyline stroke=\"#FF0000\" points=\"2,30 10,30 10,40\"/>";
    priv_ra8_svgp_draw_polyline(line.ptr, line.len, &identityXform());
    const cs2 = calls();
    try testing.expectEqual(@as(usize, 2), cs2.len);
    try testing.expectEqual(@as(i32, 10), cs2[1].a);
    try testing.expectEqual(@as(i32, 30), cs2[1].b);
    try testing.expectEqual(@as(i32, 10), cs2[1].c);
    try testing.expectEqual(@as(i32, 40), cs2[1].d);
}

test "arc flatten degenerate and quarter" {
    const t = identityXform();
    var xs: [k_svg_poly_max]i32 = @splat(0);
    var ys: [k_svg_poly_max]i32 = @splat(0);

    // Zero radius: the degenerate line-to.
    var n: i32 = 0;
    const args0 = [_]i32{ 0, 0, 0, 0, 1, 0, 0 };
    priv_ra8_svgp_flatten_arc(&t, .{ .x = 0, .y = 0 }, &args0, .{ .x = 10, .y = 0 }, &xs, &ys, &n);
    try testing.expectEqual(@as(i32, 1), n);
    try testing.expectEqual(@as(i32, 10), xs[0]);

    // Quarter arc from (0, 0) to (10, 0) with r = 5, sweep = 1: the samples
    // bulge off the chord and the last sample lands on the endpoint.
    n = 0;
    const args = [_]i32{ 5, 5, 0, 0, 1, 10, 0 };
    priv_ra8_svgp_flatten_arc(&t, .{ .x = 0, .y = 0 }, &args, .{ .x = 10, .y = 0 }, &xs, &ys, &n);
    try testing.expect(n > 1);
    try testing.expect(n <= @as(i32, @intCast(k_svg_poly_max)));
    try testing.expectEqual(@as(i32, 10), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 0), ys[@intCast(n - 1)]);
    // Sweep=1 runs angles 180 to 360 degrees: mid samples sit above the
    // chord (negative y in SVG user space).
    try testing.expect(ys[@intCast(@divTrunc(n, 2))] < 0);
}

test "arc center solves the quarter circle" {
    const a = internal_arc_center(.{ .x = 0, .y = 0 }, .{ .x = 10, .y = 0 }, 5, 5, 0, false, true);
    try testing.expect(a.ok);
    try testing.expectEqual(@as(f32, 5.0), a.cx);
    try testing.expectEqual(@as(f32, 0.0), a.cy);

    // Coincident endpoints are degenerate.
    const d = internal_arc_center(.{ .x = 3, .y = 3 }, .{ .x = 3, .y = 3 }, 5, 5, 0, false, true);
    try testing.expect(!d.ok);
}

test "arc segs bounded" {
    try testing.expectEqual(@as(i32, 1), internal_arc_segs(0.0));
    try testing.expectEqual(@as(i32, 2), internal_arc_segs(0.4));
    try testing.expectEqual(k_svg_arc_seg_max, internal_arc_segs(100.0));
    try testing.expectEqual(k_svg_arc_seg_max, internal_arc_segs(-100.0));
}
