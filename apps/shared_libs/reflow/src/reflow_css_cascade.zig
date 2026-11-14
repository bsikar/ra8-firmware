//! Stylesheet scanner, specificity cascade, and `@font-face` lookup for the
//! reflow v1 CSS subset (inc/reflow_css.h).
//!
//! Zig port of reflow_css_cascade.c (RA8FW-1219). The top-level scanner
//! `ra8_css_parse` walks `selector { ... }` blocks (skipping `/* */`
//! comments) and hands each to the rules dispatcher in
//! src/reflow_css_rules.zig (RA8FW-1218); the cascade resolves
//! inherited / rule / inline sources into one `abi.CssStyle`, and the face
//! lookup matches a family against the sheet's `@font-face` entries
//! case-insensitively with a normal-weight fallback. No MMIO, no heap;
//! all state lives in the caller-owned sheet.
//!
//! The seven public entry points keep C linkage (`@export`) so the
//! remaining C translation units (layout, render, the C test suite) link
//! against them exactly as before.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const css = @import("reflow_css.zig");
const rules = @import("reflow_css_rules.zig");

const CssSheet = abi.CssSheet;
const CssRule = abi.CssRule;
const CssAnc = abi.CssAnc;
const CssStyle = abi.CssStyle;
const CssElement = abi.CssElement;
const CssFontFace = abi.CssFontFace;

// ---- Shared constants (inc/reflow_css.h, ra8_err.h) ----

const k_ra8_css_max_rules: usize = 128;
const k_ra8_css_max_anc: usize = 4;

const k_ra8_ok: c_int = 0;
const k_ra8_err_null_ptr: c_int = 0x504;

/// ::ra8_css_match_face sentinel: no `@font-face` entry matched.
const k_ra8_css_no_face: i16 = -1;

/// ::ra8_css_set_t presence bits.
const k_ra8_css_set_bold: u8 = 1 << 0;
const k_ra8_css_set_italic: u8 = 1 << 1;
const k_ra8_css_set_underline: u8 = 1 << 2;
const k_ra8_css_set_align: u8 = 1 << 3;
const k_ra8_css_set_color: u8 = 1 << 4;
const k_ra8_css_set_fontsize: u8 = 1 << 5;
const k_ra8_css_set_display: u8 = 1 << 6;
const k_ra8_css_set_family: u8 = 1 << 7;

/// ::reflow_font_style_t style bits.
const k_reflow_style_bold: u8 = 1 << 0;
const k_reflow_style_italic: u8 = 1 << 1;
const k_reflow_style_underline: u8 = 1 << 2;

/// ::reflow_html_tag_t sentinel: no type constraint.
const k_reflow_tag_unknown: u8 = 0;

/// Cascade specificity weights: id > class > type; inheritance ranks 0.
const k_priv_rank_inherited: u16 = 0;
const k_priv_spec_type: u16 = 1;
const k_priv_spec_class: u16 = 100;
const k_priv_spec_id: u16 = 10000;

/// Byte length of a CSS comment delimiter pair (`/*` or `*/`).
const k_priv_cmt_marker: usize = 2;

/// Interned name access: the (off, len) pairs point into the sheet pool.
fn nameAt(sheet: *const CssSheet, off: u16) []const u8 {
    return sheet.names[off..];
}

// ---- Stylesheet scanner ----

/// Step over a `/* ... */` comment whose open delimiter sits at `start`,
/// returning the offset just past the close delimiter, clamped to the
/// span when the comment is unterminated.
fn skipComment(s: []const u8, start: usize) usize {
    var j = start + k_priv_cmt_marker;
    while (j + 1 < s.len and !(s[j] == '*' and s[j + 1] == '/')) : (j += 1) {}
    const past = j + k_priv_cmt_marker;
    return if (past <= s.len) past else s.len;
}

/// Locate the `{ ... }` block beginning at selector offset `i`; null when
/// no block-open byte exists at or after `i`.
const Block = struct { open: usize, close: usize };

fn findBlock(s: []const u8, i: usize) ?Block {
    var brace = i;
    while (brace < s.len and s[brace] != '{') : (brace += 1) {}
    if (brace >= s.len) return null;
    var close = brace + 1;
    while (close < s.len and s[close] != '}') : (close += 1) {}
    return .{ .open = brace, .close = close };
}

pub fn ra8_css_sheet_reset(sheet: ?*CssSheet) callconv(.c) c_int {
    const sh = sheet orelse return k_ra8_err_null_ptr;
    sh.rule_count = 0;
    sh.face_count = 0;
    sh.next_order = 0;
    sh.names_used = 0;
    return k_ra8_ok;
}

pub fn ra8_css_parse(sheet: ?*CssSheet, src: ?[*]const u8, len: u32) callconv(.c) c_int {
    const sh = sheet orelse return k_ra8_err_null_ptr;
    const s = src orelse return k_ra8_err_null_ptr;
    const text = s[0..len];
    var i: usize = 0;
    while (i < len) {
        if (i + 1 < len and text[i] == '/' and text[i + 1] == '*') {
            i = skipComment(text, i);
            continue;
        }
        if (css.priv_reflow_css_is_ws(text[i])) {
            i += 1;
            continue;
        }
        const blk = findBlock(text, i) orelse break; // no block -> done
        rules.priv_reflow_css_parse_one_block(
            sh,
            text.ptr + i,
            blk.open - i,
            text.ptr + blk.open + 1,
            blk.close - (blk.open + 1),
        );
        i = if (blk.close < len) blk.close + 1 else len;
    }
    return k_ra8_ok;
}

// ---- Selector matching ----

/// True iff a space-separated class list contains `name` exactly
/// (byte-exact token comparison).
fn classListHas(list: []const u8, name: []const u8) bool {
    var i: usize = 0;
    while (i < list.len) {
        while (i < list.len and css.priv_reflow_css_is_ws(list[i])) : (i += 1) {}
        const start = i;
        while (i < list.len and !css.priv_reflow_css_is_ws(list[i])) : (i += 1) {}
        if (i - start == name.len and std.mem.eql(u8, list[start..i], name)) return true;
    }
    return false;
}

pub fn ra8_css_rule_matches(
    rule: ?*const CssRule,
    el: ?*const CssElement,
    sheet: ?*const CssSheet,
) callconv(.c) bool {
    const r = rule orelse return false;
    const e = el orelse return false;
    const sh = sheet orelse return false;
    // Every present constraint must match (no constraint = universal).
    if (r.sel_tag != k_reflow_tag_unknown and e.tag != r.sel_tag) return false;
    if (r.class_len != 0) {
        const cls = e.class_str orelse return false;
        if (!classListHas(cls[0..e.class_len], nameAt(sh, r.class_off)[0..r.class_len]))
            return false;
    }
    if (r.id_len != 0) {
        const id = e.id orelse return false;
        if (e.id_len != r.id_len) return false;
        if (!std.mem.eql(u8, id[0..r.id_len], nameAt(sh, r.id_off)[0..r.id_len]))
            return false;
    }
    return true;
}

/// True iff one descendant ancestor part matches an element.
fn ancMatches(anc: *const CssAnc, e: *const CssElement, sh: *const CssSheet) bool {
    if (anc.tag != k_reflow_tag_unknown and e.tag != anc.tag) return false;
    if (anc.class_len != 0) {
        const cls = e.class_str orelse return false;
        if (!classListHas(cls[0..e.class_len], nameAt(sh, anc.class_off)[0..anc.class_len]))
            return false;
    }
    if (anc.id_len != 0) {
        const id = e.id orelse return false;
        if (e.id_len != anc.id_len) return false;
        if (!std.mem.eql(u8, id[0..anc.id_len], nameAt(sh, anc.id_off)[0..anc.id_len]))
            return false;
    }
    return true;
}

/// Full match of a (possibly descendant) rule against the element and its
/// open-element stack: greedy right-to-left walk, innermost first.
fn ruleMatchesCtx(
    r: *const CssRule,
    e: *const CssElement,
    ancestors: ?[*]const CssElement,
    n_anc: u8,
    sh: *const CssSheet,
) bool {
    if (!ra8_css_rule_matches(r, e, sh)) return false;
    if (r.anc_count == 0) return true;
    const anc = ancestors orelse return false;
    var ai: i32 = @as(i32, r.anc_count) - 1;
    var si: i32 = @as(i32, n_anc) - 1;
    while (ai >= 0 and si >= 0) : (si -= 1) {
        if (ancMatches(&r.anc[@intCast(ai)], &anc[@intCast(si)], sh)) ai -= 1;
    }
    return ai < 0;
}

/// Packed specificity (id*10000 + class*100 + type) over the subject and
/// every descendant part.
fn ruleRank(r: *const CssRule) u16 {
    var spec: u16 = 0;
    if (r.sel_tag != k_reflow_tag_unknown) spec +%= k_priv_spec_type;
    if (r.class_len != 0) spec +%= k_priv_spec_class;
    if (r.id_len != 0) spec +%= k_priv_spec_id;
    for (r.anc[0..r.anc_count]) |*a| {
        if (a.tag != k_reflow_tag_unknown) spec +%= k_priv_spec_type;
        if (a.class_len != 0) spec +%= k_priv_spec_class;
        if (a.id_len != 0) spec +%= k_priv_spec_id;
    }
    return spec;
}

// ---- Cascade ----

/// Resolve the winning declaration for one property across inherited,
/// matched rules, and inline sources; null when no source declares it.
fn resolve(
    setbit: u8,
    inherited: *const CssStyle,
    sh: *const CssSheet,
    matched: []const bool,
    inl: *const CssStyle,
) ?*const CssStyle {
    var win: ?*const CssStyle = null;
    var best_rank: u16 = 0;
    var best_order: u16 = 0;
    var have = false;
    if (inherited.set & setbit != 0) {
        win = inherited;
        best_rank = k_priv_rank_inherited;
        have = true;
    }
    var i: usize = 0;
    while (i < sh.rule_count) : (i += 1) {
        if (!matched[i]) continue;
        if (sh.rules[i].decl.set & setbit == 0) continue;
        const rank = ruleRank(&sh.rules[i]);
        // rules[].order is assigned monotonically in source order and this
        // loop scans in that order, so a later same-rank rule always wins.
        if (!have or rank > best_rank or
            (rank == best_rank and sh.rules[i].order >= best_order))
        {
            win = &sh.rules[i].decl;
            best_rank = rank;
            best_order = sh.rules[i].order;
            have = true;
        }
    }
    if (inl.set & setbit != 0) win = inl;
    return win;
}

const CascadeCtx = struct {
    sheet: *const CssSheet,
    matched: []const bool,
    inherited: *const CssStyle,
    inl: *const CssStyle,
};

/// Resolve the bold / italic / underline emphasis bits into `out`.
fn cascadeEmphasis(out: *CssStyle, ctx: CascadeCtx) void {
    const k_props = [_]struct { setbit: u8, stylebit: u8 }{
        .{ .setbit = k_ra8_css_set_bold, .stylebit = k_reflow_style_bold },
        .{ .setbit = k_ra8_css_set_italic, .stylebit = k_reflow_style_italic },
        .{ .setbit = k_ra8_css_set_underline, .stylebit = k_reflow_style_underline },
    };
    for (k_props) |p| {
        const win = resolve(p.setbit, ctx.inherited, ctx.sheet, ctx.matched, ctx.inl) orelse continue;
        out.set |= p.setbit;
        if (win.style & p.stylebit != 0) out.style |= p.stylebit;
    }
}

/// Resolve the scalar properties (align / colour / font-size / display).
/// font-size and display resolve from rules and inline only; inheritance
/// for those is applied by the caller to avoid double-applying a `%`.
fn cascadeScalars(out: *CssStyle, ctx: CascadeCtx) void {
    if (resolve(k_ra8_css_set_align, ctx.inherited, ctx.sheet, ctx.matched, ctx.inl)) |w| {
        out.set |= k_ra8_css_set_align;
        out.@"align" = w.@"align";
    }
    if (resolve(k_ra8_css_set_color, ctx.inherited, ctx.sheet, ctx.matched, ctx.inl)) |w| {
        out.set |= k_ra8_css_set_color;
        out.color = w.color;
    }
    if (resolve(k_ra8_css_set_fontsize, ctx.inherited, ctx.sheet, ctx.matched, ctx.inl)) |w| {
        out.set |= k_ra8_css_set_fontsize;
        out.font_val = w.font_val;
        out.font_unit = w.font_unit;
    }
    if (resolve(k_ra8_css_set_display, ctx.inherited, ctx.sheet, ctx.matched, ctx.inl)) |w| {
        out.set |= k_ra8_css_set_display;
        out.display = w.display;
    }
}

/// Resolve the inherited `font-family` slice into `out`; the name lives in
/// the sheet's name pool and is copied as (offset, length) only.
fn cascadeFamily(out: *CssStyle, ctx: CascadeCtx) void {
    if (resolve(k_ra8_css_set_family, ctx.inherited, ctx.sheet, ctx.matched, ctx.inl)) |w| {
        out.set |= k_ra8_css_set_family;
        out.family_off = w.family_off;
        out.family_len = w.family_len;
    }
}

pub fn ra8_css_cascade_ctx(
    sheet: ?*const CssSheet,
    el: ?*const CssElement,
    inherited: CssStyle,
    inline_decl: CssStyle,
    ancestors: ?[*]const CssElement,
    n_anc: u8,
) callconv(.c) CssStyle {
    const sh = sheet orelse return inherited;
    const e = el orelse return inherited;
    var matched: [k_ra8_css_max_rules]bool = std.mem.zeroes([k_ra8_css_max_rules]bool);
    var i: usize = 0;
    while (i < sh.rule_count) : (i += 1) {
        matched[i] = ruleMatchesCtx(&sh.rules[i], e, ancestors, n_anc, sh);
    }
    var out: CssStyle = std.mem.zeroes(CssStyle);
    const ctx = CascadeCtx{ .sheet = sh, .matched = &matched, .inherited = &inherited, .inl = &inline_decl };
    cascadeEmphasis(&out, ctx);
    cascadeScalars(&out, ctx);
    cascadeFamily(&out, ctx);
    return out;
}

pub fn ra8_css_cascade(
    sheet: ?*const CssSheet,
    el: ?*const CssElement,
    inherited: CssStyle,
    inline_decl: CssStyle,
) callconv(.c) CssStyle {
    return ra8_css_cascade_ctx(sheet, el, inherited, inline_decl, null, 0);
}

// ---- `@font-face` lookup ----

/// Case-insensitive equality of two byte spans (ASCII fold).
fn ciEqSpan(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |ca, cb| {
        if (css.priv_reflow_css_lower(ca) != css.priv_reflow_css_lower(cb)) return false;
    }
    return true;
}

fn familyEq(sh: *const CssSheet, f: *const CssFontFace, family: []const u8) bool {
    return ciEqSpan(nameAt(sh, f.family_off)[0..f.family_len], family);
}

pub fn ra8_css_match_face(
    sheet: ?*const CssSheet,
    family: ?[*]const u8,
    family_len: u16,
    want_bold: bool,
    want_italic: bool,
) callconv(.c) i16 {
    const sh = sheet orelse return k_ra8_css_no_face;
    const fam = family orelse return k_ra8_css_no_face;
    if (family_len == 0) return k_ra8_css_no_face;
    var fallback: i16 = k_ra8_css_no_face;
    var i: usize = 0;
    while (i < sh.face_count) : (i += 1) {
        const f = &sh.faces[i];
        if (!familyEq(sh, f, fam[0..family_len])) continue;
        if ((f.weight_bold != 0) == want_bold and (f.style_italic != 0) == want_italic)
            return @intCast(i);
        if (f.weight_bold == 0 and f.style_italic == 0 and fallback < 0)
            fallback = @intCast(i);
    }
    return fallback;
}

pub fn ra8_css_face_src(
    sheet: ?*const CssSheet,
    idx: u16,
    out_src: ?*?[*]const u8,
    out_len: ?*u16,
) callconv(.c) bool {
    const sh = sheet orelse return false;
    const os = out_src orelse return false;
    const ol = out_len orelse return false;
    if (idx >= sh.face_count) return false;
    os.* = @ptrCast(&sh.names[sh.faces[idx].src_off]);
    ol.* = sh.faces[idx].src_len;
    return true;
}

comptime {
    @export(&ra8_css_sheet_reset, .{ .name = "ra8_css_sheet_reset" });
    @export(&ra8_css_parse, .{ .name = "ra8_css_parse" });
    @export(&ra8_css_rule_matches, .{ .name = "ra8_css_rule_matches" });
    @export(&ra8_css_cascade_ctx, .{ .name = "ra8_css_cascade_ctx" });
    @export(&ra8_css_cascade, .{ .name = "ra8_css_cascade" });
    @export(&ra8_css_match_face, .{ .name = "ra8_css_match_face" });
    @export(&ra8_css_face_src, .{ .name = "ra8_css_face_src" });
}

// ---- Tests ----

const lex = @import("reflow_tokenize_lex.zig");

fn testSheet() CssSheet {
    var sheet: CssSheet = std.mem.zeroes(CssSheet);
    _ = ra8_css_sheet_reset(&sheet);
    return sheet;
}

fn testElement(tag: []const u8, id: ?[]const u8, class: ?[]const u8) CssElement {
    var el: CssElement = std.mem.zeroes(CssElement);
    el.tag = lex.priv_reflow_tok_classify(tag.ptr, tag.len);
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
    _ = ra8_css_parse(&sheet, "p { color: red; }", 17);
    try std.testing.expect(sheet.rule_count == 1);
    _ = ra8_css_sheet_reset(&sheet);
    try std.testing.expectEqual(@as(u16, 0), sheet.rule_count);
    try std.testing.expectEqual(@as(u16, 0), sheet.names_used);
}

test "parse skips comments and whitespace between blocks" {
    var sheet = testSheet();
    const text = "/* header */  p { color: red; } /* tail */ .note { color: green; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 2), sheet.rule_count);
    // Unterminated comment consumes the rest.
    var sheet2 = testSheet();
    const unter = "p { color: red; } /* never closed";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet2, unter.ptr, unter.len));
    try std.testing.expectEqual(@as(u16, 1), sheet2.rule_count);
}

test "null args are rejected" {
    var sheet = testSheet();
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_css_sheet_reset(null));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_css_parse(null, "x", 1));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_css_parse(&sheet, null, 1));
    try std.testing.expect(!ra8_css_rule_matches(null, null, null));
    try std.testing.expectEqual(k_ra8_css_no_face, ra8_css_match_face(null, "x", 1, false, false));
    var src: ?[*]const u8 = null;
    var srclen: u16 = 0;
    try std.testing.expect(!ra8_css_face_src(null, 0, &src, &srclen));
}

test "rule matching honours tag, class list, and id" {
    var sheet = testSheet();
    const text = "p { color: red; } .note { color: green; } #x { color: blue; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 3), sheet.rule_count);

    var cls = std.mem.zeroes([12]u8);
    @memcpy(cls[0..4], "note");
    const p = testElement("p", null, null);
    try std.testing.expect(ra8_css_rule_matches(&sheet.rules[0], &p, &sheet));
    try std.testing.expect(!ra8_css_rule_matches(&sheet.rules[1], &p, &sheet));

    const pn = testElement("p", null, cls[0..4]);
    try std.testing.expect(ra8_css_rule_matches(&sheet.rules[1], &pn, &sheet));
    // Exact token only: 'notebook' does not contain 'note'.
    const pn2 = testElement("p", null, "notebook");
    try std.testing.expect(!ra8_css_rule_matches(&sheet.rules[1], &pn2, &sheet));

    var idv = std.mem.zeroes([4]u8);
    idv[0] = 'x';
    const px = testElement("p", idv[0..1], null);
    try std.testing.expect(ra8_css_rule_matches(&sheet.rules[2], &px, &sheet));
    var idy = std.mem.zeroes([4]u8);
    idy[0] = 'y';
    const py = testElement("p", idy[0..1], null);
    try std.testing.expect(!ra8_css_rule_matches(&sheet.rules[2], &py, &sheet));
}

test "source order breaks same-rank ties" {
    var sheet = testSheet();
    const text = "p { color: red; } p { color: blue; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, null);
    const out = ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle));
    try std.testing.expect(out.set & k_ra8_css_set_color != 0);
    try std.testing.expectEqual(@as(u32, 0x0000FF), out.color);
}

test "class specificity beats type" {
    var sheet = testSheet();
    const text = "p { color: red; } .note { color: green; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, "note");
    const out = ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle));
    try std.testing.expectEqual(@as(u32, 0x008000), out.color);
}

test "descendant selectors match through the ancestor stack" {
    var sheet = testSheet();
    const text = "ul .chapter li { color: blue; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 1), sheet.rule_count);
    const el = testElement("li", null, "chapter");
    var anc = [_]CssElement{ testElement("ul", null, null), testElement("div", null, "chapter") };
    const out = ra8_css_cascade_ctx(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle), &anc, 2);
    try std.testing.expect(out.set & k_ra8_css_set_color != 0);
    try std.testing.expectEqual(@as(u32, 0x0000FF), out.color);
    // Without the matching ancestor stack the rule does not apply.
    var sheet2 = testSheet();
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet2, text.ptr, text.len));
    var anc2 = [_]CssElement{testElement("div", null, null)};
    const out2 = ra8_css_cascade_ctx(&sheet2, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle), &anc2, 1);
    try std.testing.expect(out2.set & k_ra8_css_set_color == 0);
}

test "inline declarations beat everything" {
    var sheet = testSheet();
    const text = "p { color: red; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, null);
    var inline_decl: CssStyle = std.mem.zeroes(CssStyle);
    _ = css.ra8_css_parse_inline("color: black".ptr, 12, &inline_decl);
    const out = ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), inline_decl);
    try std.testing.expectEqual(@as(u32, 0), out.color);
}

test "emphasis and family resolve across sources" {
    var sheet = testSheet();
    const text = "b { font-weight: bold; } p { font-family: \"Serif\"; }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    const el = testElement("p", null, null);
    const out = ra8_css_cascade(&sheet, &el, std.mem.zeroes(CssStyle), std.mem.zeroes(CssStyle));
    try std.testing.expect(out.set & k_ra8_css_set_bold == 0); // b rule does not match p
    try std.testing.expect(out.set & k_ra8_css_set_family != 0);
    try std.testing.expect(std.mem.eql(u8, nameAt(&sheet, out.family_off)[0..out.family_len], "Serif"));
}

test "face lookup: exact emphasis, case-insensitive, fallback, and src" {
    var sheet = testSheet();
    const text = "@font-face { font-family: \"Serif\"; src: url(serif.ttf); }";
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse(&sheet, text.ptr, text.len));
    try std.testing.expectEqual(@as(u16, 1), sheet.face_count);
    // Case-insensitive family match on the normal face.
    try std.testing.expectEqual(@as(i16, 0), ra8_css_match_face(&sheet, "serif", 5, false, false));
    // Bold is wanted but only a normal face exists -> the normal fallback.
    try std.testing.expectEqual(@as(i16, 0), ra8_css_match_face(&sheet, "Serif", 5, true, false));
    // Unknown family -> sentinel.
    try std.testing.expectEqual(k_ra8_css_no_face, ra8_css_match_face(&sheet, "Mono", 4, false, false));
    try std.testing.expectEqual(k_ra8_css_no_face, ra8_css_match_face(&sheet, "", 0, false, false));
    var src: ?[*]const u8 = null;
    var srclen: u16 = 0;
    try std.testing.expect(ra8_css_face_src(&sheet, 0, &src, &srclen));
    try std.testing.expectEqual(@as(u16, 9), srclen);
    try std.testing.expect(std.mem.eql(u8, src.?[0..srclen], "serif.ttf"));
    try std.testing.expect(!ra8_css_face_src(&sheet, 1, &src, &srclen));
}

test "names over the 64-byte limit drop their rule" {
    var sheet = testSheet();
    const ok = ".overflowclassoverflowclassoverflowclassoverflowclassoverflowclas { color: red; }"; // 64-char name
    _ = ra8_css_parse(&sheet, ok.ptr, ok.len);
    try std.testing.expectEqual(@as(u16, 1), sheet.rule_count);
    const long = ".overflowclassoverflowclassoverflowclassoverflowclassoverflowclasx { color: red; }"; // 65-char name
    _ = ra8_css_parse(&sheet, long.ptr, long.len);
    try std.testing.expectEqual(@as(u16, 1), sheet.rule_count);
}
