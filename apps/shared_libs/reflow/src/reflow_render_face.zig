//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Coverage-based fallback-face resolution for the render pass (RA8FW-1221).
//! Port of reflow_render_face.c: the pure picker
//! (priv_reflow_render_pick_face) and the cmap-backed entry
//! (priv_reflow_render_face_for) keep their C ABI so reflow_render.c calls
//! them unchanged and the CMake suite can drive the picker from a table with
//! no font file. The cmap question goes through the vendored stb_truetype
//! via extern declarations, per the repo convention (no cImport). The
//! stbtt_fontinfo mirror exists only for faces-array pointer arithmetic;
//! its layout is checked against the C struct by the ABI-probe test below.

const std = @import("std");

extern fn priv_reflow_render_is_blank_cp(cp: c_int) bool;
extern fn stbtt_FindGlyphIndex(info: *const anyopaque, unicode_codepoint: c_int) c_int;

/// Mirror of the vendored stbtt__buf (stb_truetype.h).
const StbttBuf = extern struct {
    data: ?[*]const u8,
    cursor: c_int,
    size: c_int,
};

/// Mirror of the vendored stbtt_fontinfo (stb_truetype.h, RA8 local patch
/// tree: the data_size bounds-check field sits between fontstart and
/// numGlyphs). Only the struct size participates in pointer arithmetic
/// here; no field is ever read from Zig.
const StbttFontinfo = extern struct {
    userdata: ?*anyopaque,
    data: ?[*]const u8,
    fontstart: c_int,
    data_size: c_int,
    num_glyphs: c_int,
    loca: c_int,
    head: c_int,
    glyf: c_int,
    hhea: c_int,
    hmtx: c_int,
    kern: c_int,
    gpos: c_int,
    svg: c_int,
    index_map: c_int,
    index_to_loc_format: c_int,
    cff: StbttBuf,
    charstrings: StbttBuf,
    gsubrs: StbttBuf,
    subrs: StbttBuf,
    fontdicts: StbttBuf,
    fdselect: StbttBuf,
};

/// Face set handed to the coverage probe during one render pass: the
/// per-render stbtt_fontinfo array, default face at 0, registered at 1..N.
const FaceCov = extern struct {
    faces: ?[*]const StbttFontinfo,
};

/// C ABI of priv_reflow_face_has_glyph_fn (reflow_internal.h).
const HasGlyphFn = fn (?*const anyopaque, u8, c_int) callconv(.c) bool;

/// Answer the coverage probe from a render face array: asks the face's own
/// cmap through stbtt_FindGlyphIndex, where glyph index 0 is stb's "this
/// face has no glyph for that code point" answer.
fn faceHasGlyph(ctx: ?*const anyopaque, face_idx: u8, cp: c_int) callconv(.c) bool {
    const cov: *const FaceCov = @ptrCast(@alignCast(ctx orelse return false));
    const faces = cov.faces orelse return false;
    return stbtt_FindGlyphIndex(&faces[face_idx], cp) != 0;
}

/// Pick the face that draws cp out of one render pass's face set (see
/// reflow_internal.h for the contract). No probe, a single face, a bogus
/// index, or a code point that must stay blank: the run keeps the face the
/// layout pass chose.
export fn priv_reflow_render_pick_face(
    has_glyph: ?*const HasGlyphFn,
    ctx: ?*const anyopaque,
    face_count: u8,
    primary: u8,
    cp: c_int,
) u8 {
    if (has_glyph == null or face_count <= 1 or primary >= face_count or
        priv_reflow_render_is_blank_cp(cp))
    {
        return primary;
    }
    if (has_glyph.?(ctx, primary, cp)) {
        return primary;
    }
    if (primary != 0 and has_glyph.?(ctx, 0, cp)) {
        return 0; // The engine's bound default face is the first fallback.
    }
    var k: u8 = 1;
    while (k < face_count) : (k += 1) {
        if (k != primary and has_glyph.?(ctx, k, cp)) {
            return k;
        }
    }
    return primary; // Nothing covers it: the missing-glyph box draws here.
}

/// Wraps priv_reflow_render_pick_face with the cmap-backed coverage probe,
/// so the render pass never has to know how coverage is answered (see
/// reflow_internal.h).
export fn priv_reflow_render_face_for(
    faces: ?[*]const StbttFontinfo,
    face_count: u8,
    primary: u8,
    cp: c_int,
) u8 {
    const cov = FaceCov{ .faces = faces };
    return priv_reflow_render_pick_face(faceHasGlyph, &cov, face_count, primary, cp);
}

test "face picker keeps the layout pick when fallback is disabled" {
    // No probe: the run keeps its face.
    try std.testing.expectEqual(@as(u8, 2), priv_reflow_render_pick_face(null, null, 3, 2, 65));
    // A face set of one: no coverage probe at all.
    try std.testing.expectEqual(@as(u8, 0), priv_reflow_render_pick_face(&faceHasGlyph, null, 1, 0, 65));
    // A primary past the end of the set is bogus: keep it.
    try std.testing.expectEqual(@as(u8, 5), priv_reflow_render_pick_face(&faceHasGlyph, null, 3, 5, 65));
}

test "face picker falls back by coverage" {
    const harness = struct {
        var covered: [4]bool = .{ false, false, false, false };

        fn probe(ctx: ?*const anyopaque, idx: u8, cp: c_int) callconv(.c) bool {
            _ = ctx;
            _ = cp;
            return covered[idx];
        }
    };
    const ctx: ?*const anyopaque = null;

    // The run's own face covers it: no change.
    harness.covered = .{ false, true, false, false };
    try std.testing.expectEqual(@as(u8, 1), priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));

    // The engine's bound default face (0) is the first fallback.
    harness.covered = .{ true, false, false, false };
    try std.testing.expectEqual(@as(u8, 0), priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));

    // Neither the run's face nor the default covers it: the first
    // registered face that does, by index.
    harness.covered = .{ false, false, true, false };
    try std.testing.expectEqual(@as(u8, 2), priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));

    // Nothing covers it: the missing-glyph box draws at the run's face.
    harness.covered = .{ false, false, false, false };
    try std.testing.expectEqual(@as(u8, 1), priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));
}

test "stbtt_fontinfo mirror matches the C struct size" {
    // Checked against the vendored stb_truetype.h layout by the repo's ABI
    // probe: LP64 sizes are asserted here and against C in the CMake suite.
    // The mirror must stay size-identical for faces-array indexing.
    try std.testing.expectEqual(@as(usize, 168), @sizeOf(StbttFontinfo));
}
