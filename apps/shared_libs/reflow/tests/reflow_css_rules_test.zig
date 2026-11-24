// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for CSS selector and `@font-face` rule parsing (`src/reflow_css_rules.zig`).

const std = @import("std");
const rules = @import("reflow").css_rules;

const testing = std.testing;
const CssSheet = rules.CssSheet;

fn freshSheet() CssSheet {
    return std.mem.zeroes(CssSheet);
}

fn sheetName(sheet: *const CssSheet, off: u16, len: u16) []const u8 {
    return sheet.names[off..][0..len];
}

test "simple selector list shares its declaration" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(&sheet, "p.note, #x", 10, "color: red; font-size: 12px;", 28);
    try testing.expectEqual(@as(u16, 2), sheet.rule_count);
    try testing.expectEqual(@as(u8, rules.lex.priv_reflow_tok_classify("p", 1)), sheet.rules[0].sel_tag);
    try testing.expectEqualStrings("note", sheetName(&sheet, sheet.rules[0].class_off, sheet.rules[0].class_len));
    try testing.expectEqualStrings("x", sheetName(&sheet, sheet.rules[1].id_off, sheet.rules[1].id_len));
    const want_set: u8 = (1 << 4) | (1 << 5);
    try testing.expectEqual(want_set, sheet.rules[0].decl.set);
    try testing.expectEqual(@as(u32, 0xFF0000), sheet.rules[0].decl.color);
    try testing.expectEqual(@as(u16, 12), sheet.rules[0].decl.font_val);
    try testing.expectEqual(want_set, sheet.rules[1].decl.set);
    try testing.expectEqual(@as(u16, 0), sheet.rules[0].order);
    try testing.expectEqual(@as(u16, 1), sheet.rules[1].order);
    try testing.expectEqual(@as(u16, 2), sheet.next_order);
}

test "descendant selector stores ancestors outermost first" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(&sheet, "ul .chapter li", 14, "color: blue;", 12);
    try testing.expectEqual(@as(u16, 1), sheet.rule_count);
    const rule = sheet.rules[0];
    try testing.expectEqual(@as(u8, 2), rule.anc_count);
    try testing.expectEqual(rules.lex.priv_reflow_tok_classify("ul", 2), rule.anc[0].tag);
    try testing.expectEqualStrings("chapter", sheetName(&sheet, rule.anc[1].class_off, rule.anc[1].class_len));
    try testing.expectEqual(rules.lex.priv_reflow_tok_classify("li", 2), rule.sel_tag);
}

test "universal selector matches everything" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(&sheet, "*", 1, "color: red;", 11);
    try testing.expectEqual(@as(u16, 1), sheet.rule_count);
    try testing.expectEqual(rules.k_reflow_tag_unknown, sheet.rules[0].sel_tag);
}

test "unsupported selectors are dropped" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(&sheet, "p.a.b", 5, "color: red;", 11);
    rules.priv_reflow_css_parse_one_block(&sheet, "foobar", 6, "color: red;", 11);
    rules.priv_reflow_css_parse_one_block(&sheet, "", 0, "color: red;", 11);
    rules.priv_reflow_css_parse_one_block(&sheet, "p:hover", 7, "color: red;", 11);
    try testing.expectEqual(@as(u16, 0), sheet.rule_count);
}

test "class name over the pool limit drops the rule" {
    var sheet = freshSheet();
    var long_name: [70]u8 = undefined;
    @memset(&long_name, 'a');
    var buf: [80]u8 = undefined;
    const sel = try std.fmt.bufPrint(&buf, "p.{s}", .{long_name});
    rules.priv_reflow_css_parse_one_block(&sheet, sel.ptr, sel.len, "color: red;", 11);
    try testing.expectEqual(@as(u16, 0), sheet.rule_count);
}

test "font-face block captures family, weight, style and src" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(
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
    rules.priv_reflow_css_parse_one_block(&sheet, "@font-face", 10, "font-weight: bold;", 18);
    try testing.expectEqual(@as(u16, 1), sheet.face_count);
}

test "font-family on a rule is interned and flagged" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(&sheet, "p", 1, "font-family: \"Serif\";", 21);
    try testing.expectEqual(@as(u16, 1), sheet.rule_count);
    try testing.expectEqual(rules.k_ra8_css_set_family, sheet.rules[0].decl.set);
    try testing.expectEqualStrings("Serif", sheetName(&sheet, sheet.rules[0].decl.family_off, sheet.rules[0].decl.family_len));
}

test "non-font-face at-rules are skipped" {
    var sheet = freshSheet();
    rules.priv_reflow_css_parse_one_block(&sheet, "@media screen", 13, "color: red;", 11);
    try testing.expectEqual(@as(u16, 0), sheet.rule_count);
    try testing.expectEqual(@as(u16, 0), sheet.face_count);
}
