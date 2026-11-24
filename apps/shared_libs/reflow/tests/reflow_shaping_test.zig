// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Comprehensive shaping, OpenType GSUB/GPOS, mark positioning, multi-face
//! fallback, and golden-pixel rasterisation tests (`src/reflow_opentype.zig`,
//! `src/reflow_shape.zig`, `src/reflow_shape_layout.zig`, `src/reflow_render.zig`).

const std = @import("std");
const reflow = @import("reflow");
const sh = reflow.shape;
const shl = reflow.shape_layout;
const ot = reflow.opentype;
const drv = reflow.layout_driver;
const rnd = reflow.render;
const link = reflow.link;
const font_builder = @import("reflow_shaping_test_font.zig");
const seam = @import("reflow_render_test_seam.zig");

const testing = std.testing;
const ReflowEngine = reflow.ReflowEngine;
const StbttFontinfo = sh.StbttFontinfo;

extern fn stbtt_InitFont(info: *StbttFontinfo, data: [*]const u8, data_size: c_int, offset: c_int) callconv(.c) c_int;

fn initTestFont(blob: []const u8) !StbttFontinfo {
    var info: StbttFontinfo = std.mem.zeroes(StbttFontinfo);
    if (stbtt_InitFont(&info, blob.ptr, @intCast(blob.len), 0) == 0) return error.InvalidTestFont;
    return info;
}

fn makeRaw(cps: []const u32, px: u16) [64]sh.RawChar {
    var buf: [64]sh.RawChar = undefined;
    for (cps, 0..) |cp, i| {
        buf[i] = .{ .cp = cp, .font_px = px, .color = 0x000000 };
    }
    return buf;
}

const PixelBounds = struct {
    count: usize = 0,
    min_x: i32 = std.math.maxInt(i32),
    max_x: i32 = std.math.minInt(i32),
    min_y: i32 = std.math.maxInt(i32),
    max_y: i32 = std.math.minInt(i32),
};

fn collectPixelBounds(y_lo: i32, y_hi: i32, x_lo: i32, x_hi: i32) PixelBounds {
    var b = PixelBounds{};
    for (seam.calls()) |c| {
        if (c.kind != 4) continue;
        // White background pixels shouldn't exist in ra8_gfx_pixel glyph writes, only foreground (black/ink).
        if (c.color == 0xFFFFFF) continue;
        if (c.b < y_lo or c.b > y_hi or c.a < x_lo or c.a > x_hi) continue;
        b.count += 1;
        b.min_x = @min(b.min_x, c.a);
        b.max_x = @max(b.max_x, c.a);
        b.min_y = @min(b.min_y, c.b);
        b.max_y = @max(b.max_y, c.b);
    }
    return b;
}

test "arabic presentation forms fallback: joining, ZWNJ/ZWJ, diacritics, and Lam-Alef" {
    const glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 }, // GID 0: .notdef
        .{ .cp = ' ', .adv = 300, .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0 },
        .{ .cp = 0x0628 }, // Beh base
        .{ .cp = 0xFE91 }, // Beh initial
        .{ .cp = 0xFE92 }, // Beh medial
        .{ .cp = 0xFE90 }, // Beh final
        .{ .cp = 0x0644 }, // Lam base
        .{ .cp = 0xFEDF }, // Lam initial
        .{ .cp = 0xFEE0 }, // Lam medial
        .{ .cp = 0x0627 }, // Alef base
        .{ .cp = 0xFE8E }, // Alef final
        .{ .cp = 0xFEFB }, // Lam-Alef isolated
        .{ .cp = 0xFEFC }, // Lam-Alef final
        .{ .cp = 0x064E, .adv = 0, .x0 = 100, .y0 = 700, .x1 = 350, .y1 = 800, .gdef_class = 3 }, // Fatha
        // Persian letters
        .{ .cp = 0x067E }, // Peh
        .{ .cp = 0xFB58 }, // Peh initial
        .{ .cp = 0x06CC }, // Farsi Yeh
        .{ .cp = 0xFBFD }, // Farsi Yeh final
    };
    const blob = try font_builder.buildTestFont(testing.allocator, &glyphs, &.{}, &.{}, &.{}, &.{});
    defer testing.allocator.free(blob);
    const fnt = try initTestFont(blob);

    // 1) Word Beh-Beh-Beh -> initial, medial, final (and visual RTL reverses final..initial).
    var para_lvl: u8 = 0;
    const w3 = makeRaw(&.{ 0x0628, 0x0628, 0x0628 }, 18);
    const out3 = sh.shapeLogicalSequence(w3[0..3], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(u8, 1), para_lvl);
    try testing.expectEqual(@as(usize, 3), out3.len);
    try testing.expectEqual(@as(i32, 0xFE91), out3[0].cp);
    try testing.expectEqual(@as(i32, 0xFE92), out3[1].cp);
    try testing.expectEqual(@as(i32, 0xFE90), out3[2].cp);

    // 2) ZWNJ breaks joining between Beh and Beh (first Beh stays isolated 0x0628, second initial 0xFE91).
    const zwnj = makeRaw(&.{ 0x0628, 0x200C, 0x0628, 0x0628 }, 18);
    const out_z = sh.shapeLogicalSequence(zwnj[0..4], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 3), out_z.len); // ZWNJ stripped after joining
    try testing.expectEqual(@as(i32, 0x0628), out_z[0].cp);
    try testing.expectEqual(@as(i32, 0xFE91), out_z[1].cp);
    try testing.expectEqual(@as(i32, 0xFE90), out_z[2].cp);

    // 3) Lam + Fatha + Alef -> mandatory Lam-Alef isolated + transparent diacritic retained!
    const la_d = makeRaw(&.{ 0x0644, 0x064E, 0x0627 }, 18);
    const out_la = sh.shapeLogicalSequence(la_d[0..3], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 2), out_la.len);
    try testing.expectEqual(@as(i32, 0xFEFB), out_la[0].cp);
    try testing.expectEqual(@as(i32, 0x064E), out_la[1].cp);
    try testing.expect(out_la[1].is_mark);

    // 4) Beh + Lam + Alef -> connected final Lam-Alef (0xFEFC)!
    const bla = makeRaw(&.{ 0x0628, 0x0644, 0x0627 }, 18);
    const out_bla = sh.shapeLogicalSequence(bla[0..3], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 2), out_bla.len);
    try testing.expectEqual(@as(i32, 0xFE91), out_bla[0].cp);
    try testing.expectEqual(@as(i32, 0xFEFC), out_bla[1].cp);

    // 5) Persian Peh + Farsi Yeh
    const per = makeRaw(&.{ 0x067E, 0x06CC }, 18);
    const out_per = sh.shapeLogicalSequence(per[0..2], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 2), out_per.len);
    try testing.expectEqual(@as(i32, 0xFB58), out_per[0].cp);
    try testing.expectEqual(@as(i32, 0xFBFD), out_per[1].cp);
}

test "opentype GSUB contextual arabic init/fina + rlig IgnoreMarks ligature + GPOS mkmk" {
    // GIDs:
    // 0: .notdef, 1: space, 2: Beh(0628), 3: Lam(0644), 4: Alef(0627), 5: Fatha(064E, mark),
    // 6: Shadda(0651, mark), 7: beh.init(unmapped), 8: lam_alef.lig(unmapped)
    const glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = ' ', .adv = 300, .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0 },
        .{ .cp = 0x0628, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 500 },
        .{ .cp = 0x0644, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 700 },
        .{ .cp = 0x0627, .adv = 400, .x0 = 100, .y0 = 0, .x1 = 300, .y1 = 700 },
        .{ .cp = 0x064E, .adv = 0, .x0 = 0, .y0 = 0, .x1 = 200, .y1 = 100, .gdef_class = 3 },
        .{ .cp = 0x0651, .adv = 0, .x0 = 0, .y0 = 0, .x1 = 200, .y1 = 100, .gdef_class = 3 },
        .{ .cp = 0, .adv = 480, .x0 = 40, .y0 = 0, .x1 = 440, .y1 = 500 }, // 7: beh.init
        .{ .cp = 0, .adv = 800, .x0 = 60, .y0 = 0, .x1 = 760, .y1 = 700, .gdef_class = 2 }, // 8: lam_alef
    };
    const singles = [_]font_builder.GsubSingleSpec{
        .{ .script = "arab".*, .feat = "init".*, .from_gid = 2, .to_gid = 7 },
    };
    const ligs = [_]font_builder.GsubLigSpec{
        .{ .script = "arab".*, .feat = "rlig".*, .ignore_marks = true, .comp0 = 3, .comp1 = 4, .lig_gid = 8 },
    };
    const gpos = [_]font_builder.GposMarkSpec{
        // GPOS Type 4: Fatha (gid 5) attaches above Beh (gid 7) at (240, 620) vs mark anchor (100, 20) => dx=+140, dy=+600
        .{ .script = "arab".*, .mark_to_mark = false, .base_gid = 7, .base_ax = 240, .base_ay = 620, .mark_gid = 5, .mark_ax = 100, .mark_ay = 20 },
        // GPOS Type 6: Shadda (gid 6) stacks above Fatha (gid 5) at (100, 150) vs (100, 20) => mkmk dx=0, dy=+130
        .{ .script = "arab".*, .mark_to_mark = true, .base_gid = 5, .base_ax = 100, .base_ay = 150, .mark_gid = 6, .mark_ax = 100, .mark_ay = 20 },
    };

    const blob = try font_builder.buildTestFont(testing.allocator, &glyphs, &singles, &ligs, &.{}, &gpos);
    defer testing.allocator.free(blob);
    const fnt = try initTestFont(blob);

    // Beh + Fatha + Shadda + Alef (Beh -> GID 7 via GSUB init, Fatha/Shadda GPOS mark + mkmk!)
    var para_lvl: u8 = 0;
    const raw1 = makeRaw(&.{ 0x0628, 0x064E, 0x0651, 0x0627 }, 20);
    const out1 = sh.shapeLogicalSequence(raw1[0..4], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 4), out1.len);
    try testing.expectEqual(sh.encodeShapedGid(7), out1[0].cp);
    try testing.expect(out1[1].is_mark and out1[2].is_mark);
    // At 20px (scale = 0.02): Fatha dy = -round(600 * 0.02) = -12, Shadda dy = -12 - round(130 * 0.02) = -15
    try testing.expectEqual(@as(i32, 3), out1[1].dx);
    try testing.expectEqual(@as(i32, -12), out1[1].dy);
    try testing.expectEqual(@as(i32, 3), out1[2].dx);
    try testing.expectEqual(@as(i32, -15), out1[2].dy);

    // Lam + Fatha + Alef -> GSUB rlig IgnoreMarks collapses to GID 8 with Fatha retained on cluster!
    const raw2 = makeRaw(&.{ 0x0644, 0x064E, 0x0627 }, 20);
    const out2 = sh.shapeLogicalSequence(raw2[0..3], .auto, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 2), out2.len);
    try testing.expectEqual(sh.encodeShapedGid(8), out2[0].cp);
    try testing.expectEqual(@as(u32, 0x064E), out2[1].orig_cp);
    try testing.expectEqual(out2[0].cluster_id, out2[1].cluster_id);
}

test "indic shaping: pre-base matra, adjacent syllable isolation, GSUB akhn/rphf, bengali split matra" {
    // GIDs:
    // 0:.notdef, 1:' ', 2:Ka(0915), 3:Ssa(0937), 4:Virama(094D), 5:MatraI(093F), 6:Ra(0930),
    // 7:kssa.akhn(unmapped), 8:repha.rphf(unmapped, mark),
    // 9:Bengali Ka(0995), 10:Bengali O(09CB), 11:Bengali E(09C7), 12:Bengali Aa(09BE)
    const glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = ' ', .adv = 300, .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0 },
        .{ .cp = 0x0915, .adv = 600 },
        .{ .cp = 0x0937, .adv = 600 },
        .{ .cp = 0x094D, .adv = 0, .x0 = 0, .y0 = 0, .x1 = 100, .y1 = 100, .gdef_class = 3 },
        .{ .cp = 0x093F, .adv = 350 },
        .{ .cp = 0x0930, .adv = 500 },
        .{ .cp = 0, .adv = 800, .gdef_class = 2 }, // 7: Kssa conjunct
        .{ .cp = 0, .adv = 0, .x0 = 100, .y0 = 700, .x1 = 300, .y1 = 820, .gdef_class = 3 }, // 8: Repha
        .{ .cp = 0x0995, .adv = 600 },
        .{ .cp = 0x09CB, .adv = 600 },
        .{ .cp = 0x09C7, .adv = 350 },
        .{ .cp = 0x09BE, .adv = 350 },
    };
    const ligs = [_]font_builder.GsubLigSpec{
        .{ .script = "deva".*, .feat = "akhn".*, .ignore_marks = false, .comp0 = 2, .comp1 = 4, .comp2 = 3, .lig_gid = 7 },
        .{ .script = "deva".*, .feat = "rphf".*, .ignore_marks = false, .comp0 = 6, .comp1 = 4, .lig_gid = 8 },
    };
    const blob = try font_builder.buildTestFont(testing.allocator, &glyphs, &.{}, &ligs, &.{}, &.{});
    defer testing.allocator.free(blob);
    const fnt = try initTestFont(blob);

    var para_lvl: u8 = 0;

    // 1) Adjacent syllables: "Ka" followed by "Ka + MatraI" must NOT leak MatraI into the first syllable!
    const two = makeRaw(&.{ 0x0915, ' ', 0x0915, 0x093F }, 18);
    const out_two = sh.shapeLogicalSequence(two[0..4], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 4), out_two.len);
    try testing.expectEqual(@as(i32, 0x0915), out_two[0].cp);
    try testing.expectEqual(@as(i32, ' '), out_two[1].cp);
    try testing.expectEqual(@as(i32, 0x093F), out_two[2].cp);
    try testing.expectEqual(@as(i32, 0x0915), out_two[3].cp);

    // 2) Conjunct Ka+Virama+Ssa + MatraI -> GSUB akhn forms Kssa + MatraI reorders left -> [MatraI, Kssa]
    const kssa = makeRaw(&.{ 0x0915, 0x094D, 0x0937, 0x093F }, 18);
    const out_kssa = sh.shapeLogicalSequence(kssa[0..4], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 2), out_kssa.len);
    try testing.expectEqual(@as(i32, 0x093F), out_kssa[0].cp);
    try testing.expectEqual(sh.encodeShapedGid(7), out_kssa[1].cp);

    // 3) Repha + Ka + MatraI -> GSUB rphf forms Repha moved right + MatraI moved left -> [MatraI, Ka, Repha]
    const rki = makeRaw(&.{ 0x0930, 0x094D, 0x0915, 0x093F }, 18);
    const out_rki = sh.shapeLogicalSequence(rki[0..4], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 3), out_rki.len);
    try testing.expectEqual(@as(i32, 0x093F), out_rki[0].cp);
    try testing.expectEqual(@as(i32, 0x0915), out_rki[1].cp);
    try testing.expectEqual(sh.encodeShapedGid(8), out_rki[2].cp);
    try testing.expect(out_rki[2].is_mark);

    // 4) Bengali split matra Ko (0995 + 09CB) -> [09C7, 0995, 09BE]
    const ko = makeRaw(&.{ 0x0995, 0x09CB }, 18);
    const out_ko = sh.shapeLogicalSequence(ko[0..2], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 3), out_ko.len);
    try testing.expectEqual(@as(i32, 0x09C7), out_ko[0].cp);
    try testing.expectEqual(@as(i32, 0x0995), out_ko[1].cp);
    try testing.expectEqual(@as(i32, 0x09BE), out_ko[2].cp);
}

test "thai syllable clustering, Sara Am decomposition, tone stacking, and multi-face fallback" {
    const glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = ' ', .adv = 300, .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0 },
        .{ .cp = 0x0E01, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 500 }, // Ko Kai
        .{ .cp = 0x0E40, .adv = 300, .x0 = 50, .y0 = 0, .x1 = 250, .y1 = 600 }, // Sara E
        .{ .cp = 0x0E49, .adv = 0, .x0 = 100, .y0 = 700, .x1 = 300, .y1 = 800, .gdef_class = 3 }, // Mai Tho
        .{ .cp = 0x0E33, .adv = 500 }, // Sara Am
        .{ .cp = 0x0E4D, .adv = 0, .x0 = 120, .y0 = 600, .x1 = 260, .y1 = 680, .gdef_class = 3 }, // Nikhahit
        .{ .cp = 0x0E32, .adv = 400 }, // Sara Aa
    };
    const blob = try font_builder.buildTestFont(testing.allocator, &glyphs, &.{}, &.{}, &.{}, &.{});
    defer testing.allocator.free(blob);
    const fnt = try initTestFont(blob);

    // Syllables get distinct wrap_ids; Ko + Mai Tho + Sara Am expands to [Ko, Nikhahit(mark), Mai Tho(mark), Sara Aa]
    var para_lvl: u8 = 0;
    const th = makeRaw(&.{ 0x0E40, 0x0E01, 0x0E49, 0x0E01, 0x0E49, 0x0E33 }, 18);
    const out = sh.shapeLogicalSequence(th[0..6], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 7), out.len);
    // First syllable wrap_id == second syllable's wrap_id? NO, they differ!
    try testing.expectEqual(out[0].wrap_id, out[1].wrap_id);
    try testing.expectEqual(out[1].wrap_id, out[2].wrap_id);
    try testing.expect(out[3].wrap_id != out[0].wrap_id);
    // Sara Am decomposition places Nikhahit (0E4D) before Mai Tho (0E49), followed by Sara Aa (0E32).
    try testing.expectEqual(@as(i32, 0x0E4D), out[4].cp);
    try testing.expect(out[4].is_mark);
    try testing.expectEqual(@as(i32, 0x0E49), out[5].cp);
    try testing.expect(out[5].is_mark);
    try testing.expectEqual(@as(i32, 0x0E32), out[6].cp);
    try testing.expect(!out[6].is_mark);
    // Tone mark (out[5]) stacks strictly above Nikhahit (out[4]) -> more negative y delta!
    try testing.expect(out[5].dy < out[4].dy);

    // Multi-face fallback: face 0 has Latin 'A', face 1 has Arabic Beh (0628).
    const lat_glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = 'A', .adv = 600 },
    };
    const lat_blob = try font_builder.buildTestFont(testing.allocator, &lat_glyphs, &.{}, &.{}, &.{}, &.{});
    defer testing.allocator.free(lat_blob);
    const lat_fnt = try initTestFont(lat_blob);

    const ar_glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = 0x0628, .adv = 500 },
        .{ .cp = 0xFE91, .adv = 500 },
        .{ .cp = 0xFE90, .adv = 500 },
    };
    const ar_blob = try font_builder.buildTestFont(testing.allocator, &ar_glyphs, &.{}, &.{}, &.{}, &.{});
    defer testing.allocator.free(ar_blob);
    const ar_fnt = try initTestFont(ar_blob);

    const faces = [_]sh.FaceEntry{
        .{ .font = &lat_fnt, .data = lat_blob },
        .{ .font = &ar_fnt, .data = ar_blob },
    };
    const mix = makeRaw(&.{ 'A', ' ', 0x0628, 0x0628 }, 18);
    const mout = sh.shapeMultiFaceSequence(mix[0..4], .ltr, &faces, &para_lvl);
    try testing.expectEqual(@as(usize, 4), mout.len);
    try testing.expectEqual(@as(u8, 0), mout[0].face_slot);
    try testing.expectEqual(@as(u8, 1), mout[2].face_slot);
    try testing.expectEqual(@as(i32, 0xFE91), mout[2].cp);
    try testing.expectEqual(@as(i32, 0xFE90), mout[3].cp);
}

test "golden pixel rendering: combining mark stacking, Arabic RTL & Lam-Alef, Devanagari ki, RTL link hitbox" {
    // Distinct bbox sizes allow unambiguous pixel-level positional assertions via ra8_gfx_pixel seam:
    // - 'e': [100, 0, 700, 400] (baseline bottomHalf)
    // - 0301 acute: [200, 0, 400, 150] (mark above, positioned by metric bbox fallback)
    // - 0300 grave: [200, 0, 400, 150] (stacks strictly above 0301)
    // - 0628 Beh / FE91 init / FE90 final, 0644 Lam, 0627 Alef, FEFB Lam-Alef
    // - 0915 Ka [100, 0, 700, 500], 093F MatraI [50, 0, 250, 700]
    const glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = ' ', .adv = 300, .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0 },
        .{ .cp = 'e', .adv = 700, .x0 = 100, .y0 = 0, .x1 = 600, .y1 = 350 },
        .{ .cp = 0x0301, .adv = 0, .x0 = 0, .y0 = 0, .x1 = 200, .y1 = 150, .gdef_class = 3 },
        .{ .cp = 0x0300, .adv = 0, .x0 = 0, .y0 = 0, .x1 = 200, .y1 = 150, .gdef_class = 3 },
        .{ .cp = 0x0628, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 300 },
        .{ .cp = 0xFE91, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 300 },
        .{ .cp = 0xFE90, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 300 },
        .{ .cp = 0x0644, .adv = 500, .x0 = 50, .y0 = 0, .x1 = 450, .y1 = 500 },
        .{ .cp = 0x0627, .adv = 400, .x0 = 100, .y0 = 0, .x1 = 300, .y1 = 500 },
        .{ .cp = 0xFEFB, .adv = 700, .x0 = 100, .y0 = 0, .x1 = 600, .y1 = 500 },
        .{ .cp = 0xFEFC, .adv = 700, .x0 = 100, .y0 = 0, .x1 = 600, .y1 = 500 },
        .{ .cp = 0x0915, .adv = 700, .x0 = 100, .y0 = 0, .x1 = 600, .y1 = 350 },
        .{ .cp = 0x093F, .adv = 400, .x0 = 50, .y0 = 0, .x1 = 250, .y1 = 500 },
    };
    const blob = try font_builder.buildTestFont(testing.allocator, &glyphs, &.{}, &.{}, &.{}, &.{});
    defer testing.allocator.free(blob);

    const eng = try testing.allocator.create(ReflowEngine);
    defer testing.allocator.destroy(eng);
    eng.* = std.mem.zeroes(ReflowEngine);

    // 1) Stacked combining marks on 'e' at 12px: must keep mark 1 above base and mark 2 above mark 1!
    try testing.expectEqual(@as(c_int, 0), drv.reflow_init(120, 60, blob.ptr, blob.len, 12, 0x000000, 0x0000FF, eng));
    defer _ = drv.reflow_close(eng);
    var pages: u32 = 0;
    const xhtml_marks = "<p>e\xcc\x81\xcc\x80</p>";
    try testing.expectEqual(@as(c_int, 0), drv.reflow_layout_chapter(eng, xhtml_marks.ptr, xhtml_marks.len, &pages));
    try testing.expectEqual(@as(u32, 1), pages);
    try testing.expectEqual(@as(u32, 3), eng.glyph_count);
    try testing.expectEqual(@as(i32, 'e'), eng.glyphs[0].cp);
    // Both marks sit horizontally within base advance and stack strictly higher (more negative dy/y).
    try testing.expect(eng.glyphs[1].y < eng.glyphs[0].y);
    try testing.expect(eng.glyphs[2].y < eng.glyphs[1].y);

    seam.shapeCallsReset();
    try testing.expectEqual(@as(c_int, 0), rnd.reflow_render_page(eng, 0, null));
    // Verify rendered rasterized pixels actually sit in 3 vertically separated bands inside 120x60!
    const all_px = collectPixelBounds(0, 60, 0, 120);
    try testing.expect(all_px.count >= 15);
    // Top pixels come from marks (y strictly smaller than base bottom), and horizontal span sits near left margin (16).
    try testing.expect(all_px.min_x >= 16 and all_px.max_x <= 30);

    // 2) Devanagari Pre-base Matra I (<p>&#x0915;&#x093F;</p>): MatraI glyph renders to LEFT of Ka!
    const xhtml_deva = "<p>\xe0\xa4\x95\xe0\xa4\xbf</p>";
    try testing.expectEqual(@as(c_int, 0), drv.reflow_layout_chapter(eng, xhtml_deva.ptr, xhtml_deva.len, &pages));
    try testing.expectEqual(@as(u32, 2), eng.glyph_count);
    try testing.expectEqual(@as(i32, 0x093F), eng.glyphs[0].cp);
    try testing.expectEqual(@as(i32, 0x0915), eng.glyphs[1].cp);
    try testing.expect(eng.glyphs[0].x < eng.glyphs[1].x);

    seam.shapeCallsReset();
    try testing.expectEqual(@as(c_int, 0), rnd.reflow_render_page(eng, 0, null));
    const left_band = collectPixelBounds(0, 60, 16, eng.glyphs[1].x);
    const right_band = collectPixelBounds(0, 60, eng.glyphs[1].x, 120);
    try testing.expect(left_band.count >= 4 and right_band.count >= 4);

    // 3) RTL Arabic Lam-Alef + ZWNJ + Beh with link hitbox on right-aligned RTL line!
    // Text: <p dir="rtl"><a href="chap2.xhtml">&#x0644;&#x0627; </a>&#x0628;</p>
    const xhtml_rtl = "<p dir=\"rtl\"><a href=\"chap2.xhtml\">\xd9\x84\xd8\xa7 </a>\xd8\xa8</p>";
    try testing.expectEqual(@as(c_int, 0), drv.reflow_layout_chapter(eng, xhtml_rtl.ptr, xhtml_rtl.len, &pages));
    // Visual order (L-to-R): isolated Beh (0628) on left, ' ', Lam-Alef ligature (FEFB) rightmost!
    try testing.expectEqual(@as(u32, 3), eng.glyph_count);
    try testing.expectEqual(@as(i32, 0x0628), eng.glyphs[0].cp);
    try testing.expectEqual(@as(i32, 0xFEFB), eng.glyphs[2].cp);
    try testing.expect(eng.glyphs[0].x < eng.glyphs[2].x);
    // RTL block auto-right-aligns against right margin (120 - 16 = 104): rightmost glyph sits near x=90..104.
    try testing.expect(eng.glyphs[2].x > 70);
    // Link hit-test hits the Lam-Alef link on the right half of the line.
    const lx = eng.glyphs[2].x + 2;
    const ly = eng.glyphs[2].y + 4;
    var href_off: u32 = 0;
    var href_len: u32 = 0;
    try testing.expectEqual(@as(c_int, 0), link.reflow_hit_test_link(eng, 0, lx, ly, &href_off, &href_len));
    try testing.expectEqual(@as(u32, 11), href_len); // "chap2.xhtml"

    // 4) ZWNJ + RLM bidi format chars emit zero glyph slots / no tofu!
    const xhtml_ctrl = "<p dir=\"rtl\">\xd8\xa8\xe2\x80\x8c\xd8\xa8\xe2\x80\x8f</p>";
    try testing.expectEqual(@as(c_int, 0), drv.reflow_layout_chapter(eng, xhtml_ctrl.ptr, xhtml_ctrl.len, &pages));
    try testing.expectEqual(@as(u32, 2), eng.glyph_count);
    try testing.expectEqual(@as(i32, 0x0628), eng.glyphs[0].cp);
    try testing.expectEqual(@as(i32, 0x0628), eng.glyphs[1].cp);

    // 5) CSS direction: rtl overrides left block default + RTL table cell right-aligns and shapes.
    const xhtml_tbl =
        \\<p style="direction: rtl">&#x0628;&#x0644;&#x0627;</p>
        \\<table><tr><td>&#x0644;&#x0627; &#x0628;</td></tr></table>
    ;
    try testing.expectEqual(@as(c_int, 0), drv.reflow_layout_chapter(eng, xhtml_tbl.ptr, xhtml_tbl.len, &pages));
    try testing.expectEqual(@as(u32, 5), eng.glyph_count);
    try testing.expectEqual(@as(i32, 0xFEFB), eng.glyphs[4].cp); // Table cell Arabic Lam-Alef rightmost!
}

test "opentype Thai GSUB ccmp, Repha fallback without GSUB, Latin liga, and below/overlay marks" {
    // GIDs:
    // 0:.notdef, 1:' ', 2:'f', 3:'i', 4:fi.liga,
    // 5:dot_below(0323, mark), 6:stroke_overlay(0336, mark),
    // 7:Ka(0915), 8:Ra(0930), 9:Virama(094D), 10:MatraI(093F),
    // 11:Ko(0E01), 12:SaraAm(0E33), 13:Nikhahit(0E4D, mark), 14:SaraAa(0E32)
    const glyphs = [_]font_builder.GlyphDef{
        .{ .cp = 0 },
        .{ .cp = ' ', .adv = 300, .x0 = 0, .y0 = 0, .x1 = 0, .y1 = 0 },
        .{ .cp = 'f', .adv = 400, .x0 = 50, .y0 = 0, .x1 = 350, .y1 = 600 },
        .{ .cp = 'i', .adv = 300, .x0 = 100, .y0 = 0, .x1 = 220, .y1 = 550 },
        .{ .cp = 0, .adv = 600, .x0 = 50, .y0 = 0, .x1 = 550, .y1 = 600, .gdef_class = 2 }, // 4: fi.liga
        .{ .cp = 0x0323, .adv = 0, .x0 = 50, .y0 = -180, .x1 = 150, .y1 = -80, .gdef_class = 3 }, // 5: below
        .{ .cp = 0x0336, .adv = 0, .x0 = 50, .y0 = 200, .x1 = 350, .y1 = 280, .gdef_class = 3 }, // 6: overlay
        .{ .cp = 0x0915, .adv = 600 },
        .{ .cp = 0x0930, .adv = 500 },
        .{ .cp = 0x094D, .adv = 0, .x0 = 0, .y0 = 0, .x1 = 100, .y1 = 100, .gdef_class = 3 },
        .{ .cp = 0x093F, .adv = 350 },
        .{ .cp = 0x0E01, .adv = 500 },
        .{ .cp = 0x0E33, .adv = 500 },
        .{ .cp = 0x0E4D, .adv = 0, .x0 = 100, .y0 = 600, .x1 = 220, .y1 = 680, .gdef_class = 3 },
        .{ .cp = 0x0E32, .adv = 400 },
    };
    const ligs = [_]font_builder.GsubLigSpec{
        .{ .script = "latn".*, .feat = "liga".*, .ignore_marks = true, .comp0 = 2, .comp1 = 3, .lig_gid = 4 },
    };
    const multis = [_]font_builder.GsubMultiSpec{
        .{ .script = "thai".*, .feat = "ccmp".*, .from_gid = 12, .out0 = 13, .out1 = 14 },
    };
    const blob = try font_builder.buildTestFont(testing.allocator, &glyphs, &.{}, &ligs, &multis, &.{});
    defer testing.allocator.free(blob);
    const fnt = try initTestFont(blob);

    var para_lvl: u8 = 0;

    // 1) Latin "fi" -> GSUB liga collapses to GID 4, while below & overlay marks attach at expected Y offsets!
    const lat = makeRaw(&.{ 'f', 0x0323, 0x0336, 'f', 'i' }, 20);
    const lout = sh.shapeLogicalSequence(lat[0..5], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 4), lout.len);
    try testing.expect(lout[1].dy >= 0); // dot_below stays below baseline (rendered box y0 > 0)
    try testing.expectEqual(sh.encodeShapedGid(4), lout[3].cp); // fi ligature

    // 2) Devanagari Repha fallback (no GSUB rphf): Ra+Virama+Ka+MatraI -> [MatraI, Ka, Ra(mark), Virama(mark)]
    const rph = makeRaw(&.{ 0x0930, 0x094D, 0x0915, 0x093F }, 18);
    const rout = sh.shapeLogicalSequence(rph[0..4], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 4), rout.len);
    try testing.expectEqual(@as(i32, 0x093F), rout[0].cp);
    try testing.expectEqual(@as(i32, 0x0915), rout[1].cp);
    try testing.expectEqual(@as(i32, 0x0930), rout[2].cp);
    try testing.expect(rout[2].is_mark and rout[3].is_mark);

    // 3) Thai GSUB ccmp MultipleSubst expands GID 12 (Sara Am) -> [GID 13 (Nikhahit, mark), GID 14 (Sara Aa)]
    const th = makeRaw(&.{ 0x0E01, 0x0E33 }, 18);
    const tout = sh.shapeLogicalSequence(th[0..2], .ltr, &fnt, blob, &para_lvl);
    try testing.expectEqual(@as(usize, 3), tout.len);
    try testing.expectEqual(sh.encodeShapedGid(13), tout[1].cp);
    try testing.expect(tout[1].is_mark);
    try testing.expectEqual(sh.encodeShapedGid(14), tout[2].cp);
    try testing.expect(!tout[2].is_mark);
}
