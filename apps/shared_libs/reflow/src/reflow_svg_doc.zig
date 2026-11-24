// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! SVG document walk, gradient definitions, and the public render API
//! (src/reflow_svg_doc.c, RA8FW-1237).
//!
//! Zig port of the SVG document driver: the document-order element walk with
//! a bounded nested `<g>` transform stack, the `<linearGradient>` /
//! `<radialGradient>` + `<stop>` gradient-definition scan, and the public
//! `ra8_svg_is_svg` / `ra8_svg_image_href` / `ra8_svg_size` / `ra8_svg_render`
//! entry points. Each shape is dispatched to its rasteriser in the sibling
//! reflow_svg_*.zig modules. The C TU this module replaces is deleted in the
//! same commit; the public surface keeps C linkage for the still-C reflow TU.
//! No DOM, no heap. Mirrors slices RA8FW-1233/1234/1235/1236.
//!
//! @since 0.1.0

const std = @import("std");
pub const svg = @import("reflow_svg.zig");
const xf = @import("reflow_svg_xform.zig");
const shape = @import("reflow_svg_shape.zig");
const path = @import("reflow_svg_path.zig");

pub const SvgXform = svg.SvgXform;
pub const SvgGrads = svg.SvgGrads;
pub const SvgGrad = svg.SvgGrad;
pub const SvgUtf = xf.SvgUtf;

// ---- Constants (src/reflow_svg_internal.h, priv_svg_consts_t) ----

/// `viewBox` number count.
const k_svg_viewbox_n: usize = 4;
/// UTF-8 BOM bytes and length.
const k_svg_bom0: u8 = 0xEF;
const k_svg_bom1: u8 = 0xBB;
const k_svg_bom2: u8 = 0xBF;
const k_svg_bom_len: usize = 3;
/// Max nested `<g>` transform depth.
pub const k_svg_g_depth_max: usize = 8;
/// Max gradients defined per document.
const k_svg_grad_max: i32 = @intCast(svg.k_svg_grad_max);
/// Max colour stops per gradient.
const k_svg_grad_stops: u8 = @intCast(svg.k_svg_grad_stops);
/// Max gradient `id` length, bytes.
pub const k_svg_grad_id: usize = svg.k_svg_grad_id;

/// Error codes from libs/ra8_core/inc/ra8_err.h (the C ABI keeps ra8_err_t
/// as c_int, so the public exports below return c_int).
pub const k_ra8_ok: c_int = 0;
pub const k_ra8_err_invalid_arg: c_int = 0x103;
pub const k_ra8_err_not_found: c_int = 0x106;
pub const k_ra8_err_null_ptr: c_int = 0x504;

/// Gradient parameter / offset upper clamp.
const s_svg_unit: f32 = 1.0;

// ===========================================================================
// Document parsing + render walk
// ===========================================================================

/// Test whether an element keyword begins at `s[at]` with a valid delimiter:
/// XML whitespace, `>` or `/` after the keyword (or end-of-buffer).
pub fn elemAt(s: [*]const u8, len: usize, at: usize, name: [*]const u8) bool {
    if (!svg.priv_ra8_svgp_starts_ci(s, len, at, name)) {
        return false;
    }
    var nlen: usize = 0;
    while (name[nlen] != 0) : (nlen += 1) {}
    const after = at + nlen;
    if (after >= len) {
        return true;
    }
    const c = s[after];
    return svg.priv_ra8_svgp_ws(c) or (c == '>') or (c == '/');
}

/// Locate the tag span [*open, *close) of element `name` at or after `from`:
/// the first case-insensitive match, then the next `>` terminator.
pub fn tagSpan(s: [*]const u8, len: usize, from: usize, name: [*]const u8, open: *usize, close: *usize) bool {
    const at = svg.priv_ra8_svgp_find_ci(s, len, from, name);
    if (at >= len) {
        return false;
    }
    var j = at;
    while ((j < len) and (s[j] != '>')) : (j += 1) {}
    open.* = at;
    close.* = j;
    return true;
}

/// Fill `t`'s viewBox fields from the SVG root element: `viewBox` (four
/// integers) first, then `width`/`height`, else the target box itself.
pub fn readViewbox(s: [*]const u8, len: usize, t: *SvgXform) void {
    t.vx = 0;
    t.vy = 0;
    t.vw = t.bw;
    t.vh = t.bh;
    var open: usize = 0;
    var close: usize = 0;
    if (!tagSpan(s, len, 0, "<svg", &open, &close)) {
        return;
    }
    const tag = s + open;
    const tlen = close - open;
    var voff: usize = 0;
    var vlen: usize = 0;
    if (svg.priv_ra8_svgp_attr(tag, tlen, "viewBox", &voff, &vlen)) {
        var nums: [k_svg_viewbox_n]i32 = @splat(0);
        var k: usize = 0;
        for (0..k_svg_viewbox_n) |n| {
            nums[n] = svg.priv_ra8_svgp_num(tag + voff, vlen, &k);
        }
        if ((nums[2] > 0) and (nums[3] > 0)) {
            t.vx = nums[0];
            t.vy = nums[1];
            t.vw = nums[2];
            t.vh = nums[3];
        }
    } else {
        const wv = svg.priv_ra8_svgp_attr_num(tag, tlen, "width", t.bw);
        const hv = svg.priv_ra8_svgp_attr_num(tag, tlen, "height", t.bh);
        if ((wv > 0) and (hv > 0)) {
            t.vw = wv;
            t.vh = hv;
        }
    }
}

/// Dispatch one element tag span to the appropriate shape-drawing function,
/// composing any element-level `transform=` onto a local copy of `t`.
fn dispatchShape(s: [*]const u8, len: usize, at: usize, close: usize, t: *const SvgXform) void {
    const tag = s + at;
    const tlen = if (close > at) (close - at) else 0;
    var lt = t.*;
    xf.priv_ra8_svgp_apply_xform(&lt, tag, tlen);
    if (elemAt(s, len, at, "<rect")) {
        shape.priv_ra8_svgp_draw_rect(tag, tlen, &lt);
    } else if (elemAt(s, len, at, "<circle")) {
        shape.priv_ra8_svgp_draw_circle(tag, tlen, &lt);
    } else if (elemAt(s, len, at, "<polygon")) {
        shape.priv_ra8_svgp_draw_polygon(tag, tlen, &lt);
    } else if (elemAt(s, len, at, "<polyline")) {
        shape.priv_ra8_svgp_draw_polyline(tag, tlen, &lt);
    } else if (elemAt(s, len, at, "<path")) {
        path.priv_ra8_svgp_draw_path(tag, tlen, &lt);
    } else if (elemAt(s, len, at, "<line")) {
        shape.priv_ra8_svgp_draw_line(tag, tlen, &lt);
    } else {
        // Unsupported element -> skip.
    }
}

/// Copy the base transform `t` into `out` and replace the user affine with `g`.
fn xformWithGroup(t: *const SvgXform, g: SvgUtf, out: *SvgXform) void {
    out.* = t.*;
    out.ua = g.a;
    out.ub = g.b;
    out.uc = g.c;
    out.ud = g.d;
    out.ue = g.e;
    out.uf = g.f;
}

/// Handle a `g` open tag: compose its `transform=` over the stack top and
/// push. Self-closing `g/` elements and groups past the depth cap are not
/// pushed; `gsp` is returned unchanged in those cases.
pub fn groupOpen(s: [*]const u8, i: usize, close: usize, t: *const SvgXform, gstk: *[k_svg_g_depth_max + 1]SvgUtf, gsp: i32) i32 {
    var gt = std.mem.zeroes(SvgXform);
    xformWithGroup(t, gstk[@intCast(gsp)], &gt);
    xf.priv_ra8_svgp_apply_xform(&gt, s + i, if (close > i) (close - i) else 0);
    const self_close = (close > i) and (s[close - 1] == '/');
    if (self_close or (gsp >= @as(i32, @intCast(k_svg_g_depth_max)))) {
        return gsp;
    }
    gstk[@intCast(gsp + 1)] = .{
        .a = gt.ua,
        .b = gt.ub,
        .c = gt.uc,
        .d = gt.ud,
        .e = gt.ue,
        .f = gt.uf,
    };
    return gsp + 1;
}

/// Walk every element in document order and draw the supported shapes, with
/// a bounded group transform stack of depth `k_svg_g_depth_max`.
fn drawShapes(s: [*]const u8, len: usize, t: *const SvgXform) void {
    var gstk: [k_svg_g_depth_max + 1]SvgUtf = undefined;
    var gsp: i32 = 0;
    gstk[0] = .{ .a = t.ua, .b = t.ub, .c = t.uc, .d = t.ud, .e = t.ue, .f = t.uf };
    var i: usize = 0;
    while (i < len) {
        while ((i < len) and (s[i] != '<')) : (i += 1) {}
        if (i >= len) {
            break;
        }
        var close = i;
        while ((close < len) and (s[close] != '>')) : (close += 1) {}
        if (elemAt(s, len, i, "</g")) {
            gsp = if (gsp > 0) (gsp - 1) else 0;
        } else if (elemAt(s, len, i, "<g")) {
            gsp = groupOpen(s, i, close, t, &gstk, gsp);
        } else {
            var ct = std.mem.zeroes(SvgXform);
            xformWithGroup(t, gstk[@intCast(gsp)], &ct);
            dispatchShape(s, len, i, close, &ct);
        }
        i = if (close < len) (close + 1) else len;
    }
}

// ===========================================================================
// Gradient definitions (<linearGradient> / <radialGradient> + <stop>)
// ===========================================================================

/// Read an SVG attribute as a float, returning `def` when the attribute is
/// absent.
pub fn attrNumf(s: [*]const u8, len: usize, name: [*]const u8, def: f32) f32 {
    var off: usize = 0;
    var vl: usize = 0;
    if (!svg.priv_ra8_svgp_attr(s, len, name, &off, &vl)) {
        return def;
    }
    var k: usize = 0;
    return xf.priv_ra8_svgp_numf(s + off, vl, &k);
}

/// Copy the `id` attribute value into `dst` NUL-terminated, truncated to
/// `k_svg_grad_id - 1` bytes. Absent id leaves the empty string.
pub fn copyAttrId(s: [*]const u8, len: usize, dst: *[k_svg_grad_id]u8) void {
    dst[0] = 0;
    var off: usize = 0;
    var vl: usize = 0;
    if (!svg.priv_ra8_svgp_attr(s, len, "id", &off, &vl)) {
        return;
    }
    const n = @min(vl, k_svg_grad_id - 1);
    for (0..n) |i| {
        dst[i] = s[off + i];
    }
    dst[n] = 0;
}

/// Clamp a float value to the unit interval [0, 1].
pub fn clamp01(v: f32) f32 {
    if (v < 0.0) {
        return 0.0;
    }
    return if (v > s_svg_unit) s_svg_unit else v;
}

/// Parse a gradient `stop` element's `offset` attribute as a fraction,
/// dividing by 100 when a `%` is present. Absent attribute: 0.0.
pub fn stopOffset(tag: [*]const u8, tlen: usize) f32 {
    const k_percent: f32 = 100.0;
    var ooff: usize = 0;
    var ovl: usize = 0;
    if (!svg.priv_ra8_svgp_attr(tag, tlen, "offset", &ooff, &ovl)) {
        return 0.0;
    }
    var k: usize = 0;
    const off = xf.priv_ra8_svgp_numf(tag + ooff, ovl, &k);
    for (0..ovl) |z| {
        if (tag[ooff + z] == '%') {
            return off / k_percent;
        }
    }
    return off;
}

/// Parse a gradient `stop` element's `stop-color` attribute as a paint;
/// absent or `none` parses to the SVG default fill (black).
pub fn stopColor(tag: [*]const u8, tlen: usize) u32 {
    var coff: usize = 0;
    var cvl: usize = 0;
    if (!svg.priv_ra8_svgp_attr(tag, tlen, "stop-color", &coff, &cvl)) {
        return svg.k_svg_def_fill;
    }
    const pc = svg.priv_ra8_svgp_paint(tag + coff, cvl);
    return if (pc != svg.k_svg_no_paint) pc else svg.k_svg_def_fill;
}

/// Parse the `stop` child elements of a gradient definition into `g`,
/// bounded by `k_svg_grad_stops`; stops beyond the limit are dropped.
pub fn parseStops(s: [*]const u8, from: usize, end: usize, g: *SvgGrad) void {
    var at = from;
    while (g.nstops < k_svg_grad_stops) {
        const sp = svg.priv_ra8_svgp_find_ci(s, end, at, "<stop");
        if (sp >= end) {
            break;
        }
        var close = sp;
        while ((close < end) and (s[close] != '>')) : (close += 1) {}
        const tag = s + sp;
        const tlen = if (close > sp) (close - sp) else 0;
        g.stops[g.nstops].off = clamp01(stopOffset(tag, tlen));
        g.stops[g.nstops].col = stopColor(tag, tlen);
        g.nstops += 1;
        at = if (close < end) (close + 1) else end;
    }
}

/// Scan the document for gradient definitions and populate `gs`, bounded by
/// `k_svg_grad_max`; additional gradients are silently dropped. A gradient
/// with no stops is not counted.
pub fn scanGrads(s: [*]const u8, len: usize, gs: *SvgGrads) void {
    const k_half: f32 = 0.5;
    gs.n = 0;
    var from: usize = 0;
    while (gs.n < k_svg_grad_max) {
        const lin = svg.priv_ra8_svgp_find_ci(s, len, from, "<linearGradient");
        const rad = svg.priv_ra8_svgp_find_ci(s, len, from, "<radialGradient");
        const is_rad = (rad < lin);
        const at = if (is_rad) rad else lin;
        if (at >= len) {
            break;
        }
        var close = at;
        while ((close < len) and (s[close] != '>')) : (close += 1) {}
        const tag = s + at;
        const tlen = if (close > at) (close - at) else 0;
        const g = &gs.g[@intCast(gs.n)];
        g.* = std.mem.zeroes(SvgGrad);
        g.kind = if (is_rad) svg.k_svg_grad_radial else svg.k_svg_grad_linear;
        copyAttrId(tag, tlen, &g.id);
        if (is_rad) {
            g.x1 = attrNumf(tag, tlen, "cx", k_half);
            g.y1 = attrNumf(tag, tlen, "cy", k_half);
            g.x2 = attrNumf(tag, tlen, "r", k_half);
        } else {
            g.x1 = attrNumf(tag, tlen, "x1", 0.0);
            g.y1 = attrNumf(tag, tlen, "y1", 0.0);
            g.x2 = attrNumf(tag, tlen, "x2", s_svg_unit);
            g.y2 = attrNumf(tag, tlen, "y2", 0.0);
        }
        const endtag: [*]const u8 = if (is_rad) "</radialGradient" else "</linearGradient";
        const endpos = svg.priv_ra8_svgp_find_ci(s, len, close, endtag);
        const stopend = if (endpos < len) endpos else len;
        parseStops(s, close, stopend, g);
        if (g.nstops > 0) {
            gs.n += 1;
        }
        from = if (endpos < len) (endpos + 1) else len;
    }
}

// ===========================================================================
// Public API (C linkage, reflow_svg.h)
// ===========================================================================

/// `ra8_svg_is_svg`: whether a buffer starts (after an optional UTF-8 BOM
/// and whitespace) with `<?xml` or `<svg`.
pub export fn ra8_svg_is_svg(bytes: ?[*]const u8, len: usize) callconv(.c) bool {
    const b = bytes orelse return false;
    if (len == 0) {
        return false;
    }
    var i: usize = 0;
    if ((len >= k_svg_bom_len) and (b[0] == k_svg_bom0) and (b[1] == k_svg_bom1) and (b[2] == k_svg_bom2)) {
        i = k_svg_bom_len;
    }
    while ((i < len) and svg.priv_ra8_svgp_ws(b[i])) : (i += 1) {}
    return svg.priv_ra8_svgp_starts_ci(b, len, i, "<?xml") or
        svg.priv_ra8_svgp_starts_ci(b, len, i, "<svg");
}

/// `ra8_svg_image_href`: locate the first `<image>` tag and return the byte
/// span of its `xlink:href` (or `href`) attribute value.
pub export fn ra8_svg_image_href(svg_buf: ?[*]const u8, len: usize, out_off: ?*usize, out_len: ?*usize) callconv(.c) c_int {
    const s = svg_buf orelse return k_ra8_err_null_ptr;
    const oo = out_off orelse return k_ra8_err_null_ptr;
    const ol = out_len orelse return k_ra8_err_null_ptr;
    var open: usize = 0;
    var close: usize = 0;
    if (!tagSpan(s, len, 0, "<image", &open, &close)) {
        return k_ra8_err_not_found;
    }
    const tag = s + open;
    const tlen = close - open;
    var voff: usize = 0;
    var vlen: usize = 0;
    if (!svg.priv_ra8_svgp_attr(tag, tlen, "xlink:href", &voff, &vlen) and
        !svg.priv_ra8_svgp_attr(tag, tlen, "href", &voff, &vlen))
    {
        return k_ra8_err_not_found;
    }
    oo.* = open + voff;
    ol.* = vlen;
    return k_ra8_ok;
}

/// `ra8_svg_size`: the intrinsic SVG size from `width`/`height`, falling
/// back to the `viewBox` width/height.
pub export fn ra8_svg_size(svg_buf: ?[*]const u8, len: usize, out_w: ?*i32, out_h: ?*i32) callconv(.c) c_int {
    const s = svg_buf orelse return k_ra8_err_null_ptr;
    const ow = out_w orelse return k_ra8_err_null_ptr;
    const oh = out_h orelse return k_ra8_err_null_ptr;
    var open: usize = 0;
    var close: usize = 0;
    if (!tagSpan(s, len, 0, "<svg", &open, &close)) {
        return k_ra8_err_not_found;
    }
    const tag = s + open;
    const tlen = close - open;
    var w = svg.priv_ra8_svgp_attr_num(tag, tlen, "width", 0);
    var h = svg.priv_ra8_svgp_attr_num(tag, tlen, "height", 0);
    if ((w <= 0) or (h <= 0)) {
        var voff: usize = 0;
        var vlen: usize = 0;
        if (svg.priv_ra8_svgp_attr(tag, tlen, "viewBox", &voff, &vlen)) {
            var nums: [k_svg_viewbox_n]i32 = @splat(0);
            var k: usize = 0;
            for (0..k_svg_viewbox_n) |n| {
                nums[n] = svg.priv_ra8_svgp_num(tag + voff, vlen, &k);
            }
            w = nums[2];
            h = nums[3];
        }
    }
    if ((w <= 0) or (h <= 0)) {
        return k_ra8_err_not_found;
    }
    ow.* = w;
    oh.* = h;
    return k_ra8_ok;
}

/// `ra8_svg_render`: scan the gradient definitions, read the viewBox, then
/// walk the document drawing every supported shape into the box at
/// (x, y) sized (w, h).
pub export fn ra8_svg_render(svg_buf: ?[*]const u8, len: usize, x: i32, y: i32, w: i32, h: i32) callconv(.c) c_int {
    const s = svg_buf orelse return k_ra8_err_null_ptr;
    if ((w <= 0) or (h <= 0)) {
        return k_ra8_err_invalid_arg;
    }
    var t = std.mem.zeroes(SvgXform);
    t.bx = x;
    t.by = y;
    t.bw = w;
    t.bh = h;
    t.ua = 1.0;
    t.ub = 0.0;
    t.uc = 0.0;
    t.ud = 1.0;
    t.ue = 0.0;
    t.uf = 0.0;
    var grads = std.mem.zeroes(SvgGrads);
    scanGrads(s, len, &grads);
    t.grads = &grads;
    readViewbox(s, len, &t);
    drawShapes(s, len, &t);
    return k_ra8_ok;
}

comptime {
    std.debug.assert(@sizeOf(SvgUtf) == 24);
}
