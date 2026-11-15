//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Page rasteriser for the reflow engine (RA8FW-1222): the port of
//! src/reflow_render.c. Walks one page's glyph slice, rasterises each glyph
//! through the vendored stb_truetype (extern, no cImport), blits the alpha-8
//! mask into the framebuffer bound by ra8_gfx_init(), draws the missing-glyph
//! (tofu) box for code points the resolved face cannot cover, and decodes the
//! page's image boxes through the engine's loader (cover-wrapper SVG hrefs are
//! unwrapped first; shape SVGs and rasters go through the SVG / imgdec seam).
//! The public ABI (reflow_render_page, reflow_render_page_at,
//! reflow_set_glyph_atlas and the priv_reflow_render_* helpers) keeps its C
//! shape so the CMake suite and the C callers link unchanged.

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const picker = @import("reflow_render_face.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowGlyph = abi.ReflowGlyph;
const ReflowPage = abi.ReflowPage;
const ReflowImageBox = abi.ReflowImageBox;
const ReflowFace = abi.ReflowFace;
const ReflowLoaderFn = abi.ReflowLoaderFn;
const StbttFontinfo = picker.StbttFontinfo;

// ---------------------------------------------------------------------------
// Internal sizing constants (no magic numbers).
// ---------------------------------------------------------------------------

const k_priv_alpha_threshold: u8 = 96;
const k_priv_underline_offset_px: i32 = 2;
const k_priv_glyph_dim_max: u32 = 192;
const k_priv_svg_href_max: usize = 256;
const k_priv_glyph_mode_aa: u16 = 0;

const k_priv_tofu_min_px: i32 = 3;
const k_priv_tofu_inset_den: i32 = 8;
const k_priv_tofu_adv_den: i32 = 2;
const k_priv_tofu_asc_num: i32 = 2;
const k_priv_tofu_asc_den: i32 = 3;
const k_priv_tofu_h_num: i32 = 3;
const k_priv_tofu_h_den: i32 = 4;
const k_priv_tofu_edges: i32 = 2;

const k_reflow_style_underline: u8 = 1 << 2;
const k_reflow_face_shift: u5 = 4;
const k_reflow_face_mask: u32 = 0x0F;
const k_reflow_max_faces: usize = 8;

const k_ra8_ok: c_int = 0;
const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_not_initialized: c_int = 0x10F;
const k_ra8_err_out_of_range: c_int = 0x208;
const k_ra8_err_validation_failed: c_int = 0x501;
const k_ra8_err_null_ptr: c_int = 0x504;

/// Code points that must draw nothing even when the face has no glyph
/// (reflow_render.c priv_render_blank_cp_t): space and the zero-width format
/// characters carry no ink, so a face that maps them to glyph 0 stays blank
/// rather than gaining a missing-glyph box.
fn isBlankCodePoint(cp: c_int) bool {
    return switch (@as(u32, @bitCast(cp))) {
        0x09, 0x0A, 0x0D, 0x20, 0xA0, 0x200B, 0x200C, 0x200D, 0x2060, 0xFEFF => true,
        else => false,
    };
}

fn needsTofu(glyph_index: c_int, cp: c_int) bool {
    return (glyph_index == 0) and (!isBlankCodePoint(cp));
}

// ---------------------------------------------------------------------------
// ABI mirrors.
// ---------------------------------------------------------------------------

/// Mirror of ra8_glyph_key_t (libs/ra8_mem/inc/ra8_glyph_atlas.h): the
/// padding-free byte-wise cache key.
const GlyphKey = extern struct {
    glyph_id: u32,
    face_id: u16,
    size_px: u16,
    mode: u16,
    reserved: u16,
};

/// Mirror of ra8_glyph_dims_t.
const GlyphDims = extern struct {
    w: u16,
    h: u16,
};

/// Mirror of ra8_glyph_t: a pinned view of one cached glyph bitmap.
const GlyphView = extern struct {
    bitmap: ?[*]const u8,
    width: u16,
    height: u16,
};

/// Mirror of ra8_keycache_cell_t (libs/ra8_mem/inc/ra8_keycache.h): the
/// per-cell link metadata the keyed-LRU cache keeps.
const KeycacheCell = extern struct {
    prev: i32,
    next: i32,
    hash_next: i32,
    pin_count: u16,
    seg: u8,
    valid: u8,
};

/// Mirror of reflow_glyph_atlas_storage_t (inc/reflow_api.h): caller-owned
/// backing storage for the render-path glyph cache, minus the render seam the
/// engine supplies itself.
const AtlasStorage = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    meta: ?[*]KeycacheCell,
    keys: ?[*]GlyphKey,
    dims: ?[*]GlyphDims,
    buckets: ?[*]i32,
    bucket_count: u32,
};

/// Mirror of priv_reflow_tofu_rect_t (reflow_internal.h): the missing-glyph
/// box geometry relative to the glyph pen.
const TofuRect = extern struct {
    x_off: i32,
    y_off: i32,
    w: i32,
    h: i32,
};

/// Render context stashed before each ra8_glyph_atlas_get so the
/// render-on-miss callback can rasterise the right glyph without recomputing
/// the box (reflow_render.c priv_glyph_render_ctx_t). Single instance:
/// rendering is single-threaded.
const GlyphRenderCtx = extern struct {
    font: ?*const StbttFontinfo,
    scale: f32,
    w: c_int,
    h: c_int,
};

/// Mirror of ra8_glyph_atlas_cfg_t (libs/ra8_mem/inc/ra8_glyph_atlas.h):
/// caller storage plus this engine's rasteriser as the render-on-miss callback.
const GlyphRenderFn = *const fn (?*anyopaque, ?*const GlyphKey, ?[*]u8, u32, ?*u16, ?*u16) callconv(.c) c_int;
const AtlasCfg = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    meta: ?[*]KeycacheCell,
    keys: ?[*]GlyphKey,
    dims: ?[*]GlyphDims,
    buckets: ?[*]i32,
    bucket_count: u32,
    render: ?GlyphRenderFn,
    render_ctx: ?*anyopaque,
};

// ---------------------------------------------------------------------------
// Extern seam: the vendored stb_truetype and the cross-library entry points.
// ---------------------------------------------------------------------------

extern fn stbtt_GetFontOffsetForIndex(data: [*]const u8, index: c_int) c_int;
extern fn stbtt_InitFont(info: *StbttFontinfo, data: [*]const u8, data_size: c_int, offset: c_int) c_int;
extern fn stbtt_ScaleForPixelHeight(info: *const StbttFontinfo, height: f32) f32;
extern fn stbtt_GetCodepointHMetrics(info: *const StbttFontinfo, codepoint: c_int, advance: *c_int, lsb: *c_int) void;
extern fn stbtt_GetFontVMetrics(info: *const StbttFontinfo, ascent: *c_int, descent: *c_int, line_gap: *c_int) void;
extern fn stbtt_GetCodepointBitmapBox(info: *const StbttFontinfo, codepoint: c_int, scale_x: f32, scale_y: f32, ix0: *c_int, iy0: *c_int, ix1: *c_int, iy1: *c_int) void;
extern fn stbtt_MakeCodepointBitmap(info: *const StbttFontinfo, output: [*]u8, out_w: c_int, out_h: c_int, out_stride: c_int, scale_x: f32, scale_y: f32, codepoint: c_int) void;

extern fn ra8_gfx_pixel(x: c_int, y: c_int, color: u32) c_int;
extern fn ra8_glyph_atlas_init(atlas: *anyopaque, cfg: *const AtlasCfg) c_int;
extern fn ra8_glyph_atlas_get(atlas: *anyopaque, key: *const GlyphKey, out_glyph: *GlyphView) c_int;
extern fn ra8_glyph_atlas_put(atlas: *anyopaque, bitmap: [*]const u8) c_int;
extern fn ra8_svg_is_svg(bytes: [*]const u8, len: usize) bool;
extern fn ra8_svg_image_href(svg: [*]const u8, len: usize, out_off: *usize, out_len: *usize) c_int;
extern fn ra8_svg_render(svg: [*]const u8, len: usize, x: c_int, y: c_int, w: c_int, h: c_int) c_int;
extern fn ra8_img_decode_blit(arena: *anyopaque, bytes: [*]const u8, len: usize, dst_x: c_int, dst_y: c_int, box_w: c_int, box_h: c_int, out_w: ?*i32, out_h: ?*i32) c_int;

/// The face picker, exported by src/reflow_render_face.zig (RA8FW-1221) and
/// linked by symbol, exactly as the C render linked it before the port.
extern fn priv_reflow_render_face_for(faces: ?*const anyopaque, face_count: u8, primary: u8, cp: c_int) u8;

// ---------------------------------------------------------------------------
// File-static render state (single-threaded, like the C module).
// ---------------------------------------------------------------------------

/// Shared with the bound glyph atlas: set by the blit path immediately before
/// every ra8_glyph_atlas_get so a miss rasterises the right glyph at the known
/// size. Not re-entrant.
var s_glyph_render_ctx: GlyphRenderCtx = .{ .font = null, .scale = 0, .w = 0, .h = 0 };

/// Fixed rasterisation storage for the heap-free direct path. Glyphs larger
/// than the mask are skipped (not truncated), exactly as before the cache
/// existed.
var s_glyph_mask: [k_priv_glyph_dim_max * k_priv_glyph_dim_max]u8 = undefined;

/// One fontinfo per face (static because render is single-threaded; keeps the
/// ~1.4 KB off the stack). Default at 0, embedded at 1..N.
var s_faces: [1 + k_reflow_max_faces]StbttFontinfo = std.mem.zeroes([1 + k_reflow_max_faces]StbttFontinfo);

// ---------------------------------------------------------------------------
// Glyph-atlas render-on-miss callback.
// ---------------------------------------------------------------------------

/// Rasterise one glyph into an atlas cell (see reflow_render.c
/// internal_atlas_render_glyph). The font, scale, and extent come from
/// s_glyph_render_ctx, already computed and validated by the blit path before
/// the get; the code point rides in key->glyph_id. stbtt_MakeCodepointBitmap
/// runs with stride == w exactly as the direct path, so a cached bitmap is
/// byte-identical to a freshly rasterised one.
fn atlasRenderGlyph(ctx: ?*anyopaque, key: ?*const GlyphKey, cell: ?[*]u8, cell_bytes: u32, out_w: ?*u16, out_h: ?*u16) callconv(.c) c_int {
    const rc: *const GlyphRenderCtx = @ptrCast(@alignCast(ctx orelse return k_ra8_err_invalid_size));
    const k = key orelse return k_ra8_err_invalid_size;
    const c = cell orelse return k_ra8_err_invalid_size;
    const total = @as(usize, @intCast(rc.w)) * @as(usize, @intCast(rc.h));
    if (total > cell_bytes) {
        return k_ra8_err_invalid_size; // Too big to cache -> caller blits directly.
    }
    const font = rc.font orelse return k_ra8_err_invalid_size;
    stbtt_MakeCodepointBitmap(font, c, rc.w, rc.h, rc.w, rc.scale, rc.scale, @bitCast(k.glyph_id));
    out_w.?.* = @intCast(rc.w);
    out_h.?.* = @intCast(rc.h);
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Font / glyph plumbing.
// ---------------------------------------------------------------------------

/// Initialise an stbtt_fontinfo from the engine's font blob.
fn initFont(engine: *const ReflowEngine, out_font: *StbttFontinfo) c_int {
    const data = engine.font_data orelse return k_ra8_err_validation_failed;
    const offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) {
        return k_ra8_err_validation_failed;
    }
    if (stbtt_InitFont(out_font, data, @intCast(engine.font_len), offset) == 0) {
        return k_ra8_err_validation_failed;
    }
    return k_ra8_ok;
}

/// Walk one glyph bitmap and blit any sufficiently-covered pixels into the
/// bound framebuffer. Clipping is left to ra8_gfx_pixel, as in C.
fn blitAlphaMask(g: *const ReflowGlyph, bitmap: [*]const u8, w: c_int, h: c_int, xoff: c_int, yoff: c_int, ox: i32, oy: i32) void {
    const wu: usize = @intCast(w);
    const hu: usize = @intCast(h);
    var row: usize = 0;
    while (row < hu) : (row += 1) {
        var col: usize = 0;
        while (col < wu) : (col += 1) {
            const alpha = bitmap[row * wu + col];
            if (alpha < k_priv_alpha_threshold) {
                continue;
            }
            const px = g.x + @as(i32, @intCast(col)) + @as(i32, @intCast(xoff)) + ox;
            const py = g.y + @as(i32, @intCast(row)) + @as(i32, @intCast(yoff)) + oy;
            _ = ra8_gfx_pixel(px, py, g.color);
        }
    }
}

/// Draw the underline strip for a link glyph: one pixel row, `advance` pixels
/// wide, k_priv_underline_offset_px below the baseline.
fn drawUnderline(font: *const StbttFontinfo, g: *const ReflowGlyph, scale: f32, ox: i32, oy: i32) void {
    var advance_units: c_int = 0;
    var lsb: c_int = 0;
    stbtt_GetCodepointHMetrics(font, g.cp, &advance_units, &lsb);
    const advance: i32 = @intFromFloat(@as(f32, @floatFromInt(advance_units)) * scale);
    const y_under = g.y + k_priv_underline_offset_px + oy;
    var i: i32 = 0;
    while (i < advance) : (i += 1) {
        _ = ra8_gfx_pixel(g.x + i + ox, y_under, g.color);
    }
}

/// Rasterise one glyph into the fixed mask and blit it: the heap-free fallback
/// path used when no glyph cache is bound or a glyph is too large to cache.
fn glyphRenderDirect(font: *const StbttFontinfo, scale: f32, g: *const ReflowGlyph, w: c_int, h: c_int, x0: c_int, y0: c_int, ox: i32, oy: i32) void {
    const total = @as(usize, @intCast(w)) * @as(usize, @intCast(h));
    if (total <= s_glyph_mask.len) {
        stbtt_MakeCodepointBitmap(font, &s_glyph_mask, w, h, w, scale, scale, g.cp);
        blitAlphaMask(g, &s_glyph_mask, w, h, x0, y0, ox, oy);
    }
}

/// Draw one glyph through the bound glyph atlas (cache hit or render-miss),
/// keyed by (face, size, code point, mode). Any atlas error (oversized glyph,
/// or every cell pinned) returns false so the caller falls back to the direct
/// path -- the cache is a pure optimisation and never changes output.
fn glyphRenderCached(atlas: *anyopaque, face_id: u8, font: *const StbttFontinfo, scale: f32, g: *const ReflowGlyph, w: c_int, h: c_int, x0: c_int, y0: c_int, ox: i32, oy: i32) bool {
    s_glyph_render_ctx = .{ .font = font, .scale = scale, .w = w, .h = h };
    const key: GlyphKey = .{
        .glyph_id = @bitCast(g.cp),
        .face_id = face_id,
        .size_px = g.font_px,
        .mode = k_priv_glyph_mode_aa,
        .reserved = 0,
    };
    var glyph: GlyphView = undefined;
    if (ra8_glyph_atlas_get(atlas, &key, &glyph) != k_ra8_ok) {
        return false; // Uncacheable (oversized / full) -> caller blits directly.
    }
    const bitmap = glyph.bitmap orelse return false;
    blitAlphaMask(g, bitmap, glyph.width, glyph.height, x0, y0, ox, oy);
    _ = ra8_glyph_atlas_put(atlas, bitmap);
    return true;
}

/// Draw the outline of one rectangle, one pixel thick, in the glyph colour.
/// The rectangle is relative to the glyph pen; clipping is ra8_gfx_pixel's.
fn drawBoxOutline(g: *const ReflowGlyph, r: *const TofuRect, ox: i32, oy: i32) void {
    const x = g.x + r.x_off + ox;
    const y = g.y + r.y_off + oy;
    var i: i32 = 0;
    while (i < r.w) : (i += 1) {
        _ = ra8_gfx_pixel(x + i, y, g.color);
        _ = ra8_gfx_pixel(x + i, y + r.h - 1, g.color);
    }
    var j: i32 = 1;
    while (j < r.h - 1) : (j += 1) {
        _ = ra8_gfx_pixel(x, y + j, g.color);
        _ = ra8_gfx_pixel(x + r.w - 1, y + j, g.color);
    }
}

/// Draw the missing-glyph (tofu) box for a code point the face cannot draw.
/// Metrics come from the face itself so the box matches the surrounding text
/// size; the exported tofu_rect helper supplies the fallbacks.
fn drawTofu(font: *const StbttFontinfo, g: *const ReflowGlyph, scale: f32, ox: i32, oy: i32) void {
    var advance_units: c_int = 0;
    var lsb: c_int = 0;
    stbtt_GetCodepointHMetrics(font, g.cp, &advance_units, &lsb);
    var ascent_units: c_int = 0;
    var descent_units: c_int = 0;
    var line_gap: c_int = 0;
    stbtt_GetFontVMetrics(font, &ascent_units, &descent_units, &line_gap);

    var rect: TofuRect = .{ .x_off = 0, .y_off = 0, .w = 0, .h = 0 };
    if (priv_reflow_render_tofu_rect(
        @intFromFloat(@as(f32, @floatFromInt(advance_units)) * scale),
        @intFromFloat(@as(f32, @floatFromInt(ascent_units)) * scale),
        @intCast(g.font_px),
        &rect,
    )) {
        drawBoxOutline(g, &rect, ox, oy);
    }
}

/// Rasterise one glyph at its baseline position into the bound framebuffer.
/// The bitmap-box query reports the extent and stb baseline offsets, so the
/// bitmap lands in the correct row above the baseline and columns place
/// accents/descenders correctly. When an atlas is bound the bitmap is fetched
/// from (or rendered once into) the cache; otherwise rasterised directly.
/// Underlined link glyphs additionally get an underline strip.
fn blitGlyph(atlas: ?*anyopaque, face_id: u8, font: *const StbttFontinfo, g: *const ReflowGlyph, ox: i32, oy: i32) void {
    const scale = stbtt_ScaleForPixelHeight(font, @floatFromInt(g.font_px));
    if (needsTofu(stbtt_FindGlyphIndex(font, g.cp), g.cp)) {
        drawTofu(font, g, scale, ox, oy);
        if ((g.style & k_reflow_style_underline) != 0) {
            drawUnderline(font, g, scale, ox, oy);
        }
        return; // The face has no glyph: the box IS the rendering.
    }
    var x0: c_int = 0;
    var y0: c_int = 0;
    var x1: c_int = 0;
    var y1: c_int = 0;
    stbtt_GetCodepointBitmapBox(font, g.cp, scale, scale, &x0, &y0, &x1, &y1);
    const w = x1 - x0;
    const h = y1 - y0;
    // w and h are the glyph bitmap-box extents: a code point is either inked
    // (w > 0 and h > 0) or empty (a zero box, e.g. U+0020 space); the mixed
    // states never occur for a real font glyph.
    if ((w > 0) and (h > 0)) {
        var drawn = false;
        if (atlas) |a| {
            drawn = glyphRenderCached(a, face_id, font, scale, g, w, h, x0, y0, ox, oy);
        }
        if (!drawn) {
            glyphRenderDirect(font, scale, g, w, h, x0, y0, ox, oy);
        }
    }
    if ((g.style & k_reflow_style_underline) != 0) {
        drawUnderline(font, g, scale, ox, oy);
    }
}

/// stbtt_FindGlyphIndex, declared at this file's fontinfo type (the picker's
/// extern uses an opaque view of the same symbol).
extern fn stbtt_FindGlyphIndex(info: *const StbttFontinfo, unicode_codepoint: c_int) c_int;

// ---------------------------------------------------------------------------
// Image boxes.
// ---------------------------------------------------------------------------

/// Render an SVG image box: unwrap a cover `<image>` href, else draw shapes.
/// The href string is copied into a stack buffer before re-invoking the
/// loader (the loader call may overwrite the SVG buffer); hrefs longer than
/// k_priv_svg_href_max - 1 bytes are silently dropped.
fn renderSvg(engine: *const ReflowEngine, svg: [*]const u8, len: usize, x: i32, y: i32, w: i32, h: i32) void {
    var hoff: usize = 0;
    var hlen: usize = 0;
    if (ra8_svg_image_href(svg, len, &hoff, &hlen) != k_ra8_ok) {
        _ = ra8_svg_render(svg, len, x, y, w, h);
        return;
    }
    if (hlen >= k_priv_svg_href_max) {
        return;
    }
    var href: [k_priv_svg_href_max]u8 = undefined;
    @memset(&href, 0);
    @memcpy(href[0..hlen], svg[hoff .. hoff + hlen]);
    const loader = engine.img_loader orelse return;
    var rbytes: [*]const u8 = undefined;
    var rlen: usize = 0;
    if (loader(engine.img_loader_ctx, &href, @intCast(hlen), &rbytes, &rlen) == k_ra8_ok) {
        const arena = engine.img_arena orelse return;
        _ = ra8_img_decode_blit(arena, rbytes, rlen, x, y, w, h, null, null);
    }
}

/// Render one image box: SVG-route or raster-decode at the page offset. The
/// source href lives at engine->text_pool[src_off .. src_len]; a loader
/// failure leaves a blank gap in the framebuffer rather than aborting the
/// page render.
fn renderOneImage(engine: *const ReflowEngine, box: *const ReflowImageBox, ox: i32, oy: i32) void {
    const loader = engine.img_loader orelse return;
    const off: usize = box.src_off;
    if (off > engine.text_pool.len or box.src_len > engine.text_pool.len - off) {
        return; // Out-of-pool href (contract violation): blank gap, as C's OOB read would be undefined.
    }
    const href: [*]const u8 = @ptrCast(&engine.text_pool[off]);
    var bytes: [*]const u8 = undefined;
    var blen: usize = 0;
    if (loader(engine.img_loader_ctx, href, @intCast(box.src_len), &bytes, &blen) != k_ra8_ok) {
        return;
    }
    const loaded = bytes;
    const bx = box.x + ox;
    const by = box.y + oy;
    if (ra8_svg_is_svg(loaded, blen)) {
        renderSvg(engine, loaded, blen, bx, by, box.w, box.h);
        return;
    }
    const arena = engine.img_arena orelse return;
    _ = ra8_img_decode_blit(arena, loaded, blen, bx, by, box.w, box.h, null, null);
}

/// Decode and blit every laid-out image that belongs to one page. No-op when
/// the engine binds no image loader or arena, so loader-less callers share the
/// same render path. Failed boxes are skipped, leaving a blank gap.
fn renderImages(engine: *const ReflowEngine, page_idx: u32, ox: i32, oy: i32) void {
    if ((engine.img_loader == null) or (engine.img_arena == null)) {
        return;
    }
    var i: u32 = 0;
    while (i < engine.image_box_count) : (i += 1) {
        const box = &engine.image_boxes[i];
        if (box.page_index == page_idx) {
            renderOneImage(engine, box, ox, oy);
        }
    }
}

// ---------------------------------------------------------------------------
// Page render.
// ---------------------------------------------------------------------------

/// Build the per-render stbtt_fontinfo set: default at 0, faces at 1..N. A
/// registered blob that fails init (already validated at register time, so
/// unexpected) is replaced by a copy of the default face so every slot is
/// always initialised. The write loop clamps at the array bound; face_count
/// beyond k_reflow_max_faces is a contract violation that C left undefined.
fn initFaces(engine: *const ReflowEngine, faces: *[1 + k_reflow_max_faces]StbttFontinfo, out_n: *u8) c_int {
    const err = initFont(engine, &faces[0]);
    if (err != k_ra8_ok) {
        return err;
    }
    var k: u8 = 0;
    while (k < engine.face_count and k < k_reflow_max_faces) : (k += 1) {
        const blob = engine.faces[k].blob orelse {
            faces[k + 1] = faces[0];
            continue;
        };
        const offset = stbtt_GetFontOffsetForIndex(blob, 0);
        if ((offset < 0) or (stbtt_InitFont(&faces[k + 1], blob, @intCast(engine.faces[k].len), offset) == 0)) {
            faces[k + 1] = faces[0]; // graceful: bad face -> default
        }
    }
    out_n.* = @intCast(@min(@as(u32, engine.face_count), k_reflow_max_faces) + 1);
    return k_ra8_ok;
}

/// Shared render body for one page, offsetting every element by (ox, oy):
/// validates engine and page, builds the face set, walks the page's glyph
/// slice (clamping any out-of-range face index to the default face and
/// re-resolving it by coverage so a code point the run's face cannot draw is
/// taken by another registered face), then blits the page's image boxes.
fn renderPage(engine: *const ReflowEngine, page_idx: u32, ox: i32, oy: i32) c_int {
    if (engine.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (page_idx >= engine.page_count) {
        return k_ra8_err_out_of_range;
    }

    var nfaces: u8 = 0;
    const err = initFaces(engine, &s_faces, &nfaces);
    if (err != k_ra8_ok) {
        return err;
    }

    const page: *const ReflowPage = &engine.pages[page_idx];
    const atlas: ?*anyopaque = engine.glyph_atlas;
    var i: u32 = 0;
    while (i < page.glyph_count) : (i += 1) {
        const g: *const ReflowGlyph = &engine.glyphs[page.glyph_first + i];
        var fi: u8 = @intCast((@as(u32, g.style) >> k_reflow_face_shift) & k_reflow_face_mask);
        if (fi >= nfaces) {
            fi = 0; // defensive: out-of-range face index -> default
        }
        // CSS picked the face by family and weight; coverage can still send
        // this code point to another registered face.
        fi = priv_reflow_render_face_for(@ptrCast(&s_faces), nfaces, fi, g.cp);
        blitGlyph(atlas, fi, &s_faces[fi], g, ox, oy);
    }
    renderImages(engine, page_idx, ox, oy);
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Public ABI -- render (unchanged C shape).
// ---------------------------------------------------------------------------

export fn reflow_render_page(engine: ?*ReflowEngine, page_idx: c_uint, framebuffer: ?*anyopaque) c_int {
    _ = framebuffer; // Reserved hook -- ra8_gfx is bound externally.
    const e = engine orelse return k_ra8_err_null_ptr;
    return renderPage(e, page_idx, 0, 0);
}

export fn reflow_render_page_at(engine: ?*ReflowEngine, page_idx: c_uint, origin_x: i32, origin_y: i32) c_int {
    const e = engine orelse return k_ra8_err_null_ptr;
    return renderPage(e, page_idx, origin_x, origin_y);
}

export fn reflow_set_glyph_atlas(engine: ?*ReflowEngine, atlas: ?*anyopaque, storage: ?*const AtlasStorage) c_int {
    const e = engine orelse return k_ra8_err_null_ptr;
    if (e.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (atlas == null) {
        e.glyph_atlas = null; // Detach -> revert to direct rasterisation.
        return k_ra8_ok;
    }
    const st = storage orelse return k_ra8_err_null_ptr;
    if ((st.cell_bytes == 0) or (st.cell_count == 0) or (st.bucket_count == 0)) {
        return k_ra8_err_invalid_size;
    }

    const cfg: AtlasCfg = .{
        .cell_mem = st.cell_mem,
        .cell_bytes = st.cell_bytes,
        .cell_count = st.cell_count,
        .meta = st.meta,
        .keys = st.keys,
        .dims = st.dims,
        .buckets = st.buckets,
        .bucket_count = st.bucket_count,
        .render = atlasRenderGlyph,
        .render_ctx = &s_glyph_render_ctx,
    };
    const err = ra8_glyph_atlas_init(atlas.?, &cfg);
    if (err != k_ra8_ok) {
        return err;
    }
    e.glyph_atlas = atlas;
    return k_ra8_ok;
}

export fn priv_reflow_render_is_blank_cp(cp: c_int) bool {
    return isBlankCodePoint(cp);
}

export fn priv_reflow_render_needs_tofu(glyph_index: c_int, cp: c_int) bool {
    return needsTofu(glyph_index, cp);
}

export fn priv_reflow_render_tofu_rect(advance_px: i32, ascent_px: i32, font_px: i32, out: ?*TofuRect) bool {
    const rect = out orelse return false;
    if (font_px <= 0) {
        return false;
    }
    var width = advance_px;
    if (width <= 0) {
        width = @divTrunc(font_px, k_priv_tofu_adv_den); // Degenerate advance.
    }
    var height = @divTrunc(font_px * k_priv_tofu_asc_num, k_priv_tofu_asc_den);
    if (ascent_px > 0) {
        height = @divTrunc(ascent_px * k_priv_tofu_h_num, k_priv_tofu_h_den);
    }
    var inset = @divTrunc(width, k_priv_tofu_inset_den);
    var box_w = width - k_priv_tofu_edges * inset;
    if (box_w < k_priv_tofu_min_px) {
        box_w = k_priv_tofu_min_px; // Keep the box legible at any size.
        inset = 0;
    }
    if (height < k_priv_tofu_min_px) {
        height = k_priv_tofu_min_px;
    }
    rect.x_off = inset;
    rect.y_off = -height;
    rect.w = box_w;
    rect.h = height;
    return true;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

test "blank code points draw nothing" {
    try std.testing.expect(priv_reflow_render_is_blank_cp(0x20));
    try std.testing.expect(priv_reflow_render_is_blank_cp(0xA0));
    try std.testing.expect(priv_reflow_render_is_blank_cp(0xFEFF));
    try std.testing.expect(!priv_reflow_render_is_blank_cp(0x41));
    try std.testing.expect(!priv_reflow_render_is_blank_cp(0xFFFF));
}

test "tofu decision mirrors the cmap probe" {
    // Glyph 0 means "no glyph for that code point"; blank code points stay
    // blank even then.
    try std.testing.expect(priv_reflow_render_needs_tofu(0, 0x41));
    try std.testing.expect(!priv_reflow_render_needs_tofu(0, 0x20));
    try std.testing.expect(!priv_reflow_render_needs_tofu(7, 0x41));
}

test "tofu rect geometry" {
    var rect: TofuRect = undefined;
    // Advance-driven box: inset = 10/8 = 1, height from font_px.
    try std.testing.expect(priv_reflow_render_tofu_rect(10, 12, 12, &rect));
    try std.testing.expectEqual(@as(i32, 1), rect.x_off);
    try std.testing.expectEqual(@as(i32, -9), rect.y_off);
    try std.testing.expectEqual(@as(i32, 8), rect.w);
    try std.testing.expectEqual(@as(i32, 9), rect.h);

    // Ascent-driven height overrides the font_px fallback.
    try std.testing.expect(priv_reflow_render_tofu_rect(16, 16, 12, &rect));
    try std.testing.expectEqual(@as(i32, 12), rect.h);

    // Degenerate advance: width falls back to font_px / 2.
    try std.testing.expect(priv_reflow_render_tofu_rect(0, 0, 12, &rect));
    try std.testing.expectEqual(@as(i32, 6), rect.w);

    // Tiny advance: the box stays legible at the minimum edge.
    try std.testing.expect(priv_reflow_render_tofu_rect(1, 0, 12, &rect));
    try std.testing.expectEqual(@as(i32, 3), rect.w);
    try std.testing.expectEqual(@as(i32, 0), rect.x_off);

    // No output slot or a non-positive font size: nothing to draw.
    try std.testing.expect(!priv_reflow_render_tofu_rect(10, 12, 12, null));
    try std.testing.expect(!priv_reflow_render_tofu_rect(10, 12, 0, &rect));
}

test "ABI mirrors match the C layout" {
    // ra8_glyph_key_t: u32 + 4 x u16.
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(GlyphKey));
    // ra8_glyph_t: pointer + 2 x u16, rounded up to pointer alignment
    // (C's sizeof agrees).
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(GlyphView));
    // ra8_keycache_cell_t: 3 x i32 + u16 + 2 x u8.
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(KeycacheCell));
    // reflow_glyph_atlas_storage_t: bucket_count trails the pointers.
    try std.testing.expectEqual(@as(usize, 48), @offsetOf(AtlasStorage, "bucket_count"));
    // priv_reflow_tofu_rect_t: 4 x i32.
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(TofuRect));
}
