//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Token-stream driver + public lifecycle API for the reflow engine
//! (RA8FW-1227). Zig port of src/reflow_layout_driver.c: the outer driver
//! walks the parsed token stream once, dispatching <table> ranges to
//! priv_reflow_layout_table and every other token to
//! priv_reflow_layout_apply_token, then flushes a final page and builds the
//! tappable link rectangles. The public API covers engine lifecycle
//! (reflow_init / reflow_close), loader binding (image / CSS), chapter
//! dispatch (reflow_layout_chapter, reflow_set_font_size), font (re)binding
//! and face registration, all exported with C linkage behind the unchanged
//! headers in inc/.
//!
//! The stbtt_fontinfo storage uses the ABI-probed mirror from
//! reflow_render_face.zig (no field is ever read from Zig; the layout pass
//! only passes the handle through to the stbtt_* entry points).
//!
//! SPDX-License-Identifier: MIT

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lz = @import("reflow_layout.zig");
const tbl = @import("reflow_layout_table.zig");
const rf = @import("reflow_render_face.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowToken = abi.ReflowToken;
const ReflowLoaderFn = abi.ReflowLoaderFn;
const PrivCursor = lz.PrivCursor;
const StbttFontinfo = lz.StbttFontinfo;

// ---- Constants (inc/reflow_types.h, inc/reflow_layout_internal.h) ----

const k_reflow_margin_px: u16 = 16;
const k_reflow_min_font_px: u16 = 8;
const k_reflow_max_font_px: u16 = 96;
const k_reflow_min_font_bytes: usize = 16;
const k_reflow_max_faces: u8 = 8;
const k_reflow_max_pages: u32 = 256;
const k_priv_min_chapter_pages: u32 = 1;

const k_reflow_tok_block_start: u8 = 0;
const k_reflow_tag_table: u8 = 20;
const k_reflow_style_normal: u8 = 0;
const k_reflow_align_left: u8 = 0;

const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;
const k_ra8_err_invalid_arg: c_int = 0x103;
const k_ra8_err_invalid_state: c_int = 0x104;
const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_not_supported: c_int = 0x107;
const k_ra8_err_not_initialized: c_int = 0x10F;
const k_ra8_err_null_ptr: c_int = 0x504;

extern fn stbtt_GetFontOffsetForIndex(data: [*]const u8, index: c_int) c_int;
extern fn stbtt_InitFont(info: *StbttFontinfo, data: [*]const u8, data_size: c_int, offset: c_int) c_int;
extern fn ra8_stbtt_sfnt_dir_in_bounds(data: [*]const u8, len: usize, fontstart: u32) callconv(.c) bool;

/// Stack storage for one `stbtt_fontinfo`: the mirror from
/// reflow_render_face.zig, whose host ABI-probe test checks its layout
/// against the C struct. Only the byte size participates here; no field
/// is ever read from Zig.
const FontInfo = rf.StbttFontinfo;

/// Hand the layout siblings an opaque fontinfo pointer for stack storage.
fn font_ptr(font: *FontInfo) *StbttFontinfo {
    return @ptrCast(font);
}

/// Run one pass over the token stream populating engine.glyphs[] and
/// engine.pages[]: <table> ranges go to the table grid, every other token
/// to the line/page engine, then the final page flush.
fn internal_layout_tokens(engine: *ReflowEngine, font: *const StbttFontinfo) c_int {
    var cur = PrivCursor{
        .x = k_reflow_margin_px,
        .y = k_reflow_margin_px,
        .line_top = k_reflow_margin_px,
        .line_height_px = lz.priv_reflow_layout_line_height(engine.font_px),
        .active_font_px = engine.font_px,
        .active_style = k_reflow_style_normal,
        .indent_px = 0,
        .page_first_glyph = 0,
        .page_first_image = 0,
        .line_first_glyph = 0,
        .line_has_content = 0,
        .@"align" = k_reflow_align_left,
        .reserved8 = .{ 0, 0 },
    };

    var i: u32 = 0;
    while (i < engine.token_count) {
        const tok = &engine.tokens[i];
        if (tok.kind == k_reflow_tok_block_start and tok.tag == k_reflow_tag_table) {
            const err = tbl.priv_reflow_layout_table(engine, &cur, font, i, &i);
            if (err != k_ra8_ok) {
                return err;
            }
        } else {
            const err = lz.priv_reflow_layout_apply_token(engine, &cur, font, tok);
            if (err != k_ra8_ok) {
                return err;
            }
            i += 1;
        }
    }

    // Flush the final page if it has glyph or image content.
    if ((engine.glyph_count > cur.page_first_glyph) or
        (engine.image_box_count > cur.page_first_image))
    {
        if (!lz.priv_reflow_layout_finish_page(engine, &cur)) {
            return k_ra8_err_no_mem;
        }
    }
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Public API (declared in inc/reflow.h) -- C linkage preserved.
// ---------------------------------------------------------------------------

pub export fn reflow_run_layout(engine: ?*ReflowEngine) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }

    // Wipe glyph, page, image, link and anchor state but keep the token stream.
    engine.?.glyph_count = 0;
    engine.?.page_count = 0;
    engine.?.image_box_count = 0;
    engine.?.link_rect_count = 0;
    engine.?.anchor_count = 0;

    var font = std.mem.zeroes(FontInfo);
    var err = lz.priv_reflow_layout_init_font(engine.?, font_ptr(&font));
    if (err != k_ra8_ok) {
        return err;
    }
    err = internal_layout_tokens(engine.?, font_ptr(&font));
    if (err != k_ra8_ok) {
        return err;
    }
    // Guarantee at least one page on non-empty input.
    if (lz.priv_reflow_internal_final_page_needed(engine.?.page_count, engine.?.token_count)) {
        if (engine.?.page_count >= k_reflow_max_pages) {
            return k_ra8_err_no_mem;
        }
        engine.?.pages[0].glyph_first = 0;
        engine.?.pages[0].glyph_count = engine.?.glyph_count;
        engine.?.page_count = k_priv_min_chapter_pages;
    }
    // Build tappable link rectangles from the link-tagged glyphs.
    lz.priv_reflow_layout_build_link_rects(engine.?, font_ptr(&font));
    return k_ra8_ok;
}

pub export fn reflow_init(viewport_w: u16,
    viewport_h: u16,
    font_data: ?[*]const u8,
    font_len: usize,
    font_px: u16,
    body_color: u32,
    link_color: u32,
    out_engine: ?*ReflowEngine,
) callconv(.c) c_int {
    if ((font_data == null) or (out_engine == null)) {
        return k_ra8_err_null_ptr;
    }
    if ((viewport_w == 0) or (viewport_h == 0)) {
        return k_ra8_err_invalid_arg;
    }
    if ((font_px < k_reflow_min_font_px) or (font_px > k_reflow_max_font_px)) {
        return k_ra8_err_invalid_arg;
    }
    if (font_len < k_reflow_min_font_bytes) {
        return k_ra8_err_invalid_size;
    }

    lz.priv_reflow_layout_byte_zero(@ptrCast(out_engine.?), @sizeOf(ReflowEngine));

    out_engine.?.viewport_w = viewport_w;
    out_engine.?.viewport_h = viewport_h;
    out_engine.?.font_data = font_data;
    out_engine.?.font_len = font_len;
    out_engine.?.font_px = font_px;
    out_engine.?.body_color = body_color;
    out_engine.?.link_color = link_color;
    out_engine.?.in_use = 1;
    return k_ra8_ok;
}

pub export fn reflow_close(engine: ?*ReflowEngine) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    engine.?.in_use = 0;
    engine.?.page_count = 0;
    engine.?.glyph_count = 0;
    engine.?.token_count = 0;
    engine.?.text_pool_used = 0;
    engine.?.image_box_count = 0;
    engine.?.xhtml_buf = null;
    engine.?.xhtml_len = 0;
    return k_ra8_ok;
}

pub export fn reflow_set_image_loader(engine: ?*ReflowEngine,
    loader: ?ReflowLoaderFn,
    ctx: ?*anyopaque,
    arena: ?*anyopaque,
) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    engine.?.img_loader = loader;
    engine.?.img_loader_ctx = ctx;
    engine.?.img_arena = arena;
    return k_ra8_ok;
}

pub export fn reflow_set_css_loader(engine: ?*ReflowEngine, loader: ?ReflowLoaderFn, ctx: ?*anyopaque) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    engine.?.css_loader = loader;
    engine.?.css_loader_ctx = ctx;
    return k_ra8_ok;
}

pub export fn reflow_layout_chapter(engine: ?*ReflowEngine,
    xhtml_buf: ?[*]const u8,
    xhtml_len: usize,
    out_total_pages: ?*u32,
) callconv(.c) c_int {
    if ((engine == null) or (xhtml_buf == null) or (out_total_pages == null)) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (xhtml_len == 0) {
        return k_ra8_err_invalid_size;
    }

    out_total_pages.?.* = 0;

    var err = abi.reflow_parse_xhtml(engine, xhtml_buf, xhtml_len);
    if (err != k_ra8_ok) {
        return err;
    }
    err = reflow_run_layout(engine);
    if (err != k_ra8_ok) {
        return err;
    }

    // Cache the pointer so set_font_size() can re-flow without the
    // caller resupplying the buffer.
    engine.?.xhtml_buf = xhtml_buf;
    engine.?.xhtml_len = xhtml_len;
    out_total_pages.?.* = engine.?.page_count;
    return k_ra8_ok;
}

pub export fn reflow_get_page_count(engine: ?*const ReflowEngine, out_count: ?*u32) callconv(.c) c_int {
    if ((engine == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = engine.?.page_count;
    return k_ra8_ok;
}

pub export fn reflow_set_font_size(engine: ?*ReflowEngine, new_font_px: u16) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if ((new_font_px < k_reflow_min_font_px) or (new_font_px > k_reflow_max_font_px)) {
        return k_ra8_err_invalid_arg;
    }
    if (lz.priv_reflow_internal_xhtml_invalid(engine.?.xhtml_buf, engine.?.xhtml_len)) {
        return k_ra8_err_invalid_state;
    }
    engine.?.font_px = new_font_px;

    // Re-flow against the cached buffer.
    var pages: u32 = 0;
    return reflow_layout_chapter(engine, engine.?.xhtml_buf, engine.?.xhtml_len, &pages);
}

/// Implementation of `reflow_bind_font()` -- validate then swap.
pub export fn reflow_bind_font(engine: ?*ReflowEngine, font_data: ?[*]const u8, font_len: usize) callconv(.c) c_int {
    if ((engine == null) or (font_data == null)) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (font_len < k_reflow_min_font_bytes) {
        return k_ra8_err_invalid_size;
    }

    // Validate the blob BEFORE swapping; on failure the current face is kept
    // (graceful degradation). The probe fontinfo is a stack temporary -- the
    // layout / render paths re-init their own per-call fontinfo from font_data.
    const offset = stbtt_GetFontOffsetForIndex(font_data.?, 0);
    // Bound-check the sfnt table directory before stbtt_InitFont walks it: the
    // @font-face bytes are attacker-controlled and stb_truetype reads the
    // directory with no length check. A non-sfnt blob (offset < 0) is
    // left to the existing decision below.
    if (offset >= 0) {
        if (!ra8_stbtt_sfnt_dir_in_bounds(font_data.?, font_len, @intCast(offset))) {
            return k_ra8_err_not_supported;
        }
    }
    var probe = std.mem.zeroes(FontInfo);
    if ((offset < 0) or (stbtt_InitFont(font_ptr(&probe), font_data.?, @intCast(font_len), offset) == 0)) {
        return k_ra8_err_not_supported;
    }

    engine.?.font_data = font_data;
    engine.?.font_len = font_len;

    // If a chapter is already laid out, re-flow it against the new face so the
    // change is immediate (mirrors reflow_set_font_size); otherwise the next
    // layout picks it up.
    if (!lz.priv_reflow_internal_xhtml_invalid(engine.?.xhtml_buf, engine.?.xhtml_len)) {
        var pages: u32 = 0;
        return reflow_layout_chapter(engine, engine.?.xhtml_buf, engine.?.xhtml_len, &pages);
    }
    return k_ra8_ok;
}

/// Implementation of `reflow_register_face()` -- validate then append.
pub export fn reflow_register_face(engine: ?*ReflowEngine,
    css_face_idx: u8,
    blob: ?[*]const u8,
    len: usize,
) callconv(.c) c_int {
    if ((engine == null) or (blob == null)) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (len < k_reflow_min_font_bytes) {
        return k_ra8_err_invalid_size;
    }
    if (engine.?.face_count >= k_reflow_max_faces) {
        return k_ra8_err_no_mem;
    }
    // Validate the blob before recording it; a bad face is rejected so layout falls
    // back to the default face (graceful degradation). The probe is a stack
    // temporary -- the render pass re-inits its own per-face fontinfo.
    const offset = stbtt_GetFontOffsetForIndex(blob.?, 0);
    // Bound-check the sfnt table directory before stbtt_InitFont walks it: the
    // @font-face bytes are attacker-controlled and stb_truetype reads the
    // directory with no length check. A non-sfnt blob (offset < 0) is
    // left to the existing decision below.
    if (offset >= 0) {
        if (!ra8_stbtt_sfnt_dir_in_bounds(blob.?, len, @intCast(offset))) {
            return k_ra8_err_not_supported;
        }
    }
    var probe = std.mem.zeroes(FontInfo);
    if ((offset < 0) or (stbtt_InitFont(font_ptr(&probe), blob.?, @intCast(len), offset) == 0)) {
        return k_ra8_err_not_supported;
    }
    const face = &engine.?.faces[engine.?.face_count];
    face.blob = blob;
    face.len = len;
    face.css_face_idx = css_face_idx;
    engine.?.face_count += 1;
    return k_ra8_ok;
}
