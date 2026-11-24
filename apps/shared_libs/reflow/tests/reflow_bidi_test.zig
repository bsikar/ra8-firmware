// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for UAX #9 bidirectional algorithm (`src/reflow_bidi.zig`),
//! Unicode property lookups (`src/reflow_unicode.zig`), and CSS/HTML `dir`
//! token integration (`src/reflow_css.zig`, `src/reflow_tokenize.zig`).

const std = @import("std");
const reflow = @import("reflow");
const u = reflow.unicode;
const bidi = reflow.bidi;
const css = reflow.css;
const tok = reflow.tokenize;

const testing = std.testing;
const ReflowEngine = reflow.ReflowEngine;

const TestVis = struct {
    cp: i32,
    level: u8,
    orig_cls: u.BidiClass,
    cluster_id: u32,
};

fn runBidiSequence(cps: []const u32, dir: bidi.BaseDir, out_items: []bidi.BidiItem, out_len: *usize) u8 {
    var sc: bidi.BidiScratch = .{};
    var tmp: [64]bidi.BidiItem = undefined;
    const n = @min(cps.len, tmp.len);
    for (cps[0..n], 0..) |cp, i| {
        const c = u.bidiClass(cp);
        tmp[i] = .{
            .cp = cp,
            .cls = c,
            .orig_cls = c,
            .level = 0,
            .cluster_id = @intCast(i + 1),
            .removed = false,
            .paired_mirror = false,
        };
    }
    const para_lvl = bidi.resolveParagraph(tmp[0..n], dir, &sc);
    var wn: usize = 0;
    for (tmp[0..n]) |it| {
        if (it.removed or u.isBidiFormatChar(it.cp)) continue;
        out_items[wn] = it;
        wn += 1;
    }
    var vis: [64]TestVis = undefined;
    for (out_items[0..wn], 0..) |it, i| {
        vis[i] = .{
            .cp = @intCast(it.cp),
            .level = it.level,
            .orig_cls = it.orig_cls,
            .cluster_id = it.cluster_id,
        };
    }
    bidi.reorderVisualLine(TestVis, vis[0..wn], para_lvl);
    for (vis[0..wn], 0..) |v, i| {
        out_items[i].cp = @intCast(v.cp);
        out_items[i].level = v.level;
    }
    out_len.* = wn;
    return para_lvl;
}

test "unicode properties: bidi classes, mirroring, arabic forms, indic & thai roles" {
    try testing.expectEqual(u.BidiClass.l, u.bidiClass('A'));
    try testing.expectEqual(u.BidiClass.r, u.bidiClass(0x05D0)); // Hebrew Alef
    try testing.expectEqual(u.BidiClass.al, u.bidiClass(0x0627)); // Arabic Alef
    try testing.expectEqual(u.BidiClass.an, u.bidiClass(0x0665)); // Arabic-Indic digit 5
    try testing.expectEqual(u.BidiClass.en, u.bidiClass('5'));
    try testing.expectEqual(u.BidiClass.nsm, u.bidiClass(0x064E)); // Fatha

    try testing.expectEqual(@as(u32, ')'), u.bidiMirror('('));
    try testing.expectEqual(@as(u32, '['), u.bidiMirror(']'));
    try testing.expectEqual(@as(u32, 'A'), u.bidiMirror('A'));

    try testing.expectEqual(u.JoinType.dual_joining, u.arabicJoinType(0x0628)); // Beh
    try testing.expectEqual(u.JoinType.dual_joining, u.arabicJoinType(0x067E)); // Persian Peh
    try testing.expectEqual(u.JoinType.dual_joining, u.arabicJoinType(0x06AF)); // Persian Gaf
    try testing.expectEqual(u.JoinType.right_joining, u.arabicJoinType(0x0627)); // Alef
    try testing.expectEqual(u.JoinType.right_joining, u.arabicJoinType(0x0698)); // Persian Jeh
    try testing.expectEqual(u.JoinType.transparent, u.arabicJoinType(0x064E));

    try testing.expectEqual(@as(u32, 0xFEFB), u.arabicLamAlefForm(0x0627, false).?);
    try testing.expectEqual(@as(u32, 0xFEFC), u.arabicLamAlefForm(0x0627, true).?);
    try testing.expectEqual(@as(u32, 0xFEF7), u.arabicLamAlefForm(0x0623, false).?);

    try testing.expectEqual(u.IndicRole.consonant, u.indicRole(0x0915)); // Devanagari Ka
    try testing.expectEqual(u.IndicRole.matra_pre, u.indicRole(0x093F)); // Devanagari Matra I
    try testing.expectEqual(u.IndicRole.virama, u.indicRole(0x094D));
    try testing.expectEqual(@as(u32, 0x09C7), u.splitIndicMatra(0x09CB).?.pre); // Bengali O
    try testing.expectEqual(@as(u32, 0x09BE), u.splitIndicMatra(0x09CB).?.post);

    try testing.expectEqual(u.ThaiRole.leading_vowel, u.thaiRole(0x0E40)); // Sara E
    try testing.expectEqual(u.ThaiRole.vowel_above, u.thaiRole(0x0E35));
    try testing.expectEqual(u.ThaiRole.tone_mark, u.thaiRole(0x0E48));
    try testing.expectEqual(u.ThaiRole.sara_am, u.thaiRole(0x0E33));
}

test "uax9 P2-P3 base direction and isolate skipping" {
    var items: [16]bidi.BidiItem = undefined;
    var n: usize = 0;

    // Leading strong Hebrew determines RTL level 1.
    const heb = [_]u32{ '(', 0x05E9, 0x05DC, 0x05D5, 0x05DD, ')' };
    try testing.expectEqual(@as(u8, 1), runBidiSequence(&heb, .auto, &items, &n));
    // Reordered visual order reverses Hebrew and mirrors paired parentheses back to ( ... )?
    // Wait: N0 gives enclosing '(' and ')' RTL level 1, so L4 mirrors '('->')' & ')'->'(',
    // then L2 reverses the entire level-1 run, putting closing-mirrored '(' on left and ')' on right!
    try testing.expectEqual(@as(u32, '('), items[0].cp);
    try testing.expectEqual(@as(u32, 0x05DD), items[1].cp);
    try testing.expectEqual(@as(u32, 0x05E9), items[4].cp);
    try testing.expectEqual(@as(u32, ')'), items[5].cp);

    // P2 skips strong RTL inside an LRI..PDI isolate before finding strong LTR 'A'.
    const iso = [_]u32{ 0x2067, 0x05D0, 0x2069, ' ', 'A', 'B' };
    try testing.expectEqual(@as(u8, 0), runBidiSequence(&iso, .auto, &items, &n));
}

test "uax9 mixed Arabic + digits + English stays readable under RTL and LTR" {
    var items: [32]bidi.BidiItem = undefined;
    var n: usize = 0;

    // Arabic base with embedded European digits -> W2 turns EN after AL into AN (level 2, even -> LTR digits).
    const ar_num = [_]u32{ 0x0645, 0x0631, ' ', '1', '2', '3', ' ', 0x0628 };
    const lvl = runBidiSequence(&ar_num, .auto, &items, &n);
    try testing.expectEqual(@as(u8, 1), lvl);
    // Visual left-to-right: Beh, ' ', '1', '2', '3', ' ', Reh, Meem
    try testing.expectEqual(@as(u32, 0x0628), items[0].cp);
    try testing.expectEqual(@as(u32, '1'), items[2].cp);
    try testing.expectEqual(@as(u32, '2'), items[3].cp);
    try testing.expectEqual(@as(u32, '3'), items[4].cp);
    try testing.expectEqual(@as(u32, 0x0631), items[6].cp);
    try testing.expectEqual(@as(u32, 0x0645), items[7].cp);

    // Mixed LTR paragraph with Arabic word + LTR continuation.
    const mix = [_]u32{ 'H', 'i', ' ', 0x0633, 0x0644, ' ', '!' };
    try testing.expectEqual(@as(u8, 0), runBidiSequence(&mix, .ltr, &items, &n));
    try testing.expectEqual(@as(u32, 'H'), items[0].cp);
    try testing.expectEqual(@as(u32, 0x0644), items[3].cp);
    try testing.expectEqual(@as(u32, 0x0633), items[4].cp);
    try testing.expectEqual(@as(u32, '!'), items[6].cp);
}

test "uax9 L2 preserves atomic cluster ordering for RTL base + combining mark" {
    // Cluster id groups base 0x05D1 + dagesh 0x05BC together inside RTL reversal.
    var vis = [_]TestVis{
        .{ .cp = 0x05D0, .level = 1, .orig_cls = .r, .cluster_id = 1 },
        .{ .cp = 0x05D1, .level = 1, .orig_cls = .r, .cluster_id = 2 },
        .{ .cp = 0x05BC, .level = 1, .orig_cls = .nsm, .cluster_id = 2 },
    };
    bidi.reorderVisualLine(TestVis, &vis, 1);
    // Visual order: cluster 2 (0x05D1 then 0x05BC) followed by cluster 1 (0x05D0).
    try testing.expectEqual(@as(i32, 0x05D1), vis[0].cp);
    try testing.expectEqual(@as(i32, 0x05BC), vis[1].cp);
    try testing.expectEqual(@as(i32, 0x05D0), vis[2].cp);
}

test "uax9 L1 resets trailing whitespace to paragraph level and L4 mirrors RTL brackets" {
    var items: [16]bidi.BidiItem = undefined;
    var n: usize = 0;

    // LTR base containing Hebrew + trailing space: trailing space reverts to level 0 (stays on right).
    const line = [_]u32{ 'a', ' ', 0x05D0, 0x05D1, ' ' };
    try testing.expectEqual(@as(u8, 0), runBidiSequence(&line, .ltr, &items, &n));
    try testing.expectEqual(@as(u32, 'a'), items[0].cp);
    try testing.expectEqual(@as(u32, 0x05D1), items[2].cp);
    try testing.expectEqual(@as(u32, 0x05D0), items[3].cp);
    try testing.expectEqual(@as(u32, ' '), items[4].cp);
    try testing.expectEqual(@as(u8, 0), items[4].level);
}

test "css direction property and html dir attribute populate token metadata" {
    var st = std.mem.zeroes(css.CssStyle);
    const prop = "direction: rtl; text-align: left";
    _ = css.ra8_css_parse_inline(prop.ptr, prop.len, &st);
    try testing.expectEqual(@as(u8, 2), st.pad);

    var eng: ReflowEngine = std.mem.zeroes(ReflowEngine);
    eng.in_use = 1;
    eng.viewport_w = 240;
    eng.viewport_h = 320;
    eng.font_px = 18;
    const html = "<p dir=\"rtl\">Hi <span dir=\"ltr\">C++</span> \xd9\x85</p>";
    const rc = tok.priv_reflow_xml_walk(&eng, html.ptr, html.len);
    try testing.expectEqual(@as(c_int, 0), rc);
    try testing.expect(eng.token_count >= 4);
    // BLOCK_START stores block dir (rtl = 2) in bits 8..9 of reserved16.
    try testing.expectEqual(@as(u16, 2), (eng.tokens[0].reserved16 >> 8) & 0x03);
}
