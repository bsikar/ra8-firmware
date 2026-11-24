// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for CSS stylesheet cascade and `@font-face` lookup (`src/reflow_css_cascade.zig`).

const std = @import("std");
const reflow = @import("reflow");
const cascade = reflow.css_cascade;
const css = reflow.css;

const CssSheet = reflow.CssSheet;
const CssElement = cascade.CssElement;
const CssStyle = cascade.CssStyle;

fn testSheet() CssSheet {
    var sheet: CssSheet = std.mem.zeroes(CssSheet);
    _ = cascade.ra8_css_sheet_reset(&sheet);
    return sheet;
}

fn testElement(tag: []const u8, id: ?[]const u8, class: ?[]const u8) CssElement {
    var el: CssElement = std.mem.zeroes(CssElement);
    el.tag = cascade.lex.priv_reflow_tok_classify(tag.ptr, tag.len);
    if (id) |i| {
        el.id = i.ptr;
        el.id_len = @intCast(i.len);
    }
    if (class) |c| {
        el.class_str = c.ptr;
        el.class_len = @intCast(c.len);
    }
    return el;
}

test "sheet reset clears counters" {
    var sheet = testSheet();
    _ = cascade.ra8_css_parse(&sheet, "p { color: red; }", 17);
    try std.testing.expect(sheet.rule_count == 1);
    _ = cascade.ra8_css_sheet_reset(&sheet);
    try std.testing.expectEqual(@as(u16, 0), sheet.rule_count);
    try std.testing.expectEqual(@as(u16, 0), sheet.names_used);
}

test "parse skips comments and whitespace between blocks" {
    var sheet = testSheet();
    const text = "/* header */  p { color: red; } /* tail */ .note { color: green; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 2), sheet.rule_count);
    var sheet2 = testSheet();
    const unter = "p { color: red; } /* never closed";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet2, unter.ptr, unter.len));
    try std.testing.expectEqual(@as(u16, 1), sheet2.rule_count);
}

test "null args are rejected" {
    var sheet = testSheet();
    try std.testing.expectEqual(cascade.k_ra8_err_null_ptr, cascade.ra8_css_sheet_reset(null));
    try std.testing.expectEqual(cascade.k_ra8_err_null_ptr, cascade.ra8_css_parse(null, "x", 1));
    try std.testing.expectEqual(cascade.k_ra8_err_null_ptr, cascade.ra8_css_parse(&sheet, null, 1));
    try std.testing.expect(!cascade.ra8_css_rule_matches(null, null, null));
    try std.testing.expectEqual(cascade.k_ra8_css_no_face, cascade.ra8_css_match_face(null, "x", 1, false, false));
    var src: ?[*]const u8 = null;
    var srclen: u16 = 0;
    try std.testing.expect(!cascade.ra8_css_face_src(null, 0, &src, &srclen));
}

test "rule matching honours tag, class list, and id" {
    var sheet = testSheet();
    const text = "p { color: red; } .note { color: green; } #x { color: blue; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 3), sheet.rule_count);

    var cls = std.mem.zeroes([12]u8);
    @memcpy(cls[0..4], "note");
    const p = testElement("p", null, null);
    try std.testing.expect(cascade.ra8_css_rule_matches(&sheet.rules[0], &p, &sheet));
    try std.testing.expect(!cascade.ra8_css_rule_matches(&sheet.rules[1], &p, &sheet));

    const pn = testElement("p", null, cls[0..4]);
    try std.testing.expect(cascade.ra8_css_rule_matches(&sheet.rules[1], &pn, &sheet));
    const pn2 = testElement("p", null, "notebook");
    try std.testing.expect(!cascade.ra8_css_rule_matches(&sheet.rules[1], &pn2, &sheet));

    var idv = std.mem.zeroes([4]u8);
    idv[0] = 'x';
    const px = testElement("p", idv[0..1], null);
    try std.testing.expect(cascade.ra8_css_rule_matches(&sheet.rules[2], &px, &sheet));
    var idy = std.mem.zeroes([4]u8);
    idy[0] = 'y';
    const py = testElement("p", idy[0..1], null);
    try std.testing.expect(!cascade.ra8_css_rule_matches(&sheet.rules[2], &py, &sheet));
}

test "source order breaks same-rank ties" {
    var sheet = testSheet();
    const text = "p { color: red; } p { color: blue; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, null);
    const out = cascade.ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle));
    try std.testing.expect(out.set & cascade.k_ra8_css_set_color != 0);
    try std.testing.expectEqual(@as(u32, 0x0000FF), out.color);
}

test "class specificity beats type" {
    var sheet = testSheet();
    const text = "p { color: red; } .note { color: green; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, "note");
    const out = cascade.ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle));
    try std.testing.expectEqual(@as(u32, 0x008000), out.color);
}

test "descendant selectors match through the ancestor stack" {
    var sheet = testSheet();
    const text = "ul .chapter li { color: blue; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 1), sheet.rule_count);
    const el = testElement("li", null, "chapter");
    var anc = [_]CssElement{ testElement("ul", null, null), testElement("div", null, "chapter") };
    const out = cascade.ra8_css_cascade_ctx(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle), &anc, 2);
    try std.testing.expect(out.set & cascade.k_ra8_css_set_color != 0);
    try std.testing.expectEqual(@as(u32, 0x0000FF), out.color);
    var sheet2 = testSheet();
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet2, text.ptr, text.len));
    var anc2 = [_]CssElement{testElement("div", null, null)};
    const out2 = cascade.ra8_css_cascade_ctx(&sheet2, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle), &anc2, 1);
    try std.testing.expect(out2.set & cascade.k_ra8_css_set_color == 0);
}

test "inline declarations beat everything" {
    var sheet = testSheet();
    const text = "p { color: red; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, null);
    var inline_decl: CssStyle = std.mem.zeroes(CssStyle);
    _ = css.ra8_css_parse_inline("color: black".ptr, 12, &inline_decl);
    const out = cascade.ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), inline_decl);
    try std.testing.expectEqual(@as(u32, 0), out.color);
}

test "emphasis and family resolve across sources" {
    var sheet = testSheet();
    const text = "b { font-weight: bold; } p { font-family: \"Serif\"; }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, null);
    const out = cascade.ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle));
    try std.testing.expect(out.set & cascade.k_ra8_css_set_bold == 0);
    try std.testing.expect(out.set & cascade.k_ra8_css_set_family != 0);
    try std.testing.expect(std.mem.eql(u8, cascade.nameAt(&sheet, out.family_off)[0..out.family_len], "Serif"));
}

test "face lookup: exact emphasis, case-insensitive, fallback, and src" {
    var sheet = testSheet();
    const text = "@font-face { font-family: \"Serif\"; src: url(serif.ttf); }";
    try std.testing.expectEqual(cascade.k_ra8_ok, cascade.ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 1), sheet.face_count);
    try std.testing.expectEqual(@as(i16, 0), cascade.ra8_css_match_face(&sheet, "serif", 5, false, false));
    try std.testing.expectEqual(@as(i16, 0), cascade.ra8_css_match_face(&sheet, "Serif", 5, true, false));
    try std.testing.expectEqual(cascade.k_ra8_css_no_face, cascade.ra8_css_match_face(&sheet, "Mono", 4, false, false));
    try std.testing.expectEqual(cascade.k_ra8_css_no_face, cascade.ra8_css_match_face(&sheet, "", 0, false, false));
    var src: ?[*]const u8 = null;
    var srclen: u16 = 0;
    try std.testing.expect(cascade.ra8_css_face_src(&sheet, 0, &src, &srclen));
    try std.testing.expectEqual(@as(u16, 9), srclen);
    try std.testing.expect(std.mem.eql(u8, src.?[0..srclen], "serif.ttf"));
    try std.testing.expect(!cascade.ra8_css_face_src(&sheet, 1, &src, &srclen));
}

test "names over the 64-byte limit drop their rule" {
    var sheet = testSheet();
    const ok = ".overflowclassoverflowclassoverflowclassoverflowclassoverflowclas { color: red; }";
    _ = cascade.ra8_css_parse(&sheet, ok.ptr, ok.len);
    try std.testing.expectEqual(@as(u16, 1), sheet.rule_count);
    const long = ".overflowclassoverflowclassoverflowclassoverflowclassoverflowclasx { color: red; }";
    _ = cascade.ra8_css_parse(&sheet, long.ptr, long.len);
    try std.testing.expectEqual(@as(u16, 1), sheet.rule_count);
}
