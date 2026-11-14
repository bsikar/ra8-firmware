//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig port of apps/shared_libs/reflow/src/reflow_tokenize.c (RA8FW-1213):
//! the markup walk `priv_reflow_xml_walk` behind the unchanged C headers.
//! The lexical + attribute primitives (whitespace classify, entity decode,
//! attribute scan) stay C in reflow_tokenize_lex.c / reflow_tokenize_attr.c /
//! reflow_tokenize_entities.c and are reached through the extern declarations
//! below; stylesheet resolution goes through the ra8_css library.

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowToken = abi.ReflowToken;
const CssSheet = abi.CssSheet;
const CssStyle = abi.CssStyle;
const CssElement = abi.CssElement; // ra8_css_element_t
const CssAnc = abi.CssAnc; // ra8_css_anc_t (rule ancestors)

// ---- Constants from inc/reflow_types.h / inc/reflow_css.h / the internal header ----
const k_reflow_style_bold: u8 = 1;
const k_reflow_style_italic: u8 = 2;
const k_reflow_style_underline: u8 = 4;
const k_priv_style_mask: u8 = k_reflow_style_bold | k_reflow_style_italic | k_reflow_style_underline;

const k_reflow_tag_unknown: u8 = 0;
const k_reflow_tag_br: u8 = 12;
const k_reflow_tag_hr: u8 = 13;
const k_reflow_tag_a: u8 = 18;
const k_reflow_tag_img: u8 = 19;
const k_reflow_tag_link: u8 = 24;

const k_reflow_tok_block_start: u8 = 0;
const k_reflow_tok_block_end: u8 = 1;
const k_reflow_tok_text: u8 = 2;
const k_reflow_tok_break: u8 = 3;
const k_reflow_tok_rule: u8 = 4;
const k_reflow_tok_image: u8 = 5;

const k_reflow_color_inherit: u32 = 0xFFFFFFFF;
const k_reflow_align_left: u8 = 0;
const k_reflow_pct_full: u32 = 100;
const k_reflow_min_font_px: u32 = 8;
const k_reflow_max_font_px: u32 = 96;

const k_ra8_css_set_align: u8 = 1 << 3;
const k_ra8_css_set_color: u8 = 1 << 4;
const k_ra8_css_set_fontsize: u8 = 1 << 5;
const k_ra8_css_set_display: u8 = 1 << 6;
const k_ra8_css_set_family: u8 = 1 << 7;
const k_ra8_css_font_pct: u8 = 1;

const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;
const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_validation_failed: c_int = 0x501;
const k_ra8_err_null_ptr: c_int = 0x504;

const k_priv_max_depth: u32 = 64;

// ---- C lexical/attribute/CSS helpers that stay C this slice ----
extern fn priv_reflow_tok_is_xml_whitespace(c: u8) bool;
extern fn priv_reflow_tok_classify(name: [*]const u8, len: usize) u8;
extern fn priv_reflow_tok_is_block(tag: u8) bool;
extern fn priv_reflow_tok_style_for(tag: u8) u8;
extern fn priv_reflow_tok_emit(engine: *ReflowEngine, kind: u8, tag: u8, style: u8, off: u32, len: u32) bool;
extern fn priv_reflow_tok_feed(engine: *ReflowEngine, ch: u8, last_ws: *bool) bool;
extern fn priv_reflow_tok_stash_run(engine: *ReflowEngine, buf: [*]const u8, start: usize, end: usize, out_off: *u32, out_len: *u32) bool;
extern fn priv_reflow_tok_starts_with(buf: [*]const u8, i: usize, len: usize, lit: [*:0]const u8) bool;
extern fn priv_reflow_tok_skip_past(buf: [*]const u8, i: usize, len: usize, lit: [*:0]const u8) usize;
extern fn priv_reflow_tok_find_lit(buf: [*]const u8, i: usize, len: usize, lit: [*:0]const u8) usize;
extern fn priv_reflow_tok_find_attr(tag: [*]const u8, tag_len: usize, name: [*]const u8, name_len: usize, out_voff: *usize, out_vlen: *usize) bool;
extern fn priv_reflow_tok_capture_attr(engine: *ReflowEngine, tag: [*]const u8, tag_len: usize, name: [*]const u8, name_len: usize, out_off: *u32, out_len: *u32) void;
extern fn priv_reflow_tok_css_element(kind: u8, tag: [*]const u8, span: usize) CssElement;
extern fn priv_reflow_tok_css_inline(tag: [*]const u8, span: usize) CssStyle;
extern fn priv_reflow_tok_intern_link(engine: *ReflowEngine, href_off: u32, href_len: u32) u8;
extern fn priv_reflow_tok_rel_is_stylesheet(rel: [*]const u8, len: usize) bool;
extern fn priv_reflow_tok_resolve_face_slot(engine: *const ReflowEngine, comp: *const CssStyle) u8;
extern fn ra8_css_sheet_reset(sheet: *CssSheet) c_int;
extern fn ra8_css_parse(sheet: *CssSheet, css: [*]const u8, len: u32) c_int;
extern fn ra8_css_cascade_ctx(sheet: *const CssSheet, el: *const CssElement, inherited: CssStyle, inline_decl: CssStyle, ancestors: [*]const CssElement, n_anc: u8) CssStyle;

// ---- Mutable tokenizer context (the C file's tok_ctx_t) ----
const TokCtx = struct {
    engine: *ReflowEngine,
    style: u8,
    active_link: u8,
    color: u32,
    css_font_px: u16,
    family_off: u16,
    family_len: u16,
    face_slot: u8,
    stack_el: [k_priv_max_depth]CssElement,
    stack_style: [k_priv_max_depth]u8,
    stack_link: [k_priv_max_depth]u8,
    stack_color: [k_priv_max_depth]u32,
    stack_font: [k_priv_max_depth]u16,
    stack_fam_off: [k_priv_max_depth]u16,
    stack_fam_len: [k_priv_max_depth]u16,
    stack_face: [k_priv_max_depth]u8,
    sp: u32,
    suppress_sp: u32,
    saw_element: bool,
};

fn internal_suppressed(ctx: *const TokCtx) bool {
    return ctx.suppress_sp != 0;
}

fn internal_tag_is(buf: [*]const u8, i: usize, len: usize, name: [*:0]const u8) bool {
    if (!priv_reflow_tok_starts_with(buf, i, len, name)) {
        return false;
    }
    const j = i + std.mem.len(name);
    if (j >= len) {
        return true; // end-of-buffer after the name
    }
    const c = buf[j];
    return c == '>' or c == '/' or priv_reflow_tok_is_xml_whitespace(c);
}

fn internal_handle_cdata(ctx: *TokCtx, buf: [*]const u8, pi: *usize, len: usize) c_int {
    ctx.saw_element = true;
    const inner = pi.* + 9; // strlen("<![CDATA[")
    var close = inner;
    while ((close + 3) <= len and
        !(buf[close] == ']' and buf[close + 1] == ']' and buf[close + 2] == '>'))
    {
        close += 1;
    }
    if (ctx.sp > 0 and !internal_suppressed(ctx)) {
        const off = ctx.engine.text_pool_used;
        var last_ws = true;
        var k = inner;
        while (k < close) : (k += 1) {
            if (!priv_reflow_tok_feed(ctx.engine, buf[k], &last_ws)) {
                return k_ra8_err_no_mem;
            }
        }
        const tlen = ctx.engine.text_pool_used - off;
        if (tlen > 0 and
            !priv_reflow_tok_emit(ctx.engine, k_reflow_tok_text, k_reflow_tag_unknown, ctx.style, off, tlen))
        {
            return k_ra8_err_no_mem;
        }
        if (tlen > 0) {
            ctx.engine.tokens[ctx.engine.token_count - 1].reserved16 = ctx.face_slot;
        }
    }
    pi.* = if ((close + 3) <= len) close + 3 else len;
    return k_ra8_ok;
}

fn internal_handle_end(ctx: *TokCtx, buf: [*]const u8, pi: *usize, len: usize) c_int {
    var i = pi.* + 2; // past "</"
    while (i < len and buf[i] != '>') : (i += 1) {}
    pi.* = if (i < len) i + 1 else len;
    if (ctx.sp == 0) {
        return k_ra8_err_validation_failed;
    }
    ctx.sp -= 1;
    const tag = ctx.stack_el[ctx.sp].tag;
    ctx.style = ctx.stack_style[ctx.sp];
    ctx.active_link = ctx.stack_link[ctx.sp];
    ctx.color = ctx.stack_color[ctx.sp];
    ctx.css_font_px = ctx.stack_font[ctx.sp];
    ctx.family_off = ctx.stack_fam_off[ctx.sp];
    ctx.family_len = ctx.stack_fam_len[ctx.sp];
    ctx.face_slot = ctx.stack_face[ctx.sp];
    if (priv_reflow_tok_is_block(tag) and !internal_suppressed(ctx) and
        !priv_reflow_tok_emit(ctx.engine, k_reflow_tok_block_end, tag, ctx.style, 0, 0))
    {
        return k_ra8_err_no_mem;
    }
    // Exited the display:none subtree once we pop back above its depth.
    if (ctx.suppress_sp != 0 and ctx.sp < ctx.suppress_sp) {
        ctx.suppress_sp = 0;
    }
    return k_ra8_ok;
}

fn internal_parse_start(buf: [*]const u8, pi: *usize, len: usize, selfclose: *bool) u8 {
    var i = pi.* + 1; // past '<'
    const nstart = i;
    while (i < len and buf[i] != '>' and buf[i] != '/' and
        !priv_reflow_tok_is_xml_whitespace(buf[i]))
    {
        i += 1;
    }
    const tag = priv_reflow_tok_classify(buf + nstart, i - nstart);
    selfclose.* = false;
    while (i < len and buf[i] != '>') {
        const c = buf[i];
        if (c == '"' or c == '\'') {
            i += 1;
            while (i < len and buf[i] != c) : (i += 1) {}
        } else if (c == '/' and (i + 1) < len and buf[i + 1] == '>') {
            selfclose.* = true;
        } else {
            // Continue scanning the current start tag.
        }
        i += 1;
    }
    pi.* = if (i < len) i + 1 else len;
    return tag;
}

fn internal_open_attrs(ctx: *TokCtx, tag: [*]const u8, span: usize, kind: u8, block: bool, comp: *const CssStyle, css_font_px: u16) c_int {
    if (internal_suppressed(ctx)) {
        return k_ra8_ok; // inside a display:none subtree -> emit nothing
    }
    if (block) {
        var id_off: u32 = 0;
        var id_len: u32 = 0;
        priv_reflow_tok_capture_attr(ctx.engine, tag, span, "id", 2, &id_off, &id_len);
        if (!priv_reflow_tok_emit(ctx.engine, k_reflow_tok_block_start, kind, ctx.style, id_off, id_len)) {
            return k_ra8_err_no_mem;
        }
        // Stash the block's cascaded alignment + CSS font px on the block-start
        // token (left + 0 = unstyled defaults, so unstyled blocks lay out
        // byte-identically).
        const align_v: u8 = if ((comp.set & k_ra8_css_set_align) != 0)
            comp.@"align"
        else
            k_reflow_align_left;
        const bst = &ctx.engine.tokens[ctx.engine.token_count - 1];
        bst.reserved = align_v;
        bst.css_font_px = css_font_px;
        return k_ra8_ok;
    }
    if (kind == k_reflow_tag_a) {
        var href_off: u32 = 0;
        var href_len: u32 = 0;
        priv_reflow_tok_capture_attr(ctx.engine, tag, span, "href", 4, &href_off, &href_len);
        ctx.active_link = priv_reflow_tok_intern_link(ctx.engine, href_off, href_len);
    }
    return k_ra8_ok;
}

fn internal_handle_link(ctx: *TokCtx, buf: [*]const u8, tag_lt: usize, end: usize) void {
    if (ctx.engine.css_loader == null) {
        return;
    }
    const tagbuf = buf + tag_lt;
    const span: usize = if (end > tag_lt) end - tag_lt else 0;
    var rel_off: usize = 0;
    var rel_len: usize = 0;
    var href_off: usize = 0;
    var href_len: usize = 0;
    if (!priv_reflow_tok_find_attr(tagbuf, span, "rel", 3, &rel_off, &rel_len) or
        !priv_reflow_tok_find_attr(tagbuf, span, "href", 4, &href_off, &href_len) or
        !priv_reflow_tok_rel_is_stylesheet(tagbuf + rel_off, rel_len))
    {
        return;
    }
    var css_bytes: [*]const u8 = undefined;
    var css_len: usize = 0;
    const rc = ctx.engine.css_loader.?(
        ctx.engine.css_loader_ctx,
        tagbuf + href_off,
        @intCast(href_len),
        &css_bytes,
        &css_len,
    );
    if (rc == k_ra8_ok and css_len > 0) {
        _ = ra8_css_parse(&ctx.engine.css, css_bytes, @intCast(css_len));
    }
}

fn internal_handle_void(ctx: *TokCtx, buf: [*]const u8, tag_lt: usize, end: usize, tag: u8, out_err: *c_int) bool {
    if (tag == k_reflow_tag_link) {
        internal_handle_link(ctx, buf, tag_lt, end); // external stylesheet (no token)
        out_err.* = k_ra8_ok;
        return true;
    }
    const is_void = (tag == k_reflow_tag_br) or (tag == k_reflow_tag_hr) or (tag == k_reflow_tag_img);
    if (is_void and internal_suppressed(ctx)) {
        out_err.* = k_ra8_ok; // void tag inside a display:none subtree -> drop
        return true;
    }
    if (tag == k_reflow_tag_br) {
        out_err.* = if (priv_reflow_tok_emit(ctx.engine, k_reflow_tok_break, tag, ctx.style, 0, 0))
            k_ra8_ok
        else
            k_ra8_err_no_mem;
        return true;
    }
    if (tag == k_reflow_tag_hr) {
        out_err.* = if (priv_reflow_tok_emit(ctx.engine, k_reflow_tok_rule, tag, ctx.style, 0, 0))
            k_ra8_ok
        else
            k_ra8_err_no_mem;
        return true;
    }
    if (tag == k_reflow_tag_img) {
        var src_off: u32 = 0;
        var src_len: u32 = 0;
        const span: usize = if (end > tag_lt) end - tag_lt else 0;
        priv_reflow_tok_capture_attr(ctx.engine, buf + tag_lt, span, "src", 3, &src_off, &src_len);
        out_err.* = if (priv_reflow_tok_emit(ctx.engine, k_reflow_tok_image, tag, ctx.style, src_off, src_len))
            k_ra8_ok
        else
            k_ra8_err_no_mem;
        return true;
    }
    return false;
}

fn internal_css_font_px(ctx: *const TokCtx, comp: *const CssStyle) u16 {
    if ((comp.set & k_ra8_css_set_fontsize) == 0) {
        return ctx.css_font_px; // inherit (0 = none -> UA default)
    }
    const parent: u32 = if (ctx.css_font_px != 0) @as(u32, ctx.css_font_px) else @as(u32, ctx.engine.font_px);
    var px: u32 = if (comp.font_unit == k_ra8_css_font_pct)
        (parent * @as(u32, comp.font_val)) / k_reflow_pct_full
    else
        @as(u32, comp.font_val);
    if (px < k_reflow_min_font_px) {
        px = k_reflow_min_font_px;
    }
    if (px > k_reflow_max_font_px) {
        px = k_reflow_max_font_px;
    }
    return @intCast(px);
}

fn internal_open_styled(ctx: *TokCtx, tagbuf: [*]const u8, span: usize, tag: u8, block: bool) c_int {
    const base: u8 = ctx.style | priv_reflow_tok_style_for(tag);
    const el: CssElement = ctx.stack_el[ctx.sp - 1]; // pushed by the caller
    var inh = std.mem.zeroes(CssStyle);
    inh.set = k_priv_style_mask;
    inh.style = base;
    if (ctx.color != k_reflow_color_inherit) {
        inh.set |= k_ra8_css_set_color;
        inh.color = ctx.color;
    }
    // font-family inherits: seed the cascade with the parent's resolved
    // family so a child without its own `font-family` keeps the ancestor's face.
    if (ctx.family_len != 0) {
        inh.set |= k_ra8_css_set_family;
        inh.family_off = ctx.family_off;
        inh.family_len = ctx.family_len;
    }
    const inl = priv_reflow_tok_css_inline(tagbuf, span);
    // Ancestors = the open-element stack below this one (stack_el[0 .. sp-2]).
    const n_anc: u8 = @intCast(ctx.sp - 1);
    const comp = ra8_css_cascade_ctx(&ctx.engine.css, &el, inh, inl, &ctx.stack_el, n_anc);
    const fpx = internal_css_font_px(ctx, &comp);
    // display:none: begin suppressing this element + its subtree at the
    // current depth; internal_open_attrs and the emit sites then drop every
    // token until the matching close pops back above this depth.
    const hidden = (comp.set & k_ra8_css_set_display) != 0 and comp.display != 0;
    if (hidden and ctx.suppress_sp == 0) {
        ctx.suppress_sp = ctx.sp;
    }
    const aerr = internal_open_attrs(ctx, tagbuf, span, tag, block, &comp, fpx);
    if (aerr != k_ra8_ok) {
        return aerr;
    }
    ctx.style = comp.style & k_priv_style_mask;
    ctx.color = if ((comp.set & k_ra8_css_set_color) != 0) comp.color else k_reflow_color_inherit;
    ctx.css_font_px = fpx;
    // Resolve the embedded face for this element from its cascaded family +
    // emphasis. Children inherit the family slice; text runs stamp the
    // resolved slot. Both are 0 / unchanged for content without `@font-face`.
    if ((comp.set & k_ra8_css_set_family) != 0) {
        ctx.family_off = comp.family_off;
        ctx.family_len = comp.family_len;
    }
    ctx.face_slot = priv_reflow_tok_resolve_face_slot(ctx.engine, &comp);
    return k_ra8_ok;
}

fn internal_handle_start(ctx: *TokCtx, buf: [*]const u8, pi: *usize, len: usize) c_int {
    const tag_lt = pi.*; // index of '<' before parse
    var selfclose = false;
    const tag = internal_parse_start(buf, pi, len, &selfclose);
    ctx.saw_element = true;

    var verr: c_int = k_ra8_ok;
    if (internal_handle_void(ctx, buf, tag_lt, pi.*, tag, &verr)) {
        return verr;
    }

    const block = priv_reflow_tok_is_block(tag);
    if (selfclose) {
        if (block and !internal_suppressed(ctx)) {
            if (!priv_reflow_tok_emit(ctx.engine, k_reflow_tok_block_start, tag, ctx.style, 0, 0)) {
                return k_ra8_err_no_mem;
            }
            if (!priv_reflow_tok_emit(ctx.engine, k_reflow_tok_block_end, tag, ctx.style, 0, 0)) {
                return k_ra8_err_no_mem;
            }
        }
        return k_ra8_ok;
    }

    if (ctx.sp >= k_priv_max_depth) {
        return k_ra8_err_no_mem;
    }
    const span: usize = if (pi.* > tag_lt) pi.* - tag_lt else 0;
    // Record the element's full identity (tag + id + class, aliasing the
    // chapter buffer) so the cascade reads it back and descendant selectors
    // can match it as an ancestor of a later element.
    ctx.stack_el[ctx.sp] = priv_reflow_tok_css_element(tag, buf + tag_lt, span);
    ctx.stack_style[ctx.sp] = ctx.style;
    ctx.stack_link[ctx.sp] = ctx.active_link;
    ctx.stack_color[ctx.sp] = ctx.color;
    ctx.stack_font[ctx.sp] = ctx.css_font_px;
    ctx.stack_fam_off[ctx.sp] = ctx.family_off;
    ctx.stack_fam_len[ctx.sp] = ctx.family_len;
    ctx.stack_face[ctx.sp] = ctx.face_slot;
    ctx.sp += 1;
    return internal_open_styled(ctx, buf + tag_lt, span, tag, block);
}

fn internal_handle_raw_text(ctx: *TokCtx, buf: [*]const u8, pi: *usize, len: usize, close_lit: [*:0]const u8, is_style: bool) void {
    ctx.saw_element = true;
    const open_end = priv_reflow_tok_skip_past(buf, pi.*, len, ">");
    const close_at = priv_reflow_tok_find_lit(buf, open_end, len, close_lit);
    if (is_style and close_at > open_end) {
        _ = ra8_css_parse(&ctx.engine.css, buf + open_end, @intCast(close_at - open_end));
    }
    pi.* = if (close_at < len) close_at + std.mem.len(close_lit) else len;
}

fn internal_handle_lt(ctx: *TokCtx, buf: [*]const u8, pi: *usize, len: usize) c_int {
    if (priv_reflow_tok_starts_with(buf, pi.*, len, "<!--")) {
        pi.* = priv_reflow_tok_skip_past(buf, pi.* + 4, len, "-->");
        return k_ra8_ok;
    }
    if (priv_reflow_tok_starts_with(buf, pi.*, len, "<![CDATA[")) {
        return internal_handle_cdata(ctx, buf, pi, len);
    }
    if (priv_reflow_tok_starts_with(buf, pi.*, len, "<?")) {
        pi.* = priv_reflow_tok_skip_past(buf, pi.* + 2, len, "?>");
        return k_ra8_ok;
    }
    if (priv_reflow_tok_starts_with(buf, pi.*, len, "<!")) {
        pi.* = priv_reflow_tok_skip_past(buf, pi.* + 2, len, ">");
        return k_ra8_ok;
    }
    if (internal_tag_is(buf, pi.*, len, "<style")) {
        internal_handle_raw_text(ctx, buf, pi, len, "</style>", true);
        return k_ra8_ok;
    }
    if (internal_tag_is(buf, pi.*, len, "<script")) {
        internal_handle_raw_text(ctx, buf, pi, len, "</script>", false);
        return k_ra8_ok;
    }
    if ((pi.* + 1) < len and buf[pi.* + 1] == '/') {
        return internal_handle_end(ctx, buf, pi, len);
    }
    return internal_handle_start(ctx, buf, pi, len);
}

fn internal_emit_text_run(ctx: *TokCtx, buf: [*]const u8, run: usize, end: usize) bool {
    if (ctx.sp == 0 or internal_suppressed(ctx)) {
        return true; // outside any element, or inside display:none -> drop
    }
    var off: u32 = 0;
    var tln: u32 = 0;
    if (!priv_reflow_tok_stash_run(ctx.engine, buf, run, end, &off, &tln)) {
        return false;
    }
    if (tln == 0) {
        return true;
    }
    if (!priv_reflow_tok_emit(ctx.engine, k_reflow_tok_text, k_reflow_tag_unknown, ctx.style, off, tln)) {
        return false;
    }
    ctx.engine.tokens[ctx.engine.token_count - 1].reserved = ctx.active_link;
    ctx.engine.tokens[ctx.engine.token_count - 1].color = ctx.color;
    ctx.engine.tokens[ctx.engine.token_count - 1].reserved16 = ctx.face_slot;
    return true;
}

/// The walk: fresh CSS rules per chapter, reset scratch state, scan.
pub fn priv_reflow_xml_walk(engine: *ReflowEngine, xhtml_buf: [*]const u8, xhtml_len: usize) c_int {
    _ = ra8_css_sheet_reset(&engine.css); // fresh CSS rules per chapter

    // The tokenizer scratch (per-open-element cascade stacks) is ~2 kB, which
    // would push the frame past the project stack-usage budget. Hold it in
    // module-static storage instead of on the stack and reset it on entry.
    // Safe: a single, non-recursive, single-threaded caller.
    var tok: TokCtx = undefined;
    const ctx = &tok;
    ctx.engine = engine;
    ctx.style = 0; // k_reflow_style_normal
    ctx.active_link = 0;
    ctx.color = k_reflow_color_inherit;
    ctx.css_font_px = 0;
    ctx.family_off = 0;
    ctx.family_len = 0;
    ctx.face_slot = 0;
    ctx.sp = 0;
    ctx.suppress_sp = 0;
    ctx.saw_element = false;
    @memset(&ctx.stack_el, std.mem.zeroes(CssElement));
    @memset(&ctx.stack_style, 0);
    @memset(&ctx.stack_link, 0);
    @memset(&ctx.stack_color, 0);
    @memset(&ctx.stack_font, 0);
    @memset(&ctx.stack_fam_off, 0);
    @memset(&ctx.stack_fam_len, 0);
    @memset(&ctx.stack_face, 0);

    var i: usize = 0;
    while (i < xhtml_len) {
        if (xhtml_buf[i] == '<') {
            const err = internal_handle_lt(ctx, xhtml_buf, &i, xhtml_len);
            if (err != k_ra8_ok) {
                return err;
            }
            continue;
        }
        const run = i;
        while (i < xhtml_len and xhtml_buf[i] != '<') : (i += 1) {}
        if (!internal_emit_text_run(ctx, xhtml_buf, run, i)) {
            return k_ra8_err_no_mem;
        }
    }

    if (!ctx.saw_element) {
        return k_ra8_err_validation_failed;
    }
    if (ctx.sp != 0) {
        return k_ra8_err_validation_failed;
    }
    return k_ra8_ok;
}

comptime {
    _ = ReflowToken;
    _ = CssSheet;
}
