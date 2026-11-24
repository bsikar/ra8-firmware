//! Greedy line-break + page-break engine for the reflow v1 layout pass
//! (inc/reflow_layout_internal.h).
//!
//! Zig port of src/reflow_layout.c (RA8FW-1220). Consumes the token stream
//! produced by reflow_parse_xhtml() and lays it out as a flat list of
//! positioned glyphs grouped into pages; the algorithm is a textbook greedy
//! break-on-overflow (measure each word with stb_truetype advance widths,
//! finalise the line on a right-margin crossing, finalise the page on a
//! bottom-margin crossing). Block tokens flush the line, add a paragraph gap
//! and (for headings) bump the active font size. No heap, no recursion.
//!
//! The cross-TU entry points keep C linkage (@export callconv(.c)) so the
//! remaining C layout siblings (driver, image) and render TUs link
//! against them exactly as before. The four `stbtt_*` calls stay extern:
//! the vendored stb_truetype implementation TU remains C and is linked by
//! CMake.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lex = @import("reflow_tokenize_lex.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowToken = abi.ReflowToken;

// ---- Constants (inc/reflow_types.h, inc/reflow_layout_internal.h) ----

const k_reflow_margin_px: u16 = 16;
const k_reflow_line_spacing_num: u16 = 12;
const k_reflow_line_spacing_den: u16 = 10;
const k_reflow_paragraph_gap_px: u16 = 8;
const k_reflow_indent_px: u16 = 24;
const k_reflow_h1_scale_pct: u16 = 200;
const k_reflow_h2_scale_pct: u16 = 175;
const k_reflow_h3_scale_pct: u16 = 150;
const k_reflow_h4_scale_pct: u16 = 125;
const k_reflow_h5_scale_pct: u16 = 110;
const k_reflow_h6_scale_pct: u16 = 100;
const k_reflow_pct_full: u16 = 100;
const k_reflow_face_shift: u4 = 4;
const k_reflow_face_mask: u32 = 0x0F;
const k_reflow_color_inherit: u32 = 0xFFFFFFFF;

const k_reflow_tag_h1: u8 = 2;
const k_reflow_tag_h2: u8 = 3;
const k_reflow_tag_h3: u8 = 4;
const k_reflow_tag_h4: u8 = 5;
const k_reflow_tag_h5: u8 = 6;
const k_reflow_tag_h6: u8 = 7;
const k_reflow_tag_li: u8 = 16;
const k_reflow_tag_blockquote: u8 = 17;

const k_reflow_align_left: u8 = 0;
const k_reflow_align_right: u8 = 1;
const k_reflow_align_center: u8 = 2;
const k_reflow_align_justify: u8 = 3;

const k_reflow_tok_block_start: u8 = 0;
const k_reflow_tok_block_end: u8 = 1;
const k_reflow_tok_text: u8 = 2;
const k_reflow_tok_break: u8 = 3;
const k_reflow_tok_rule: u8 = 4;
const k_reflow_tok_image: u8 = 5;

const k_reflow_max_glyphs: usize = 32768;
const k_reflow_max_pages: usize = 256;
const k_reflow_max_anchors: usize = 256;
const k_reflow_max_link_rects: usize = 512;

const k_priv_min_word_w_px: i32 = 1;
const k_priv_hr_thickness_px: i32 = 2;

const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;
const k_ra8_err_validation_failed: c_int = 0x501;

/// Opaque stand-in for `stbtt_fontinfo`: the layout pass only ever passes
/// the handle through to the four `stbtt_*` entry points, so no field of
/// the vendored struct is touched from Zig.
pub const StbttFontinfo = opaque {};

extern fn stbtt_GetFontOffsetForIndex(data: [*]const u8, index: c_int) callconv(.c) c_int;
extern fn stbtt_InitFont(info: *StbttFontinfo, data: [*]const u8, data_size: c_int, offset: c_int) callconv(.c) c_int;
extern fn stbtt_ScaleForPixelHeight(info: *const StbttFontinfo, height: f32) callconv(.c) f32;
extern fn stbtt_GetCodepointHMetrics(info: *const StbttFontinfo, codepoint: c_int, advance: *c_int, lsb: *c_int) callconv(.c) void;

/// The image-layout sibling TU, Zig since RA8FW-1226 (its entry point
/// keeps C linkage for any remaining C sibling).
const img = @import("reflow_layout_image.zig");

/// Mirror of `priv_cursor_t` (src/reflow_layout_internal.h): the mutable
/// state carried through the layout loop. The C siblings allocate it by
/// value and hand it over by pointer, so the layout has to match exactly.
pub const PrivCursor = extern struct {
    x: i32,
    y: i32,
    line_top: i32,
    line_height_px: u16,
    active_font_px: u16,
    active_style: u8,
    indent_px: u16,
    page_first_glyph: u32,
    page_first_image: u32,
    line_first_glyph: u32,
    line_has_content: u8,
    @"align": u8,
    reserved8: [2]u8,
};

comptime {
    std.debug.assert(@sizeOf(PrivCursor) == 36);
    std.debug.assert(@offsetOf(PrivCursor, "indent_px") == 18);
    std.debug.assert(@offsetOf(PrivCursor, "page_first_glyph") == 20);
    std.debug.assert(@offsetOf(PrivCursor, "line_has_content") == 32);
    std.debug.assert(@offsetOf(PrivCursor, "reserved8") == 34);
}

/// True iff `tag` is a block-level indent tag (`li`, `blockquote`). Pure
/// helper kept as its own symbol so the C MC/DC tests keep driving both
/// arms of the decision.
pub export fn priv_reflow_internal_is_indent_tag(tag: u8) callconv(.c) bool {
    return (tag == k_reflow_tag_li) or (tag == k_reflow_tag_blockquote);
}

/// AND helper for the right-margin overflow break decision: break only
/// when the cursor would cross `right_limit` and the line already has
/// glyphs.
pub export fn priv_reflow_internal_right_overflow_break(cursor_x: i32, advance: i32, right_limit: i32, line_has_content: u8) callconv(.c) bool {
    return ((cursor_x + advance) > right_limit) and (line_has_content != 0);
}

/// OR helper for the cached-XHTML invalid decision.
pub export fn priv_reflow_internal_xhtml_invalid(xhtml_buf: ?*const anyopaque, xhtml_len: usize) callconv(.c) bool {
    return (xhtml_buf == null) or (xhtml_len == 0);
}

/// AND helper for the synthesise-final-page decision.
pub export fn priv_reflow_internal_final_page_needed(page_count: u32, token_count: u32) callconv(.c) bool {
    return (page_count == 0) and (token_count > 0);
}

/// Bounded byte-walk used in place of `memset(0)` (the C side avoids the
/// deprecated string API for the analyser; Zig keeps the same shape).
pub export fn priv_reflow_layout_byte_zero(dst: [*]u8, n: usize) callconv(.c) void {
    for (0..n) |i| {
        dst[i] = 0;
    }
}

/// Parse the TTF blob into `out_font` through the vendored stb_truetype.
pub export fn priv_reflow_layout_init_font(engine: *const ReflowEngine, out_font: *StbttFontinfo) callconv(.c) c_int {
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

/// Scaled integer ratio: `font_px * num / den`.
pub export fn priv_reflow_layout_line_height(font_px: u16) callconv(.c) u16 {
    const lh = @as(u32, font_px) * @as(u32, k_reflow_line_spacing_num) / @as(u32, k_reflow_line_spacing_den);
    return @intCast(lh);
}

/// Pick the font size for a block tag: heading scale percentage of the
/// body size, or the body size otherwise.
pub fn block_font_px(body_px: u16, tag: u8) u16 {
    const pct: u32 = switch (tag) {
        k_reflow_tag_h1 => k_reflow_h1_scale_pct,
        k_reflow_tag_h2 => k_reflow_h2_scale_pct,
        k_reflow_tag_h3 => k_reflow_h3_scale_pct,
        k_reflow_tag_h4 => k_reflow_h4_scale_pct,
        k_reflow_tag_h5 => k_reflow_h5_scale_pct,
        k_reflow_tag_h6 => k_reflow_h6_scale_pct,
        else => k_reflow_pct_full,
    };
    return @intCast((@as(u32, body_px) * pct) / @as(u32, k_reflow_pct_full));
}

/// Measure one code point's advance width in pixels (font units scaled by
/// `stbtt_ScaleForPixelHeight`).
pub export fn priv_reflow_layout_glyph_advance(font: *const StbttFontinfo, font_px: u16, cp: i32) callconv(.c) i32 {
    const scale = stbtt_ScaleForPixelHeight(font, @floatFromInt(font_px));
    var advance_units: c_int = 0;
    var lsb: c_int = 0;
    stbtt_GetCodepointHMetrics(font, cp, &advance_units, &lsb);
    return @intFromFloat(@as(f32, @floatFromInt(advance_units)) * scale);
}

/// Append one positioned glyph to the engine pool; false when full.
pub export fn priv_reflow_layout_push_glyph(engine: *ReflowEngine, x: i32, y: i32, cp: i32, font_px: u16, style: u8, color: u32, link_id: u8) callconv(.c) bool {
    if (engine.glyph_count >= k_reflow_max_glyphs) {
        return false;
    }
    const g = &engine.glyphs[engine.glyph_count];
    g.x = x;
    g.y = y;
    g.cp = cp;
    g.color = color;
    g.font_px = font_px;
    g.style = style;
    g.reserved = link_id; // 1-based `<a>` link id (0 = not a link)
    engine.glyph_count += 1;
    return true;
}

/// Finalise the current page and reset the cursor to a fresh one; false
/// when the page pool is full.
pub export fn priv_reflow_layout_finish_page(engine: *ReflowEngine, cur: *PrivCursor) callconv(.c) bool {
    if (engine.page_count >= k_reflow_max_pages) {
        return false;
    }
    const count = engine.glyph_count - cur.page_first_glyph;
    const page = &engine.pages[engine.page_count];
    page.glyph_first = cur.page_first_glyph;
    page.glyph_count = count;
    engine.page_count += 1;
    cur.page_first_glyph = engine.glyph_count;
    cur.page_first_image = engine.image_box_count;
    cur.line_first_glyph = engine.glyph_count;
    cur.y = @as(i32, k_reflow_margin_px);
    cur.line_top = cur.y;
    cur.x = @as(i32, k_reflow_margin_px) + @as(i32, cur.indent_px);
    cur.line_has_content = 0;
    return true;
}

/// Spread `slack` across the inter-word gaps of glyphs `[lo, hi)`: each
/// space absorbs an equal share with the remainder spread one pixel at a
/// time across the leftmost gaps, and every glyph shifts right by the
/// accumulated widening to its left. No space glyphs means no change.
pub fn justify_glyphs(engine: *ReflowEngine, lo: u32, hi: u32, slack: i32) void {
    var spaces: u32 = 0;
    for (lo..hi) |i| {
        if (engine.glyphs[i].cp == ' ') {
            spaces += 1;
        }
    }
    if (spaces == 0) {
        return;
    }
    const per = @divTrunc(slack, @as(i32, @intCast(spaces)));
    const rem = @rem(slack, @as(i32, @intCast(spaces)));
    var added: i32 = 0;
    var seen: u32 = 0;
    for (lo..hi) |i| {
        engine.glyphs[i].x += added;
        if (engine.glyphs[i].cp == ' ') {
            added += per;
            if (seen < rem) {
                added += 1;
            }
            seen += 1;
        }
    }
}

/// Apply the active block alignment to the just-completed line. Left is a
/// no-op; centre and right shift the line so its content edge meets the
/// margin; justify distributes the slack across inter-word gaps on wrapped
/// lines only. A trailing space at the break point is excluded from the
/// content extent first.
pub fn finish_line(engine: *ReflowEngine, cur: *PrivCursor, allow_justify: bool) void {
    if (cur.@"align" == k_reflow_align_left) {
        return;
    }
    const lo = cur.line_first_glyph;
    var hi = engine.glyph_count;
    if (hi <= lo) {
        return;
    }
    var content_right = cur.x;
    if (engine.glyphs[hi - 1].cp == ' ') {
        content_right = engine.glyphs[hi - 1].x;
        hi -= 1;
    }
    const right_limit = @as(i32, engine.viewport_w) - @as(i32, k_reflow_margin_px);
    const slack = right_limit - content_right;
    if ((hi <= lo) or (slack <= 0)) {
        return;
    }
    if (cur.@"align" == k_reflow_align_justify) {
        if (allow_justify) {
            justify_glyphs(engine, lo, hi, slack);
        }
        return;
    }
    const offset: i32 = if (cur.@"align" == k_reflow_align_center) @divTrunc(slack, 2) else slack;
    for (lo..hi) |i| {
        engine.glyphs[i].x += offset;
    }
}

/// Wrap to a new line, finishing a page when the bottom margin is hit.
pub export fn priv_reflow_layout_newline(engine: *ReflowEngine, cur: *PrivCursor, allow_justify: bool) callconv(.c) bool {
    finish_line(engine, cur, allow_justify);
    cur.y += @as(i32, cur.line_height_px);
    cur.line_top = cur.y;
    cur.x = @as(i32, k_reflow_margin_px) + @as(i32, cur.indent_px);
    cur.line_has_content = 0;
    cur.line_first_glyph = engine.glyph_count;

    const bottom_limit = @as(i32, engine.viewport_h) - @as(i32, k_reflow_margin_px);
    if (cur.y + @as(i32, cur.line_height_px) > bottom_limit) {
        if (!priv_reflow_layout_finish_page(engine, cur)) {
            return false;
        }
    }
    return true;
}

/// Append one code point at the cursor, wrapping if it would overflow the
/// right margin. The advance is clamped to at least one pixel so a
/// zero-width glyph cannot stall the greedy walk.
fn emit_char(engine: *ReflowEngine, cur: *PrivCursor, font: *const StbttFontinfo, cp: i32, color: u32, link_id: u8) c_int {
    const advance = priv_reflow_layout_glyph_advance(font, cur.active_font_px, cp);
    const advance_clamped = if (advance < k_priv_min_word_w_px) k_priv_min_word_w_px else advance;

    const right_limit = @as(i32, engine.viewport_w) - @as(i32, k_reflow_margin_px);
    if (priv_reflow_internal_right_overflow_break(cur.x, advance_clamped, right_limit, cur.line_has_content)) {
        if (!priv_reflow_layout_newline(engine, cur, true)) { // wrap -> previous line may justify
            return k_ra8_err_no_mem;
        }
    }
    if (!priv_reflow_layout_push_glyph(engine, cur.x, cur.y, cp, cur.active_font_px, cur.active_style, color, link_id)) {
        return k_ra8_err_no_mem;
    }
    cur.x += advance_clamped;
    cur.line_has_content = 1;
    return k_ra8_ok;
}

/// Lay out one text token: walk it code point by code point, break at
/// whitespace, and emit each through `emit_char`. The pool holds UTF-8, so
/// the walk decodes with `priv_reflow_tok_utf8_decode`; measuring and
/// emitting share that one decoder, which keeps the greedy pre-measure and
/// the emit pass agreeing on where each word ends. ASCII space is the only
/// break byte and can never appear inside a multi-byte sequence, so word
/// boundaries always land on a sequence boundary.
fn layout_text(engine: *ReflowEngine, cur: *PrivCursor, font: *const StbttFontinfo, tok: *const ReflowToken) c_int {
    const base = engine.text_pool[tok.text_off..].ptr;
    const len = tok.text_len;
    const link_id = tok.reserved; // 1-based `<a>` link id (0 = none)
    // Colour precedence: an explicit CSS `color` wins; otherwise a link run
    // uses the link colour and ordinary text the body colour. Link colour is
    // keyed on actual link membership, not the underline style bit, so CSS
    // `text-decoration: underline` on non-link text keeps the body colour.
    const fallback: u32 = if (link_id != 0) engine.link_color else engine.body_color;
    const color: u32 = if (tok.color != k_reflow_color_inherit) tok.color else fallback;

    // Pre-scan to find each word boundary. Greedy: if word_w + cursor_x
    // exceeds the right margin AND the line already has content, break
    // before emitting the word.
    var i: u32 = 0;
    while (i < len) {
        // Emit any leading whitespace as one space (already collapsed by
        // the parser).
        if (base[i] == ' ') {
            const err = emit_char(engine, cur, font, ' ', color, link_id);
            if (err != k_ra8_ok) {
                return err;
            }
            i += 1;
            continue;
        }

        // Word: bytes up to the next space. Pre-measure for greedy break.
        var word_end = i;
        var word_w: i32 = 0;
        while ((word_end < len) and (base[word_end] != ' ')) {
            var cp: u32 = 0;
            const used = lex.priv_reflow_tok_utf8_decode(base + word_end, len - word_end, &cp);
            word_w += priv_reflow_layout_glyph_advance(font, cur.active_font_px, @intCast(cp));
            word_end += @intCast(used);
        }
        const right_limit = @as(i32, engine.viewport_w) - @as(i32, k_reflow_margin_px);
        if (priv_reflow_internal_right_overflow_break(cur.x, word_w, right_limit, cur.line_has_content)) {
            if (!priv_reflow_layout_newline(engine, cur, true)) { // wrap -> previous line may justify
                return k_ra8_err_no_mem;
            }
        }
        while (i < word_end) {
            var cp: u32 = 0;
            const used = lex.priv_reflow_tok_utf8_decode(base + i, word_end - i, &cp);
            const err = emit_char(engine, cur, font, @intCast(cp), color, link_id);
            if (err != k_ra8_ok) {
                return err;
            }
            i += @intCast(used);
        }
    }
    return k_ra8_ok;
}

/// Apply a `block_start` token: flush the current line, set the heading
/// font size, set the indent, record an `id=` anchor, and take the block's
/// text alignment from the token's reserved byte.
fn open_block(engine: *ReflowEngine, cur: *PrivCursor, tok: *const ReflowToken) bool {
    if (cur.line_has_content != 0) {
        if (!priv_reflow_layout_newline(engine, cur, false)) {
            return false;
        }
    }
    // The block's text alignment travels in the block-start token's
    // reserved byte (set by the tokenizer from `style="text-align:..."`).
    cur.@"align" = tok.reserved;
    // Record an `id=` anchor (carried in the block-start token's slice) at
    // this block's page + top, for same-chapter `#fragment` jumps.
    if ((tok.text_len > 0) and (engine.anchor_count < k_reflow_max_anchors)) {
        const anchor = &engine.anchors[engine.anchor_count];
        anchor.id_off = tok.text_off;
        anchor.id_len = tok.text_len;
        anchor.page_index = engine.page_count;
        anchor.y = cur.y;
        engine.anchor_count += 1;
    }
    // CSS `font-size` on the block-start token wins; else the UA default
    // (body size or heading scale). 0 = no CSS font, so unstyled content is
    // byte-identical.
    cur.active_font_px = if (tok.css_font_px != 0) tok.css_font_px else block_font_px(engine.font_px, tok.tag);
    cur.line_height_px = priv_reflow_layout_line_height(cur.active_font_px);
    if (priv_reflow_internal_is_indent_tag(tok.tag)) {
        cur.indent_px = k_reflow_indent_px;
        cur.x = @as(i32, k_reflow_margin_px) + @as(i32, cur.indent_px);
    }
    return true;
}

/// Apply a `block_end` token: flush the line, add the paragraph gap, clear
/// the indent, and page-break when the bottom margin is hit.
fn close_block(engine: *ReflowEngine, cur: *PrivCursor, tok: *const ReflowToken) bool {
    if (cur.line_has_content != 0) {
        if (!priv_reflow_layout_newline(engine, cur, false)) {
            return false;
        }
    }
    // Paragraph gap.
    cur.y += @as(i32, k_reflow_paragraph_gap_px);
    cur.line_top = cur.y;
    cur.x = @as(i32, k_reflow_margin_px);
    if (priv_reflow_internal_is_indent_tag(tok.tag)) {
        cur.indent_px = 0;
    }
    cur.active_font_px = engine.font_px;
    cur.line_height_px = priv_reflow_layout_line_height(cur.active_font_px);

    const bottom_limit = @as(i32, engine.viewport_h) - @as(i32, k_reflow_margin_px);
    if (cur.y + @as(i32, cur.line_height_px) > bottom_limit) {
        if (!priv_reflow_layout_finish_page(engine, cur)) {
            return false;
        }
    }
    return true;
}

/// Apply an `<hr>` rule token: flush the line and bump the cursor.
fn apply_rule(engine: *ReflowEngine, cur: *PrivCursor) bool {
    if (cur.line_has_content != 0) {
        if (!priv_reflow_layout_newline(engine, cur, false)) {
            return false;
        }
    }
    cur.y += k_priv_hr_thickness_px;
    cur.line_top = cur.y;
    return true;
}

/// Record one tappable link rect spanning glyphs `[lo, hi)`: a same-link,
/// same-baseline run on one page. The rect spans from the first glyph's
/// left edge to the last glyph's right edge with a generous vertical band
/// (about 1.5 em centred on the baseline) for forgiving tap targets. The
/// target field is stored 0-based; a full link-rect pool changes nothing.
fn emit_link_rect(engine: *ReflowEngine, font: *const StbttFontinfo, lo: u32, hi: u32, link: u8, page: u32) void {
    if (engine.link_rect_count >= k_reflow_max_link_rects) {
        return;
    }
    const gfirst = &engine.glyphs[lo];
    const glast = &engine.glyphs[hi - 1];
    const fpx: i32 = gfirst.font_px;
    const x1 = glast.x + priv_reflow_layout_glyph_advance(font, glast.font_px, glast.cp);
    const rect = &engine.link_rects[engine.link_rect_count];
    rect.x = gfirst.x;
    rect.y = gfirst.y - fpx;
    rect.w = if (x1 > gfirst.x) x1 - gfirst.x else 1;
    rect.h = fpx + @divTrunc(fpx, 2);
    rect.target = @as(u32, link) - 1; // 1-based -> 0-based
    rect.page_index = page;
    engine.link_rect_count += 1;
}

/// Post-layout pass: group link-tagged glyphs into tappable rects. For
/// each page, walk the glyph run; when a glyph carries a non-zero link id,
/// scan forward to the end of the consecutive same-link, same-baseline run
/// and emit a rect for that span. A multi-line anchor link therefore
/// produces one rect per wrapped line.
pub export fn priv_reflow_layout_build_link_rects(engine: *ReflowEngine, font: *const StbttFontinfo) callconv(.c) void {
    for (0..engine.page_count) |p| {
        const page = &engine.pages[p];
        const base = page.glyph_first;
        var k: u32 = 0;
        while (k < page.glyph_count) {
            const link = engine.glyphs[base + k].reserved;
            if (link == 0) {
                k += 1;
                continue;
            }
            const y = engine.glyphs[base + k].y;
            var e = k + 1;
            while ((e < page.glyph_count) and
                (engine.glyphs[base + e].reserved == link) and
                (engine.glyphs[base + e].y == y))
            {
                e += 1;
            }
            emit_link_rect(engine, font, base + k, base + e, link, @intCast(p));
            k = e;
        }
    }
}

/// Dispatch one parsed token through the layout cursor. The per-run
/// embedded-face index (stamped on the token by the cascade, embedded
/// fonts) is packed into the free high nibble of the style stamp; bits 0-2
/// keep the bold/italic/underline emphasis. Single-face content carries
/// face 0, so the style byte is unchanged.
pub export fn priv_reflow_layout_apply_token(engine: *ReflowEngine, cur: *PrivCursor, font: *const StbttFontinfo, tok: *const ReflowToken) callconv(.c) c_int {
    cur.active_style = tok.style | @as(u8, @intCast(((@as(u32, tok.reserved16) & k_reflow_face_mask) << k_reflow_face_shift) & 0xF0));

    switch (tok.kind) {
        k_reflow_tok_block_start => return if (open_block(engine, cur, tok)) k_ra8_ok else k_ra8_err_no_mem,
        k_reflow_tok_block_end => return if (close_block(engine, cur, tok)) k_ra8_ok else k_ra8_err_no_mem,
        k_reflow_tok_text => return layout_text(engine, cur, font, tok),
        k_reflow_tok_break => return if (priv_reflow_layout_newline(engine, cur, false)) k_ra8_ok else k_ra8_err_no_mem,
        k_reflow_tok_rule => return if (apply_rule(engine, cur)) k_ra8_ok else k_ra8_err_no_mem,
        k_reflow_tok_image => return if (img.priv_reflow_layout_apply_image(engine, cur, tok)) k_ra8_ok else k_ra8_err_no_mem,
        else => return k_ra8_ok,
    }
}
