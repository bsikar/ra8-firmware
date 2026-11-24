// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Test-only synthetic SFNT/TrueType + OpenType (GDEF/GSUB/GPOS) builder
//! for deterministic host and golden-render shaping verification.

const std = @import("std");

pub const GlyphDef = struct {
    cp: u32 = 0, // 0 => unmapped GID (substituted ligature / contextual glyph)
    adv: u16 = 600,
    x0: i16 = 100,
    y0: i16 = 0,
    x1: i16 = 500,
    y1: i16 = 500,
    gdef_class: u16 = 1, // 1=base, 2=ligature, 3=mark
};

pub const GsubSingleSpec = struct {
    script: [4]u8,
    feat: [4]u8,
    from_gid: u16,
    to_gid: u16,
};

pub const GsubLigSpec = struct {
    script: [4]u8,
    feat: [4]u8,
    ignore_marks: bool = true,
    comp0: u16,
    comp1: u16,
    comp2: u16 = 0, // 0 => 2-component ligature
    lig_gid: u16,
};

pub const GsubMultiSpec = struct {
    script: [4]u8,
    feat: [4]u8,
    from_gid: u16,
    out0: u16,
    out1: u16,
};

pub const GposMarkSpec = struct {
    script: [4]u8,
    mark_to_mark: bool = false,
    base_gid: u16,
    base_ax: i16,
    base_ay: i16,
    mark_gid: u16,
    mark_ax: i16,
    mark_ay: i16,
};

fn wrU16(buf: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, v: u16) !void {
    try buf.append(alloc, @intCast((v >> 8) & 0xFF));
    try buf.append(alloc, @intCast(v & 0xFF));
}

fn wrI16(buf: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, v: i16) !void {
    try wrU16(buf, alloc, @bitCast(v));
}

fn wrU32(buf: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, v: u32) !void {
    try wrU16(buf, alloc, @intCast((v >> 16) & 0xFFFF));
    try wrU16(buf, alloc, @intCast(v & 0xFFFF));
}

fn wrBytes(buf: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, s: []const u8) !void {
    try buf.appendSlice(alloc, s);
}

fn patchU16(buf: *std.ArrayListUnmanaged(u8), off: usize, v: u16) void {
    buf.items[off] = @intCast((v >> 8) & 0xFF);
    buf.items[off + 1] = @intCast(v & 0xFF);
}

fn patchU32(buf: *std.ArrayListUnmanaged(u8), off: usize, v: u32) void {
    buf.items[off] = @intCast((v >> 24) & 0xFF);
    buf.items[off + 1] = @intCast((v >> 16) & 0xFF);
    buf.items[off + 2] = @intCast((v >> 8) & 0xFF);
    buf.items[off + 3] = @intCast(v & 0xFF);
}

fn buildCmap(alloc: std.mem.Allocator, glyphs: []const GlyphDef) ![]u8 {
    const Entry = struct { cp: u16, gid: u16 };
    var entries: std.ArrayListUnmanaged(Entry) = .empty;
    defer entries.deinit(alloc);
    for (glyphs, 0..) |g, idx| {
        if (g.cp != 0 and g.cp < 0xFFFF) {
            try entries.append(alloc, .{ .cp = @intCast(g.cp), .gid = @intCast(idx) });
        }
    }
    std.mem.sort(Entry, entries.items, {}, struct {
        fn lt(_: void, a: Entry, b: Entry) bool {
            return a.cp < b.cp;
        }
    }.lt);

    // Subtable format 4: one single-codepoint segment per mapped glyph + 0xFFFF sentinel.
    const seg_count: u16 = @intCast(entries.items.len + 1);
    const seg_cnt_x2: u16 = seg_count * 2;
    var sub: std.ArrayListUnmanaged(u8) = .empty;
    defer sub.deinit(alloc);

    try wrU16(&sub, alloc, 4); // format
    const len_pos = sub.items.len;
    try wrU16(&sub, alloc, 0); // length placeholder
    try wrU16(&sub, alloc, 0); // language
    try wrU16(&sub, alloc, seg_cnt_x2);
    var sr: u16 = 2;
    var es: u16 = 0;
    while (sr * 2 <= seg_cnt_x2) : (sr *= 2) {
        es += 1;
    }
    try wrU16(&sub, alloc, sr);
    try wrU16(&sub, alloc, es);
    try wrU16(&sub, alloc, seg_cnt_x2 - sr);

    for (entries.items) |e| try wrU16(&sub, alloc, e.cp);
    try wrU16(&sub, alloc, 0xFFFF); // endCode sentinel
    try wrU16(&sub, alloc, 0); // reservedPad
    for (entries.items) |e| try wrU16(&sub, alloc, e.cp);
    try wrU16(&sub, alloc, 0xFFFF); // startCode sentinel
    for (entries.items) |e| {
        const delta: i16 = @as(i16, @intCast(e.gid)) -% @as(i16, @bitCast(e.cp));
        try wrI16(&sub, alloc, delta);
    }
    try wrI16(&sub, alloc, 1); // idDelta sentinel
    var s: usize = 0;
    while (s < seg_count) : (s += 1) try wrU16(&sub, alloc, 0); // idRangeOffset
    patchU16(&sub, len_pos, @intCast(sub.items.len));

    var tbl: std.ArrayListUnmanaged(u8) = .empty;
    try wrU16(&tbl, alloc, 0); // version
    try wrU16(&tbl, alloc, 1); // numTables
    try wrU16(&tbl, alloc, 3); // Windows
    try wrU16(&tbl, alloc, 1); // Unicode BMP
    try wrU32(&tbl, alloc, 12); // subtable offset
    try wrBytes(&tbl, alloc, sub.items);
    return tbl.toOwnedSlice(alloc);
}

fn buildHead(alloc: std.mem.Allocator) ![]u8 {
    var tbl: std.ArrayListUnmanaged(u8) = .empty;
    try tbl.appendNTimes(alloc, 0, 54);
    patchU32(&tbl, 0, 0x00010000); // version 1.0
    patchU32(&tbl, 12, 0x5F0F3CF5); // magicNumber
    patchU16(&tbl, 18, 1000); // unitsPerEm
    patchU16(&tbl, 38, 1000); // xMax
    patchU16(&tbl, 40, @bitCast(@as(i16, 1000)));
    patchU16(&tbl, 42, 1000); // yMax
    patchU16(&tbl, 50, 0); // indexToLocFormat = 0 (short)
    return tbl.toOwnedSlice(alloc);
}

fn buildMaxp(alloc: std.mem.Allocator, num_glyphs: u16) ![]u8 {
    var tbl: std.ArrayListUnmanaged(u8) = .empty;
    try tbl.appendNTimes(alloc, 0, 32);
    patchU32(&tbl, 0, 0x00010000);
    patchU16(&tbl, 4, num_glyphs);
    return tbl.toOwnedSlice(alloc);
}

fn buildHhea(alloc: std.mem.Allocator, num_glyphs: u16) ![]u8 {
    var tbl: std.ArrayListUnmanaged(u8) = .empty;
    try tbl.appendNTimes(alloc, 0, 36);
    patchU32(&tbl, 0, 0x00010000);
    patchU16(&tbl, 4, @bitCast(@as(i16, 800))); // ascent
    patchU16(&tbl, 6, @bitCast(@as(i16, -200))); // descent
    patchU16(&tbl, 8, @bitCast(@as(i16, 200))); // lineGap
    patchU16(&tbl, 34, num_glyphs); // numberOfHMetrics
    return tbl.toOwnedSlice(alloc);
}

fn buildHmtx(alloc: std.mem.Allocator, glyphs: []const GlyphDef) ![]u8 {
    var tbl: std.ArrayListUnmanaged(u8) = .empty;
    for (glyphs) |g| {
        try wrU16(&tbl, alloc, g.adv);
        try wrI16(&tbl, alloc, if (g.x1 > g.x0) g.x0 else 0);
    }
    return tbl.toOwnedSlice(alloc);
}

fn buildGlyfAndLoca(alloc: std.mem.Allocator, glyphs: []const GlyphDef) !struct { glyf: []u8, loca: []u8 } {
    var glyf: std.ArrayListUnmanaged(u8) = .empty;
    var loca: std.ArrayListUnmanaged(u8) = .empty;
    for (glyphs) |g| {
        try wrU16(&loca, alloc, @intCast(glyf.items.len / 2));
        if (g.x1 <= g.x0 or g.y1 <= g.y0) continue; // empty glyph (loca stays equal)
        try wrI16(&glyf, alloc, 1); // numberOfContours
        try wrI16(&glyf, alloc, g.x0);
        try wrI16(&glyf, alloc, g.y0);
        try wrI16(&glyf, alloc, g.x1);
        try wrI16(&glyf, alloc, g.y1);
        try wrU16(&glyf, alloc, 3); // endPtsOfContours[0]
        try wrU16(&glyf, alloc, 0); // instructionLength
        try glyf.appendSlice(alloc, &[_]u8{ 0x01, 0x01, 0x01, 0x01 });
        // CW rectangle: (x0,y0) -> (x0,y1) -> (x1,y1) -> (x1,y0)
        try wrI16(&glyf, alloc, g.x0);
        try wrI16(&glyf, alloc, 0);
        try wrI16(&glyf, alloc, g.x1 - g.x0);
        try wrI16(&glyf, alloc, 0);
        try wrI16(&glyf, alloc, g.y0);
        try wrI16(&glyf, alloc, g.y1 - g.y0);
        try wrI16(&glyf, alloc, 0);
        try wrI16(&glyf, alloc, g.y0 - g.y1);
    }
    try wrU16(&loca, alloc, @intCast(glyf.items.len / 2));
    return .{ .glyf = try glyf.toOwnedSlice(alloc), .loca = try loca.toOwnedSlice(alloc) };
}

fn buildGdef(alloc: std.mem.Allocator, glyphs: []const GlyphDef) ![]u8 {
    var tbl: std.ArrayListUnmanaged(u8) = .empty;
    try wrU32(&tbl, alloc, 0x00010000);
    try wrU16(&tbl, alloc, 12); // glyphClassDefOffset
    try wrU16(&tbl, alloc, 0);
    try wrU16(&tbl, alloc, 0);
    try wrU16(&tbl, alloc, 0);
    // ClassDef format 1 at offset 12: startGlyphID=0, glyphCount=glyphs.len
    try wrU16(&tbl, alloc, 1);
    try wrU16(&tbl, alloc, 0);
    try wrU16(&tbl, alloc, @intCast(glyphs.len));
    for (glyphs) |g| try wrU16(&tbl, alloc, g.gdef_class);
    return tbl.toOwnedSlice(alloc);
}

fn writeCoverage(alloc: std.mem.Allocator, gids: []const u16) ![]u8 {
    var buf: std.ArrayListUnmanaged(u8) = .empty;
    try wrU16(&buf, alloc, 1); // format 1
    try wrU16(&buf, alloc, @intCast(gids.len));
    for (gids) |g| try wrU16(&buf, alloc, g);
    return buf.toOwnedSlice(alloc);
}

const RawLookup = struct {
    script: [4]u8,
    feat: [4]u8,
    bytes: []u8,
};

fn assembleOtlHeader(alloc: std.mem.Allocator, lookups: []const RawLookup) ![]u8 {
    // Group unique (script, feat) into FeatureRecords and ScriptRecords.
    const FeatEntry = struct { script: [4]u8, feat: [4]u8, lookup_idxs: std.ArrayListUnmanaged(u16) = .empty };
    var feats: std.ArrayListUnmanaged(FeatEntry) = .empty;
    defer {
        for (feats.items) |*f| f.lookup_idxs.deinit(alloc);
        feats.deinit(alloc);
    }
    for (lookups, 0..) |lk, lidx| {
        var found: ?usize = null;
        for (feats.items, 0..) |f, fi| {
            if (std.mem.eql(u8, &f.script, &lk.script) and std.mem.eql(u8, &f.feat, &lk.feat)) {
                found = fi;
                break;
            }
        }
        if (found) |fi| {
            try feats.items[fi].lookup_idxs.append(alloc, @intCast(lidx));
        } else {
            var fe = FeatEntry{ .script = lk.script, .feat = lk.feat };
            try fe.lookup_idxs.append(alloc, @intCast(lidx));
            try feats.append(alloc, fe);
        }
    }

    var scripts: std.ArrayListUnmanaged([4]u8) = .empty;
    defer scripts.deinit(alloc);
    for (feats.items) |f| {
        var seen = false;
        for (scripts.items) |sc| {
            if (std.mem.eql(u8, &sc, &f.script)) {
                seen = true;
                break;
            }
        }
        if (!seen) try scripts.append(alloc, f.script);
    }

    // Build ScriptList
    var sl: std.ArrayListUnmanaged(u8) = .empty;
    defer sl.deinit(alloc);
    try wrU16(&sl, alloc, @intCast(scripts.items.len));
    const sl_rec_start = sl.items.len;
    for (scripts.items) |sc| {
        try wrBytes(&sl, alloc, &sc);
        try wrU16(&sl, alloc, 0); // offset placeholder
    }
    for (scripts.items, 0..) |sc, si| {
        const sc_off = sl.items.len;
        patchU16(&sl, sl_rec_start + si * 6 + 4, @intCast(sc_off));
        try wrU16(&sl, alloc, 4); // defaultLangSysOffset (relative to Script)
        try wrU16(&sl, alloc, 0); // langSysCount
        // LangSys table:
        var cnt: u16 = 0;
        for (feats.items) |f| {
            if (std.mem.eql(u8, &f.script, &sc)) cnt += 1;
        }
        try wrU16(&sl, alloc, 0); // lookupOrder
        try wrU16(&sl, alloc, 0xFFFF); // requiredFeatureIndex
        try wrU16(&sl, alloc, cnt);
        for (feats.items, 0..) |f, fi| {
            if (std.mem.eql(u8, &f.script, &sc)) try wrU16(&sl, alloc, @intCast(fi));
        }
    }

    // Build FeatureList
    var fl: std.ArrayListUnmanaged(u8) = .empty;
    defer fl.deinit(alloc);
    try wrU16(&fl, alloc, @intCast(feats.items.len));
    const fl_rec_start = fl.items.len;
    for (feats.items) |f| {
        try wrBytes(&fl, alloc, &f.feat);
        try wrU16(&fl, alloc, 0);
    }
    for (feats.items, 0..) |f, fi| {
        patchU16(&fl, fl_rec_start + fi * 6 + 4, @intCast(fl.items.len));
        try wrU16(&fl, alloc, 0); // featureParamsOffset
        try wrU16(&fl, alloc, @intCast(f.lookup_idxs.items.len));
        for (f.lookup_idxs.items) |lidx| try wrU16(&fl, alloc, lidx);
    }

    // Build LookupList
    var ll: std.ArrayListUnmanaged(u8) = .empty;
    defer ll.deinit(alloc);
    try wrU16(&ll, alloc, @intCast(lookups.len));
    const ll_rec_start = ll.items.len;
    for (lookups) |_| try wrU16(&ll, alloc, 0);
    for (lookups, 0..) |lk, li| {
        patchU16(&ll, ll_rec_start + li * 2, @intCast(ll.items.len));
        try wrBytes(&ll, alloc, lk.bytes);
    }

    var otl: std.ArrayListUnmanaged(u8) = .empty;
    try wrU32(&otl, alloc, 0x00010000);
    const sl_off: u16 = 10;
    const fl_off: u16 = @intCast(sl_off + sl.items.len);
    const ll_off: u16 = @intCast(fl_off + fl.items.len);
    try wrU16(&otl, alloc, sl_off);
    try wrU16(&otl, alloc, fl_off);
    try wrU16(&otl, alloc, ll_off);
    try wrBytes(&otl, alloc, sl.items);
    try wrBytes(&otl, alloc, fl.items);
    try wrBytes(&otl, alloc, ll.items);
    return otl.toOwnedSlice(alloc);
}

fn buildGsub(
    alloc: std.mem.Allocator,
    singles: []const GsubSingleSpec,
    ligs: []const GsubLigSpec,
    multis: []const GsubMultiSpec,
) ![]u8 {
    var lookups: std.ArrayListUnmanaged(RawLookup) = .empty;
    defer {
        for (lookups.items) |l| alloc.free(l.bytes);
        lookups.deinit(alloc);
    }

    for (singles) |sp| {
        var lk: std.ArrayListUnmanaged(u8) = .empty;
        try wrU16(&lk, alloc, 1); // LookupType 1
        try wrU16(&lk, alloc, 0); // flags
        try wrU16(&lk, alloc, 1); // subTableCount
        try wrU16(&lk, alloc, 8); // subtable offset
        // SingleSubst format 2: format=2, covOff=8, glyphCount=1, subst=[to_gid]
        try wrU16(&lk, alloc, 2);
        try wrU16(&lk, alloc, 8);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, sp.to_gid);
        const cov = try writeCoverage(alloc, &[_]u16{sp.from_gid});
        defer alloc.free(cov);
        try wrBytes(&lk, alloc, cov);
        try lookups.append(alloc, .{ .script = sp.script, .feat = sp.feat, .bytes = try lk.toOwnedSlice(alloc) });
    }

    for (multis) |mp| {
        var lk: std.ArrayListUnmanaged(u8) = .empty;
        try wrU16(&lk, alloc, 2); // LookupType 2
        try wrU16(&lk, alloc, 0);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 8);
        // MultipleSubst format 1: format=1, covOff=8, seqCount=1, seqOff[0]=14 (cov is 6 B)
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 8);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 14);
        const cov = try writeCoverage(alloc, &[_]u16{mp.from_gid});
        defer alloc.free(cov);
        try wrBytes(&lk, alloc, cov); // 6 bytes -> offset 14
        // Sequence at subtable offset 14
        try wrU16(&lk, alloc, 2);
        try wrU16(&lk, alloc, mp.out0);
        try wrU16(&lk, alloc, mp.out1);
        try lookups.append(alloc, .{ .script = mp.script, .feat = mp.feat, .bytes = try lk.toOwnedSlice(alloc) });
    }

    for (ligs) |lg| {
        var lk: std.ArrayListUnmanaged(u8) = .empty;
        try wrU16(&lk, alloc, 4); // LookupType 4
        try wrU16(&lk, alloc, if (lg.ignore_marks) 0x0008 else 0);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 8);
        // LigatureSubst format 1: format=1, covOff=8, ligSetCount=1, ligSetOff[0]=14 (cov is 6 B)
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 8);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 14);
        const cov = try writeCoverage(alloc, &[_]u16{lg.comp0});
        defer alloc.free(cov);
        try wrBytes(&lk, alloc, cov); // 6 bytes -> offset 14
        // LigatureSet: ligCount=1, ligOff[0]=4
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 4);
        // Ligature table: ligGlyph, componentCount, componentGIDs...
        const comp_cnt: u16 = if (lg.comp2 != 0) 3 else 2;
        try wrU16(&lk, alloc, lg.lig_gid);
        try wrU16(&lk, alloc, comp_cnt);
        try wrU16(&lk, alloc, lg.comp1);
        if (lg.comp2 != 0) try wrU16(&lk, alloc, lg.comp2);
        try lookups.append(alloc, .{ .script = lg.script, .feat = lg.feat, .bytes = try lk.toOwnedSlice(alloc) });
    }

    return assembleOtlHeader(alloc, lookups.items);
}

fn buildGpos(alloc: std.mem.Allocator, marks: []const GposMarkSpec) ![]u8 {
    var lookups: std.ArrayListUnmanaged(RawLookup) = .empty;
    defer {
        for (lookups.items) |l| alloc.free(l.bytes);
        lookups.deinit(alloc);
    }

    for (marks) |mk| {
        var lk: std.ArrayListUnmanaged(u8) = .empty;
        try wrU16(&lk, alloc, if (mk.mark_to_mark) 6 else 4);
        try wrU16(&lk, alloc, 0);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 8);
        // MarkBasePos / MarkMarkPos format 1 (12 byte header):
        // markCov=12 (6B), baseCov=18 (6B), classCount=1, markArray=24 (12B), baseArray=36
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 12);
        try wrU16(&lk, alloc, 18);
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 24);
        try wrU16(&lk, alloc, 36);

        const mcov = try writeCoverage(alloc, &[_]u16{mk.mark_gid});
        defer alloc.free(mcov);
        const bcov = try writeCoverage(alloc, &[_]u16{mk.base_gid});
        defer alloc.free(bcov);
        try wrBytes(&lk, alloc, mcov); // +6 -> off 18
        try wrBytes(&lk, alloc, bcov); // +6 -> off 24

        // MarkArray at 24 (12 bytes -> off 36): markCount=1, { class=0, ancOff=6 }, Anchor format 1
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 0);
        try wrU16(&lk, alloc, 6);
        try wrU16(&lk, alloc, 1);
        try wrI16(&lk, alloc, mk.mark_ax);
        try wrI16(&lk, alloc, mk.mark_ay);

        // BaseArray / Mark2Array (10 bytes): count=1, ancOff[0]=4, Anchor format 1
        try wrU16(&lk, alloc, 1);
        try wrU16(&lk, alloc, 4);
        try wrU16(&lk, alloc, 1);
        try wrI16(&lk, alloc, mk.base_ax);
        try wrI16(&lk, alloc, mk.base_ay);

        const feat: [4]u8 = if (mk.mark_to_mark) "mkmk".* else "mark".*;
        try lookups.append(alloc, .{ .script = mk.script, .feat = feat, .bytes = try lk.toOwnedSlice(alloc) });
    }

    return assembleOtlHeader(alloc, lookups.items);
}

/// Assemble a complete valid TrueType/OpenType SFNT font blob in `alloc`.
pub fn buildTestFont(
    alloc: std.mem.Allocator,
    glyphs: []const GlyphDef,
    singles: []const GsubSingleSpec,
    ligs: []const GsubLigSpec,
    multis: []const GsubMultiSpec,
    marks: []const GposMarkSpec,
) ![]u8 {
    const Tbl = struct { tag: [4]u8, data: []u8 };
    var tables: std.ArrayListUnmanaged(Tbl) = .empty;
    defer {
        for (tables.items) |t| alloc.free(t.data);
        tables.deinit(alloc);
    }

    const gl = try buildGlyfAndLoca(alloc, glyphs);
    errdefer {
        alloc.free(gl.glyf);
        alloc.free(gl.loca);
    }

    try tables.append(alloc, .{ .tag = "cmap".*, .data = try buildCmap(alloc, glyphs) });
    try tables.append(alloc, .{ .tag = "glyf".*, .data = gl.glyf });
    try tables.append(alloc, .{ .tag = "head".*, .data = try buildHead(alloc) });
    try tables.append(alloc, .{ .tag = "hhea".*, .data = try buildHhea(alloc, @intCast(glyphs.len)) });
    try tables.append(alloc, .{ .tag = "hmtx".*, .data = try buildHmtx(alloc, glyphs) });
    try tables.append(alloc, .{ .tag = "loca".*, .data = gl.loca });
    try tables.append(alloc, .{ .tag = "maxp".*, .data = try buildMaxp(alloc, @intCast(glyphs.len)) });
    try tables.append(alloc, .{ .tag = "GDEF".*, .data = try buildGdef(alloc, glyphs) });
    if (singles.len > 0 or ligs.len > 0 or multis.len > 0) {
        try tables.append(alloc, .{ .tag = "GSUB".*, .data = try buildGsub(alloc, singles, ligs, multis) });
    }
    if (marks.len > 0) {
        try tables.append(alloc, .{ .tag = "GPOS".*, .data = try buildGpos(alloc, marks) });
    }

    std.mem.sort(Tbl, tables.items, {}, struct {
        fn lt(_: void, a: Tbl, b: Tbl) bool {
            return std.mem.order(u8, &a.tag, &b.tag) == .lt;
        }
    }.lt);

    const ntbl: u16 = @intCast(tables.items.len);
    var sfnt: std.ArrayListUnmanaged(u8) = .empty;
    try wrU32(&sfnt, alloc, 0x00010000);
    try wrU16(&sfnt, alloc, ntbl);
    var sr: u16 = 16;
    var es: u16 = 0;
    while (sr * 2 <= ntbl * 16) : (sr *= 2) {
        es += 1;
    }
    try wrU16(&sfnt, alloc, sr);
    try wrU16(&sfnt, alloc, es);
    try wrU16(&sfnt, alloc, ntbl * 16 - sr);

    var cur_off: u32 = 12 + @as(u32, ntbl) * 16;
    for (tables.items) |t| {
        const padded = (t.data.len + 3) & ~@as(usize, 3);
        try wrBytes(&sfnt, alloc, &t.tag);
        try wrU32(&sfnt, alloc, 0);
        try wrU32(&sfnt, alloc, cur_off);
        try wrU32(&sfnt, alloc, @intCast(t.data.len));
        cur_off += @intCast(padded);
    }
    for (tables.items) |t| {
        try wrBytes(&sfnt, alloc, t.data);
        const rem = (4 - (t.data.len & 3)) & 3;
        try sfnt.appendNTimes(alloc, 0, rem);
    }
    return sfnt.toOwnedSlice(alloc);
}
