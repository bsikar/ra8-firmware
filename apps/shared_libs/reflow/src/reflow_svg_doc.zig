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
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const svg = @import("reflow_svg.zig");
const xf = @import("reflow_svg_xform.zig");
const shape = @import("reflow_svg_shape.zig");
const path = @import("reflow_svg_path.zig");

const SvgXform = svg.SvgXform;
const SvgGrads = svg.SvgGrads;
const SvgGrad = svg.SvgGrad;
const SvgUtf = xf.SvgUtf;

// ---- Constants (src/reflow_svg_internal.h, priv_svg_consts_t) ----

/// `viewBox` number count.
const k_svg_viewbox_n: usize = 4;
/// UTF-8 BOM bytes and length.
const k_svg_bom0: u8 = 0xEF;
const k_svg_bom1: u8 = 0xBB;
const k_svg_bom2: u8 = 0xBF;
const k_svg_bom_len: usize = 3;
/// Max nested `<g>` transform depth.
const k_svg_g_depth_max: usize = 8;
/// Max gradients defined per document.
const k_svg_grad_max: i32 = @intCast(svg.k_svg_grad_max);
/// Max colour stops per gradient.
const k_svg_grad_stops: u8 = @intCast(svg.k_svg_grad_stops);
/// Max gradient `id` length, bytes.
const k_svg_grad_id: usize = svg.k_svg_grad_id;

/// Error codes from libs/ra8_core/inc/ra8_err.h (the C ABI keeps ra8_err_t
/// as c_int, so the public exports below return c_int).
const k_ra8_ok: c_int = 0;
const k_ra8_err_invalid_arg: c_int = 0x103;
const k_ra8_err_not_found: c_int = 0x106;
const k_ra8_err_null_ptr: c_int = 0x504;

/// Gradient parameter / offset upper clamp.
const s_svg_unit: f32 = 1.0;

// ===========================================================================
// Document parsing + render walk
// ===========================================================================

/// Test whether an element keyword begins at `s[at]` with a valid delimiter:
/// XML whitespace, `>` or `/` after the keyword (or end-of-buffer).
fn elemAt(s: [*]const u8, len: usize, at: usize, name: [*]const u8) bool {
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
fn tagSpan(s: [*]const u8, len: usize, from: usize, name: [*]const u8, open: *usize, close: *usize) bool {
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
fn readViewbox(s: [*]const u8, len: usize, t: *SvgXform) void {
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
fn groupOpen(s: [*]const u8, i: usize, close: usize, t: *const SvgXform, gstk: *[k_svg_g_depth_max + 1]SvgUtf, gsp: i32) i32 {
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
fn attrNumf(s: [*]const u8, len: usize, name: [*]const u8, def: f32) f32 {
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
fn copyAttrId(s: [*]const u8, len: usize, dst: *[k_svg_grad_id]u8) void {
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
fn clamp01(v: f32) f32 {
    if (v < 0.0) {
        return 0.0;
    }
    return if (v > s_svg_unit) s_svg_unit else v;
}

/// Parse a gradient `stop` element's `offset` attribute as a fraction,
/// dividing by 100 when a `%` is present. Absent attribute: 0.0.
fn stopOffset(tag: [*]const u8, tlen: usize) f32 {
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
fn stopColor(tag: [*]const u8, tlen: usize) u32 {
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
fn parseStops(s: [*]const u8, from: usize, end: usize, g: *SvgGrad) void {
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
fn scanGrads(s: [*]const u8, len: usize, gs: *SvgGrads) void {
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

// ===========================================================================
// Tests
// ===========================================================================

const testing = std.testing;
const seam = @import("reflow_render_test_seam.zig");

fn calls() []seam.ShapeCall {
    return seam.shape_calls[0..seam.shape_call_n];
}

fn identityBox() SvgXform {
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

test "elem_at matches names with delimiters" {
    const s = "<rect x=\"1\"/><rectangle/><rect";
    try testing.expect(elemAt(s.ptr, s.len, 0, "<rect"));
    // "rectangle" is not "<rect": the following char is not a delimiter.
    try testing.expect(!elemAt(s.ptr, s.len, 13, "<rect"));
    // Match at end-of-buffer is accepted.
    try testing.expect(elemAt(s.ptr, s.len, 25, "<rect"));
}

test "tag_span finds the tag and its gt" {
    const s = "junk <svg width=\"4\"> rest";
    var open: usize = 0;
    var close: usize = 0;
    try testing.expect(tagSpan(s.ptr, s.len, 0, "<svg", &open, &close));
    try testing.expectEqual(@as(usize, 5), open);
    try testing.expectEqual(@as(usize, 19), close);
    var open2: usize = 0;
    var close2: usize = 0;
    try testing.expect(!tagSpan(s.ptr, s.len, 0, "<image", &open2, &close2));
}

test "read_viewbox parses four integers and falls back" {
    // Explicit viewBox wins.
    var t = identityBox();
    const doc1 = "<svg viewBox=\"2 3 40 50\"></svg>";
    readViewbox(doc1.ptr, doc1.len, &t);
    try testing.expectEqual(@as(i32, 2), t.vx);
    try testing.expectEqual(@as(i32, 3), t.vy);
    try testing.expectEqual(@as(i32, 40), t.vw);
    try testing.expectEqual(@as(i32, 50), t.vh);
    // No viewBox: width/height fallback.
    var t2 = identityBox();
    const doc2 = "<svg width=\"30\" height=\"20\"></svg>";
    readViewbox(doc2.ptr, doc2.len, &t2);
    try testing.expectEqual(@as(i32, 0), t2.vx);
    try testing.expectEqual(@as(i32, 30), t2.vw);
    try testing.expectEqual(@as(i32, 20), t2.vh);
    // Neither: the viewBox is the target box itself.
    var t3 = identityBox();
    const doc3 = "<svg></svg>";
    readViewbox(doc3.ptr, doc3.len, &t3);
    try testing.expectEqual(@as(i32, 64), t3.vw);
    try testing.expectEqual(@as(i32, 64), t3.vh);
    // A zero-width viewBox is ignored.
    var t4 = identityBox();
    const doc4 = "<svg viewBox=\"0 0 0 5\"></svg>";
    readViewbox(doc4.ptr, doc4.len, &t4);
    try testing.expectEqual(@as(i32, 64), t4.vw);
}

test "group_open composes, skips self-closing and caps depth" {
    var gstk: [k_svg_g_depth_max + 1]SvgUtf = @splat(.{ .a = 1, .b = 0, .c = 0, .d = 1, .e = 0, .f = 0 });
    const t = identityBox();
    // A plain <g transform="translate(2,3)"> pushes a composed affine.
    const g1 = "<g transform=\"translate(2,3)\">";
    var gsp = groupOpen(g1.ptr, 0, g1.len - 1, &t, &gstk, 0);
    try testing.expectEqual(@as(i32, 1), gsp);
    try testing.expectEqual(@as(f32, 1), gstk[1].a);
    try testing.expectEqual(@as(f32, 2), gstk[1].e);
    try testing.expectEqual(@as(f32, 3), gstk[1].f);
    // A self-closing <g transform="translate(5,5)"/> does not push.
    const g2 = "<g transform=\"translate(5,5)\"/>";
    gsp = groupOpen(g2.ptr, 0, g2.len - 1, &t, &gstk, gsp);
    try testing.expectEqual(@as(i32, 1), gsp);
    // The depth cap stops the push.
    gsp = @as(i32, @intCast(k_svg_g_depth_max));
    const g3 = "<g>";
    gsp = groupOpen(g3.ptr, 0, g3.len - 1, &t, &gstk, gsp);
    try testing.expectEqual(@as(i32, @intCast(k_svg_g_depth_max)), gsp);
}

test "draw_shapes nests groups and dispatches shapes" {
    seam.shapeCallsReset();
    const doc = "<svg><g transform=\"translate(1,0)\"><rect x=\"0\" y=\"0\" width=\"2\" height=\"2\"/></g><circle cx=\"9\" cy=\"9\" r=\"2\"/></svg>";
    try testing.expectEqual(k_ra8_ok, ra8_svg_render(doc.ptr, doc.len, 0, 0, 64, 64));
    const cs = calls();
    // One rect under the group, one circle outside it.
    try testing.expectEqual(@as(usize, 2), cs.len);
}

test "attr_numf and copy_attr_id" {
    const tag = "<linearGradient id=\"g7\" x1=\"0.25\">";
    try testing.expectEqual(@as(f32, 0.25), attrNumf(tag.ptr, tag.len, "x1", 0.5));
    try testing.expectEqual(@as(f32, 0.5), attrNumf(tag.ptr, tag.len, "x2", 0.5));
    var id: [k_svg_grad_id]u8 = @splat(0);
    copyAttrId(tag.ptr, tag.len, &id);
    try testing.expectEqualStrings("g7", std.mem.sliceTo(id[0..], 0));
    const no_id = "<linearGradient x1=\"1\">";
    copyAttrId(no_id.ptr, no_id.len, &id);
    try testing.expectEqualStrings("", std.mem.sliceTo(id[0..], 0));
}

test "clamp01 and stop_offset percent" {
    try testing.expectEqual(@as(f32, 0.0), clamp01(-0.5));
    try testing.expectEqual(@as(f32, 1.0), clamp01(1.5));
    try testing.expectEqual(@as(f32, 0.4), clamp01(0.4));
    const stop_pct = "<stop offset=\"50%\"/>";
    try testing.expectEqual(@as(f32, 0.5), stopOffset(stop_pct.ptr, stop_pct.len));
    const stop_abs = "<stop offset=\"0.75\"/>";
    try testing.expectEqual(@as(f32, 0.75), stopOffset(stop_abs.ptr, stop_abs.len));
    const stop_none = "<stop/>";
    try testing.expectEqual(@as(f32, 0.0), stopOffset(stop_none.ptr, stop_none.len));
}

test "stop_color parses and defaults" {
    const stop_hex = "<stop stop-color=\"#00FF00\"/>";
    try testing.expectEqual(@as(u32, 0x00FF00), stopColor(stop_hex.ptr, stop_hex.len));
    const stop_none = "<stop stop-color=\"none\"/>";
    try testing.expectEqual(svg.k_svg_def_fill, stopColor(stop_none.ptr, stop_none.len));
    const stop_abs = "<stop/>";
    try testing.expectEqual(svg.k_svg_def_fill, stopColor(stop_abs.ptr, stop_abs.len));
}

test "parse_stops clamps and caps" {
    var g = std.mem.zeroes(SvgGrad);
    const doc = "<stop offset=\"-0.2\" stop-color=\"#000001\"/><stop offset=\"120%\" stop-color=\"#000002\"/><stop offset=\"0.5\"/>";
    parseStops(doc.ptr, 0, doc.len, &g);
    try testing.expectEqual(@as(u8, 3), g.nstops);
    try testing.expectEqual(@as(f32, 0.0), g.stops[0].off);
    try testing.expectEqual(@as(u32, 0x000001), g.stops[0].col);
    try testing.expectEqual(@as(f32, 1.0), g.stops[1].off);
    try testing.expectEqual(@as(u32, 0x000002), g.stops[1].col);
    try testing.expectEqual(@as(f32, 0.5), g.stops[2].off);
    try testing.expectEqual(svg.k_svg_def_fill, g.stops[2].col);
}

test "scan_grads reads linear and radial" {
    const doc =
        "<defs><linearGradient id=\"a\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"><stop offset=\"0\" stop-color=\"#111111\"/><stop offset=\"1\" stop-color=\"#222222\"/></linearGradient>" ++
        "<radialGradient id=\"b\" cx=\"0.3\" cy=\"0.4\" r=\"0.6\"><stop offset=\"0\"/></radialGradient>" ++
        "<linearGradient id=\"empty\"></linearGradient></defs>";
    var gs = std.mem.zeroes(SvgGrads);
    scanGrads(doc.ptr, doc.len, &gs);
    // The stopless third gradient is not counted.
    try testing.expectEqual(@as(i32, 2), gs.n);
    try testing.expectEqual(svg.k_svg_grad_linear, gs.g[0].kind);
    try testing.expectEqualStrings("a", std.mem.sliceTo(gs.g[0].id[0..], 0));
    try testing.expectEqual(@as(u8, 2), gs.g[0].nstops);
    try testing.expectEqual(@as(u32, 0x111111), gs.g[0].stops[0].col);
    try testing.expectEqual(svg.k_svg_grad_radial, gs.g[1].kind);
    try testing.expectEqual(@as(f32, 0.3), gs.g[1].x1);
    try testing.expectEqual(@as(f32, 0.4), gs.g[1].y1);
    try testing.expectEqual(@as(f32, 0.6), gs.g[1].x2);
}

test "is_svg accepts xml decl, svg tag, BOM and rejects other" {
    try testing.expect(ra8_svg_is_svg("<svg></svg>".ptr, 11));
    try testing.expect(ra8_svg_is_svg("<?xml version=\"1.0\"?><svg/>".ptr, 28));
    try testing.expect(ra8_svg_is_svg("  <svg/>".ptr, 8));
    const bom = "\xEF\xBB\xBF<svg/>";
    try testing.expect(ra8_svg_is_svg(bom.ptr, bom.len));
    try testing.expect(!ra8_svg_is_svg("<img/>".ptr, 6));
    try testing.expect(!ra8_svg_is_svg(null, 4));
    try testing.expect(!ra8_svg_is_svg("<svg/>".ptr, 0));
}

test "image_href finds xlink:href then href" {
    const doc1 = "<svg><image xlink:href=\"a.png\"/></svg>";
    var off: usize = 0;
    var vlen: usize = 0;
    try testing.expectEqual(k_ra8_ok, ra8_svg_image_href(doc1.ptr, doc1.len, &off, &vlen));
    try testing.expectEqualStrings("a.png", doc1.ptr[off .. off + vlen]);
    const doc2 = "<svg><image href=\"b.png\"/></svg>";
    try testing.expectEqual(k_ra8_ok, ra8_svg_image_href(doc2.ptr, doc2.len, &off, &vlen));
    try testing.expectEqualStrings("b.png", doc2.ptr[off .. off + vlen]);
    // No image tag, no href: not found. Null out-params: null-ptr error.
    const doc3 = "<svg><rect/></svg>";
    try testing.expectEqual(k_ra8_err_not_found, ra8_svg_image_href(doc3.ptr, doc3.len, &off, &vlen));
    try testing.expectEqual(k_ra8_err_not_found, ra8_svg_image_href("<svg></svg>".ptr, 11, &off, &vlen));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_svg_image_href(doc1.ptr, doc1.len, null, &vlen));
}

test "size reads width/height then viewBox" {
    var w: i32 = 0;
    var h: i32 = 0;
    try testing.expectEqual(k_ra8_ok, ra8_svg_size("<svg width=\"12\" height=\"9\"/>".ptr, 28, &w, &h));
    try testing.expectEqual(@as(i32, 12), w);
    try testing.expectEqual(@as(i32, 9), h);
    try testing.expectEqual(k_ra8_ok, ra8_svg_size("<svg viewBox=\"0 0 30 40\"/>".ptr, 26, &w, &h));
    try testing.expectEqual(@as(i32, 30), w);
    try testing.expectEqual(@as(i32, 40), h);
    try testing.expectEqual(k_ra8_err_not_found, ra8_svg_size("<svg/>".ptr, 6, &w, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_svg_size(null, 6, &w, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_svg_size("<svg/>".ptr, 6, null, &h));
}

test "render gates and draws" {
    seam.shapeCallsReset();
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_svg_render(null, 4, 0, 0, 8, 8));
    try testing.expectEqual(k_ra8_err_invalid_arg, ra8_svg_render("<svg/>".ptr, 6, 0, 0, 0, 8));
    seam.shapeCallsReset();
    const doc = "<svg width=\"64\" height=\"64\"><rect x=\"1\" y=\"1\" width=\"3\" height=\"3\" fill=\"#123456\"/><line x1=\"0\" y1=\"0\" x2=\"8\" y2=\"8\" stroke=\"#000000\"/></svg>";
    try testing.expectEqual(k_ra8_ok, ra8_svg_render(doc.ptr, doc.len, 0, 0, 64, 64));
    try testing.expectEqual(@as(usize, 2), calls().len);
}
