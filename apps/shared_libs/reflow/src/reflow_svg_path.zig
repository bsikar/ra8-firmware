//! SVG `<path>` `d` mini-language parser + Bezier flatten (RA8FW-1236).
//!
//! The `d` mini-language: M/L/H/V/Z exact, the cubic 'C'/'S' and quadratic
//! 'Q'/'T' Bezier curves flattened to line segments (smooth forms reflect the
//! previous control point), and the elliptical arc 'A' delegated to the shared
//! arc flatten in reflow_svg_shape.zig. The resulting vertices are filled by
//! the shared scanline polygon fill. No DOM, no heap. Mirrors the hand-rolled
//! C TU reflow_svg_path.c, deleted in the same commit; only
//! priv_ra8_svgp_draw_path keeps C linkage for the still-C SVG doc TU.

const std = @import("std");
const svg = @import("reflow_svg.zig");
const xf = @import("reflow_svg_xform.zig");
const shape = @import("reflow_svg_shape.zig");

const SvgXform = xf.SvgXform;
const SvgPt = shape.SvgPt;
const SvgGrad = svg.SvgGrad;

// Path-scope constants mirrored from reflow_svg_internal.h.
/// Max args of a path command (`A`).
const k_svg_path_args: usize = 7;
/// Endpoint pair size (x, y); M/L/T.
const k_svg_path_ep: usize = 2;
/// Arg count of `S` / `Q`.
const k_svg_argc_quad: usize = 4;
/// Arg count of `C`.
const k_svg_argc_cube: usize = 6;
/// Segments per cubic-Bezier flatten.
const k_svg_curve_seg: usize = 12;

/// Return the argument count for a lower-cased SVG path command letter, or
/// -1 when the letter is unrecognised.
fn internal_cmd_argc(u: u8) i32 {
    return switch (u) {
        'm', 'l', 't' => @intCast(k_svg_path_ep),
        'h', 'v' => 1,
        'c' => @intCast(k_svg_argc_cube),
        's', 'q' => @intCast(k_svg_argc_quad),
        'a' => @intCast(k_svg_path_args),
        'z' => 0,
        else => -1,
    };
}

/// Advance the current path point to the endpoint of a line-type command:
/// 'h' and 'v' update one axis, every other command ends with an (x, y) pair.
fn internal_path_step(u: u8, rel: bool, args: []const i32, na: i32, cx: *i32, cy: *i32) void {
    if (u == 'h') {
        cx.* = if (rel) cx.* + args[0] else args[0];
        return;
    }
    if (u == 'v') {
        cy.* = if (rel) cy.* + args[0] else args[0];
        return;
    }
    const ex = args[@intCast(na - @as(i32, @intCast(k_svg_path_ep)))];
    const ey = args[@intCast(na - 1)];
    cx.* = if (rel) cx.* + ex else ex;
    cy.* = if (rel) cy.* + ey else ey;
}

/// Evaluate one axis of a cubic Bernstein-Bezier polynomial at parameter tt.
fn internal_bezier1(tt: f32, c0: f32, c1: f32, c2: f32, c3: f32) f32 {
    const mt = 1.0 - tt;
    return ((mt * mt * mt) * c0) + (3.0 * (mt * mt) * tt * c1) + (3.0 * mt * (tt * tt) * c2) +
        ((tt * tt * tt) * c3);
}

/// Evaluate one axis of a quadratic Bernstein-Bezier polynomial at parameter tt.
fn internal_bezier_q1(tt: f32, c0: f32, c1: f32, c2: f32) f32 {
    const mt = 1.0 - tt;
    return ((mt * mt) * c0) + (2.0 * mt * tt * c1) + ((tt * tt) * c2);
}

/// Flatten a cubic Bezier (P0..P3, user space) into the vertex arrays,
/// sampling k_svg_curve_seg values in (0, 1], capped by k_svg_poly_max.
fn internal_flatten_cubic(t: *const SvgXform, p0: SvgPt, p1: SvgPt, p2: SvgPt, p3: SvgPt, xs: []i32, ys: []i32, n: *i32) void {
    var j: i32 = 1;
    while ((j <= @as(i32, @intCast(k_svg_curve_seg))) and (n.* < @as(i32, @intCast(shape.k_svg_poly_max)))) : (j += 1) {
        const tt = @as(f32, @floatFromInt(j)) / @as(f32, @floatFromInt(k_svg_curve_seg));
        const bx = internal_bezier1(tt, @floatFromInt(p0.x), @floatFromInt(p1.x), @floatFromInt(p2.x), @floatFromInt(p3.x));
        const by = internal_bezier1(tt, @floatFromInt(p0.y), @floatFromInt(p1.y), @floatFromInt(p2.y), @floatFromInt(p3.y));
        xf.priv_ra8_svgp_map_point(t, @intFromFloat(bx), @intFromFloat(by), &xs[@intCast(n.*)], &ys[@intCast(n.*)]);
        n.* += 1;
    }
}

/// Flatten a quadratic Bezier (P0..P2, user space) into the vertex arrays,
/// sampling k_svg_curve_seg values in (0, 1], capped by k_svg_poly_max.
fn internal_flatten_quad(t: *const SvgXform, p0: SvgPt, p1: SvgPt, p2: SvgPt, xs: []i32, ys: []i32, n: *i32) void {
    var j: i32 = 1;
    while ((j <= @as(i32, @intCast(k_svg_curve_seg))) and (n.* < @as(i32, @intCast(shape.k_svg_poly_max)))) : (j += 1) {
        const tt = @as(f32, @floatFromInt(j)) / @as(f32, @floatFromInt(k_svg_curve_seg));
        const bx = internal_bezier_q1(tt, @floatFromInt(p0.x), @floatFromInt(p1.x), @floatFromInt(p2.x));
        const by = internal_bezier_q1(tt, @floatFromInt(p0.y), @floatFromInt(p1.y), @floatFromInt(p2.y));
        xf.priv_ra8_svgp_map_point(t, @intFromFloat(bx), @intFromFloat(by), &xs[@intCast(n.*)], &ys[@intCast(n.*)]);
        n.* += 1;
    }
}

/// Resolve an absolute or relative SVG point from an argument pair.
fn internal_arg_pt(rel: bool, cx: i32, cy: i32, ax: i32, ay: i32) SvgPt {
    return .{ .x = if (rel) cx + ax else ax, .y = if (rel) cy + ay else ay };
}

/// Running cursor state while parsing a `d` string: current point plus the
/// previous curve's last control point and kind, so the smooth commands
/// S/s and T/t can reflect it. kind is 'q', 'c', or 0.
const PathState = struct {
    cx: i32,
    cy: i32,
    ctrl: SvgPt,
    kind: u8,
};

/// Reflect control point ctrl through current point cur (SVG smooth-command
/// rule: implicit first control = 2*cur - ctrl).
fn internal_reflect(cur: SvgPt, ctrl: SvgPt) SvgPt {
    return .{ .x = 2 * cur.x - ctrl.x, .y = 2 * cur.y - ctrl.y };
}

/// Derive the first control point for a smooth curve command: reflect the
/// previous matching control through p0, else coincide with p0.
fn internal_smooth_ctrl(p0: SvgPt, st: *const PathState, want: u8) SvgPt {
    return if (st.kind == want) internal_reflect(p0, st.ctrl) else p0;
}

/// Flatten a cubic Bezier (P0..P3) into the vertex list and advance the path
/// state: records P2 as the new smooth control, kind 'c', cursor at P3.
fn internal_emit_cubic(t: *const SvgXform, p0: SvgPt, p1: SvgPt, p2: SvgPt, p3: SvgPt, st: *PathState, xs: []i32, ys: []i32, n: i32) i32 {
    var m = n;
    internal_flatten_cubic(t, p0, p1, p2, p3, xs, ys, &m);
    st.ctrl = p2;
    st.kind = 'c';
    st.cx = p3.x;
    st.cy = p3.y;
    return m;
}

/// Flatten a quadratic Bezier (P0..P2) into the vertex list and advance the
/// path state: records P1 as the new smooth control, kind 'q', cursor at P2.
fn internal_emit_quad(t: *const SvgXform, p0: SvgPt, p1: SvgPt, p2: SvgPt, st: *PathState, xs: []i32, ys: []i32, n: i32) i32 {
    var m = n;
    internal_flatten_quad(t, p0, p1, p2, xs, ys, &m);
    st.ctrl = p1;
    st.kind = 'q';
    st.cx = p2.x;
    st.cy = p2.y;
    return m;
}

/// Flatten a cubic/quadratic path curve command into the vertex list: 'C'/'c',
/// 'S'/'s' (smooth cubic), 'Q'/'q', 'T'/'t' (smooth quadratic), and the
/// elliptical arc 'A'/'a' (centre-parametrised via the shared arc flatten).
/// Smooth forms reflect the previous matching control point. Returns -1 for
/// any other command so the caller falls back to the endpoint-chord path.
fn internal_path_curve(t: *const SvgXform, u: u8, rel: bool, args: []const i32, st: *PathState, xs: []i32, ys: []i32, n: i32) i32 {
    const p0 = SvgPt{ .x = st.cx, .y = st.cy };
    if ((u == 'q') or (u == 't')) {
        const sm = (u == 't');
        const p1 = if (sm) internal_smooth_ctrl(p0, st, 'q') else internal_arg_pt(rel, st.cx, st.cy, args[0], args[1]);
        const e: usize = if (sm) 0 else 2;
        const p2 = internal_arg_pt(rel, st.cx, st.cy, args[e], args[e + 1]);
        return internal_emit_quad(t, p0, p1, p2, st, xs, ys, n);
    }
    if ((u == 'c') or (u == 's')) {
        const sm = (u == 's');
        const p1 = if (sm) internal_smooth_ctrl(p0, st, 'c') else internal_arg_pt(rel, st.cx, st.cy, args[0], args[1]);
        const b: usize = if (sm) 0 else 2;
        const p2 = internal_arg_pt(rel, st.cx, st.cy, args[b], args[b + 1]);
        const p3 = internal_arg_pt(rel, st.cx, st.cy, args[b + 2], args[b + 3]);
        return internal_emit_cubic(t, p0, p1, p2, p3, st, xs, ys, n);
    }
    if (u == 'a') {
        const p_end = internal_arg_pt(rel, st.cx, st.cy, args[shape.k_svg_arc_ex], args[shape.k_svg_arc_ey]);
        var m = n;
        shape.priv_ra8_svgp_flatten_arc(t, p0, args.ptr, p_end, xs.ptr, ys.ptr, &m);
        st.kind = 0; // an arc breaks the smooth-reflection chain
        st.cx = p_end.x;
        st.cy = p_end.y;
        return m;
    }
    return -1;
}

/// Resolve the next path command at d[i]: skips whitespace and commas, reads
/// an ASCII letter (advancing past it and storing it as last), or repeats the
/// previous letter for an implicit argument run. Returns 0 at end of buffer.
fn internal_next_cmd(d: []const u8, i: *usize, last: *u8) u8 {
    while ((i.* < d.len) and (svg.priv_ra8_svgp_ws(d[i.*]) or (d[i.*] == ','))) {
        i.* += 1;
    }
    if (i.* >= d.len) {
        return 0;
    }
    const c = d[i.*];
    if (((c >= 'A') and (c <= 'Z')) or ((c >= 'a') and (c <= 'z'))) {
        last.* = c;
        i.* += 1;
        return c;
    }
    return last.*;
}

/// Parse a path `d` value into framebuffer-space vertices; return the count.
/// M/L/H/V/Z exact (absolute + relative, implicit-L repeats after M), curves
/// and arcs flattened into the vertex array, multiple subpaths merged into one
/// polygon. 'Z' is silently accepted; the polygon fill closes implicitly.
fn internal_parse_path(d: []const u8, t: *const SvgXform, xs: []i32, ys: []i32) i32 {
    var n: i32 = 0;
    var st = PathState{ .cx = 0, .cy = 0, .ctrl = .{ .x = 0, .y = 0 }, .kind = 0 };
    var last: u8 = 0;
    var i: usize = 0;
    // Bounded: <= k_svg_poly_max commands. Every non-'z' command emits at
    // least one vertex (n advances) or hits the vertex cap, and the 'z' arm
    // advances i on a malformed implicit repeat, so the loop always makes
    // progress and cannot spin on untrusted input (NASA P10 Rule 2).
    while ((i < d.len) and (n < @as(i32, @intCast(shape.k_svg_poly_max)))) {
        const i_before = i;
        const c = internal_next_cmd(d, &i, &last);
        const u = svg.priv_ra8_svgp_lc(c);
        // internal_next_cmd only yields an ASCII letter or 0; (c <= 'z') is
        // always true once (c >= 'a') holds, so rel is the lower-case test.
        const rel = (c >= 'a') and (c <= 'z');
        const na: i32 = if (c != 0) internal_cmd_argc(u) else -1;
        if (na < 0) {
            break; // unknown / no current command
        }
        if (u == 'z') {
            // Close: the fill closes implicitly. 'z' takes no args, so an
            // implicit repeat after a non-command byte leaves the cursor
            // unmoved and would spin forever: skip the byte for progress.
            if (i == i_before) {
                i += 1;
            }
            continue;
        }
        var args: [k_svg_path_args]i32 = @splat(0);
        var a: i32 = 0;
        while (a < na) : (a += 1) {
            args[@intCast(a)] = svg.priv_ra8_svgp_num(d.ptr, d.len, &i);
        }
        const cn = internal_path_curve(t, u, rel, &args, &st, xs, ys, n);
        if (cn >= 0) {
            n = cn; // C/S/Q/T flattened into segments
        } else {
            internal_path_step(u, rel, &args, na, &st.cx, &st.cy);
            xf.priv_ra8_svgp_map_point(t, st.cx, st.cy, &xs[@intCast(n)], &ys[@intCast(n)]);
            n += 1;
            st.kind = 0; // a non-curve breaks the smooth-reflection chain
        }
        if (u == 'm') {
            last = if (rel) 'l' else 'L'; // implicit coords after M are line-tos
        }
    }
    return n;
}

/// Draw one SVG 'path' element as a filled polygon: resolve the fill (solid
/// or gradient), read the 'd' attribute, parse it into framebuffer-space
/// vertices, and fill with the shared scanline polygon fill. Returns without
/// drawing when the fill is absent/none or the 'd' attribute is missing.
pub export fn priv_ra8_svgp_draw_path(s: [*]const u8, len: usize, t: *const SvgXform) callconv(.c) void {
    var gi: i32 = -1;
    const fill = svg.priv_ra8_svgp_resolve_fill(s, len, t, svg.k_svg_def_fill, &gi);
    var off: usize = 0;
    var vl: usize = 0;
    if (((gi < 0) and (fill == svg.k_svg_no_paint)) or (!svg.priv_ra8_svgp_attr(s, len, "d", &off, &vl))) {
        return;
    }
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const n = internal_parse_path(s[off .. off + vl], t, &xs, &ys);
    if (gi >= 0) {
        shape.priv_ra8_svgp_fill_poly_grad(&xs, &ys, n, &t.grads.?.g[@intCast(gi)]);
    } else {
        shape.priv_ra8_svgp_fill_poly(&xs, &ys, n, fill);
    }
}

// ===========================================================================
// Tests (host test harness, RA8FW-1236)
// ===========================================================================

const testing = std.testing;
const seam = @import("reflow_render_test_seam.zig");

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

test "cmd argc table" {
    try testing.expectEqual(@as(i32, 2), internal_cmd_argc('m'));
    try testing.expectEqual(@as(i32, 2), internal_cmd_argc('l'));
    try testing.expectEqual(@as(i32, 2), internal_cmd_argc('t'));
    try testing.expectEqual(@as(i32, 1), internal_cmd_argc('h'));
    try testing.expectEqual(@as(i32, 1), internal_cmd_argc('v'));
    try testing.expectEqual(@as(i32, 6), internal_cmd_argc('c'));
    try testing.expectEqual(@as(i32, 4), internal_cmd_argc('s'));
    try testing.expectEqual(@as(i32, 4), internal_cmd_argc('q'));
    try testing.expectEqual(@as(i32, 7), internal_cmd_argc('a'));
    try testing.expectEqual(@as(i32, 0), internal_cmd_argc('z'));
    try testing.expectEqual(@as(i32, -1), internal_cmd_argc('w'));
}

test "path step h v and endpoint pair" {
    var cx: i32 = 0;
    var cy: i32 = 0;
    const args_h = [_]i32{8};
    internal_path_step('h', false, &args_h, 1, &cx, &cy);
    try testing.expectEqual(@as(i32, 8), cx);
    try testing.expectEqual(@as(i32, 0), cy);
    const args_v = [_]i32{5};
    internal_path_step('v', true, &args_v, 1, &cx, &cy);
    try testing.expectEqual(@as(i32, 8), cx);
    try testing.expectEqual(@as(i32, 5), cy);
    const args_pt = [_]i32{ 3, 4 };
    internal_path_step('l', true, &args_pt, 2, &cx, &cy);
    try testing.expectEqual(@as(i32, 11), cx);
    try testing.expectEqual(@as(i32, 9), cy);
    internal_path_step('l', false, &args_pt, 2, &cx, &cy);
    try testing.expectEqual(@as(i32, 3), cx);
    try testing.expectEqual(@as(i32, 4), cy);
}

test "bezier cubic and quad sample curves" {
    try testing.expectEqual(@as(f32, 0.0), internal_bezier1(0.0, 0, 0, 0, 1));
    try testing.expectEqual(@as(f32, 1.0), internal_bezier1(1.0, 0, 0, 0, 1));
    try testing.expectEqual(@as(f32, 0.125), internal_bezier1(0.5, 0, 0, 0, 1));
    // A symmetric cubic bulges to 3/4 at the midpoint.
    try testing.expectEqual(@as(f32, 0.75), internal_bezier1(0.5, 0, 1, 1, 0));
    // A quadratic with a centred control is linear, midpoint 1.
    try testing.expectEqual(@as(f32, 1.0), internal_bezier_q1(0.5, 0, 1, 2));
}

test "parse m l and z exact" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 L 10,2 L 10,10 Z";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 2), xs[0]);
    try testing.expectEqual(@as(i32, 2), ys[0]);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 2), ys[1]);
    try testing.expectEqual(@as(i32, 10), xs[2]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "implicit repeats after m are line-tos" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 10,2 10,10";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "relative commands move and line" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "m 2,2 l 8,0 0,8";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 2), xs[0]);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "h and v shorthand" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 H 10 V 10";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 3), n);
    try testing.expectEqual(@as(i32, 10), xs[1]);
    try testing.expectEqual(@as(i32, 2), ys[1]);
    try testing.expectEqual(@as(i32, 10), ys[2]);
}

test "cubic flatten hits the exact midpoint" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 C 4,0 8,10 12,10";
    const n = internal_parse_path(d, &t, &xs, &ys);
    // M plus k_svg_curve_seg samples.
    try testing.expectEqual(@as(i32, @intCast(1 + k_svg_curve_seg)), n);
    // tt = 1/2 is exact in f32, so the mid sample is exact too.
    try testing.expectEqual(@as(i32, 6), xs[6]);
    try testing.expectEqual(@as(i32, 5), ys[6]);
    // The final sample is the endpoint.
    try testing.expectEqual(@as(i32, 12), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 10), ys[@intCast(n - 1)]);
}

test "smooth cubic s reflects control" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 C 2,2 4,4 6,6 S 8,8 10,10";
    const n = internal_parse_path(d, &t, &xs, &ys);
    // M + cubic samples + smooth-cubic samples.
    try testing.expectEqual(@as(i32, @intCast(1 + 2 * k_svg_curve_seg)), n);
    try testing.expectEqual(@as(i32, 10), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 10), ys[@intCast(n - 1)]);
}

test "smooth quad t reflects control" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 Q 4,4 8,8 T 16,16";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, @intCast(1 + 2 * k_svg_curve_seg)), n);
    try testing.expectEqual(@as(i32, 16), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 16), ys[@intCast(n - 1)]);
}

test "arc delegates to the shared flatten" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 0,0 A 5 5 0 0 1 10,0";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expect(n > 1);
    try testing.expectEqual(@as(i32, 0), xs[0]);
    try testing.expectEqual(@as(i32, 0), ys[0]);
    try testing.expectEqual(@as(i32, 10), xs[@intCast(n - 1)]);
    try testing.expectEqual(@as(i32, 0), ys[@intCast(n - 1)]);
}

test "malformed z repeat cannot spin" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const n1 = internal_parse_path("M 2,2 L 10,2 Z", &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 2), n1);
    // "Z2": the implicit repeat after 'z' skips the byte instead of looping.
    const n2 = internal_parse_path("Z2", &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 0), n2);
}

test "unknown letter stops parsing" {
    const t = identityXform();
    var xs: [shape.k_svg_poly_max]i32 = @splat(0);
    var ys: [shape.k_svg_poly_max]i32 = @splat(0);
    const d = "M 2,2 X 5";
    const n = internal_parse_path(d, &t, &xs, &ys);
    try testing.expectEqual(@as(i32, 1), n);
    try testing.expectEqual(@as(i32, 2), xs[0]);
}

test "draw path gate fill none and missing d" {
    const t = identityXform();
    const no_fill = "<path d=\"M 2,2 L 10,10\" fill=\"none\"/>";
    seam.shapeCallsReset();
    priv_ra8_svgp_draw_path(no_fill, no_fill.len, &t);
    try testing.expectEqual(@as(usize, 0), calls().len);
    const no_d = "<path fill=\"#FF0000\"/>";
    priv_ra8_svgp_draw_path(no_d, no_d.len, &t);
    try testing.expectEqual(@as(usize, 0), calls().len);
}

test "draw path solid fill spans" {
    seam.shapeCallsReset();
    const t = identityXform();
    const s = "<path d=\"M 2,2 L 10,2 L 10,10 L 2,10\" fill=\"#FF0000\"/>";
    priv_ra8_svgp_draw_path(s, s.len, &t);
    const cs = calls();
    // Filled rows y = 2..9, each an (x = 2 .. x = 10) span.
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
    var t = identityXform();
    var grads = svg.SvgGrads{ .n = 1, .g = @splat(gradLinear(0.0, 0x000000, 1.0, 0xFFFFFF)) };
    grads.g[0].id[0] = 'g';
    t.grads = &grads;
    const s = "<path d=\"M 2,2 L 10,2 L 10,10 L 2,10\" fill=\"url(#g)\"/>";
    priv_ra8_svgp_draw_path(s, s.len, &t);
    const cs = calls();
    try testing.expect(cs.len > 0);
    // A gradient fill goes per pixel, not per span.
    try testing.expectEqual(@as(u8, 4), cs[0].kind);
}
