// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

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
pub const svg = @import("reflow_svg.zig");
const xf = @import("reflow_svg_xform.zig");
pub const shape = @import("reflow_svg_shape.zig");

pub const SvgXform = xf.SvgXform;
const SvgPt = shape.SvgPt;
pub const SvgGrad = svg.SvgGrad;

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
pub const k_svg_curve_seg: usize = 12;

/// Return the argument count for a lower-cased SVG path command letter, or
/// -1 when the letter is unrecognised.
pub fn internal_cmd_argc(u: u8) i32 {
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
pub fn internal_path_step(u: u8, rel: bool, args: []const i32, na: i32, cx: *i32, cy: *i32) void {
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
pub fn internal_bezier1(tt: f32, c0: f32, c1: f32, c2: f32, c3: f32) f32 {
    const mt = 1.0 - tt;
    return ((mt * mt * mt) * c0) + (3.0 * (mt * mt) * tt * c1) + (3.0 * mt * (tt * tt) * c2) +
        ((tt * tt * tt) * c3);
}

/// Evaluate one axis of a quadratic Bernstein-Bezier polynomial at parameter tt.
pub fn internal_bezier_q1(tt: f32, c0: f32, c1: f32, c2: f32) f32 {
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
pub fn internal_parse_path(d: []const u8, t: *const SvgXform, xs: []i32, ys: []i32) i32 {
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
