//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Heap-free, bounds-checked OpenType `GSUB`, `GPOS`, and `GDEF` reader for
//! `reflow` (`RA8FW-1173` / `RA8FW-296`). Reads single/multiple/ligature
//! substitutions (`GSUB` Types 1, 2, 4, and 7) with script/feature selection
//! and `IgnoreMarks` awareness, plus `GPOS` MarkToBase / MarkToMark anchor
//! positioning deltas (`GPOS` Types 4, 6, and 9) directly out of the caller's
//! font byte slice without heap allocation.

const std = @import("std");

pub const Tag = [4]u8;

pub const k_flag_ignore_base: u16 = 0x0002;
pub const k_flag_ignore_lig: u16 = 0x0004;
pub const k_flag_ignore_marks: u16 = 0x0008;

pub const AnchorDelta = struct {
    dx: i16,
    dy: i16,
};

pub const MultiSubstOut = struct {
    len: u8 = 0,
    gids: [4]u16 = @splat(0),
};

pub const Tables = struct {
    data: []const u8,
    gsub: []const u8,
    gpos: []const u8,
    gdef: []const u8,
};

fn readU16(buf: []const u8, off: usize) ?u16 {
    if (off + 2 > buf.len) return null;
    return (@as(u16, buf[off]) << 8) | @as(u16, buf[off + 1]);
}

fn readI16(buf: []const u8, off: usize) ?i16 {
    const v = readU16(buf, off) orelse return null;
    return @bitCast(v);
}

fn readU32(buf: []const u8, off: usize) ?u32 {
    if (off + 4 > buf.len) return null;
    return (@as(u32, buf[off]) << 24) |
        (@as(u32, buf[off + 1]) << 16) |
        (@as(u32, buf[off + 2]) << 8) |
        @as(u32, buf[off + 3]);
}

fn subSlice(buf: []const u8, off: usize) ?[]const u8 {
    if (off == 0 or off >= buf.len) return null;
    return buf[off..];
}

/// Locate `GSUB`, `GPOS`, and `GDEF` sub-slices in an SFNT font blob starting at `font_start`.
pub fn loadTables(data: []const u8, font_start: usize) Tables {
    var out = Tables{
        .data = data,
        .gsub = &.{},
        .gpos = &.{},
        .gdef = &.{},
    };
    const ntables = readU16(data, font_start + 4) orelse return out;
    const max_tables: usize = @min(@as(usize, ntables), 64);
    var i: usize = 0;
    while (i < max_tables) : (i += 1) {
        const rec = font_start + 12 + i * 16;
        if (rec + 16 > data.len) break;
        const off: usize = readU32(data, rec + 8) orelse continue;
        const len: usize = readU32(data, rec + 12) orelse continue;
        if (off > data.len or len > data.len - off) continue;
        const tag = data[rec .. rec + 4];
        if (std.mem.eql(u8, tag, "GSUB")) {
            out.gsub = data[off .. off + len];
        } else if (std.mem.eql(u8, tag, "GPOS")) {
            out.gpos = data[off .. off + len];
        } else if (std.mem.eql(u8, tag, "GDEF")) {
            out.gdef = data[off .. off + len];
        }
    }
    return out;
}

/// Return Coverage index of `gid`, or null if not covered (Format 1 & Format 2).
pub fn coverageIndex(cov: []const u8, gid: u16) ?u16 {
    const fmt = readU16(cov, 0) orelse return null;
    const count = readU16(cov, 2) orelse return null;
    if (fmt == 1) {
        var lo: usize = 0;
        var hi: usize = count;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            const g = readU16(cov, 4 + mid * 2) orelse return null;
            if (gid < g) hi = mid else if (gid > g) lo = mid + 1 else return @intCast(mid);
        }
        return null;
    }
    if (fmt == 2) {
        var lo: usize = 0;
        var hi: usize = count;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            const base = 4 + mid * 6;
            const start_g = readU16(cov, base) orelse return null;
            const end_g = readU16(cov, base + 2) orelse return null;
            if (gid < start_g) {
                hi = mid;
            } else if (gid > end_g) {
                lo = mid + 1;
            } else {
                const start_idx = readU16(cov, base + 4) orelse return null;
                return start_idx + (gid - start_g);
            }
        }
    }
    return null;
}

/// Return GDEF GlyphClass (1=base, 2=lig, 3=mark, 4=component) or 0 if absent.
pub fn gdefGlyphClass(gdef: []const u8, gid: u16) u16 {
    if (gdef.len < 12) return 0;
    const cls_off = readU16(gdef, 4) orelse return 0;
    const cls = subSlice(gdef, cls_off) orelse return 0;
    return classDefValue(cls, gid);
}

fn classDefValue(cls: []const u8, gid: u16) u16 {
    const fmt = readU16(cls, 0) orelse return 0;
    if (fmt == 1) {
        const start_g = readU16(cls, 2) orelse return 0;
        const cnt = readU16(cls, 4) orelse return 0;
        if (gid < start_g or gid - start_g >= cnt) return 0;
        return readU16(cls, 6 + @as(usize, gid - start_g) * 2) orelse 0;
    }
    if (fmt == 2) {
        const cnt = readU16(cls, 2) orelse return 0;
        var lo: usize = 0;
        var hi: usize = cnt;
        while (lo < hi) {
            const mid = lo + (hi - lo) / 2;
            const base = 4 + mid * 6;
            const s = readU16(cls, base) orelse return 0;
            const e = readU16(cls, base + 2) orelse return 0;
            if (gid < s) hi = mid else if (gid > e) lo = mid + 1 else return readU16(cls, base + 4) orelse 0;
        }
    }
    return 0;
}

fn matchScriptOffset(script_list: []const u8, wanted: Tag) ?usize {
    const cnt = readU16(script_list, 0) orelse return null;
    var dflt_off: ?usize = null;
    var i: usize = 0;
    while (i < cnt) : (i += 1) {
        const rec = 2 + i * 6;
        if (rec + 6 > script_list.len) break;
        const tag = script_list[rec .. rec + 4];
        const off = readU16(script_list, rec + 4) orelse continue;
        if (std.mem.eql(u8, tag, &wanted)) return off;
        if (std.mem.eql(u8, tag, "DFLT") or std.mem.eql(u8, tag, "dflt")) dflt_off = off;
    }
    return dflt_off;
}

fn defaultLangSys(script_tbl: []const u8) ?[]const u8 {
    const dflt_off = readU16(script_tbl, 0) orelse 0;
    if (dflt_off != 0) return subSlice(script_tbl, dflt_off);
    const lang_cnt = readU16(script_tbl, 2) orelse 0;
    if (lang_cnt == 0) return null;
    const off = readU16(script_tbl, 8) orelse return null;
    return subSlice(script_tbl, off);
}

/// Collect lookup indices in `ot_table` (`GSUB` or `GPOS`) enabled for `script_tag` + `feat_tag`.
pub fn collectFeatureLookups(
    ot_table: []const u8,
    script_tag: Tag,
    feat_tag: Tag,
    out_lookups: []u16,
) usize {
    if (ot_table.len < 10) return 0;
    const sl_off = readU16(ot_table, 4) orelse return 0;
    const fl_off = readU16(ot_table, 6) orelse return 0;
    const script_list = subSlice(ot_table, sl_off) orelse return 0;
    const feat_list = subSlice(ot_table, fl_off) orelse return 0;

    const s_off = matchScriptOffset(script_list, script_tag) orelse return 0;
    const script_tbl = subSlice(script_list, s_off) orelse return 0;
    const lang_sys = defaultLangSys(script_tbl) orelse return 0;

    const req_idx = readU16(lang_sys, 2) orelse 0xFFFF;
    const f_idx_cnt = readU16(lang_sys, 4) orelse return 0;
    const fl_cnt = readU16(feat_list, 0) orelse return 0;

    var written: usize = 0;
    var fi_pos: usize = 0;
    const total_refs = @as(usize, f_idx_cnt) + (if (req_idx != 0xFFFF) @as(usize, 1) else @as(usize, 0));
    while (fi_pos < total_refs) : (fi_pos += 1) {
        const f_idx = if (req_idx != 0xFFFF and fi_pos == 0)
            req_idx
        else
            readU16(lang_sys, 6 + (fi_pos - (if (req_idx != 0xFFFF) @as(usize, 1) else @as(usize, 0))) * 2) orelse continue;
        if (f_idx >= fl_cnt) continue;
        const rec = 2 + @as(usize, f_idx) * 6;
        if (rec + 6 > feat_list.len) break;
        if (!std.mem.eql(u8, feat_list[rec .. rec + 4], &feat_tag)) continue;
        const f_off = readU16(feat_list, rec + 4) orelse continue;
        const feat_tbl = subSlice(feat_list, f_off) orelse continue;
        const lcnt = readU16(feat_tbl, 2) orelse continue;
        var li: usize = 0;
        while (li < lcnt and written < out_lookups.len) : (li += 1) {
            out_lookups[written] = readU16(feat_tbl, 4 + li * 2) orelse break;
            written += 1;
        }
    }
    return written;
}

const LookupMeta = struct {
    ltype: u16,
    flags: u16,
    sub_count: u16,
    table: []const u8,
};

fn getLookup(ot_table: []const u8, lookup_idx: u16) ?LookupMeta {
    const ll_off = readU16(ot_table, 8) orelse return null;
    const ll = subSlice(ot_table, ll_off) orelse return null;
    const cnt = readU16(ll, 0) orelse return null;
    if (lookup_idx >= cnt) return null;
    const l_off = readU16(ll, 2 + @as(usize, lookup_idx) * 2) orelse return null;
    const tbl = subSlice(ll, l_off) orelse return null;
    return .{
        .ltype = readU16(tbl, 0) orelse return null,
        .flags = readU16(tbl, 2) orelse return null,
        .sub_count = readU16(tbl, 4) orelse return null,
        .table = tbl,
    };
}

fn unwrapSubtable(meta: LookupMeta, sub_i: usize, ext_type: u16) ?struct { ltype: u16, sub: []const u8 } {
    const s_off = readU16(meta.table, 6 + sub_i * 2) orelse return null;
    const sub = subSlice(meta.table, s_off) orelse return null;
    if (meta.ltype != ext_type) {
        return .{ .ltype = meta.ltype, .sub = sub };
    }
    const fmt = readU16(sub, 0) orelse return null;
    if (fmt != 1) return null;
    const real_type = readU16(sub, 2) orelse return null;
    const ext_off = readU32(sub, 4) orelse return null;
    if (ext_off == 0 or ext_off >= sub.len) return null;
    return .{ .ltype = real_type, .sub = sub[ext_off..] };
}

/// Apply one `GSUB` single substitution lookup (`Type 1`) to `gid`.
pub fn gsubApplySingle(gsub: []const u8, lookup_idx: u16, gid: u16) ?u16 {
    const meta = getLookup(gsub, lookup_idx) orelse return null;
    var s: usize = 0;
    while (s < meta.sub_count) : (s += 1) {
        const u = unwrapSubtable(meta, s, 7) orelse continue;
        if (u.ltype != 1) continue;
        const fmt = readU16(u.sub, 0) orelse continue;
        const cov_off = readU16(u.sub, 2) orelse continue;
        const cov = subSlice(u.sub, cov_off) orelse continue;
        const cidx = coverageIndex(cov, gid) orelse continue;
        if (fmt == 1) {
            const delta = readI16(u.sub, 4) orelse continue;
            const res: i32 = @as(i32, gid) + @as(i32, delta);
            return @intCast(@as(u32, @bitCast(res)) & 0xFFFF);
        }
        if (fmt == 2) {
            const cnt = readU16(u.sub, 4) orelse continue;
            if (cidx >= cnt) continue;
            return readU16(u.sub, 6 + @as(usize, cidx) * 2);
        }
    }
    return null;
}

/// Apply one `GSUB` multiple substitution lookup (`Type 2`) to `gid`.
pub fn gsubApplyMultiple(gsub: []const u8, lookup_idx: u16, gid: u16) ?MultiSubstOut {
    const meta = getLookup(gsub, lookup_idx) orelse return null;
    var s: usize = 0;
    while (s < meta.sub_count) : (s += 1) {
        const u = unwrapSubtable(meta, s, 7) orelse continue;
        if (u.ltype != 2) continue;
        const fmt = readU16(u.sub, 0) orelse continue;
        if (fmt != 1) continue;
        const cov_off = readU16(u.sub, 2) orelse continue;
        const cov = subSlice(u.sub, cov_off) orelse continue;
        const cidx = coverageIndex(cov, gid) orelse continue;
        const scnt = readU16(u.sub, 4) orelse continue;
        if (cidx >= scnt) continue;
        const seq_off = readU16(u.sub, 6 + @as(usize, cidx) * 2) orelse continue;
        const seq = subSlice(u.sub, seq_off) orelse continue;
        const gcnt = readU16(seq, 0) orelse continue;
        if (gcnt == 0 or gcnt > 4) continue;
        var out = MultiSubstOut{ .len = @intCast(gcnt) };
        var k: usize = 0;
        while (k < gcnt) : (k += 1) {
            out.gids[k] = readU16(seq, 2 + k * 2) orelse return null;
        }
        return out;
    }
    return null;
}

/// Apply one `GSUB` ligature substitution lookup (`Type 4`) at index 0 of
/// `gids[0..]` (up to 4 components), skipping glyphs whose GDEF class matches
/// `IgnoreMarks` when enabled by `meta.flags`. Returns `{ lig_gid, consumed_end }`.
pub fn gsubApplyLigature(
    gsub: []const u8,
    gdef: []const u8,
    lookup_idx: u16,
    gids: []const u16,
    is_mark: []const bool,
) ?struct { lig_gid: u16, end_pos: usize, ignore_marks: bool } {
    if (gids.len < 2) return null;
    const meta = getLookup(gsub, lookup_idx) orelse return null;
    const ignore_marks = (meta.flags & k_flag_ignore_marks) != 0;
    if (ignore_marks and (is_mark[0] or gdefGlyphClass(gdef, gids[0]) == 3)) return null;

    var s: usize = 0;
    while (s < meta.sub_count) : (s += 1) {
        const u = unwrapSubtable(meta, s, 7) orelse continue;
        if (u.ltype != 4) continue;
        const fmt = readU16(u.sub, 0) orelse continue;
        if (fmt != 1) continue;
        const cov_off = readU16(u.sub, 2) orelse continue;
        const cov = subSlice(u.sub, cov_off) orelse continue;
        const cidx = coverageIndex(cov, gids[0]) orelse continue;
        const set_cnt = readU16(u.sub, 4) orelse continue;
        if (cidx >= set_cnt) continue;
        const set_off = readU16(u.sub, 6 + @as(usize, cidx) * 2) orelse continue;
        const lset = subSlice(u.sub, set_off) orelse continue;
        const lcnt = readU16(lset, 0) orelse continue;
        var li: usize = 0;
        while (li < lcnt) : (li += 1) {
            const lig_off = readU16(lset, 2 + li * 2) orelse continue;
            const lig = subSlice(lset, lig_off) orelse continue;
            const lig_gid = readU16(lig, 0) orelse continue;
            const comp_cnt = readU16(lig, 2) orelse continue;
            if (comp_cnt < 2) continue;
            var pos: usize = 1;
            var ci: usize = 1;
            var ok = true;
            while (ci < comp_cnt) : (ci += 1) {
                while (pos < gids.len and ignore_marks and
                    (is_mark[pos] or gdefGlyphClass(gdef, gids[pos]) == 3)) : (pos += 1)
                {}
                if (pos >= gids.len) {
                    ok = false;
                    break;
                }
                const want = readU16(lig, 4 + (ci - 1) * 2) orelse {
                    ok = false;
                    break;
                };
                if (gids[pos] != want) {
                    ok = false;
                    break;
                }
                pos += 1;
            }
            if (ok) {
                return .{ .lig_gid = lig_gid, .end_pos = pos, .ignore_marks = ignore_marks };
            }
        }
    }
    return null;
}

fn readAnchor(tbl: []const u8, off: u16) ?AnchorDelta {
    const a = subSlice(tbl, off) orelse return null;
    return .{
        .dx = readI16(a, 2) orelse return null,
        .dy = readI16(a, 4) orelse return null,
    };
}

/// Extract GPOS anchor delta `(dx, dy)` in font design units for `mark_gid` on `base_gid`
/// using `mark`/`mkmk` lookups (`LookupType 4` or `LookupType 6`).
pub fn gposMarkAttachment(
    gpos: []const u8,
    script_tag: Tag,
    base_gid: u16,
    mark_gid: u16,
    mark_to_mark: bool,
) ?AnchorDelta {
    if (gpos.len < 10) return null;
    var lookups: [32]u16 = undefined;
    const feat: Tag = if (mark_to_mark) "mkmk".* else "mark".*;
    const want_type: u16 = if (mark_to_mark) 6 else 4;
    const n = collectFeatureLookups(gpos, script_tag, feat, &lookups);
    for (lookups[0..n]) |lidx| {
        const meta = getLookup(gpos, lidx) orelse continue;
        var s: usize = 0;
        while (s < meta.sub_count) : (s += 1) {
            const u = unwrapSubtable(meta, s, 9) orelse continue;
            if (u.ltype != want_type) continue;
            const fmt = readU16(u.sub, 0) orelse continue;
            if (fmt != 1) continue;
            const m_cov = subSlice(u.sub, readU16(u.sub, 2) orelse continue) orelse continue;
            const b_cov = subSlice(u.sub, readU16(u.sub, 4) orelse continue) orelse continue;
            const m_idx = coverageIndex(m_cov, mark_gid) orelse continue;
            const b_idx = coverageIndex(b_cov, base_gid) orelse continue;
            const cls_cnt = readU16(u.sub, 6) orelse continue;
            const m_arr = subSlice(u.sub, readU16(u.sub, 8) orelse continue) orelse continue;
            const b_arr = subSlice(u.sub, readU16(u.sub, 10) orelse continue) orelse continue;

            const m_cnt = readU16(m_arr, 0) orelse continue;
            if (m_idx >= m_cnt) continue;
            const m_rec = 2 + @as(usize, m_idx) * 4;
            const m_cls = readU16(m_arr, m_rec) orelse continue;
            if (m_cls >= cls_cnt) continue;
            const m_anc_off = readU16(m_arr, m_rec + 2) orelse continue;
            const m_anc = readAnchor(m_arr, m_anc_off) orelse continue;

            const b_cnt = readU16(b_arr, 0) orelse continue;
            if (b_idx >= b_cnt) continue;
            const b_rec = 2 + (@as(usize, b_idx) * @as(usize, cls_cnt) + @as(usize, m_cls)) * 2;
            const b_anc_off = readU16(b_arr, b_rec) orelse continue;
            const b_anc = readAnchor(b_arr, b_anc_off) orelse continue;

            return .{
                .dx = b_anc.dx -% m_anc.dx,
                .dy = b_anc.dy -% m_anc.dy,
            };
        }
    }
    return null;
}
