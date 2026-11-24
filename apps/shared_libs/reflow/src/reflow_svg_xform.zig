// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! SVG viewBox->box coordinate mapping and `transform=` affine parsing
//! (src/reflow_svg_xform.c, RA8FW-1234).
//!
//! Zig port of the coordinate-transform stage of the minimal SVG subset:
//! the axis-aligned fast-path mappers, the full-affine point map through
//! viewBox->box, the SVG floating-point number parser, and the complete 2x3
//! affine `transform=` machinery (translate / scale / rotate / skewX /
//! skewY / matrix, composed left-to-right and applied per point). The
//! shape, path, and document-walk stages stay C for now and call these
//! helpers through reflow_svg_internal.h, so every export below keeps C
//! linkage and svg_xform_t / svg_utf_t are extern-struct mirrors of the
//! header. The scan-side helpers it composes (whitespace, case-insensitive
//! literals, attribute slicing) come from the reflow_svg.zig port.
//! No DOM, no heap.
//!
//! @since 0.1.0

const std = @import("std");
pub const svg = @import("reflow_svg.zig");

// ---- Constants (src/reflow_svg_internal.h, priv_svg_consts_t) ----

/// Decimal base.
const k_svg_dec: f32 = 10.0;
/// Max args of a path command (`A`); also the arg scratch size here, as in C.
const k_svg_path_args: usize = 7;
/// Arg count of `C`; the max arguments one transform function may consume.
const k_svg_argc_cube: usize = 6;

/// Degrees-per-radian source constant, as the C module spells it.
const k_pi: f32 = 3.14159265;
const k_deg_half: f32 = 180.0;

// ---- Extern mirrors of the shared geometry types ----
//
// svg_xform_t lives in the reflow_svg.zig port (RA8FW-1233) with comptime
// offset assertions; reuse it so there is exactly one mirror. svg_utf_t
// (the 2x3 affine) is new here: six f32 fields, byte-exact by construction.

/// A user `transform=` as a full 2x3 affine matrix: a point (x, y) maps to
/// (a*x + c*y + e, b*x + d*y + f).
pub const SvgUtf = extern struct {
    a: f32,
    b: f32,
    c: f32,
    d: f32,
    e: f32,
    f: f32,
};

pub const SvgXform = svg.SvgXform;

comptime {
    std.debug.assert(@sizeOf(SvgUtf) == 24);
}

// ---- Internal: affine algebra ----

/// The 2x3 identity affine (a = d = 1, everything else 0).
fn internal_utf_identity() SvgUtf {
    return .{ .a = 1.0, .b = 0.0, .c = 0.0, .d = 1.0, .e = 0.0, .f = 0.0 };
}

/// Compose two 2x3 affines: `a` (outer) over `b` (inner), so a point is
/// first mapped by `b` and then by `a`.
pub fn internal_utf_compose(a: SvgUtf, b: SvgUtf) SvgUtf {
    return .{
        .a = (a.a * b.a) + (a.c * b.b),
        .b = (a.b * b.a) + (a.d * b.b),
        .c = (a.a * b.c) + (a.c * b.d),
        .d = (a.b * b.c) + (a.d * b.d),
        .e = (a.a * b.e) + (a.c * b.f) + a.e,
        .f = (a.b * b.e) + (a.d * b.f) + a.f,
    };
}

/// Convert an angle from degrees to radians.
fn internal_deg2rad(deg: f32) f32 {
    return deg * (k_pi / k_deg_half);
}

// ---- Axis-aligned fast path ----

/// Apply the axis-aligned user transform (a / translate) to a user-space x.
/// Valid only where shear terms ub and uc are both zero; use
/// priv_ra8_svgp_map_point for the general case.
fn internal_ux(t: *const SvgXform, sx: i32) i32 {
    const fx: f32 = (@as(f32, @floatFromInt(sx)) * t.ua) + t.ue;
    return @intFromFloat(fx);
}

/// Apply the axis-aligned user transform (d / translate) to a user-space y.
fn internal_uy(t: *const SvgXform, sy: i32) i32 {
    const fy: f32 = (@as(f32, @floatFromInt(sy)) * t.ud) + t.uf;
    return @intFromFloat(fy);
}

/// Test whether the user transform contains rotation or shear: either
/// shear term non-zero means the fast-path primitives cannot represent the
/// shape and callers must fall back to priv_ra8_svgp_map_point per vertex.
pub export fn priv_ra8_svgp_has_rot(t: *const SvgXform) callconv(.c) bool {
    return (t.ub != 0.0) or (t.uc != 0.0);
}

/// Map a user-space point through the FULL affine, then viewBox->box. When
/// the affine is axis-aligned (ub == 0 and uc == 0) the result is identical
/// to (priv_ra8_svgp_mx, priv_ra8_svgp_my).
pub export fn priv_ra8_svgp_map_point(t: *const SvgXform, ux: i32, uy: i32, fx: *i32, fy: *i32) callconv(.c) void {
    const pxf: f32 = ((@as(f32, @floatFromInt(ux)) * t.ua) + (@as(f32, @floatFromInt(uy)) * t.uc)) + t.ue;
    const pyf: f32 = ((@as(f32, @floatFromInt(ux)) * t.ub) + (@as(f32, @floatFromInt(uy)) * t.ud)) + t.uf;
    const px: i32 = @intFromFloat(pxf);
    const py: i32 = @intFromFloat(pyf);
    fx.* = t.bx + @as(i32, @intCast(@divTrunc(@as(i64, px - t.vx) * @as(i64, t.bw), @as(i64, t.vw))));
    fy.* = t.by + @as(i32, @intCast(@divTrunc(@as(i64, py - t.vy) * @as(i64, t.bh), @as(i64, t.vh))));
}

/// Map a user-space x coordinate to a framebuffer x coordinate:
/// bx + (ux - vx) * bw / vw with 64-bit intermediates.
pub export fn priv_ra8_svgp_mx(t: *const SvgXform, sx: i32) callconv(.c) i32 {
    const ux = internal_ux(t, sx);
    return t.bx + @as(i32, @intCast(@divTrunc(@as(i64, ux - t.vx) * @as(i64, t.bw), @as(i64, t.vw))));
}

/// Map a user-space y coordinate to a framebuffer y coordinate:
/// by + (uy - vy) * bh / vh with 64-bit intermediates.
pub export fn priv_ra8_svgp_my(t: *const SvgXform, sy: i32) callconv(.c) i32 {
    const uy = internal_uy(t, sy);
    return t.by + @as(i32, @intCast(@divTrunc(@as(i64, uy - t.vy) * @as(i64, t.bh), @as(i64, t.vh))));
}

/// Scale a user-space length by the user x scale and the viewBox x ratio.
pub export fn priv_ra8_svgp_sx(t: *const SvgXform, sw: i32) callconv(.c) i32 {
    const uw: i32 = @intFromFloat(@as(f32, @floatFromInt(sw)) * t.ua);
    return @intCast(@divTrunc(@as(i64, uw) * @as(i64, t.bw), @as(i64, t.vw)));
}

// ---- SVG floating-point number parsing ----

/// Parse one SVG floating-point number at s[i.*], skipping leading
/// whitespace and comma separators, then an optional sign, integer digits,
/// and a fraction. On return i.* points one past the last consumed byte.
/// The accumulation order (v = v*10 + digit; frac /= 10; v += frac*digit)
/// is the C one, kept so parsed values match the C port bit for bit.
pub export fn priv_ra8_svgp_numf(s: [*]const u8, len: usize, i: *usize) callconv(.c) f32 {
    while ((i.* < len) and (svg.priv_ra8_svgp_ws(s[i.*]) or (s[i.*] == ','))) {
        i.* += 1;
    }
    var sgn: f32 = 1.0;
    if ((i.* < len) and ((s[i.*] == '-') or (s[i.*] == '+'))) {
        sgn = if (s[i.*] == '-') -1.0 else 1.0;
        i.* += 1;
    }
    var v: f32 = 0.0;
    while ((i.* < len) and (s[i.*] >= '0') and (s[i.*] <= '9')) {
        v = (v * k_svg_dec) + @as(f32, @floatFromInt(s[i.*] - '0'));
        i.* += 1;
    }
    if ((i.* < len) and (s[i.*] == '.')) {
        i.* += 1;
        var frac: f32 = 1.0;
        while ((i.* < len) and (s[i.*] >= '0') and (s[i.*] <= '9')) {
            frac /= k_svg_dec;
            v += frac * @as(f32, @floatFromInt(s[i.*] - '0'));
            i.* += 1;
        }
    }
    return v * sgn;
}

/// Test whether a byte position starts an SVG number literal: a digit, a
/// sign, or a decimal point. False past the end of the buffer.
fn internal_is_num_start(s: [*]const u8, len: usize, at: usize) bool {
    if (at >= len) {
        return false;
    }
    const c: u8 = s[at];
    return ((c >= '0') and (c <= '9')) or (c == '-') or (c == '+') or (c == '.');
}

// ---- `transform=` parsing ----

/// `transform=` function kinds recognised by the parser.
const SvgXfKind = enum(u8) {
    none = 0,
    translate = 1,
    scale = 2,
    rotate = 3,
    skewx = 4,
    skewy = 5,
    matrix = 6,
};

/// Read up to k_svg_argc_cube float arguments from a transform function's
/// `(...)` list, skipping separators between them. Returns the count read.
fn internal_xform_read(v: [*]const u8, vlen: usize, j: *usize, args: []f32) i32 {
    var na: i32 = 0;
    // Bounded: <= k_svg_argc_cube args; each numf call advances j.
    while (na < @as(i32, @intCast(k_svg_argc_cube))) {
        while ((j.* < vlen) and (svg.priv_ra8_svgp_ws(v[j.*]) or (v[j.*] == ','))) {
            j.* += 1;
        }
        if (!internal_is_num_start(v, vlen, j.*)) {
            break;
        }
        args[@intCast(na)] = priv_ra8_svgp_numf(v, vlen, j);
        na += 1;
    }
    return na;
}

/// Build the 2x3 affine for an SVG `rotate(deg[,cx,cy])` transform. With a
/// centre given, e and f keep that centre fixed; otherwise the origin maps
/// to itself.
fn internal_xform_rotate(args: []const f32, na: i32) SvgUtf {
    const co: f32 = @floatCast(std.math.cos(@as(f64, internal_deg2rad(args[0]))));
    const si: f32 = @floatCast(std.math.sin(@as(f64, internal_deg2rad(args[0]))));
    const cx: f32 = if (na >= 3) args[1] else 0.0;
    const cy: f32 = if (na >= 3) args[2] else 0.0;
    return .{
        .a = co,
        .b = si,
        .c = -si,
        .d = co,
        .e = (cx * (1.0 - co)) + (cy * si),
        .f = (cy * (1.0 - co)) - (cx * si),
    };
}

/// Build the 2x3 affine for one parsed SVG transform function. An
/// unrecognised kind returns the identity.
fn internal_xform_build(kind: SvgXfKind, args: []const f32, na: i32) SvgUtf {
    return switch (kind) {
        .translate => .{
            .a = 1.0,
            .b = 0.0,
            .c = 0.0,
            .d = 1.0,
            .e = args[0],
            .f = if (na >= 2) args[1] else 0.0,
        },
        .scale => .{
            .a = args[0],
            .b = 0.0,
            .c = 0.0,
            .d = if (na >= 2) args[1] else args[0],
            .e = 0.0,
            .f = 0.0,
        },
        .rotate => internal_xform_rotate(args, na),
        .skewx => .{
            .a = 1.0,
            .b = 0.0,
            .c = @floatCast(std.math.tan(@as(f64, internal_deg2rad(args[0])))),
            .d = 1.0,
            .e = 0.0,
            .f = 0.0,
        },
        .skewy => .{
            .a = 1.0,
            .b = @floatCast(std.math.tan(@as(f64, internal_deg2rad(args[0])))),
            .c = 0.0,
            .d = 1.0,
            .e = 0.0,
            .f = 0.0,
        },
        .matrix => .{
            .a = args[0],
            .b = args[1],
            .c = args[2],
            .d = args[3],
            .e = args[4],
            .f = args[5],
        },
        .none => internal_utf_identity(),
    };
}

/// Identify the SVG transform function keyword starting at v[i], scanning
/// the recognised keywords in the C order.
fn internal_xform_kind(v: [*]const u8, vlen: usize, i: usize) SvgXfKind {
    if (svg.priv_ra8_svgp_starts_ci(v, vlen, i, "translate(")) {
        return .translate;
    }
    if (svg.priv_ra8_svgp_starts_ci(v, vlen, i, "scale(")) {
        return .scale;
    }
    if (svg.priv_ra8_svgp_starts_ci(v, vlen, i, "rotate(")) {
        return .rotate;
    }
    if (svg.priv_ra8_svgp_starts_ci(v, vlen, i, "skewx(")) {
        return .skewx;
    }
    if (svg.priv_ra8_svgp_starts_ci(v, vlen, i, "skewy(")) {
        return .skewy;
    }
    if (svg.priv_ra8_svgp_starts_ci(v, vlen, i, "matrix(")) {
        return .matrix;
    }
    return .none;
}

/// Parse a `transform=` attribute value into a composed 2x3 affine,
/// recognising translate / scale / rotate / skewX / skewY / matrix,
/// composed left-to-right (the leftmost function applies last). Unknown or
/// unrecognised function names are skipped whole.
fn internal_parse_xform(v: [*]const u8, vlen: usize) SvgUtf {
    var acc = internal_utf_identity();
    var i: usize = 0;
    // Bounded: each pass consumes one `name(...)` group or breaks.
    while (i < vlen) {
        while ((i < vlen) and (svg.priv_ra8_svgp_ws(v[i]) or (v[i] == ','))) {
            i += 1;
        }
        const kind = internal_xform_kind(v, vlen, i);
        var op = i;
        while ((op < vlen) and (v[op] != '(')) {
            op += 1;
        }
        if (op >= vlen) {
            break;
        }
        var j: usize = op + 1;
        if (kind != .none) {
            var args: [k_svg_path_args]f32 = @splat(0.0);
            const na = internal_xform_read(v, vlen, &j, &args);
            acc = internal_utf_compose(acc, internal_xform_build(kind, &args, na));
        }
        while ((j < vlen) and (v[j] != ')')) {
            j += 1;
        }
        i = if (j < vlen) (j + 1) else vlen;
    }
    return acc;
}

/// Compose the `transform=` attribute found in tag[0..tlen) onto t's user
/// affine in place. Without a `transform` attribute, t is unchanged.
pub export fn priv_ra8_svgp_apply_xform(t: *SvgXform, tag: [*]const u8, tlen: usize) callconv(.c) void {
    var off: usize = 0;
    var vl: usize = 0;
    if (!svg.priv_ra8_svgp_attr(tag, tlen, "transform", &off, &vl)) {
        return;
    }
    const cur = SvgUtf{ .a = t.ua, .b = t.ub, .c = t.uc, .d = t.ud, .e = t.ue, .f = t.uf };
    const out = internal_utf_compose(cur, internal_parse_xform(tag + off, vl));
    t.ua = out.a;
    t.ub = out.b;
    t.uc = out.c;
    t.ud = out.d;
    t.ue = out.e;
    t.uf = out.f;
}
