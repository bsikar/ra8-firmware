// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! SVG scan / attribute / colour-parsing core
//! (src/reflow_svg.c, RA8FW-1233).
//!
//! Zig port of the pure string-scanning leaf helpers for the minimal SVG
//! subset: XML whitespace and ASCII-fold classifiers, case-insensitive
//! literal scans, integer parsing, attribute slicing, colour-keyword /
//! hex-paint parsing, and the `fill` / gradient-reference resolver. The
//! transform, shape, path, and document-walk stages live in the sibling
//! reflow_svg_*.c files, which stay C for now and call these helpers
//! through reflow_svg_internal.h, so every export below keeps C linkage
//! and the shared geometry types are extern-struct mirrors of the header.
//! No DOM, no heap.
//!
//! @since 0.1.0

const std = @import("std");

// ---- Constants (src/reflow_svg_internal.h, priv_svg_consts_t) ----

/// `none` / absent paint sentinel.
pub const k_svg_no_paint: u32 = 0xFFFFFFFF;
/// SVG default fill is black.
pub const k_svg_def_fill: u32 = 0x000000;
/// Decimal base.
pub const k_svg_dec: u32 = 10;
/// Hex base / "not a hex digit" sentinel.
pub const k_svg_hex_base: u8 = 16;
/// Value of hex 'a'/'A'.
pub const k_svg_hex_a10: u8 = 10;
/// Bits per hex nibble.
pub const k_svg_hex_nib: u5 = 4;
/// Bits per colour channel.
pub const k_svg_hex_chan: u5 = 8;
/// `#rgb` digit count.
pub const k_svg_hex3: usize = 3;
/// `#rrggbb` digit count.
pub const k_svg_hex6: usize = 6;
/// Max gradients defined per document.
pub const k_svg_grad_max: usize = 8;
/// Max colour stops per gradient.
pub const k_svg_grad_stops: usize = 8;
/// Max gradient `id` length, bytes.
pub const k_svg_grad_id: usize = 32;
/// `<linearGradient>` kind.
pub const k_svg_grad_linear: u8 = 0;
/// `<radialGradient>` kind.
pub const k_svg_grad_radial: u8 = 1;

// ---- Extern mirrors of the shared geometry types ----
//
// Byte-exact mirrors of svg_stop_t / svg_grad_t / svg_grads / svg_xform_t
// from reflow_svg_internal.h. The C sibling TUs pass svg_xform_t* and
// svg_grads_t* through priv_match_grad and priv_ra8_svgp_resolve_fill, so
// the layout has to match on every target the firmware builds for.

/// One gradient colour stop: an offset in [0,1] and an 0x00RRGGBB colour.
pub const SvgStop = extern struct {
    off: f32,
    col: u32,
};

/// A parsed linear/radial gradient in objectBoundingBox units.
pub const SvgGrad = extern struct {
    kind: u8,
    nstops: u8,
    id: [k_svg_grad_id]u8,
    x1: f32,
    y1: f32,
    x2: f32,
    y2: f32,
    stops: [k_svg_grad_stops]SvgStop,
};

/// Bounded set of gradients scanned from the document before the render.
pub const SvgGrads = extern struct {
    n: i32,
    g: [k_svg_grad_max]SvgGrad,
};

/// SVG-user-space -> framebuffer-box coordinate transform.
pub const SvgXform = extern struct {
    bx: i32,
    by: i32,
    bw: i32,
    bh: i32,
    vx: i32,
    vy: i32,
    vw: i32,
    vh: i32,
    ua: f32,
    ub: f32,
    uc: f32,
    ud: f32,
    ue: f32,
    uf: f32,
    grads: ?*const SvgGrads,
};

comptime {
    // Field offsets before the trailing pointer are target-independent;
    // the pointer sits at the same offset too (all preceding fields are
    // 4-byte scalars and 56 is both 4- and 8-aligned).
    std.debug.assert(@offsetOf(SvgStop, "col") == 4);
    std.debug.assert(@offsetOf(SvgGrad, "id") == 2);
    std.debug.assert(@offsetOf(SvgGrad, "x1") == 36);
    std.debug.assert(@offsetOf(SvgGrad, "stops") == 52);
    std.debug.assert(@offsetOf(SvgGrads, "g") == 4);
    std.debug.assert(@offsetOf(SvgXform, "ua") == 32);
    std.debug.assert(@offsetOf(SvgXform, "grads") == 56);
}

// ---- Pure string / number helpers ----

/// Test whether a byte is XML whitespace: space, tab, LF, CR, or FF.
pub export fn priv_ra8_svgp_ws(c: u8) callconv(.c) bool {
    return (c == ' ') or (c == '\t') or (c == '\n') or (c == '\r') or (c == 0x0C);
}

/// ASCII-fold one byte to lower case; non-letters pass through unchanged.
pub export fn priv_ra8_svgp_lc(c: u8) callconv(.c) u8 {
    return if ((c >= 'A') and (c <= 'Z')) (c + ('a' - 'A')) else c;
}

/// Length of a NUL-terminated literal.
fn internal_lit_len(lit: [*]const u8) usize {
    var n: usize = 0;
    while (lit[n] != 0) : (n += 1) {}
    return n;
}

/// Test whether `s[at..len)` starts with `lit`, case-insensitively.
pub export fn priv_ra8_svgp_starts_ci(s: [*]const u8, len: usize, at: usize, lit: [*]const u8) callconv(.c) bool {
    const n = internal_lit_len(lit);
    if (at > len) {
        return false;
    }
    if (n > (len - at)) {
        return false;
    }
    var k: usize = 0;
    while (k < n) : (k += 1) {
        if (priv_ra8_svgp_lc(s[at + k]) != priv_ra8_svgp_lc(lit[k])) {
            return false;
        }
    }
    return true;
}

/// Find the first case-insensitive occurrence of `lit` in `s[from..len)`.
/// An empty literal is a defined sentinel: `len`.
pub export fn priv_ra8_svgp_find_ci(s: [*]const u8, len: usize, from: usize, lit: [*]const u8) callconv(.c) usize {
    const n = internal_lit_len(lit);
    if (n == 0) {
        return len;
    }
    var i: usize = from;
    // Bounded: i advances by 1 each step, capped by len - n + 1.
    while ((i + n) <= len) : (i += 1) {
        if (priv_ra8_svgp_starts_ci(s, len, i, lit)) {
            return i;
        }
    }
    return len;
}

/// Convert one hex digit to its value, or `k_svg_hex_base` when not a digit.
fn internal_hex(c: u8) u8 {
    if ((c >= '0') and (c <= '9')) {
        return c - '0';
    }
    const l = priv_ra8_svgp_lc(c);
    if ((l >= 'a') and (l <= 'f')) {
        return (l - 'a') + k_svg_hex_a10;
    }
    return k_svg_hex_base;
}

/// Parse the integer part of one SVG number at `s[i.*]`, skipping leading
/// whitespace / commas; any fraction is consumed and truncated.
pub export fn priv_ra8_svgp_num(s: [*]const u8, len: usize, i: *usize) callconv(.c) i32 {
    while ((i.* < len) and (priv_ra8_svgp_ws(s[i.*]) or (s[i.*] == ','))) {
        i.* += 1;
    }
    var sign: i32 = 1;
    if ((i.* < len) and ((s[i.*] == '-') or (s[i.*] == '+'))) {
        sign = if (s[i.*] == '-') -1 else 1;
        i.* += 1;
    }
    var v: i32 = 0;
    while ((i.* < len) and (s[i.*] >= '0') and (s[i.*] <= '9')) {
        v = (v *% @as(i32, @intCast(k_svg_dec))) +% (@as(i32, s[i.*]) - '0');
        i.* += 1;
    }
    // Skip a fractional part (truncated).
    if ((i.* < len) and (s[i.*] == '.')) {
        i.* += 1;
        while ((i.* < len) and (s[i.*] >= '0') and (s[i.*] <= '9')) {
            i.* += 1;
        }
    }
    return v *% sign;
}

// ---- Attribute + colour parsing ----

/// Test whether attribute keyword `name` begins at `s[at]` on a word
/// boundary: preceded by whitespace (when not at 0) and followed by '=' or
/// whitespace.
fn internal_attr_at(s: [*]const u8, len: usize, at: usize, name: [*]const u8) bool {
    const n = internal_lit_len(name);
    if (!priv_ra8_svgp_starts_ci(s, len, at, name)) {
        return false;
    }
    if ((at > 0) and !priv_ra8_svgp_ws(s[at - 1])) {
        return false; // must be preceded by whitespace
    }
    const after = at + n;
    if (after >= len) {
        return false;
    }
    return (s[after] == '=') or priv_ra8_svgp_ws(s[after]);
}

/// Find attribute `name` in tag span `s[0..len)`; set the value slice.
/// Outputs are written only on success.
pub export fn priv_ra8_svgp_attr(s: [*]const u8, len: usize, name: [*]const u8, voff: *usize, vlen: *usize) callconv(.c) bool {
    // Bounded: i advances by 1 each step, capped by len.
    var i: usize = 0;
    while (i < len) : (i += 1) {
        if (!internal_attr_at(s, len, i, name)) {
            continue;
        }
        var j: usize = i + internal_lit_len(name);
        while ((j < len) and (s[j] != '=')) {
            j += 1;
        }
        j += 1; // past '='
        while ((j < len) and priv_ra8_svgp_ws(s[j])) {
            j += 1;
        }
        if ((j >= len) or ((s[j] != '"') and (s[j] != '\''))) {
            return false;
        }
        const q = s[j];
        j += 1;
        const start = j;
        while ((j < len) and (s[j] != q)) {
            j += 1;
        }
        voff.* = start;
        vlen.* = j - start;
        return true;
    }
    return false;
}

/// Read an SVG attribute as a signed integer; `def` when absent. Fractional
/// parts are truncated.
pub export fn priv_ra8_svgp_attr_num(s: [*]const u8, len: usize, name: [*]const u8, def: i32) callconv(.c) i32 {
    var off: usize = 0;
    var vl: usize = 0;
    if (!priv_ra8_svgp_attr(s, len, name, &off, &vl)) {
        return def;
    }
    var k: usize = 0;
    return priv_ra8_svgp_num(s + off, vl, &k);
}

/// Case-insensitive equality between a byte span and a NUL-terminated
/// literal: lengths must agree too.
fn internal_ci_eq(s: [*]const u8, len: usize, lit: [*]const u8) bool {
    var k: usize = 0;
    while ((k < len) and (lit[k] != 0)) : (k += 1) {
        if (priv_ra8_svgp_lc(s[k]) != priv_ra8_svgp_lc(lit[k])) {
            return false;
        }
    }
    return (k == len) and (lit[k] == 0);
}

/// Parse a hex colour from raw digit bytes (no leading '#') into
/// 0x00RRGGBB. Exactly 3 or 6 digits; 3-digit channels expand to NN.
pub fn internal_hex_color(s: [*]const u8, len: usize) u32 {
    var rgb: u32 = 0;
    if (len == k_svg_hex6) {
        var i: usize = 0;
        while (i < len) : (i += 1) {
            const v = internal_hex(s[i]);
            if (v >= k_svg_hex_base) {
                return k_svg_no_paint;
            }
            rgb = (rgb << k_svg_hex_nib) | v;
        }
        return rgb;
    }
    if (len == k_svg_hex3) {
        var i: usize = 0;
        while (i < len) : (i += 1) {
            const v = internal_hex(s[i]);
            if (v >= k_svg_hex_base) {
                return k_svg_no_paint;
            }
            rgb = (rgb << k_svg_hex_chan) | (@as(u32, v) << k_svg_hex_nib) | v;
        }
        return rgb;
    }
    return k_svg_no_paint;
}

const NamedColor = struct { name: []const u8, rgb: u32 };

/// The twelve most common SVG/CSS colour keywords, matched case-insensitively.
const k_named_colors = [_]NamedColor{
    .{ .name = "black", .rgb = 0x000000 },
    .{ .name = "white", .rgb = 0xFFFFFF },
    .{ .name = "red", .rgb = 0xFF0000 },
    .{ .name = "green", .rgb = 0x008000 },
    .{ .name = "blue", .rgb = 0x0000FF },
    .{ .name = "gray", .rgb = 0x808080 },
    .{ .name = "grey", .rgb = 0x808080 },
    .{ .name = "silver", .rgb = 0xC0C0C0 },
    .{ .name = "maroon", .rgb = 0x800000 },
    .{ .name = "navy", .rgb = 0x000080 },
    .{ .name = "yellow", .rgb = 0xFFFF00 },
    .{ .name = "orange", .rgb = 0xFFA500 },
};

/// Map a named SVG colour keyword to its 0x00RRGGBB value; "none" and any
/// unknown name return the no-paint sentinel.
pub fn internal_named_color(s: [*]const u8, len: usize) u32 {
    for (k_named_colors) |entry| {
        if (internal_ci_eq(s, len, entry.name.ptr)) {
            return entry.rgb;
        }
    }
    return k_svg_no_paint;
}

/// Parse an SVG paint value ('#rgb'/'#rrggbb'/name/'none') to 0x00RRGGBB;
/// the no-paint sentinel on empty, "none", or unrecognised values.
pub export fn priv_ra8_svgp_paint(s: [*]const u8, len: usize) callconv(.c) u32 {
    if (len == 0) {
        return k_svg_no_paint;
    }
    if (s[0] == '#') {
        return internal_hex_color(s + 1, len - 1);
    }
    return internal_named_color(s, len);
}

/// Read an SVG attribute as a paint colour; `def` when absent.
pub export fn priv_ra8_svgp_attr_paint(s: [*]const u8, len: usize, name: [*]const u8, def: u32) callconv(.c) u32 {
    var off: usize = 0;
    var vl: usize = 0;
    if (!priv_ra8_svgp_attr(s, len, name, &off, &vl)) {
        return def;
    }
    return priv_ra8_svgp_paint(s + off, vl);
}

/// Implementation of `priv_match_grad()`: linear id search over the
/// document gradient set. The `url(#` prefix matches case-insensitively;
/// the id comparison itself is case-sensitive byte equality, as in C.
pub export fn priv_match_grad(grads: ?*const SvgGrads, val: [*]const u8, vlen: usize) callconv(.c) i32 {
    if ((grads == null) or !priv_ra8_svgp_starts_ci(val, vlen, 0, "url(#")) {
        return -1;
    }
    const idoff: usize = 5; // strlen("url(#")
    var idend: usize = idoff;
    while ((idend < vlen) and (val[idend] != ')')) {
        idend += 1;
    }
    const idlen = idend - idoff;
    // Bounded: <= grads.n (<= k_svg_grad_max) gradients.
    var i: i32 = 0;
    while (i < grads.?.n) : (i += 1) {
        const gid: [*:0]const u8 = @ptrCast(&grads.?.g[@intCast(i)].id);
        var equal = internal_lit_len(gid) == idlen;
        var k: usize = 0;
        while ((k < idlen) and equal) : (k += 1) {
            equal = gid[k] == val[idoff + k];
        }
        if (equal) {
            return i;
        }
    }
    return -1;
}

/// Resolve a shape's `fill`: a solid colour, or a gradient index via `gi`.
/// A `url(#id)` reference returns the no-paint sentinel with `gi` set to
/// the matched index (or -1 when it does not resolve); an absent `fill`
/// returns `def`.
pub export fn priv_ra8_svgp_resolve_fill(s: [*]const u8, len: usize, t: ?*const SvgXform, def: u32, gi: *i32) callconv(.c) u32 {
    gi.* = -1;
    var off: usize = 0;
    var vl: usize = 0;
    if (!priv_ra8_svgp_attr(s, len, "fill", &off, &vl)) {
        return def;
    }
    if (priv_ra8_svgp_starts_ci(s + off, vl, 0, "url(#")) {
        const grads: ?*const SvgGrads = if (t) |x| x.grads else null;
        gi.* = priv_match_grad(grads, s + off, vl);
        return k_svg_no_paint;
    }
    return priv_ra8_svgp_paint(s + off, vl);
}
