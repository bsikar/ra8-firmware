// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the SVG document driver (`src/reflow_svg_doc.zig`).

const std = @import("std");
const doc = @import("reflow").svg_doc;
const seam = @import("reflow_render_test_seam.zig");

const testing = std.testing;
const svg = doc.svg;
const SvgUtf = doc.SvgUtf;
const SvgGrad = doc.SvgGrad;
const SvgGrads = doc.SvgGrads;

test "elem_at matches names with delimiters" {
    const s = "<rect x=\"1\"/><rectangle/><rect";
    try testing.expect(doc.elemAt(s.ptr, s.len, 0, "<rect"));
    try testing.expect(!doc.elemAt(s.ptr, s.len, 13, "<rect"));
    try testing.expect(doc.elemAt(s.ptr, s.len, 25, "<rect"));
}

test "tag_span finds the tag and its gt" {
    const s = "junk <svg width=\"4\"> rest";
    var open: usize = 0;
    var close: usize = 0;
    try testing.expect(doc.tagSpan(s.ptr, s.len, 0, "<svg", &open, &close));
    try testing.expectEqual(@as(usize, 5), open);
    try testing.expectEqual(@as(usize, 19), close);
    var open2: usize = 0;
    var close2: usize = 0;
    try testing.expect(!doc.tagSpan(s.ptr, s.len, 0, "<image", &open2, &close2));
}

test "read_viewbox parses four integers and falls back" {
    var t = seam.identityXform();
    const doc1 = "<svg viewBox=\"2 3 40 50\"></svg>";
    doc.readViewbox(doc1.ptr, doc1.len, &t);
    try testing.expectEqual(@as(i32, 2), t.vx);
    try testing.expectEqual(@as(i32, 3), t.vy);
    try testing.expectEqual(@as(i32, 40), t.vw);
    try testing.expectEqual(@as(i32, 50), t.vh);

    var t2 = seam.identityXform();
    const doc2 = "<svg width=\"30\" height=\"20\"></svg>";
    doc.readViewbox(doc2.ptr, doc2.len, &t2);
    try testing.expectEqual(@as(i32, 0), t2.vx);
    try testing.expectEqual(@as(i32, 30), t2.vw);
    try testing.expectEqual(@as(i32, 20), t2.vh);

    var t3 = seam.identityXform();
    const doc3 = "<svg></svg>";
    doc.readViewbox(doc3.ptr, doc3.len, &t3);
    try testing.expectEqual(@as(i32, 64), t3.vw);
    try testing.expectEqual(@as(i32, 64), t3.vh);

    var t4 = seam.identityXform();
    const doc4 = "<svg viewBox=\"0 0 0 5\"></svg>";
    doc.readViewbox(doc4.ptr, doc4.len, &t4);
    try testing.expectEqual(@as(i32, 64), t4.vw);
}

test "group_open composes, skips self-closing and caps depth" {
    var gstk: [doc.k_svg_g_depth_max + 1]SvgUtf = @splat(.{ .a = 1, .b = 0, .c = 0, .d = 1, .e = 0, .f = 0 });
    const t = seam.identityXform();
    const g1 = "<g transform=\"translate(2,3)\">";
    var gsp = doc.groupOpen(g1.ptr, 0, g1.len - 1, &t, &gstk, 0);
    try testing.expectEqual(@as(i32, 1), gsp);
    try testing.expectEqual(@as(f32, 1), gstk[1].a);
    try testing.expectEqual(@as(f32, 2), gstk[1].e);
    try testing.expectEqual(@as(f32, 3), gstk[1].f);

    const g2 = "<g transform=\"translate(5,5)\"/>";
    gsp = doc.groupOpen(g2.ptr, 0, g2.len - 1, &t, &gstk, gsp);
    try testing.expectEqual(@as(i32, 1), gsp);

    gsp = @as(i32, @intCast(doc.k_svg_g_depth_max));
    const g3 = "<g>";
    gsp = doc.groupOpen(g3.ptr, 0, g3.len - 1, &t, &gstk, gsp);
    try testing.expectEqual(@as(i32, @intCast(doc.k_svg_g_depth_max)), gsp);
}

test "draw_shapes nests groups and dispatches shapes" {
    seam.shapeCallsReset();
    const s = "<svg><g transform=\"translate(1,0)\"><rect x=\"0\" y=\"0\" width=\"2\" height=\"2\"/></g><circle cx=\"9\" cy=\"9\" r=\"2\"/></svg>";
    try testing.expectEqual(doc.k_ra8_ok, doc.ra8_svg_render(s.ptr, s.len, 0, 0, 64, 64));
    const cs = seam.calls();
    try testing.expectEqual(@as(usize, 2), cs.len);
}

test "attr_numf and copy_attr_id" {
    const tag = "<linearGradient id=\"g7\" x1=\"0.25\">";
    try testing.expectEqual(@as(f32, 0.25), doc.attrNumf(tag.ptr, tag.len, "x1", 0.5));
    try testing.expectEqual(@as(f32, 0.5), doc.attrNumf(tag.ptr, tag.len, "x2", 0.5));
    var id: [doc.k_svg_grad_id]u8 = @splat(0);
    doc.copyAttrId(tag.ptr, tag.len, &id);
    try testing.expectEqualStrings("g7", std.mem.sliceTo(id[0..], 0));
    const no_id = "<linearGradient x1=\"1\">";
    doc.copyAttrId(no_id.ptr, no_id.len, &id);
    try testing.expectEqualStrings("", std.mem.sliceTo(id[0..], 0));
}

test "clamp01 and stop_offset percent" {
    try testing.expectEqual(@as(f32, 0.0), doc.clamp01(-0.5));
    try testing.expectEqual(@as(f32, 1.0), doc.clamp01(1.5));
    try testing.expectEqual(@as(f32, 0.4), doc.clamp01(0.4));
    const stop_pct = "<stop offset=\"50%\"/>";
    try testing.expectEqual(@as(f32, 0.5), doc.stopOffset(stop_pct.ptr, stop_pct.len));
    const stop_abs = "<stop offset=\"0.75\"/>";
    try testing.expectEqual(@as(f32, 0.75), doc.stopOffset(stop_abs.ptr, stop_abs.len));
    const stop_none = "<stop/>";
    try testing.expectEqual(@as(f32, 0.0), doc.stopOffset(stop_none.ptr, stop_none.len));
}

test "stop_color parses and defaults" {
    const stop_hex = "<stop stop-color=\"#00FF00\"/>";
    try testing.expectEqual(@as(u32, 0x00FF00), doc.stopColor(stop_hex.ptr, stop_hex.len));
    const stop_none = "<stop stop-color=\"none\"/>";
    try testing.expectEqual(svg.k_svg_def_fill, doc.stopColor(stop_none.ptr, stop_none.len));
    const stop_abs = "<stop/>";
    try testing.expectEqual(svg.k_svg_def_fill, doc.stopColor(stop_abs.ptr, stop_abs.len));
}

test "parse_stops clamps and caps" {
    var g = std.mem.zeroes(SvgGrad);
    const s = "<stop offset=\"-0.2\" stop-color=\"#000001\"/><stop offset=\"120%\" stop-color=\"#000002\"/><stop offset=\"0.5\"/>";
    doc.parseStops(s.ptr, 0, s.len, &g);
    try testing.expectEqual(@as(u8, 3), g.nstops);
    try testing.expectEqual(@as(f32, 0.0), g.stops[0].off);
    try testing.expectEqual(@as(u32, 0x000001), g.stops[0].col);
    try testing.expectEqual(@as(f32, 1.0), g.stops[1].off);
    try testing.expectEqual(@as(u32, 0x000002), g.stops[1].col);
    try testing.expectEqual(@as(f32, 0.5), g.stops[2].off);
    try testing.expectEqual(svg.k_svg_def_fill, g.stops[2].col);
}

test "scan_grads reads linear and radial" {
    const s =
        "<defs><linearGradient id=\"a\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"><stop offset=\"0\" stop-color=\"#111111\"/><stop offset=\"1\" stop-color=\"#222222\"/></linearGradient>" ++
        "<radialGradient id=\"b\" cx=\"0.3\" cy=\"0.4\" r=\"0.6\"><stop offset=\"0\"/></radialGradient>" ++
        "<linearGradient id=\"empty\"></linearGradient></defs>";
    var gs = std.mem.zeroes(SvgGrads);
    doc.scanGrads(s.ptr, s.len, &gs);
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
    try testing.expect(doc.ra8_svg_is_svg("<svg></svg>".ptr, 11));
    try testing.expect(doc.ra8_svg_is_svg("<?xml version=\"1.0\"?><svg/>".ptr, 28));
    try testing.expect(doc.ra8_svg_is_svg("  <svg/>".ptr, 8));
    const bom = "\xEF\xBB\xBF<svg/>";
    try testing.expect(doc.ra8_svg_is_svg(bom.ptr, bom.len));
    try testing.expect(!doc.ra8_svg_is_svg("<img/>".ptr, 6));
    try testing.expect(!doc.ra8_svg_is_svg(null, 4));
    try testing.expect(!doc.ra8_svg_is_svg("<svg/>".ptr, 0));
}

test "image_href finds xlink:href then href" {
    const doc1 = "<svg><image xlink:href=\"a.png\"/></svg>";
    var off: usize = 0;
    var vlen: usize = 0;
    try testing.expectEqual(doc.k_ra8_ok, doc.ra8_svg_image_href(doc1.ptr, doc1.len, &off, &vlen));
    try testing.expectEqualStrings("a.png", doc1.ptr[off .. off + vlen]);
    const doc2 = "<svg><image href=\"b.png\"/></svg>";
    try testing.expectEqual(doc.k_ra8_ok, doc.ra8_svg_image_href(doc2.ptr, doc2.len, &off, &vlen));
    try testing.expectEqualStrings("b.png", doc2.ptr[off .. off + vlen]);
    const doc3 = "<svg><rect/></svg>";
    try testing.expectEqual(doc.k_ra8_err_not_found, doc.ra8_svg_image_href(doc3.ptr, doc3.len, &off, &vlen));
    try testing.expectEqual(doc.k_ra8_err_not_found, doc.ra8_svg_image_href("<svg></svg>".ptr, 11, &off, &vlen));
    try testing.expectEqual(doc.k_ra8_err_null_ptr, doc.ra8_svg_image_href(doc1.ptr, doc1.len, null, &vlen));
}

test "size reads width/height then viewBox" {
    var w: i32 = 0;
    var h: i32 = 0;
    try testing.expectEqual(doc.k_ra8_ok, doc.ra8_svg_size("<svg width=\"12\" height=\"9\"/>".ptr, 28, &w, &h));
    try testing.expectEqual(@as(i32, 12), w);
    try testing.expectEqual(@as(i32, 9), h);
    try testing.expectEqual(doc.k_ra8_ok, doc.ra8_svg_size("<svg viewBox=\"0 0 30 40\"/>".ptr, 26, &w, &h));
    try testing.expectEqual(@as(i32, 30), w);
    try testing.expectEqual(@as(i32, 40), h);
    try testing.expectEqual(doc.k_ra8_err_not_found, doc.ra8_svg_size("<svg/>".ptr, 6, &w, &h));
    try testing.expectEqual(doc.k_ra8_err_null_ptr, doc.ra8_svg_size(null, 6, &w, &h));
    try testing.expectEqual(doc.k_ra8_err_null_ptr, doc.ra8_svg_size("<svg/>".ptr, 6, null, &h));
}

test "render gates and draws" {
    seam.shapeCallsReset();
    try testing.expectEqual(doc.k_ra8_err_null_ptr, doc.ra8_svg_render(null, 4, 0, 0, 8, 8));
    try testing.expectEqual(doc.k_ra8_err_invalid_arg, doc.ra8_svg_render("<svg/>".ptr, 6, 0, 0, 0, 8));
    seam.shapeCallsReset();
    const s = "<svg width=\"64\" height=\"64\"><rect x=\"1\" y=\"1\" width=\"3\" height=\"3\" fill=\"#123456\"/><line x1=\"0\" y1=\"0\" x2=\"8\" y2=\"8\" stroke=\"#000000\"/></svg>";
    try testing.expectEqual(doc.k_ra8_ok, doc.ra8_svg_render(s.ptr, s.len, 0, 0, 64, 64));
    try testing.expectEqual(@as(usize, 2), seam.calls().len);
}
