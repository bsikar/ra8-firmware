//! Content-CSS selector + `@font-face` rule parsing for the reflow engine.
//!
//! Zig port of reflow_css_rules.c (RA8FW-1218). Companion unit to
//! reflow_css.zig: the stylesheet scanner (reflow_css_cascade.zig since
//! RA8FW-1219) splits the CSS text into `selector { ... }` blocks and hands each one
//! here, where the selector grammar (type / class / id / descendant
//! compounds), the `@font-face` descriptor parser, and the top-level block
//! dispatcher `priv_reflow_css_parse_one_block` intern selectors and faces
//! into the caller-owned `abi.CssSheet` name pool and rule / face tables.
//!
//! Pure byte-span parsing: no MMIO, no heap, and every loop is bounded by
//! the span length it walks. The dispatcher keeps C linkage (`@export`) so
//! the cascade translation unit links against it exactly as before.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lex = @import("reflow_tokenize_lex.zig");
const css = @import("reflow_css.zig");

const CssSheet = abi.CssSheet;
const CssRule = abi.CssRule;
const CssAnc = abi.CssAnc;
const CssStyle = abi.CssStyle;
const CssFontFace = abi.CssFontFace;

// ---- Shared constants (inc/reflow_css.h) ----

/// ::ra8_css_set_t presence bit for `font-family`.
const k_ra8_css_set_family: u8 = 1 << 7;

/// Max bytes of one class / id / font name.
const k_ra8_css_name_max: usize = 64;

/// Max descendant-ancestor parts per selector.
const k_ra8_css_max_anc: usize = 4;

/// ::reflow_html_tag_t sentinel: no type constraint.
const k_reflow_tag_unknown: u8 = 0;

/// Length of the `url(` token prefix.
const k_priv_url_len: usize = 4;

// ---- Selector parsing ----

/// True iff `c` can appear inside a bare type / class / id name: ASCII
/// letters, decimal digits, hyphen and underscore (CSS identifier chars).
fn isNameChar(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or
        c == '-' or c == '_';
}

/// Intern a class/id name into the sheet pool; null if it does not fit.
/// Mirrors internal_intern_name: the pool is left untouched on rejection.
fn internName(sheet: *CssSheet, name: []const u8) ?u16 {
    if (name.len == 0 or name.len > k_ra8_css_name_max) return null;
    const used: usize = sheet.names_used;
    if (used + name.len > sheet.names.len) return null;
    @memcpy(sheet.names[used..][0..name.len], name);
    sheet.names_used = @intCast(used + name.len);
    return @intCast(used);
}

/// Parse the optional leading type / `*` of a compound selector.
///
/// `*` consumes one byte and records no type constraint; a name run is
/// classified with `priv_reflow_tok_classify` and an unrecognised tag
/// rejects the rule.
fn parseSelType(s: []const u8, i: *usize, rule: *CssRule) bool {
    if (s[i.*] == '*') {
        i.* += 1; // universal: no type constraint
        return true;
    }
    const start = i.*;
    while (i.* < s.len and isNameChar(s[i.*])) : (i.* += 1) {}
    const tag = lex.priv_reflow_tok_classify(s.ptr + start, i.* - start);
    if (tag == k_reflow_tag_unknown) {
        return false; // unrecognised tag -> drop the rule
    }
    rule.sel_tag = tag;
    return true;
}

/// Parse one `.class` / `#id` part; advance `i` past prefix and name.
///
/// The name is interned into the sheet pool and stored as (off, len) on the
/// matching rule field. A second class or id on the same rule is unsupported
/// in v1 and rejects the rule.
fn parseSelPart(sheet: *CssSheet, s: []const u8, i: *usize, rule: *CssRule) bool {
    const kind = s[i.*];
    i.* += 1;
    const start = i.*;
    while (i.* < s.len and isNameChar(s[i.*])) : (i.* += 1) {}
    const nlen = i.* - start;
    const off = internName(sheet, s[start..][0..nlen]) orelse return false;
    if (kind == '.') {
        if (rule.class_len != 0) {
            return false; // a second class -> unsupported in v1
        }
        rule.class_off = off;
        rule.class_len = @intCast(nlen);
        return true;
    }
    if (rule.id_len != 0) {
        return false; // a second id
    }
    rule.id_off = off;
    rule.id_len = @intCast(nlen);
    return true;
}

/// Parse ONE trimmed compound selector: one type + one class + one id in CSS
/// order. Two classes, descendant combinators and pseudo selectors fail.
fn parseSelector(sheet: *CssSheet, s: []const u8, rule: *CssRule) bool {
    if (s.len == 0) {
        return false;
    }
    var i: usize = 0;
    var any = false;
    if (isNameChar(s[0]) or s[0] == '*') {
        if (!parseSelType(s, &i, rule)) {
            return false;
        }
        any = true;
    }
    // Then a run of `.class` / `#id` parts (at most one of each).
    while (i < s.len) {
        if ((s[i] != '.' and s[i] != '#') or !parseSelPart(sheet, s, &i, rule)) {
            return false; // combinator / pseudo / dup / bad name -> unsupported
        }
        any = true;
    }
    return any;
}

/// Split `s` into whitespace-separated compound spans. Returns the part
/// count, or -1 when more than `k_ra8_css_max_anc + 1` compounds appear.
fn splitCompounds(s: []const u8, starts: *[k_ra8_css_max_anc + 1]usize, lens: *[k_ra8_css_max_anc + 1]usize) i32 {
    var nparts: usize = 0;
    var i: usize = 0;
    while (i < s.len) {
        while (i < s.len and css.priv_reflow_css_is_ws(s[i])) : (i += 1) {}
        if (i >= s.len) {
            break;
        }
        if (nparts > k_ra8_css_max_anc) {
            return -1; // more than one subject + k_ra8_css_max_anc ancestor parts
        }
        const start = i;
        while (i < s.len and !css.priv_reflow_css_is_ws(s[i])) : (i += 1) {}
        starts[nparts] = start;
        lens[nparts] = i - start;
        nparts += 1;
    }
    return @intCast(nparts);
}

/// Parse a (possibly descendant) selector string: the last whitespace
/// compound is the subject, the earlier ones are ancestor constraints stored
/// in selector order (outermost first).
fn parseComplexSelector(sheet: *CssSheet, s: []const u8, rule: *CssRule) bool {
    var part_start: [k_ra8_css_max_anc + 1]usize = undefined;
    var part_len: [k_ra8_css_max_anc + 1]usize = undefined;
    const nparts = splitCompounds(s, &part_start, &part_len);
    if (nparts <= 0) {
        return false;
    }
    const np: usize = @intCast(nparts);
    // Last part = subject compound.
    if (!parseSelector(sheet, s[part_start[np - 1]..][0..part_len[np - 1]], rule)) {
        return false;
    }
    // Earlier parts = ancestor constraints (outermost first).
    rule.anc_count = @intCast(np - 1);
    var a: usize = 0;
    while (a + 1 < np) : (a += 1) {
        var tmp: CssRule = std.mem.zeroes(CssRule);
        if (!parseSelector(sheet, s[part_start[a]..][0..part_len[a]], &tmp)) {
            return false;
        }
        rule.anc[a] = .{
            .tag = tmp.sel_tag,
            .pad8 = 0,
            .class_off = tmp.class_off,
            .class_len = tmp.class_len,
            .id_off = tmp.id_off,
            .id_len = tmp.id_len,
        };
    }
    return true;
}

/// Append one fully-built rule to the sheet; drop silently if full.
fn pushRule(sheet: *CssSheet, rule: *const CssRule) void {
    if (sheet.rule_count >= sheet.rules.len) {
        return;
    }
    sheet.rules[sheet.rule_count] = rule.*;
    sheet.rules[sheet.rule_count].order = sheet.next_order;
    sheet.rule_count += 1;
    sheet.next_order += 1;
}

/// Parse a comma-grouped selector list sharing one declaration. Selectors
/// that fail to parse are silently discarded.
fn parseSelectorList(sheet: *CssSheet, sel: []const u8, decl: CssStyle) void {
    var i: usize = 0;
    while (i < sel.len) {
        var comma: usize = i;
        while (comma < sel.len and sel[comma] != ',') : (comma += 1) {}
        var one_len: usize = comma - i;
        const one = css.priv_reflow_css_trim(sel.ptr + i, &one_len);
        var rule: CssRule = std.mem.zeroes(CssRule);
        rule.decl = decl;
        if (parseComplexSelector(sheet, one[0..one_len], &rule)) {
            pushRule(sheet, &rule);
        }
        i = comma + 1;
    }
}

// ---- @font-face + font-family parsing + block dispatch ----

/// Strip one layer of matching `'` / `"` quotes from a span.
fn stripQuotes(s: []const u8) []const u8 {
    if (s.len >= 2 and (s[0] == '"' or s[0] == '\'') and s[s.len - 1] == s[0]) {
        return s[1 .. s.len - 1];
    }
    return s;
}

/// Extract the path inside the first `url(...)` of a `src` value: the span
/// between the parentheses, trimmed and de-quoted. Null when no `url(` is
/// present or the extracted path is empty after trimming.
fn extractUrl(val: []const u8) ?[]const u8 {
    var i: usize = 0;
    while (i + k_priv_url_len <= val.len) : (i += 1) {
        if (!css.priv_reflow_css_ci_eq(val.ptr + i, k_priv_url_len, "url(")) {
            continue;
        }
        const a = i + k_priv_url_len;
        var b = a;
        while (b < val.len and val[b] != ')') : (b += 1) {}
        var n: usize = b - a;
        const p = css.priv_reflow_css_trim(val.ptr + a, &n);
        const path = stripQuotes(p[0..n]);
        if (path.len == 0) {
            return null;
        }
        return path;
    }
    return null;
}

/// True for a `font-weight` keyword that selects the bold face.
fn isBoldKw(val: []const u8) bool {
    return css.priv_reflow_css_ci_eq(val.ptr, val.len, "bold") or
        css.priv_reflow_css_ci_eq(val.ptr, val.len, "bolder") or
        css.priv_reflow_css_ci_eq(val.ptr, val.len, "600") or
        css.priv_reflow_css_ci_eq(val.ptr, val.len, "700") or
        css.priv_reflow_css_ci_eq(val.ptr, val.len, "800") or
        css.priv_reflow_css_ci_eq(val.ptr, val.len, "900");
}

/// True for a `font-style` keyword that selects the italic face.
fn isItalicKw(val: []const u8) bool {
    return css.priv_reflow_css_ci_eq(val.ptr, val.len, "italic") or
        css.priv_reflow_css_ci_eq(val.ptr, val.len, "oblique");
}

/// Apply one `@font-face` descriptor to the face being built. Recognised
/// descriptors: font-family (interned), font-weight, font-style, src
/// (url() path interned). Everything else is silently ignored.
fn faceApply(sheet: *CssSheet, prop: []const u8, val: []const u8, face: *CssFontFace) void {
    if (css.priv_reflow_css_ci_eq(prop.ptr, prop.len, "font-family")) {
        const fam = stripQuotes(val);
        if (fam.len > 0) {
            if (internName(sheet, fam)) |off| {
                face.family_off = off;
                face.family_len = @intCast(fam.len);
            }
        }
    } else if (css.priv_reflow_css_ci_eq(prop.ptr, prop.len, "font-weight")) {
        face.weight_bold = if (isBoldKw(val)) 1 else 0;
    } else if (css.priv_reflow_css_ci_eq(prop.ptr, prop.len, "font-style")) {
        face.style_italic = if (isItalicKw(val)) 1 else 0;
    } else if (css.priv_reflow_css_ci_eq(prop.ptr, prop.len, "src")) {
        if (extractUrl(val)) |url| {
            if (internName(sheet, url)) |off| {
                face.src_off = off;
                face.src_len = @intCast(url.len);
            }
        }
    }
}

/// Iterate `prop:value;` pairs in a declaration block (no braces), handing
/// each trimmed non-empty pair to `cb` with `ctx`.
fn forEachDecl(ctx: anytype, comptime cb: anytype, s: []const u8) void {
    var i: usize = 0;
    while (i < s.len) {
        var semi: usize = i;
        while (semi < s.len and s[semi] != ';') : (semi += 1) {}
        var colon: usize = i;
        while (colon < semi and s[colon] != ':') : (colon += 1) {}
        if (colon < semi) {
            var plen: usize = colon - i;
            const prop = css.priv_reflow_css_trim(s.ptr + i, &plen);
            var vlen: usize = semi - (colon + 1);
            const val = css.priv_reflow_css_trim(s.ptr + colon + 1, &vlen);
            if (plen > 0 and vlen > 0) {
                cb(ctx, prop[0..plen], val[0..vlen]);
            }
        }
        i = semi + 1;
    }
}

/// Context threading a sheet + the face being built through the loop.
const FaceCtx = struct {
    sheet: *CssSheet,
    face: *CssFontFace,
};

/// Descriptor adapter for the `@font-face` scan.
fn faceCb(ctx: *FaceCtx, prop: []const u8, val: []const u8) void {
    faceApply(ctx.sheet, prop, val, ctx.face);
}

/// Parse one `@font-face { ... }` block; append it if family + src are set.
fn parseFontface(sheet: *CssSheet, block: []const u8) void {
    if (sheet.face_count >= sheet.faces.len) {
        return;
    }
    var face: CssFontFace = std.mem.zeroes(CssFontFace);
    var ctx = FaceCtx{ .sheet = sheet, .face = &face };
    forEachDecl(&ctx, faceCb, block);
    if (face.family_len != 0 and face.src_len != 0) {
        sheet.faces[sheet.face_count] = face;
        sheet.face_count += 1;
    }
}

/// Context threading a sheet + the rule declaration through the loop.
const FamilyCtx = struct {
    sheet: *CssSheet,
    decl: *CssStyle,
};

/// Declaration adapter that interns a rule's `font-family` value.
fn familyCb(ctx: *FamilyCtx, prop: []const u8, val: []const u8) void {
    if (!css.priv_reflow_css_ci_eq(prop.ptr, prop.len, "font-family")) {
        return;
    }
    const fam = stripQuotes(val);
    if (fam.len > 0) {
        if (internName(ctx.sheet, fam)) |off| {
            ctx.decl.set |= k_ra8_css_set_family;
            ctx.decl.family_off = off;
            ctx.decl.family_len = @intCast(fam.len);
        }
    }
}

/// Scan a rule's declaration block for `font-family` and intern it.
fn extractFamily(sheet: *CssSheet, block: []const u8, decl: *CssStyle) void {
    var ctx = FamilyCtx{ .sheet = sheet, .decl = decl };
    forEachDecl(&ctx, familyCb, block);
}

/// Route an at-rule: parse `@font-face`; skip every other `@`-rule.
fn parseAtRule(sheet: *CssSheet, sel: []const u8, block: []const u8) void {
    if (css.priv_reflow_css_ci_eq(sel.ptr, sel.len, "@font-face")) {
        parseFontface(sheet, block);
    }
    // @media / @import / @page / ... are out of v1 scope -> skipped.
}

/// Dispatch one `selector|@rule { block }`: at-rule vs. style rule.
pub fn priv_reflow_css_parse_one_block(
    sheet: ?*CssSheet,
    sel: ?[*]const u8,
    sel_len: usize,
    block: ?[*]const u8,
    block_len: usize,
) callconv(.c) void {
    const sh = sheet orelse return;
    const selp = sel orelse return;
    const bp = block orelse return;
    var tlen: usize = sel_len;
    const tsel = css.priv_reflow_css_trim(selp, &tlen);
    if (tlen > 0 and tsel[0] == '@') {
        parseAtRule(sh, tsel[0..tlen], bp[0..block_len]);
        return;
    }
    var decl: CssStyle = std.mem.zeroes(CssStyle);
    css.priv_reflow_css_parse_decls(bp, block_len, &decl);
    extractFamily(sh, bp[0..block_len], &decl);
    parseSelectorList(sh, selp[0..sel_len], decl);
}

comptime {
    @export(&priv_reflow_css_parse_one_block, .{ .name = "priv_reflow_css_parse_one_block" });
}

// ---- Tests ----

const testing = std.testing;

fn freshSheet() CssSheet {
    return std.mem.zeroes(CssSheet);
}

fn sheetName(sheet: *const CssSheet, off: u16, len: u16) []const u8 {
    return sheet.names[off..][0..len];
}

test "simple selector list shares its declaration" {
    var sheet = freshSheet();
    priv_reflow_css_parse_one_block(&sheet, "p.note, #x", 10, "color: red; font-size: 12px;", 28);
    try testing.expectEqual(@as(u16, 2), sheet.rule_count);
    try testing.expectEqual(@as(u8, lex.priv_reflow_tok_classify("p", 1)), sheet.rules[0].sel_tag);
    try testing.expectEqualStrings("note", sheetName(&sheet, sheet.rules[0].class_off, sheet.rules[0].class_len));
    try testing.expectEqualStrings("x", sheetName(&sheet, sheet.rules[1].id_off, sheet.rules[1].id_len));
    // Same declaration on both rules: red + 12px + family absent.
    const want_set: u8 = (1 << 4) | (1 << 5);
    try testing.expectEqual(want_set, sheet.rules[0].decl.set);
    try testing.expectEqual(@as(u32, 0xFF0000), sheet.rules[0].decl.color);
    try testing.expectEqual(@as(u16, 12), sheet.rules[0].decl.font_val);
    try testing.expectEqual(want_set, sheet.rules[1].decl.set);
    // Source order stamps.
    try testing.expectEqual(@as(u16, 0), sheet.rules[0].order);
    try testing.expectEqual(@as(u16, 1), sheet.rules[1].order);
    try testing.expectEqual(@as(u16, 2), sheet.next_order);
}

test "descendant selector stores ancestors outermost first" {
    var sheet = freshSheet();
    priv_reflow_css_parse_one_block(&sheet, "ul .chapter li", 14, "color: blue;", 12);
    try testing.expectEqual(@as(u16, 1), sheet.rule_count);
    const rule = sheet.rules[0];
    try testing.expectEqual(@as(u8, 2), rule.anc_count);
    try testing.expectEqual(lex.priv_reflow_tok_classify("ul", 2), rule.anc[0].tag);
    try testing.expectEqualStrings("chapter", sheetName(&sheet, rule.anc[1].class_off, rule.anc[1].class_len));
    try testing.expectEqual(lex.priv_reflow_tok_classify("li", 2), rule.sel_tag);
}

test "universal selector matches everything" {
    var sheet = freshSheet();
    priv_reflow_css_parse_one_block(&sheet, "*", 1, "color: red;", 11);
    try testing.expectEqual(@as(u16, 1), sheet.rule_count);
    try testing.expectEqual(k_reflow_tag_unknown, sheet.rules[0].sel_tag);
}

test "unsupported selectors are dropped" {
    var sheet = freshSheet();
    // Two classes on one compound.
    priv_reflow_css_parse_one_block(&sheet, "p.a.b", 5, "color: red;", 11);
    // Unknown tag name.
    priv_reflow_css_parse_one_block(&sheet, "foobar", 6, "color: red;", 11);
    // Empty selector.
    priv_reflow_css_parse_one_block(&sheet, "", 0, "color: red;", 11);
    // Pseudo selector.
    priv_reflow_css_parse_one_block(&sheet, "p:hover", 7, "color: red;", 11);
    try testing.expectEqual(@as(u16, 0), sheet.rule_count);
}

test "class name over the pool limit drops the rule" {
    var sheet = freshSheet();
    var long_name: [70]u8 = undefined;
    @memset(&long_name, 'a');
    var buf: [80]u8 = undefined;
    const sel = try std.fmt.bufPrint(&buf, "p.{s}", .{long_name});
    priv_reflow_css_parse_one_block(&sheet, sel.ptr, sel.len, "color: red;", 11);
    try testing.expectEqual(@as(u16, 0), sheet.rule_count);
}

test "font-face block captures family, weight, style and src" {
    var sheet = freshSheet();
    priv_reflow_css_parse_one_block(
        &sheet,
        "@font-face",
        10,
        "font-family: 'Body'; font-weight: bold; font-style: italic; unicode-range: U+0-FF; src: url(fonts/Body.ttf);",
        108,
    );
    try testing.expectEqual(@as(u16, 1), sheet.face_count);
    const face = sheet.faces[0];
    try testing.expectEqualStrings("Body", sheetName(&sheet, face.family_off, face.family_len));
    try testing.expectEqualStrings("fonts/Body.ttf", sheetName(&sheet, face.src_off, face.src_len));
    try testing.expectEqual(@as(u8, 1), face.weight_bold);
    try testing.expectEqual(@as(u8, 1), face.style_italic);
    // A face without family or src is not kept.
    priv_reflow_css_parse_one_block(&sheet, "@font-face", 10, "font-weight: bold;", 18);
    try testing.expectEqual(@as(u16, 1), sheet.face_count);
}

test "font-family on a rule is interned and flagged" {
    var sheet = freshSheet();
    priv_reflow_css_parse_one_block(&sheet, "p", 1, "font-family: \"Serif\";", 21);
    try testing.expectEqual(@as(u16, 1), sheet.rule_count);
    try testing.expectEqual(k_ra8_css_set_family, sheet.rules[0].decl.set);
    try testing.expectEqualStrings("Serif", sheetName(&sheet, sheet.rules[0].decl.family_off, sheet.rules[0].decl.family_len));
}

test "non-font-face at-rules are skipped" {
    var sheet = freshSheet();
    priv_reflow_css_parse_one_block(&sheet, "@media screen", 13, "color: red;", 11);
    try testing.expectEqual(@as(u16, 0), sheet.rule_count);
    try testing.expectEqual(@as(u16, 0), sheet.face_count);
}
