// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Page tap-target queries for the reflow engine
//! (src/reflow_link.c, RA8FW-1228).
//!
//! Zig port of the public "what did the user tap on this page" query
//! surface declared in reflow.h (hyperlinks, extended for tap-to-zoom).
//! The layout pass populates `engine.link_rects[]`, `engine.link_targets[]`,
//! `engine.anchors[]` and `engine.image_boxes[]`; this TU only reads them:
//!  - reflow_hit_test_link(): point -> href (for a tap).
//!  - reflow_hit_test_image(): point -> laid-out `<img>` box index.
//!  - reflow_find_anchor(): `#fragment` id -> page.
//!  - reflow_href_split(): pure classification of an href string.
//!
//! Pure query logic over engine fields: no layout state, no stbtt, no
//! allocation. The exports keep C linkage so the remaining C callers
//! (and the litehtml v2 engine branch, which still compiles the C TU)
//! link against them exactly as before.
//!
//! @since 0.1.0

const abi = @import("reflow_parse_abi.zig");

const ReflowEngine = abi.ReflowEngine;

// ---- Constants (inc/ra8_err.h, inc/reflow_types.h) ----

const k_ra8_ok: c_int = 0;
const k_ra8_err_invalid_arg: c_int = 0x103;
const k_ra8_err_not_found: c_int = 0x106;
const k_ra8_err_null_ptr: c_int = 0x504;

/// Classification of an in-content `<a href>` target (inc/reflow_types.h).
pub const k_reflow_href_empty: u8 = 0;
pub const k_reflow_href_fragment: u8 = 1;
pub const k_reflow_href_chapter: u8 = 2;
pub const k_reflow_href_chapter_fragment: u8 = 3;
pub const k_reflow_href_external: u8 = 4;

/// Point-in-rect test, half-open on the right/bottom edges so abutting
/// figures never both claim a tap.
fn internal_point_in_rect(x: i32, y: i32, left: i32, top: i32, w: i32, h: i32) bool {
    return (x >= left) and (x < (left + w)) and (y >= top) and (y < (top + h));
}

/// True iff `href` has a URI scheme (a ':' before any '/' in `[0,end)`).
///
/// A scheme makes the link external (http:, https:, mailto:, ...), which the
/// e-reader does not follow. A ':' after a '/' is a path character, not a
/// scheme. The scan walks bytes in order from index 0 up to (but not
/// including) `end`; the first '/' terminates with a false result and the
/// first ':' terminates with a true result. If neither appears the link is
/// treated as scheme-free.
fn internal_has_scheme(href: [*]const u8, end: u32) bool {
    var i: u32 = 0;
    while (i < end) : (i += 1) {
        if (href[i] == '/') {
            return false;
        }
        if (href[i] == ':') {
            return true;
        }
    }
    return false;
}

/// Core href split + classify (no validation; pure on the inputs).
///
/// Locates the fragment '#', detects a URI scheme, and classifies into one
/// of the `k_reflow_href_*` kinds. Factored out of reflow_href_split so the
/// public wrapper holds only the pointer validation. The scan first finds
/// the first '#' to separate the path part from the optional fragment, then
/// delegates scheme detection to internal_has_scheme on the path sub-range.
/// Fragment offset and length are written only when a '#' was found;
/// otherwise the caller-initialized zeros remain unchanged.
fn internal_href_classify(
    href: [*]const u8,
    len: u32,
    out_kind: *u8,
    out_path_len: *u32,
    out_frag_off: *u32,
    out_frag_len: *u32,
) void {
    var hash: u32 = len;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        if (href[i] == '#') {
            hash = i;
            break;
        }
    }
    if (internal_has_scheme(href, hash)) {
        out_kind.* = k_reflow_href_external;
        out_path_len.* = len;
        return;
    }
    const has_frag = (hash < len);
    out_path_len.* = hash;
    if (has_frag) {
        out_frag_off.* = hash + 1;
        out_frag_len.* = len - hash - 1;
    }
    if (hash == 0) {
        out_kind.* = if (has_frag) k_reflow_href_fragment else k_reflow_href_empty;
    } else {
        out_kind.* = if (has_frag) k_reflow_href_chapter_fragment else k_reflow_href_chapter;
    }
}

/// Point -> href (for a tap): scan the page's tappable rects in fill order
/// and hand back the target href slice through the text pool offsets.
pub export fn reflow_hit_test_link(
    engine: ?*const ReflowEngine,
    page_idx: u32,
    x: i32,
    y: i32,
    out_href_off: ?*u32,
    out_href_len: ?*u32,
) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_href_off == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_href_len == null) {
        return k_ra8_err_null_ptr;
    }
    const e = engine.?;
    var i: u32 = 0;
    while (i < e.link_rect_count) : (i += 1) {
        const rect = &e.link_rects[i];
        if (rect.page_index != page_idx) {
            continue;
        }
        if (internal_point_in_rect(x, y, rect.x, rect.y, rect.w, rect.h)) {
            const target = &e.link_targets[rect.target];
            out_href_off.?.* = target.href_off;
            out_href_len.?.* = target.href_len;
            return k_ra8_ok;
        }
    }
    return k_ra8_err_not_found;
}

/// Point -> laid-out `<img>` box index: the sibling of reflow_hit_test_link
/// and the entry point of the reader's tap-to-zoom. Returns the box index so
/// the caller can reach the source slice via the engine text pool.
pub export fn reflow_hit_test_image(
    engine: ?*const ReflowEngine,
    page_idx: u32,
    x: i32,
    y: i32,
    out_index: ?*u32,
) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_index == null) {
        return k_ra8_err_null_ptr;
    }
    const e = engine.?;
    var i: u32 = 0;
    while (i < e.image_box_count) : (i += 1) {
        const box = &e.image_boxes[i];
        if (box.page_index != page_idx) {
            continue;
        }
        if (internal_point_in_rect(x, y, box.x, box.y, box.w, box.h)) {
            out_index.?.* = i;
            return k_ra8_ok;
        }
    }
    return k_ra8_err_not_found;
}

/// `#fragment` id -> page: byte-compare the caller's id against every laid
/// out anchor id in the engine text pool and hand back the page the id
/// landed on.
pub export fn reflow_find_anchor(
    engine: ?*const ReflowEngine,
    id: ?[*]const u8,
    id_len: u32,
    out_page: ?*u32,
) callconv(.c) c_int {
    if (engine == null) {
        return k_ra8_err_null_ptr;
    }
    if (id == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_page == null) {
        return k_ra8_err_null_ptr;
    }
    if (id_len == 0) {
        return k_ra8_err_invalid_arg;
    }
    const e = engine.?;
    var i: u32 = 0;
    while (i < e.anchor_count) : (i += 1) {
        const anchor = &e.anchors[i];
        var equal = (anchor.id_len == id_len);
        var j: u32 = 0;
        while (equal and (j < id_len)) : (j += 1) {
            if (e.text_pool[anchor.id_off + j] != id.?[j]) {
                equal = false;
            }
        }
        if (equal) {
            out_page.?.* = anchor.page_index;
            return k_ra8_ok;
        }
    }
    return k_ra8_err_not_found;
}

/// Pure classification of an href string into a `k_reflow_href_*` kind,
/// with the path length and fragment slice written back.
pub export fn reflow_href_split(
    href: ?[*]const u8,
    len: u32,
    out_kind: ?*u8,
    out_path_len: ?*u32,
    out_frag_off: ?*u32,
    out_frag_len: ?*u32,
) callconv(.c) c_int {
    if (href == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_kind == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_path_len == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_frag_off == null) {
        return k_ra8_err_null_ptr;
    }
    if (out_frag_len == null) {
        return k_ra8_err_null_ptr;
    }

    out_path_len.?.* = 0;
    out_frag_off.?.* = 0;
    out_frag_len.?.* = 0;
    if (len == 0) {
        out_kind.?.* = k_reflow_href_empty;
        return k_ra8_ok;
    }
    internal_href_classify(href.?, len, out_kind.?, out_path_len.?, out_frag_off.?, out_frag_len.?);
    return k_ra8_ok;
}
