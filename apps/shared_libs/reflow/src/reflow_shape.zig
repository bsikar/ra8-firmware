//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Heap-free text shaping and UAX #9 bidirectional line layout engine
//! for `reflow` (`RA8FW-1173` / `RA8FW-296`). Sits between token decoding
//! and rasterisation: runs Arabic/Persian contextual joining + Lam-Alef
//! ligatures, Indic syllable clustering + pre-base matra/Repha reordering,
//! Thai syllable clustering + Sara-Am/tone stacking, OpenType GSUB/GPOS
//! lookups when present in the font blob, UAX #29 grapheme cluster
//! grouping, GPOS/bbox combining-mark positioning, and UAX #9 line-level
//! visual reordering.

const std = @import("std");
const face = @import("reflow_render_face.zig");
const u = @import("reflow_unicode.zig");
const bidi = @import("reflow_bidi.zig");
const ot = @import("reflow_opentype.zig");
const shape_layout = @import("reflow_shape_layout.zig");

pub const StbttFontinfo = face.StbttFontinfo;

extern fn stbtt_GetFontOffsetForIndex(data: [*]const u8, index: c_int) callconv(.c) c_int;
extern fn stbtt_FindGlyphIndex(info: *const StbttFontinfo, unicode_codepoint: c_int) callconv(.c) c_int;
extern fn stbtt_ScaleForPixelHeight(info: *const StbttFontinfo, height: f32) callconv(.c) f32;
extern fn stbtt_GetCodepointHMetrics(info: *const StbttFontinfo, codepoint: c_int, advance: *c_int, lsb: *c_int) callconv(.c) void;
extern fn stbtt_GetGlyphHMetrics(info: *const StbttFontinfo, glyph_index: c_int, advance: *c_int, lsb: *c_int) callconv(.c) void;
extern fn stbtt_GetGlyphBitmapBox(font: *const StbttFontinfo, glyph: c_int, scale_x: f32, scale_y: f32, ix0: *c_int, iy0: *c_int, ix1: *c_int, iy1: *c_int) callconv(.c) void;

pub const k_gid_flag: u32 = 0x4000_0000;
pub const k_gid_mask: u32 = 0x0000_FFFF;
pub const k_shape_cap: usize = 256;

pub const RawChar = struct {
    cp: u32 = 0,
    color: u32 = 0,
    font_px: u16 = 18,
    style: u8 = 0,
    link_id: u8 = 0,
    face_slot: u8 = 0,
};

pub const FaceEntry = struct {
    font: *const StbttFontinfo,
    data: []const u8,
};

pub const ShapedItem = struct {
    cp: i32 = 0,
    orig_cp: u32 = 0,
    color: u32 = 0,
    gid: u16 = 0,
    font_px: u16 = 18,
    advance: i16 = 0,
    dx: i16 = 0,
    dy: i16 = 0,
    cluster_id: u16 = 0,
    wrap_id: u16 = 0,
    style: u8 = 0,
    link_id: u8 = 0,
    face_slot: u8 = 0,
    bidi_cls: u.BidiClass = .l,
    orig_cls: u.BidiClass = .l,
    level: u8 = 0,
    is_space: bool = false,
    is_mark: bool = false,
    is_break: bool = false,
};

// Shared single-threaded module-static scratch keeps Cortex-M85 stack and SRAM compact.
var s_bidi_items: [k_shape_cap]bidi.BidiItem = undefined;
var s_bidi_scratch: bidi.BidiScratch = .{};
var s_shaped: [k_shape_cap]ShapedItem = undefined;
var s_gids: [16]u16 = undefined;
var s_marks: [16]bool = undefined;

pub fn decodeShapedGid(cp: i32) ?c_int {
    const raw: u32 = @bitCast(cp);
    if ((raw & k_gid_flag) != 0) {
        return @intCast(raw & k_gid_mask);
    }
    return null;
}

pub fn encodeShapedGid(gid: u16) i32 {
    return @bitCast(k_gid_flag | @as(u32, gid));
}

pub fn decodeUtf8Next(bytes: []const u8, pos: *usize) u32 {
    if (pos.* >= bytes.len) return 0;
    const b0 = bytes[pos.*];
    if (b0 < 0x80) {
        pos.* += 1;
        return b0;
    }
    var need: usize = 0;
    var cp: u32 = 0;
    var least: u32 = 0;
    if ((b0 & 0xE0) == 0xC0) {
        need = 2;
        cp = b0 & 0x1F;
        least = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 3;
        cp = b0 & 0x0F;
        least = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 4;
        cp = b0 & 0x07;
        least = 0x10000;
    } else {
        pos.* += 1;
        return 0xFFFD;
    }
    if (pos.* + need > bytes.len) {
        pos.* += 1;
        return 0xFFFD;
    }
    var k: usize = 1;
    while (k < need) : (k += 1) {
        const bk = bytes[pos.* + k];
        if ((bk & 0xC0) != 0x80) {
            pos.* += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (bk & 0x3F);
    }
    pos.* += need;
    if (cp < least or (cp >= 0xD800 and cp <= 0xDFFF) or cp > 0x10FFFF) {
        return 0xFFFD;
    }
    return cp;
}

/// Fast-path scan: returns true if a byte slice contains only printable/ASCII/Latin-1
/// bytes (< 0x0300, no Bidi controls) and paragraph base direction is not forced RTL.
pub fn needsComplexPipeline(bytes: []const u8, base_dir: bidi.BaseDir, tables: ot.Tables) bool {
    if (base_dir == .rtl) return true;
    _ = tables;
    var i: usize = 0;
    while (i < bytes.len) {
        const cp = decodeUtf8Next(bytes, &i);
        if (cp >= 0x0300) return true;
    }
    return false;
}

fn findGid(font: *const StbttFontinfo, cp: u32) u16 {
    const g = stbtt_FindGlyphIndex(font, @intCast(cp));
    if (g <= 0) return 0;
    return @intCast(@min(g, 0xFFFF));
}

fn makeItem(raw: RawChar, cp_val: i32, gid: u16, bidi_cls: u.BidiClass, orig_cls: u.BidiClass, lvl: u8, cid: u16, wid: u16) ShapedItem {
    return .{
        .cp = cp_val,
        .orig_cp = raw.cp,
        .gid = gid,
        .font_px = raw.font_px,
        .style = raw.style,
        .link_id = raw.link_id,
        .color = raw.color,
        .face_slot = raw.face_slot,
        .cluster_id = cid,
        .wrap_id = wid,
        .bidi_cls = bidi_cls,
        .orig_cls = orig_cls,
        .level = lvl,
        .is_space = raw.cp == ' ',
        .is_mark = u.isCombiningMark(raw.cp),
        .is_break = raw.cp == '\n',
    };
}

fn applyGsubFeaturesSingle(tables: ot.Tables, script_tag: ot.Tag, feat: ot.Tag, gid: u16) u16 {
    if (tables.gsub.len < 10) return gid;
    var lookups: [32]u16 = undefined;
    const n = ot.collectFeatureLookups(tables.gsub, script_tag, feat, &lookups);
    var cur = gid;
    for (lookups[0..n]) |lidx| {
        if (ot.gsubApplySingle(tables.gsub, lidx, cur)) |sub| {
            cur = sub;
        }
    }
    return cur;
}

fn applyGsubMultipleRun(out: []ShapedItem, len: *usize, idx: usize, tables: ot.Tables, script_tag: ot.Tag, feat: ot.Tag) void {
    if (tables.gsub.len < 10 or idx >= len.*) return;
    var lookups: [32]u16 = undefined;
    const n = ot.collectFeatureLookups(tables.gsub, script_tag, feat, &lookups);
    for (lookups[0..n]) |lidx| {
        if (ot.gsubApplyMultiple(tables.gsub, lidx, out[idx].gid)) |multi| {
            if (multi.len <= 1 or len.* + multi.len - 1 > out.len) continue;
            const extra = @as(usize, multi.len) - 1;
            var j = len.*;
            while (j > idx + 1) {
                j -= 1;
                out[j + extra] = out[j];
            }
            const proto = out[idx];
            var k: usize = 0;
            while (k < multi.len) : (k += 1) {
                out[idx + k] = proto;
                out[idx + k].gid = multi.gids[k];
                out[idx + k].cp = encodeShapedGid(multi.gids[k]);
                out[idx + k].is_mark = (k > 0 and proto.orig_cp == 0x0E33 and k == 1) or proto.is_mark;
            }
            // Note: for Thai 0E33 decomposition in GSUB, mark vs follow handled by GDEF class.
            if (tables.gdef.len >= 12) {
                var m: usize = 0;
                while (m < multi.len) : (m += 1) {
                    out[idx + m].is_mark = ot.gdefGlyphClass(tables.gdef, multi.gids[m]) == 3;
                }
            }
            len.* += extra;
            return;
        }
    }
}

fn applyGsubLigaturesRange(
    out: []ShapedItem,
    start: usize,
    end: *usize,
    total_len: *usize,
    tables: ot.Tables,
    script_tag: ot.Tag,
    feat: ot.Tag,
) void {
    if (tables.gsub.len < 10 or start >= end.*) return;
    var lookups: [32]u16 = undefined;
    const n = ot.collectFeatureLookups(tables.gsub, script_tag, feat, &lookups);
    if (n == 0) return;

    var i = start;
    while (i < end.*) {
        const span = end.* - i;
        if (span < 2) break;
        const win = @min(span, 16);
        var k: usize = 0;
        while (k < win) : (k += 1) {
            s_gids[k] = out[i + k].gid;
            s_marks[k] = out[i + k].is_mark;
        }
        for (lookups[0..n]) |lidx| {
            if (ot.gsubApplyLigature(tables.gsub, tables.gdef, lidx, s_gids[0..win], s_marks[0..win])) |hit| {
                const was_mark = out[i].is_mark;
                out[i].gid = hit.lig_gid;
                out[i].cp = encodeShapedGid(hit.lig_gid);
                out[i].is_mark = if (tables.gdef.len >= 12)
                    (ot.gdefGlyphClass(tables.gdef, hit.lig_gid) == 3)
                else
                    was_mark;
                // Keep only skipped marks when IgnoreMarks matched over them.
                const cid = out[i].cluster_id;
                const wid = out[i].wrap_id;
                var keep: usize = 1;
                if (hit.ignore_marks) {
                    var p: usize = 1;
                    while (p < hit.end_pos) : (p += 1) {
                        if (out[i + p].is_mark) {
                            out[i + keep] = out[i + p];
                            out[i + keep].cluster_id = cid;
                            out[i + keep].wrap_id = wid;
                            keep += 1;
                        }
                    }
                }
                const drop = hit.end_pos - keep;
                if (drop > 0) {
                    var m = i + keep;
                    while (m + drop < total_len.*) : (m += 1) {
                        out[m] = out[m + drop];
                    }
                    end.* -= drop;
                    if (end != total_len) total_len.* -= drop;
                }
                break;
            }
        }
        i += 1;
    }
}

fn nextArabicJoiner(items: []const ShapedItem, idx: usize) ?usize {
    var j = idx + 1;
    while (j < items.len) : (j += 1) {
        const jt = u.arabicJoinType(items[j].orig_cp);
        if (jt != .transparent) return j;
    }
    return null;
}

fn prevArabicJoiner(items: []const ShapedItem, idx: usize) ?usize {
    var j = idx;
    while (j > 0) {
        j -= 1;
        const jt = u.arabicJoinType(items[j].orig_cp);
        if (jt != .transparent) return j;
    }
    return null;
}

fn arabicFormFor(items: []const ShapedItem, idx: usize) u.ArabicForm {
    const jt = u.arabicJoinType(items[idx].orig_cp);
    const prev_ok = if (prevArabicJoiner(items, idx)) |pi| blk: {
        const pjt = u.arabicJoinType(items[pi].orig_cp);
        break :blk (pjt == .dual_joining or pjt == .join_causing);
    } else false;
    const next_ok = if (nextArabicJoiner(items, idx)) |ni| blk: {
        const njt = u.arabicJoinType(items[ni].orig_cp);
        break :blk (njt == .dual_joining or njt == .right_joining or njt == .join_causing);
    } else false;

    return switch (jt) {
        .right_joining => if (prev_ok) .final else .isolated,
        .dual_joining => if (prev_ok and next_ok) .medial else if (prev_ok) .final else if (next_ok) .initial else .isolated,
        else => .isolated,
    };
}

fn arabicFeatTag(form: u.ArabicForm) ot.Tag {
    return switch (form) {
        .isolated => "isol".*,
        .final => "fina".*,
        .initial => "init".*,
        .medial => "medi".*,
    };
}

fn shapeArabicSpan(out: []ShapedItem, len: *usize, font: *const StbttFontinfo, tables: ot.Tables) void {
    // 1) Resolve contextual forms (GSUB single first if present, else Unicode Presentation Forms-B).
    for (out[0..len.*], 0..) |*it, idx| {
        const jt = u.arabicJoinType(it.orig_cp);
        if (jt != .dual_joining and jt != .right_joining) continue;
        const form = arabicFormFor(out[0..len.*], idx);
        const feat = arabicFeatTag(form);
        const sub_gid = applyGsubFeaturesSingle(tables, "arab".*, feat, it.gid);
        if (sub_gid != it.gid and sub_gid != 0) {
            it.gid = sub_gid;
            it.cp = encodeShapedGid(sub_gid);
            continue;
        }
        const pcp = u.arabicPresentationForm(it.orig_cp, form);
        if (pcp != it.orig_cp) {
            const pgid = findGid(font, pcp);
            if (pgid != 0) {
                it.cp = @intCast(pcp);
                it.gid = pgid;
            }
        }
    }

    // 2) Mandatory Lam + Alef ligatures (via GSUB rlig/liga first, then Presentation Forms-B fallback).
    applyGsubLigaturesRange(out, 0, len, len, tables, "arab".*, "rlig".*);
    applyGsubLigaturesRange(out, 0, len, len, tables, "arab".*, "liga".*);

    var i: usize = 0;
    while (i < len.*) {
        if (out[i].orig_cp != 0x0644) {
            i += 1;
            continue;
        }
        const next_idx = nextArabicJoiner(out[0..len.*], i) orelse {
            i += 1;
            continue;
        };
        const prev_ok = if (prevArabicJoiner(out[0..len.*], i)) |pi| blk: {
            const pjt = u.arabicJoinType(out[pi].orig_cp);
            break :blk (pjt == .dual_joining or pjt == .join_causing);
        } else false;
        const lig_cp = u.arabicLamAlefForm(out[next_idx].orig_cp, prev_ok) orelse {
            i += 1;
            continue;
        };
        const lgid = findGid(font, lig_cp);
        if (lgid == 0) {
            i += 1;
            continue;
        }
        out[i].cp = @intCast(lig_cp);
        out[i].gid = lgid;
        // Shift trailing items down by 1, preserving intervening marks inside Lam's cluster.
        var m = i + 1;
        while (m < next_idx) : (m += 1) {
            out[m].cluster_id = out[i].cluster_id;
            out[m].wrap_id = out[i].wrap_id;
        }
        m = next_idx;
        while (m + 1 < len.*) : (m += 1) {
            out[m] = out[m + 1];
        }
        len.* -= 1;
        i += 1;
    }
}

fn shapeIndicRange(
    out: []ShapedItem,
    start: usize,
    end: *usize,
    total_len: *usize,
    font: *const StbttFontinfo,
    tables: ot.Tables,
) void {
    if (start >= end.*) return;
    const base_cp = out[start].orig_cp;
    const script_tag: ot.Tag = if (u.isDevanagari(base_cp)) "deva".* else if (u.isBengali(base_cp)) "beng".* else "dev2".*;

    // Decompose two-part split matras into pre-base + post-base marks before reordering.
    var i = start;
    while (i < end.*) : (i += 1) {
        if (u.splitIndicMatra(out[i].orig_cp)) |sp| {
            if (findGid(font, sp.pre) != 0 and findGid(font, sp.post) != 0 and total_len.* + 1 <= out.len) {
                var m = total_len.*;
                while (m > i + 1) {
                    m -= 1;
                    out[m + 1] = out[m];
                }
                const proto = out[i];
                out[i].orig_cp = sp.pre;
                out[i].cp = @intCast(sp.pre);
                out[i].gid = findGid(font, sp.pre);
                out[i + 1] = proto;
                out[i + 1].orig_cp = sp.post;
                out[i + 1].cp = @intCast(sp.post);
                out[i + 1].gid = findGid(font, sp.post);
                end.* += 1;
                if (end != total_len) total_len.* += 1;
            }
        }
    }

    // Apply OpenType Indic pre-reordering substitutions (nukt, akhn, rphf).
    const pre_feats = [_]ot.Tag{ "nukt".*, "akhn".*, "rphf".* };
    for (pre_feats) |ft| {
        const before_len = end.*;
        var k = start;
        while (k < end.*) : (k += 1) {
            const sg = applyGsubFeaturesSingle(tables, script_tag, ft, out[k].gid);
            if (sg != out[k].gid) {
                out[k].gid = sg;
                out[k].cp = encodeShapedGid(sg);
            }
        }
        applyGsubLigaturesRange(out, start, end, total_len, tables, script_tag, ft);
        if (std.mem.eql(u8, &ft, "rphf") and end.* < before_len and end.* - start >= 2) {
            // GSUB rphf collapsed Ra+Virama into one Repha glyph at `start`: move to end of cluster.
            var rep = out[start];
            rep.is_mark = true;
            var m = start;
            while (m + 1 < end.*) : (m += 1) {
                out[m] = out[m + 1];
            }
            out[end.* - 1] = rep;
        }
    }

    // Repha fallback: Ra (0x0930) + Virama (0x094D) before consonant when no GSUB rphf rewrote it.
    if (end.* - start >= 3 and out[start].orig_cp == 0x0930 and out[start + 1].orig_cp == 0x094D) {
        const ra = out[start];
        const vir = out[start + 1];
        var m = start;
        while (m + 2 < end.*) : (m += 1) {
            out[m] = out[m + 2];
        }
        out[end.* - 2] = ra;
        out[end.* - 1] = vir;
        out[end.* - 2].is_mark = true;
        out[end.* - 1].is_mark = true;
    }

    // Visual pre-base matra reordering (Devanagari U+093F and sibling pre-base vowels).
    // Any `matra_pre` inside the syllable moves to `start`, before its consonant core.
    i = start + 1;
    while (i < end.*) : (i += 1) {
        if (u.indicRole(out[i].orig_cp) == .matra_pre) {
            const pre_item = out[i];
            var k = i;
            while (k > start) {
                k -= 1;
                out[k + 1] = out[k];
            }
            out[start] = pre_item;
            out[start].is_mark = false;
        }
    }

    // Apply OpenType Indic presentation/below/above/post/conjunct substitutions.
    const post_feats = [_]ot.Tag{ "half".*, "blwf".*, "pstf".*, "cjct".*, "pres".*, "abvs".*, "blws".*, "psts".*, "liga".* };
    for (post_feats) |ft| {
        var k = start;
        while (k < end.*) : (k += 1) {
            const sg = applyGsubFeaturesSingle(tables, script_tag, ft, out[k].gid);
            if (sg != out[k].gid) {
                out[k].gid = sg;
                out[k].cp = encodeShapedGid(sg);
            }
        }
        applyGsubLigaturesRange(out, start, end, total_len, tables, script_tag, ft);
    }
}

fn shapeThaiSpan(out: []ShapedItem, len: *usize, font: *const StbttFontinfo, tables: ot.Tables) void {
    // Expand Sara Am (U+0E33) -> Nikhahit (U+0E4D, combining above base) + Sara Aa (U+0E32).
    var i: usize = 0;
    while (i < len.*) : (i += 1) {
        if (!u.isThai(out[i].orig_cp)) continue;
        if (tables.gsub.len >= 10) {
            applyGsubMultipleRun(out, len, i, tables, "thai".*, "ccmp".*);
        }
        if (out[i].orig_cp == 0x0E33 and decodeShapedGid(out[i].cp) == null) {
            const nik = findGid(font, 0x0E4D);
            const aa = findGid(font, 0x0E32);
            if (nik != 0 and aa != 0 and len.* + 1 <= out.len) {
                var m = len.*;
                while (m > i + 1) {
                    m -= 1;
                    out[m + 1] = out[m];
                }
                const proto = out[i];
                // Place 0E4D directly after the base consonant (before any tone mark that preceded 0E33).
                var ins = i;
                while (ins > 0 and out[ins - 1].wrap_id == proto.wrap_id and u.thaiRole(out[ins - 1].orig_cp) == .tone_mark) {
                    out[ins] = out[ins - 1];
                    ins -= 1;
                }
                out[ins] = proto;
                out[ins].orig_cp = 0x0E4D;
                out[ins].cp = 0x0E4D;
                out[ins].gid = nik;
                out[ins].is_mark = true;

                out[i + 1] = proto;
                out[i + 1].orig_cp = 0x0E32;
                out[i + 1].cp = 0x0E32;
                out[i + 1].gid = aa;
                out[i + 1].is_mark = false;
                len.* += 1;
                i += 1;
            }
        }
    }
    if (tables.gsub.len >= 10) {
        applyGsubLigaturesRange(out, 0, len, len, tables, "thai".*, "liga".*);
    }
}

fn shapeLatinAndGeneralGsub(out: []ShapedItem, len: *usize, tables: ot.Tables) void {
    if (tables.gsub.len < 10) return;
    applyGsubLigaturesRange(out, 0, len, len, tables, "latn".*, "ccmp".*);
    applyGsubLigaturesRange(out, 0, len, len, tables, "latn".*, "liga".*);
}

fn assignScriptAndGraphemeClusters(out: []ShapedItem, font: *const StbttFontinfo, tables: ot.Tables, len: *usize) void {
    // Pass 1: assign Indic/Thai wrap clusters before script shaping.
    var cid: u16 = 1;
    var i: usize = 0;
    while (i < len.*) {
        const cp = out[i].orig_cp;
        if (u.isIndic(cp) and u.indicRole(cp) == .consonant) {
            const sym_start = i;
            out[i].wrap_id = cid;
            out[i].cluster_id = cid;
            i += 1;
            while (i < len.* and u.isIndic(out[i].orig_cp)) {
                const role = u.indicRole(out[i].orig_cp);
                if (role == .consonant) {
                    const prev_role = u.indicRole(out[i - 1].orig_cp);
                    if (prev_role != .virama and !(prev_role == .nukta and i >= 2 and u.indicRole(out[i - 2].orig_cp) == .virama)) {
                        break;
                    }
                } else if (role == .vowel_indep or role == .none) {
                    break;
                }
                out[i].wrap_id = cid;
                out[i].cluster_id = cid;
                i += 1;
            }
            var syl_end = i;
            shapeIndicRange(out, sym_start, &syl_end, len, font, tables);
            i = syl_end;
            cid += 1;
            continue;
        }
        if (u.isThai(cp)) {
            out[i].wrap_id = cid;
            out[i].cluster_id = cid;
            if (u.thaiRole(cp) == .leading_vowel and i + 1 < len.* and u.thaiRole(out[i + 1].orig_cp) == .consonant) {
                i += 1;
                out[i].wrap_id = cid;
                out[i].cluster_id = cid;
            }
            i += 1;
            while (i < len.* and u.isThai(out[i].orig_cp)) {
                const tr = u.thaiRole(out[i].orig_cp);
                if (tr == .leading_vowel or tr == .consonant) break;
                out[i].wrap_id = cid;
                out[i].cluster_id = cid;
                i += 1;
            }
            cid += 1;
            continue;
        }
        // UAX #29 grapheme boundaries for Arabic/Hebrew/Latin/general runs.
        out[i].wrap_id = cid;
        out[i].cluster_id = cid;
        const prev_p = u.graphemeProp(cp);
        var j = i + 1;
        var ri_odd = prev_p == .regional_indicator;
        while (j < len.*) : (j += 1) {
            const curr_p = u.graphemeProp(out[j].orig_cp);
            if (u.graphemeBreaksBetween(u.graphemeProp(out[j - 1].orig_cp), curr_p, ri_odd)) break;
            out[j].wrap_id = cid;
            out[j].cluster_id = cid;
            ri_odd = (curr_p == .regional_indicator) and !ri_odd;
        }
        i = j;
        cid += 1;
    }

    shapeArabicSpan(out, len, font, tables);
    shapeThaiSpan(out, len, font, tables);
    shapeLatinAndGeneralGsub(out, len, tables);
}

fn glyphAdvancePx(font: *const StbttFontinfo, item: *const ShapedItem, scale: f32) i16 {
    if (item.is_mark or item.is_break) return 0;
    var adv: c_int = 0;
    var lsb: c_int = 0;
    if (decodeShapedGid(item.cp)) |gid| {
        stbtt_GetGlyphHMetrics(font, gid, &adv, &lsb);
    } else if (item.gid != 0) {
        stbtt_GetGlyphHMetrics(font, @intCast(item.gid), &adv, &lsb);
    } else {
        stbtt_GetCodepointHMetrics(font, item.cp, &adv, &lsb);
    }
    var px: i32 = @intFromFloat(@as(f32, @floatFromInt(adv)) * scale);
    if (px < 0) px = 0;
    if (px == 0 and item.orig_cp >= 0x20 and item.gid == 0) {
        px = @max(@divTrunc(@as(i32, item.font_px), 2), 1);
    }
    return @intCast(@min(px, std.math.maxInt(i16)));
}

fn scaleBox(font: *const StbttFontinfo, gid: u16, scale: f32) struct { x0: i32, y0: i32, x1: i32, y1: i32 } {
    var x0: c_int = 0;
    var y0: c_int = 0;
    var x1: c_int = 0;
    var y1: c_int = 0;
    if (gid != 0) {
        stbtt_GetGlyphBitmapBox(font, @intCast(gid), scale, scale, &x0, &y0, &x1, &y1);
    }
    return .{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };
}

fn roundScaled(val: i16, scale: f32) i32 {
    const f = @as(f32, @floatFromInt(val)) * scale;
    return @intFromFloat(@round(f));
}

fn computeMarkAndMetrics(items: []ShapedItem, font: *const StbttFontinfo, tables: ot.Tables) void {
    var base_idx: ?usize = null;
    var base_bx0: i32 = 0;
    var base_by0: i32 = 0;
    var base_bx1: i32 = 0;
    var base_by1: i32 = 0;
    var stack_top: i32 = 0;
    var stack_bot: i32 = 0;
    var prev_mark_idx: ?usize = null;

    for (items, 0..) |*it, idx| {
        const scale = stbtt_ScaleForPixelHeight(font, @floatFromInt(it.font_px));
        if (!it.is_mark) {
            it.advance = glyphAdvancePx(font, it, scale);
            it.dx = 0;
            it.dy = 0;
            base_idx = idx;
            prev_mark_idx = null;
            const bb = scaleBox(font, it.gid, scale);
            base_bx0 = bb.x0;
            base_by0 = bb.y0;
            base_bx1 = bb.x1;
            base_by1 = bb.y1;
            stack_top = bb.y0;
            stack_bot = @max(bb.y1, 0);
            continue;
        }
        it.advance = 0;
        const bidx = base_idx orelse continue;
        const b = &items[bidx];
        const script_tag: ot.Tag = if (u.isDevanagari(b.orig_cp))
            "deva".*
        else if (u.isBengali(b.orig_cp))
            "beng".*
        else if (u.isThai(b.orig_cp))
            "thai".*
        else if (u.bidiClass(b.orig_cp) == .al)
            "arab".*
        else if (u.bidiClass(b.orig_cp) == .r)
            "hebr".*
        else
            "latn".*;

        const mbox = scaleBox(font, it.gid, scale);
        if (prev_mark_idx) |pm| {
            if (ot.gposMarkAttachment(tables.gpos, script_tag, items[pm].gid, it.gid, true)) |d| {
                const dx: i32 = @as(i32, items[pm].dx) + roundScaled(d.dx, scale);
                const dy: i32 = @as(i32, items[pm].dy) - roundScaled(d.dy, scale);
                it.dx = @intCast(std.math.clamp(dx, std.math.minInt(i16), std.math.maxInt(i16)));
                it.dy = @intCast(std.math.clamp(dy, std.math.minInt(i16), std.math.maxInt(i16)));
                if (mbox.y1 > mbox.y0) {
                    stack_top = @min(stack_top, dy + mbox.y0);
                    stack_bot = @max(stack_bot, dy + mbox.y1);
                }
                prev_mark_idx = idx;
                continue;
            }
        }
        if (ot.gposMarkAttachment(tables.gpos, script_tag, b.gid, it.gid, false)) |d| {
            const dx: i32 = roundScaled(d.dx, scale);
            const dy: i32 = -roundScaled(d.dy, scale);
            it.dx = @intCast(std.math.clamp(dx, std.math.minInt(i16), std.math.maxInt(i16)));
            it.dy = @intCast(std.math.clamp(dy, std.math.minInt(i16), std.math.maxInt(i16)));
            if (mbox.y1 > mbox.y0) {
                stack_top = @min(stack_top, dy + mbox.y0);
                stack_bot = @max(stack_bot, dy + mbox.y1);
            }
            prev_mark_idx = idx;
            continue;
        }
        const bw = base_bx1 - base_bx0;
        const base_mid = if (bw > 0) @divTrunc(base_bx0 + base_bx1, 2) else @divTrunc(@as(i32, b.advance), 2);
        const mw = mbox.x1 - mbox.x0;
        const mark_mid = if (mw > 0) @divTrunc(mbox.x0 + mbox.x1, 2) else 0;
        it.dx = @intCast(std.math.clamp(base_mid - mark_mid, std.math.minInt(i16), std.math.maxInt(i16)));

        const side = u.markSide(it.orig_cp);
        var dy: i32 = 0;
        if (side == .below) {
            const target_top = stack_bot + 1;
            dy = if (mbox.y1 > mbox.y0) @max(0, target_top - mbox.y0) else 0;
            if (mbox.y1 > mbox.y0) stack_bot = dy + mbox.y1;
        } else if (side == .overlay) {
            const b_mid_y = @divTrunc(base_by0 + base_by1, 2);
            const m_mid_y = @divTrunc(mbox.y0 + mbox.y1, 2);
            dy = b_mid_y - m_mid_y;
        } else {
            const target_bot = stack_top - 1;
            dy = if (mbox.y1 > mbox.y0) target_bot - mbox.y1 else -@divTrunc(@as(i32, it.font_px), 2);
            if (mbox.y1 > mbox.y0) stack_top = dy + mbox.y0;
        }
        it.dy = @intCast(std.math.clamp(dy, std.math.minInt(i16), std.math.maxInt(i16)));
        prev_mark_idx = idx;
    }
}

fn pickFaceSlot(faces: []const FaceEntry, primary: u8, cp: u32) u8 {
    if (faces.len <= 1 or primary >= faces.len) return 0;
    if (u.isBidiFormatChar(cp) or cp == ' ' or cp == '\t' or cp == '\n') return primary;
    if (findGid(faces[primary].font, cp) != 0) return primary;
    if (primary != 0 and findGid(faces[0].font, cp) != 0) return 0;
    var k: usize = 1;
    while (k < faces.len) : (k += 1) {
        if (k != primary and findGid(faces[k].font, cp) != 0) return @intCast(k);
    }
    return primary;
}

pub fn tablesForBlob(blob: []const u8) ot.Tables {
    if (blob.len < 16) return ot.loadTables(&.{}, 0);
    const off = stbtt_GetFontOffsetForIndex(blob.ptr, 0);
    if (off < 0 or @as(usize, @intCast(off)) >= blob.len) return ot.loadTables(&.{}, 0);
    return ot.loadTables(blob, @intCast(off));
}

fn shapeWithFaces(
    raw: []const RawChar,
    base_dir: bidi.BaseDir,
    faces: []const FaceEntry,
    out_para_level: *u8,
) []ShapedItem {
    const n = @min(raw.len, k_shape_cap);
    if (n == 0 or faces.len == 0) {
        out_para_level.* = if (base_dir == .rtl) 1 else 0;
        return &.{};
    }

    for (raw[0..n], 0..) |r, i| {
        const cls = u.bidiClass(r.cp);
        s_bidi_items[i] = .{
            .cp = r.cp,
            .cls = cls,
            .orig_cls = cls,
            .level = 0,
            .cluster_id = @intCast(i),
            .removed = false,
            .paired_mirror = false,
        };
    }
    const para_lvl = bidi.resolveParagraph(s_bidi_items[0..n], base_dir, &s_bidi_scratch);
    out_para_level.* = para_lvl;

    var in_n: usize = 0;
    var multi_face = false;
    var first_slot: u8 = 0;
    for (raw[0..n], 0..) |r, i| {
        const bi = s_bidi_items[i];
        const keep_joiner = (r.cp == 0x200C or r.cp == 0x200D);
        if ((bi.removed or u.isBidiFormatChar(r.cp)) and !keep_joiner) continue;
        const slot = pickFaceSlot(faces, r.face_slot, r.cp);
        if (in_n == 0) first_slot = slot else if (slot != first_slot) multi_face = true;
        const fnt = faces[slot].font;
        const gid = findGid(fnt, r.cp);
        s_shaped[in_n] = makeItem(r, @intCast(r.cp), gid, bi.cls, bi.orig_cls, bi.level, @intCast(in_n + 1), @intCast(in_n + 1));
        s_shaped[in_n].face_slot = slot;
        in_n += 1;
    }

    var out_n: usize = 0;
    if (!multi_face and in_n > 0) {
        const fe = faces[first_slot];
        out_n = in_n;
        assignScriptAndGraphemeClusters(s_shaped[0..], fe.font, tablesForBlob(fe.data), &out_n);
    } else if (in_n > 0) {
        // Stage multi-face input at the top of s_shaped so each lower run has expansion headroom.
        const stage_n = @min(in_n, k_shape_cap / 2);
        const tail_start = k_shape_cap - stage_n;
        std.mem.copyBackwards(ShapedItem, s_shaped[tail_start..k_shape_cap], s_shaped[0..stage_n]);
        var rstart = tail_start;
        while (rstart < k_shape_cap) {
            const slot = s_shaped[rstart].face_slot;
            var rend = rstart + 1;
            while (rend < k_shape_cap and s_shaped[rend].face_slot == slot) : (rend += 1) {}
            const run_len = rend - rstart;
            if (out_n + run_len > rend) break;
            var k: usize = 0;
            while (k < run_len) : (k += 1) s_shaped[out_n + k] = s_shaped[rstart + k];
            var span_len = run_len;
            const fe = faces[slot];
            assignScriptAndGraphemeClusters(s_shaped[out_n..rend], fe.font, tablesForBlob(fe.data), &span_len);
            out_n += span_len;
            rstart = rend;
        }
    }

    // Strip join-control codepoints (ZWNJ/ZWJ) after contextual joining ran.
    var write_n: usize = 0;
    for (s_shaped[0..out_n]) |it| {
        if (it.orig_cp == 0x200C or it.orig_cp == 0x200D) continue;
        s_shaped[write_n] = it;
        write_n += 1;
    }
    out_n = write_n;

    var rstart: usize = 0;
    while (rstart < out_n) {
        const slot = s_shaped[rstart].face_slot;
        var rend = rstart + 1;
        while (rend < out_n and s_shaped[rend].face_slot == slot) : (rend += 1) {}
        const fe = faces[slot];
        const tables = tablesForBlob(fe.data);
        computeMarkAndMetrics(s_shaped[rstart..rend], fe.font, tables);
        rstart = rend;
    }
    return s_shaped[0..out_n];
}

/// Pure testable shaping pipeline from raw logical codepoints to shaped/positioned
/// visual items with UAX #9 levels resolved (returned slice aliases static scratch).
pub fn shapeLogicalSequence(
    raw: []const RawChar,
    base_dir: bidi.BaseDir,
    font: *const StbttFontinfo,
    font_data: []const u8,
    out_para_level: *u8,
) []ShapedItem {
    const single = [_]FaceEntry{.{ .font = font, .data = font_data }};
    return shapeWithFaces(raw, base_dir, &single, out_para_level);
}

/// Multi-face shaping entry point for layout with registered fallback/@font-face blobs.
pub fn shapeMultiFaceSequence(
    raw: []const RawChar,
    base_dir: bidi.BaseDir,
    faces: []const FaceEntry,
    out_para_level: *u8,
) []ShapedItem {
    return shapeWithFaces(raw, base_dir, faces, out_para_level);
}

pub fn reorderLineForLayout(items: []ShapedItem, para_level: u8) []ShapedItem {
    bidi.reorderVisualLine(ShapedItem, items, para_level);
    return items;
}

pub const tokensNeedShaping = shape_layout.tokensNeedShaping;
pub const resolveTokensBaseLevel = shape_layout.resolveTokensBaseLevel;
pub const layComplexBlockRun = shape_layout.layComplexBlockRun;
pub const layComplexCellText = shape_layout.layComplexCellText;
