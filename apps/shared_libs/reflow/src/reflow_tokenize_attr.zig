//! Attribute and CSS-identity helpers for the no-heap streaming XHTML
//! tokenizer: quoted-attribute scanning, `<img src>` / `<a href>` capture,
//! CSS element-identity + inline-style extraction, link interning, and
//! embedded-face resolution.
//!
//! Zig port of reflow_tokenize_attr.c (RA8FW-1215). Companion unit to
//! reflow_tokenize.zig: it groups the routines that read a raw `<...>`
//! start-tag span: locating a named quoted attribute, copying its value into
//! the engine text pool, building the `ra8_css_element_t` identity the cascade
//! matches against, parsing the inline `style="..."` declaration, interning an
//! `<a href>` into the link-target table, detecting a `<link rel>` stylesheet,
//! and mapping a cascaded font-family + emphasis to an embedded face slot.
//! None of these touch the tokenizer's mutable context. Pure forward-pass
//! scanning: no recursion and bounded loops.
//!
//! The functions keep C linkage (`export fn`) so the entities translation
//! unit and the C test suites keep linking against them. The CSS library
//! stays C: inline parsing and face matching are extern C calls.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lex = @import("reflow_tokenize_lex.zig");

const ReflowEngine = abi.ReflowEngine;
const CssElement = abi.CssElement;
const CssSheet = abi.CssSheet;
const CssStyle = abi.CssStyle;

// Layout constants from inc/reflow_types.h and inc/reflow_css.h.
const k_reflow_text_pool_bytes: usize = 65536;
const k_reflow_max_links: usize = 255;
const k_reflow_max_faces: usize = 8;

// Style + set bits from inc/reflow_types.h and inc/reflow_css.h.
const k_reflow_style_bold: u8 = 1;
const k_reflow_style_italic: u8 = 2;
const k_ra8_css_set_family: u8 = 1 << 7;

// The CSS library stays C: inline-style parsing and face matching.
extern fn ra8_css_parse_inline(decls: [*]const u8, len: u32, out: *CssStyle) c_int;
extern fn ra8_css_match_face(sheet: *const CssSheet, family: [*]const u8, family_len: u16, want_bold: bool, want_italic: bool) i16;

// ---- Quoted-attribute scanning + value capture ----

/// True iff attribute `name` (case-insensitive) begins at `tag[i]`.
///
/// Matches `name` with a non-name byte (or the leading '<') immediately
/// before it, so "xsrc" does not match "src" and "xid" does not match "id".
/// Pure; `name` must already be lower-case.
fn internal_attr_name_at(tag: [*]const u8, i: usize, name: [*]const u8, name_len: usize) bool {
    var k: usize = 0;
    while (k < name_len) : (k += 1) {
        if ((tag[i + k] | 0x20) != name[k]) {
            return false;
        }
    }
    const prev: u8 = if (i == 0) '<' else tag[i - 1];
    const lower = prev >= 'a' and prev <= 'z';
    const upper = prev >= 'A' and prev <= 'Z';
    return !(lower or upper);
}

/// Parse `= "value"` after an attribute name; return the value span.
///
/// Skips whitespace, requires `=`, skips whitespace, requires a quote, then
/// scans to the matching quote. The span excludes the quotes. Pure read of
/// `tag`; on false the outputs are unspecified (caller ignores them).
fn internal_attr_quoted_value(tag: [*]const u8, tag_len: usize, pos: usize, vstart: *usize, vlen: *usize) bool {
    var j = pos;
    while (j < tag_len and lex.priv_reflow_tok_is_xml_whitespace(tag[j])) : (j += 1) {}
    if (j >= tag_len or tag[j] != '=') {
        return false;
    }
    j += 1;
    while (j < tag_len and lex.priv_reflow_tok_is_xml_whitespace(tag[j])) : (j += 1) {}
    if (j >= tag_len or (tag[j] != '"' and tag[j] != '\'')) {
        return false;
    }
    const quote = tag[j];
    j += 1;
    vstart.* = j;
    while (j < tag_len and tag[j] != quote) : (j += 1) {}
    vlen.* = j - vstart.*;
    return true;
}

/// Locate a named attribute's quoted value span within a tag (no copy).
///
/// The non-copying core of `priv_reflow_tok_capture_attr`: returns the value's
/// offset + length within `tag` so callers can either copy it to the pool or
/// scan it in place (e.g. an inline `style`). Pure read of `tag`.
pub fn priv_reflow_tok_find_attr(tag: [*]const u8, tag_len: usize, name: [*]const u8, name_len: usize, out_voff: *usize, out_vlen: *usize) callconv(.c) bool {
    if (tag_len < name_len) {
        return false;
    }
    var i: usize = 0;
    while (i + name_len <= tag_len) : (i += 1) {
        if (internal_attr_name_at(tag, i, name, name_len) and
            internal_attr_quoted_value(tag, tag_len, i + name_len, out_voff, out_vlen))
        {
            return true;
        }
    }
    return false;
}

/// Copy a named attribute's quoted value from a tag span into the text pool.
///
/// Scans the raw `<...>` span for attribute `name` (case-insensitive, not
/// preceded by another name character) and copies its quoted value verbatim
/// into the engine text pool. Used for `<img src>` / `<a href>` / element
/// `id`. On no match, a malformed attribute, an empty value, or a full pool
/// the outputs are zeroed, which callers treat as "attribute absent".
pub fn priv_reflow_tok_capture_attr(engine: *ReflowEngine, tag: [*]const u8, tag_len: usize, name: [*]const u8, name_len: usize, out_off: *u32, out_len: *u32) callconv(.c) void {
    out_off.* = 0;
    out_len.* = 0;
    var vstart: usize = 0;
    var vlen: usize = 0;
    if (!priv_reflow_tok_find_attr(tag, tag_len, name, name_len, &vstart, &vlen)) {
        return;
    }
    if (vlen == 0 or engine.text_pool_used + @as(u32, @intCast(vlen)) > k_reflow_text_pool_bytes) {
        return;
    }
    const off = engine.text_pool_used;
    const pool = engine.text_pool[off..][0..vlen];
    @memcpy(pool, tag[vstart..][0..vlen]);
    engine.text_pool_used += @as(u32, @intCast(vlen));
    out_off.* = off;
    out_len.* = @intCast(vlen);
}

// ---- CSS identity + inline style ----

/// Build a CSS element identity from a just-opened tag's attributes.
///
/// Locates the `id` and `class` attribute value spans in place (no copy) so
/// the cascade can match `#id` / `.class` selectors; the returned pointers
/// alias `tag`. A missing attribute leaves its pointer null and its length
/// zero. The element's tag enum is stored so type selectors also work.
pub fn priv_reflow_tok_css_element(kind: u8, tag: [*]const u8, span: usize) callconv(.c) CssElement {
    var el = std.mem.zeroes(CssElement);
    el.tag = kind;
    var off: usize = 0;
    var len: usize = 0;
    if (priv_reflow_tok_find_attr(tag, span, "id", 2, &off, &len)) {
        el.id = tag + off;
        el.id_len = @intCast(len);
    }
    if (priv_reflow_tok_find_attr(tag, span, "class", 5, &off, &len)) {
        el.class_str = tag + off;
        el.class_len = @intCast(len);
    }
    return el;
}

/// Parse a just-opened tag's inline `style="..."` into a CSS declaration.
///
/// Locates the `style` attribute within the raw tag span and delegates to
/// `ra8_css_parse_inline()` to parse its value into a declaration struct.
/// When the attribute is absent or the parser fails the returned struct has
/// `set == 0`, indicating no inline declarations override the cascade.
pub fn priv_reflow_tok_css_inline(tag: [*]const u8, span: usize) callconv(.c) CssStyle {
    var inl = std.mem.zeroes(CssStyle);
    var off: usize = 0;
    var len: usize = 0;
    if (priv_reflow_tok_find_attr(tag, span, "style", 5, &off, &len)) {
        _ = ra8_css_parse_inline(tag + off, @intCast(len), &inl);
    }
    return inl;
}

// ---- Link interning + stylesheet-rel detection + face resolution ----

/// Intern an `<a>` href slice into the link-target table.
///
/// Appends a {href_off, href_len} entry and returns its 1-based id (stored in
/// subsequent text tokens' `reserved` byte). An empty href or a full table
/// returns 0 (the run renders as plain underlined text: not tappable).
pub fn priv_reflow_tok_intern_link(engine: *ReflowEngine, href_off: u32, href_len: u32) callconv(.c) u8 {
    if (href_len == 0 or engine.link_target_count >= k_reflow_max_links) {
        return 0;
    }
    const idx = engine.link_target_count;
    engine.link_targets[idx].href_off = href_off;
    engine.link_targets[idx].href_len = href_len;
    engine.link_target_count = idx + 1;
    return @intCast(idx + 1); // 1-based: 0 means "no link"
}

/// True if a `rel` attribute value contains the `stylesheet` token.
///
/// Case-sensitive substring scan (EPUB `<link>` rels are authored lowercase;
/// this also accepts the `alternate stylesheet` form). Bounded by the value
/// length, so the scan is O(len) with no dynamic allocation. Pure.
pub fn priv_reflow_tok_rel_is_stylesheet(rel: [*]const u8, len: usize) callconv(.c) bool {
    const kw = "stylesheet";
    const klen: usize = kw.len;
    if (len < klen) {
        return false;
    }
    var i: usize = 0;
    while (i + klen <= len) : (i += 1) {
        var j: usize = 0;
        while (j < klen and rel[i + j] == kw[j]) : (j += 1) {}
        if (j == klen) {
            return true;
        }
    }
    return false;
}

/// Map a cascaded run's (font-family + emphasis) to an embedded face slot.
///
/// Returns 0 (the engine's default bound face) when no embedded face is
/// registered, the run has no resolved `font-family`, no `@font-face` matches,
/// or the matched `@font-face` was never registered (its `src` had no manifest
/// font). Otherwise returns `1 + the registry index`, which the render pass
/// uses to pick the matching `stbtt_fontinfo`.
pub fn priv_reflow_tok_resolve_face_slot(engine: *const ReflowEngine, comp: *const CssStyle) callconv(.c) u8 {
    if (engine.face_count == 0 or (comp.set & k_ra8_css_set_family) == 0 or comp.family_len == 0) {
        return 0;
    }
    const family: [*]const u8 = @ptrCast(&engine.css.names[comp.family_off]);
    const bold = (comp.style & k_reflow_style_bold) != 0;
    const italic = (comp.style & k_reflow_style_italic) != 0;
    const ci = ra8_css_match_face(&engine.css, family, comp.family_len, bold, italic);
    if (ci < 0) {
        return 0;
    }
    var k: usize = 0;
    while (k < engine.face_count and k < k_reflow_max_faces) : (k += 1) {
        if (engine.faces[k].css_face_idx == @as(u8, @intCast(ci))) {
            return @intCast(k + 1);
        }
    }
    return 0;
}

comptime {
    @export(&priv_reflow_tok_find_attr, .{ .name = "priv_reflow_tok_find_attr" });
    @export(&priv_reflow_tok_capture_attr, .{ .name = "priv_reflow_tok_capture_attr" });
    @export(&priv_reflow_tok_css_element, .{ .name = "priv_reflow_tok_css_element" });
    @export(&priv_reflow_tok_css_inline, .{ .name = "priv_reflow_tok_css_inline" });
    @export(&priv_reflow_tok_intern_link, .{ .name = "priv_reflow_tok_intern_link" });
    @export(&priv_reflow_tok_rel_is_stylesheet, .{ .name = "priv_reflow_tok_rel_is_stylesheet" });
    @export(&priv_reflow_tok_resolve_face_slot, .{ .name = "priv_reflow_tok_resolve_face_slot" });
}
