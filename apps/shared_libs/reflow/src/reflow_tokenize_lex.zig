//! Lexical primitives for the no-heap streaming XHTML tokenizer:
//! whitespace/entity/UTF-8 scanning, tag classification, and the
//! text-pool / token-pool emit helpers.
//!
//! Zig port of reflow_tokenize_lex.c (RA8FW-1214). Companion unit to
//! reflow_tokenize.zig: it collects the leaf scanning routines that do not
//! touch the tokenizer's mutable element-stack context. XHTML-whitespace
//! classification, UTF-8 encode/decode, numeric/named entity decoding, tag
//! classification, the whitespace-collapse text-pool feed, the
//! entity-decoding run stash, the token-pool emit, and the literal-search
//! markup scanners. Pure forward-pass scanning: no recursion and bounded
//! loops.
//!
//! The functions keep C linkage (`export fn`) so the attribute / entities /
//! CSS translation units and the C test suites keep linking against them.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");

const ReflowEngine = abi.ReflowEngine;

// ---- Constants carried over from reflow_tokenize_internal.h ----
const k_priv_tag_name_cap: usize = 16;
const k_priv_entity_window: usize = 12;
const k_priv_entity_min: usize = 4;

const k_priv_uc_2byte: u32 = 0x80;
const k_priv_uc_3byte: u32 = 0x800;
const k_priv_uc_4byte: u32 = 0x10000;
const k_priv_uc_max: u32 = 0x10FFFF;
const k_priv_uc_over_max: u32 = 0x110000;
const k_priv_uc_replace: u32 = 0xFFFD;
const k_priv_uc_surr_lo: u32 = 0xD800;
const k_priv_uc_surr_hi: u32 = 0xDFFF;
const k_priv_uc_nonchar_lo: u32 = 0xFFFE;
const k_priv_uc_nonchar_hi: u32 = 0xFFFF;
const k_priv_uc_tab: u32 = 0x09;
const k_priv_uc_lf: u32 = 0x0A;
const k_priv_uc_cr: u32 = 0x0D;
const k_priv_uc_space: u32 = 0x20;

const k_priv_utf8_lead2: u32 = 0xC0;
const k_priv_utf8_lead3: u32 = 0xE0;
const k_priv_utf8_lead4: u32 = 0xF0;
const k_priv_utf8_cont: u32 = 0x80;
const k_priv_utf8_mask: u32 = 0x3F;
const k_priv_utf8_sh6: u5 = 6;
const k_priv_utf8_sh12: u5 = 12;
const k_priv_utf8_sh18: u5 = 18;
const k_priv_utf8_lead2_msk: u32 = 0xE0;
const k_priv_utf8_lead3_msk: u32 = 0xF0;
const k_priv_utf8_lead4_msk: u32 = 0xF8;
const k_priv_utf8_cont_msk: u32 = 0xC0;
const k_priv_utf8_load2: u32 = 0x1F;
const k_priv_utf8_load3: u32 = 0x0F;
const k_priv_utf8_load4: u32 = 0x07;

const k_priv_base_dec: u32 = 10;
const k_priv_base_hex: u32 = 16;
const k_priv_hex_offset: u32 = 10;

// ---- reflow_types.h constants ----
const k_reflow_max_tokens: u32 = 4096;
const k_reflow_text_pool_bytes: u32 = 65536;
const k_reflow_color_inherit: u32 = 0xFFFFFFFF;

const k_reflow_style_bold: u8 = 1;
const k_reflow_style_italic: u8 = 2;
const k_reflow_style_underline: u8 = 4;

const k_reflow_tag_unknown: u8 = 0;
const k_reflow_tag_p: u8 = 1;
const k_reflow_tag_h1: u8 = 2;
const k_reflow_tag_h2: u8 = 3;
const k_reflow_tag_h3: u8 = 4;
const k_reflow_tag_h4: u8 = 5;
const k_reflow_tag_h5: u8 = 6;
const k_reflow_tag_h6: u8 = 7;
const k_reflow_tag_em: u8 = 8;
const k_reflow_tag_strong: u8 = 9;
const k_reflow_tag_b: u8 = 10;
const k_reflow_tag_i: u8 = 11;
const k_reflow_tag_br: u8 = 12;
const k_reflow_tag_hr: u8 = 13;
const k_reflow_tag_ul: u8 = 14;
const k_reflow_tag_ol: u8 = 15;
const k_reflow_tag_li: u8 = 16;
const k_reflow_tag_blockquote: u8 = 17;
const k_reflow_tag_a: u8 = 18;
const k_reflow_tag_img: u8 = 19;
const k_reflow_tag_table: u8 = 20;
const k_reflow_tag_tr: u8 = 21;
const k_reflow_tag_td: u8 = 22;
const k_reflow_tag_th: u8 = 23;
const k_reflow_tag_link: u8 = 24;

// priv_reflow_tok_lookup_entity stays C (reflow_tokenize_entities.c).
extern fn priv_reflow_tok_lookup_entity(name: [*]const u8, len: usize, out_cp: *u32) bool;

pub fn priv_reflow_tok_is_xml_whitespace(c: u8) callconv(.c) bool {
    return (c == ' ') or (c == '\t') or (c == '\n') or (c == '\r') or
        (c == 0x0C) or (c == 0x0B);
}

pub fn priv_reflow_tok_is_xml_char(cp: u32) callconv(.c) bool {
    if ((cp == k_priv_uc_tab) or (cp == k_priv_uc_lf) or (cp == k_priv_uc_cr)) {
        return true;
    }
    if (cp < k_priv_uc_space) {
        return false; // every other C0 control, NUL included
    }
    if ((cp >= k_priv_uc_surr_lo) and (cp <= k_priv_uc_surr_hi)) {
        return false; // UTF-16 surrogate halves are not characters
    }
    if ((cp >= k_priv_uc_nonchar_lo) and (cp <= k_priv_uc_nonchar_hi)) {
        return false; // U+FFFE / U+FFFF
    }
    return cp <= k_priv_uc_max;
}

pub fn priv_reflow_tok_utf8_encode(cp: u32, dst: [*]u8) callconv(.c) usize {
    var value: u32 = cp;
    if (value > k_priv_uc_max) {
        value = k_priv_uc_max;
    }
    if (value < k_priv_uc_2byte) {
        dst[0] = @truncate(value);
        return 1;
    }
    if (value < k_priv_uc_3byte) {
        dst[0] = @truncate(k_priv_utf8_lead2 | (value >> k_priv_utf8_sh6));
        dst[1] = @truncate(k_priv_utf8_cont | (value & k_priv_utf8_mask));
        return 2;
    }
    if (value < k_priv_uc_4byte) {
        dst[0] = @truncate(k_priv_utf8_lead3 | (value >> k_priv_utf8_sh12));
        dst[1] = @truncate(k_priv_utf8_cont | ((value >> k_priv_utf8_sh6) & k_priv_utf8_mask));
        dst[2] = @truncate(k_priv_utf8_cont | (value & k_priv_utf8_mask));
        return 3;
    }
    dst[0] = @truncate(k_priv_utf8_lead4 | (value >> k_priv_utf8_sh18));
    dst[1] = @truncate(k_priv_utf8_cont | ((value >> k_priv_utf8_sh12) & k_priv_utf8_mask));
    dst[2] = @truncate(k_priv_utf8_cont | ((value >> k_priv_utf8_sh6) & k_priv_utf8_mask));
    dst[3] = @truncate(k_priv_utf8_cont | (value & k_priv_utf8_mask));
    return 4;
}

pub fn priv_reflow_tok_utf8_decode(src: [*]const u8, avail: usize, out_cp: *u32) callconv(.c) usize {
    out_cp.* = k_priv_uc_replace;
    if (avail == 0) {
        return 1; // nothing readable: caller still advances, so it cannot stall
    }

    const b0: u32 = src[0];
    if (b0 < k_priv_uc_2byte) {
        out_cp.* = b0; // 0xxxxxxx -- ASCII, the overwhelmingly common case
        return 1;
    }

    // Classify the lead byte. Anything that is not a well-formed lead (a stray
    // continuation 0x80..0xBF, or 0xF8..0xFF) falls through to the one-byte
    // substitution below, which is what keeps the walk in sync.
    var need: usize = 0;
    var cp: u32 = 0;
    var least: u32 = 0;
    if ((b0 & k_priv_utf8_lead2_msk) == k_priv_utf8_lead2) {
        need = 2;
        cp = b0 & k_priv_utf8_load2;
        least = k_priv_uc_2byte;
    } else if ((b0 & k_priv_utf8_lead3_msk) == k_priv_utf8_lead3) {
        need = 3;
        cp = b0 & k_priv_utf8_load3;
        least = k_priv_uc_3byte;
    } else if ((b0 & k_priv_utf8_lead4_msk) == k_priv_utf8_lead4) {
        need = 4;
        cp = b0 & k_priv_utf8_load4;
        least = k_priv_uc_4byte;
    } else {
        return 1; // not a lead byte at all
    }

    if (avail < need) {
        return 1; // truncated by the end of the pool
    }

    // Bounded by k_priv_utf8_max_len; `need` is 2, 3 or 4 by construction.
    var k: usize = 1;
    while (k < need) : (k += 1) {
        if ((@as(u32, src[k]) & k_priv_utf8_cont_msk) != k_priv_utf8_cont) {
            return 1; // a non-continuation ends the sequence early; resync on it
        }
        cp = (cp << k_priv_utf8_sh6) | (@as(u32, src[k]) & k_priv_utf8_mask);
    }

    // Structurally complete, so its extent is known: reject the value but
    // consume the whole sequence rather than re-reading its continuations.
    const overlong = cp < least;
    const surrogate = (cp >= k_priv_uc_surr_lo) and (cp <= k_priv_uc_surr_hi);
    const too_big = cp > k_priv_uc_max;
    if (overlong or surrogate or too_big) {
        return need;
    }

    out_cp.* = cp;
    return need;
}

/// Lower-case ASCII and copy the local part of a tag name, dropping any
/// `prefix:` namespace, truncating at the buffer capacity. Always
/// NUL-terminates. Returns the length written.
fn internal_local_lower(name: [*]const u8, len: usize, dst: *[k_priv_tag_name_cap:0]u8) usize {
    var start: usize = 0;
    var k: usize = 0;
    while (k < len) : (k += 1) {
        if (name[k] == ':') {
            start = k + 1;
        }
    }
    var n: usize = 0;
    k = start;
    while ((k < len) and ((n + 1) < k_priv_tag_name_cap)) : (k += 1) {
        var c = name[k];
        if ((c >= 'A') and (c <= 'Z')) {
            c = c + ('a' - 'A');
        }
        dst[n] = c;
        n += 1;
    }
    dst[n] = 0;
    return n;
}

const TagMapEntry = struct { word: []const u8, tag: u8 };
const k_tag_map = [_]TagMapEntry{
    .{ .word = "p", .tag = k_reflow_tag_p },
    .{ .word = "h1", .tag = k_reflow_tag_h1 },
    .{ .word = "h2", .tag = k_reflow_tag_h2 },
    .{ .word = "h3", .tag = k_reflow_tag_h3 },
    .{ .word = "h4", .tag = k_reflow_tag_h4 },
    .{ .word = "h5", .tag = k_reflow_tag_h5 },
    .{ .word = "h6", .tag = k_reflow_tag_h6 },
    .{ .word = "em", .tag = k_reflow_tag_em },
    .{ .word = "strong", .tag = k_reflow_tag_strong },
    .{ .word = "b", .tag = k_reflow_tag_b },
    .{ .word = "i", .tag = k_reflow_tag_i },
    .{ .word = "br", .tag = k_reflow_tag_br },
    .{ .word = "hr", .tag = k_reflow_tag_hr },
    .{ .word = "ul", .tag = k_reflow_tag_ul },
    .{ .word = "ol", .tag = k_reflow_tag_ol },
    .{ .word = "li", .tag = k_reflow_tag_li },
    .{ .word = "blockquote", .tag = k_reflow_tag_blockquote },
    .{ .word = "a", .tag = k_reflow_tag_a },
    .{ .word = "img", .tag = k_reflow_tag_img },
    .{ .word = "link", .tag = k_reflow_tag_link },
    .{ .word = "table", .tag = k_reflow_tag_table },
    .{ .word = "tr", .tag = k_reflow_tag_tr },
    .{ .word = "td", .tag = k_reflow_tag_td },
    .{ .word = "th", .tag = k_reflow_tag_th },
};

pub fn priv_reflow_tok_classify(name: ?[*]const u8, len: usize) callconv(.c) u8 {
    const src = name orelse return k_reflow_tag_unknown;
    var lower: [k_priv_tag_name_cap:0]u8 = undefined;
    const n = internal_local_lower(src, len, &lower);
    for (k_tag_map) |entry| {
        if (std.mem.eql(u8, lower[0..n], entry.word)) {
            return entry.tag;
        }
    }
    return k_reflow_tag_unknown;
}

/// Decode a `&#dec;` / `&#xhex;` numeric character reference.
///
/// A reference whose value the XML 1.0 `Char` production excludes is not
/// rejected: it is complete in shape, so it is consumed whole and decoded to
/// U+FFFD. Malformed *shape* (no digits, a bad digit, no terminator) fails
/// open, so the caller emits the literal '&'. The digit accumulator saturates
/// at k_priv_uc_over_max instead of wrapping.
fn internal_decode_numeric(src: [*]const u8, avail: usize, out_cp: *u32, out_used: *usize) bool {
    var i: usize = 2; // past "&#", guaranteed < avail by the caller's window check
    var base: u32 = k_priv_base_dec;
    if ((i < avail) and ((src[i] == 'x') or (src[i] == 'X'))) {
        base = k_priv_base_hex;
        i += 1;
    }
    var cp: u32 = 0;
    var digits: usize = 0;
    while ((i < avail) and (src[i] != ';')) {
        const c = src[i];
        var d: u32 = undefined;
        if ((c >= '0') and (c <= '9')) {
            d = @as(u32, c) - '0';
        } else if ((base == k_priv_base_hex) and (c >= 'a') and (c <= 'f')) {
            d = (@as(u32, c) - 'a') + k_priv_hex_offset;
        } else if ((base == k_priv_base_hex) and (c >= 'A') and (c <= 'F')) {
            d = (@as(u32, c) - 'A') + k_priv_hex_offset;
        } else {
            return false;
        }
        if (cp > k_priv_uc_over_max) {
            cp = k_priv_uc_over_max; // saturate: no wrap, still out of range
        } else {
            cp = (cp * base) + d;
        }
        digits += 1;
        i += 1;
    }
    if ((digits == 0) or (i >= avail) or (src[i] != ';')) {
        return false;
    }
    out_cp.* = if (priv_reflow_tok_is_xml_char(cp)) cp else k_priv_uc_replace;
    out_used.* = i + 1;
    return true;
}

pub fn priv_reflow_tok_decode_entity(src: [*]const u8, avail: usize, out_cp: *u32, out_used: *usize) callconv(.c) bool {
    const window = @min(avail, k_priv_entity_window);
    if ((window < k_priv_entity_min) or (src[1] == 0)) {
        return false;
    }
    if (src[1] == '#') {
        return internal_decode_numeric(src, window, out_cp, out_used);
    }
    var end: usize = 1;
    while ((end < window) and (src[end] != ';')) {
        end += 1;
    }
    if ((end >= window) or (src[end] != ';')) {
        return false; // no terminator inside the scan window
    }
    const nlen = end - 1;
    if (!priv_reflow_tok_lookup_entity(src + 1, nlen, out_cp)) {
        return false;
    }
    out_used.* = nlen + 2;
    return true;
}

pub fn priv_reflow_tok_is_block(tag: u8) callconv(.c) bool {
    return switch (tag) {
        k_reflow_tag_p,
        k_reflow_tag_h1,
        k_reflow_tag_h2,
        k_reflow_tag_h3,
        k_reflow_tag_h4,
        k_reflow_tag_h5,
        k_reflow_tag_h6,
        k_reflow_tag_ul,
        k_reflow_tag_ol,
        k_reflow_tag_li,
        k_reflow_tag_blockquote,
        k_reflow_tag_table,
        k_reflow_tag_tr,
        k_reflow_tag_td,
        k_reflow_tag_th,
        => true,
        else => false,
    };
}

pub fn priv_reflow_tok_style_for(tag: u8) callconv(.c) u8 {
    return switch (tag) {
        k_reflow_tag_b, k_reflow_tag_strong => k_reflow_style_bold,
        k_reflow_tag_i, k_reflow_tag_em => k_reflow_style_italic,
        k_reflow_tag_a => k_reflow_style_underline,
        else => 0,
    };
}

/// Append one token to the engine's token pool. Returns false on pool
/// overflow, leaving the pool unchanged.
pub fn priv_reflow_tok_emit(engine: *ReflowEngine, kind: u8, tag: u8, style: u8, off: u32, len: u32) callconv(.c) bool {
    if (engine.token_count >= k_reflow_max_tokens) {
        return false;
    }
    const tok = &engine.tokens[engine.token_count];
    tok.kind = kind;
    tok.tag = tag;
    tok.style = style;
    tok.reserved = 0;
    tok.text_off = off;
    tok.text_len = len;
    tok.color = k_reflow_color_inherit;
    tok.css_font_px = 0;
    tok.reserved16 = 0;
    engine.token_count += 1;
    return true;
}

/// Write one byte through the whitespace-collapse state machine. Runs of
/// ASCII whitespace collapse to a single space; a run beginning at the start
/// of a node drops its leading whitespace. Returns false on pool overflow.
pub fn priv_reflow_tok_feed(engine: *ReflowEngine, ch: u8, last_ws: *bool) callconv(.c) bool {
    var value = ch;
    if (priv_reflow_tok_is_xml_whitespace(value)) {
        if (last_ws.*) {
            return true;
        }
        value = ' ';
        last_ws.* = true;
    } else {
        last_ws.* = false;
    }
    if (engine.text_pool_used >= k_reflow_text_pool_bytes) {
        return false;
    }
    engine.text_pool[engine.text_pool_used] = value;
    engine.text_pool_used += 1;
    return true;
}

/// Feed a code point's UTF-8 bytes through the whitespace-collapse.
fn internal_feed_utf8(engine: *ReflowEngine, cp: u32, last_ws: *bool) bool {
    var enc: [4]u8 = undefined;
    const n = priv_reflow_tok_utf8_encode(cp, &enc);
    var b: usize = 0;
    while (b < n) : (b += 1) {
        if (!priv_reflow_tok_feed(engine, enc[b], last_ws)) {
            return false;
        }
    }
    return true;
}

/// Stash one source position of a text run: a recognised entity is decoded
/// and emitted as UTF-8, otherwise the single byte (including an
/// unrecognised '&') is fed through the whitespace-collapse.
fn internal_stash_one(engine: *ReflowEngine, buf: [*]const u8, i: usize, end: usize, last_ws: *bool, consumed: *usize) bool {
    if (buf[i] == '&') {
        var cp: u32 = 0;
        var used: usize = 0;
        if (priv_reflow_tok_decode_entity(buf + i, end - i, &cp, &used)) {
            consumed.* = used;
            return internal_feed_utf8(engine, cp, last_ws);
        }
    }
    consumed.* = 1;
    return priv_reflow_tok_feed(engine, buf[i], last_ws);
}

/// Entity-decode and whitespace-collapse a text run into the pool, one
/// source position at a time. Returns false on text-pool overflow.
pub fn priv_reflow_tok_stash_run(engine: *ReflowEngine, buf: [*]const u8, start: usize, end: usize, out_off: *u32, out_len: *u32) callconv(.c) bool {
    out_off.* = engine.text_pool_used;
    var last_ws = true; // drop leading whitespace
    var i = start;
    while (i < end) {
        var consumed: usize = 0;
        if (!internal_stash_one(engine, buf, i, end, &last_ws, &consumed)) {
            return false;
        }
        i += consumed;
    }
    out_len.* = engine.text_pool_used - out_off.*;
    return true;
}

/// Test whether `buf` equals the first `len` bytes of `lit`, comparing
/// exactly the caller-bounded extent.
fn internal_bytes_equal(buf: [*]const u8, lit: [*:0]const u8, len: usize) bool {
    var equal = true;
    var k: usize = 0;
    while ((k < len) and equal) : (k += 1) {
        equal = buf[k] == lit[k];
    }
    return equal;
}

pub fn priv_reflow_tok_starts_with(buf: [*]const u8, i: usize, len: usize, lit: [*:0]const u8) callconv(.c) bool {
    const n = std.mem.len(lit);
    if ((i + n) > len) {
        return false;
    }
    return internal_bytes_equal(buf + i, lit, n);
}

/// Return the index just past the first occurrence of `lit`, or `len` if
/// not found; used to skip comments / PIs / declarations.
pub fn priv_reflow_tok_skip_past(buf: [*]const u8, i: usize, len: usize, lit: [*:0]const u8) callconv(.c) usize {
    const n = std.mem.len(lit);
    var pos = i;
    while ((pos + n) <= len) : (pos += 1) {
        if (internal_bytes_equal(buf + pos, lit, n)) {
            return pos + n;
        }
    }
    return len;
}

/// Return the START index of the first occurrence of `lit`, or `len` if
/// absent, so the caller can bound the preceding content and resume past
/// the literal.
pub fn priv_reflow_tok_find_lit(buf: [*]const u8, i: usize, len: usize, lit: [*:0]const u8) callconv(.c) usize {
    const n = std.mem.len(lit);
    var pos = i;
    while ((pos + n) <= len) : (pos += 1) {
        if (internal_bytes_equal(buf + pos, lit, n)) {
            return pos;
        }
    }
    return len;
}

comptime {
    @export(&priv_reflow_tok_is_xml_whitespace, .{ .name = "priv_reflow_tok_is_xml_whitespace" });
    @export(&priv_reflow_tok_is_xml_char, .{ .name = "priv_reflow_tok_is_xml_char" });
    @export(&priv_reflow_tok_utf8_encode, .{ .name = "priv_reflow_tok_utf8_encode" });
    @export(&priv_reflow_tok_utf8_decode, .{ .name = "priv_reflow_tok_utf8_decode" });
    @export(&priv_reflow_tok_classify, .{ .name = "priv_reflow_tok_classify" });
    @export(&priv_reflow_tok_decode_entity, .{ .name = "priv_reflow_tok_decode_entity" });
    @export(&priv_reflow_tok_is_block, .{ .name = "priv_reflow_tok_is_block" });
    @export(&priv_reflow_tok_style_for, .{ .name = "priv_reflow_tok_style_for" });
    @export(&priv_reflow_tok_emit, .{ .name = "priv_reflow_tok_emit" });
    @export(&priv_reflow_tok_feed, .{ .name = "priv_reflow_tok_feed" });
    @export(&priv_reflow_tok_stash_run, .{ .name = "priv_reflow_tok_stash_run" });
    @export(&priv_reflow_tok_starts_with, .{ .name = "priv_reflow_tok_starts_with" });
    @export(&priv_reflow_tok_skip_past, .{ .name = "priv_reflow_tok_skip_past" });
    @export(&priv_reflow_tok_find_lit, .{ .name = "priv_reflow_tok_find_lit" });
}
