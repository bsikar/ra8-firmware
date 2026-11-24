// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Block-level `<img>` layout for the reflow engine
//! (src/reflow_layout_image.c, RA8FW-1226).
//!
//! Zig port of the image-layout sub-responsibility split out of
//! reflow_layout.c so each translation unit stays under the project
//! file-size cap. An `<img>` is laid out as a block: when a loader + arena
//! are bound and the token carries a source slice, the encoded bytes are
//! fetched, the intrinsic size is probed (zero-alloc), the box is scaled
//! to fit the text column without upscaling, the cursor page-breaks as
//! needed, and the box is recorded for the render pass. If no loader is
//! bound, or any sizing step fails, the historic fixed-size placeholder
//! advance is used so image-free content remains byte-identical.
//!
//! The layout core enters this module through priv_reflow_layout_apply_image
//! (kept on C linkage so any remaining C sibling links against it exactly
//! as before); the core inline-flow helpers it reuses (line wrap, page
//! flush) come from the layout core module, which is Zig since
//! RA8FW-1220.
//!
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lz = @import("reflow_layout.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowToken = abi.ReflowToken;
const PrivCursor = lz.PrivCursor;

// ---- Constants (inc/reflow_types.h, src/reflow_layout_internal.h) ----

const k_reflow_margin_px: u16 = 16;
const k_reflow_paragraph_gap_px: u16 = 8;
const k_priv_image_placeholder_px: u16 = 32;

const k_ra8_ok: c_int = 0;

extern fn ra8_img_probe_size(bytes: [*]const u8, len: usize, out_w: *i32, out_h: *i32) callconv(.c) c_int;

/// Apply an `<img>` token without a bound loader: placeholder advance.
/// Historical v1 behaviour, kept so image-free content (and content laid
/// out before a loader is bound) is byte-identical.
fn internal_apply_image_placeholder(engine: *ReflowEngine, cur: *PrivCursor) bool {
    const advance: i32 = @intCast(k_priv_image_placeholder_px);
    const right_limit: i32 = @as(i32, engine.viewport_w) - @as(i32, k_reflow_margin_px);
    if (lz.priv_reflow_internal_right_overflow_break(cur.x, advance, right_limit, cur.line_has_content)) {
        if (!lz.priv_reflow_layout_newline(engine, cur, false)) {
            return false;
        }
    }
    cur.x += advance;
    cur.line_has_content = 1;
    return true;
}

/// True iff the page under construction already holds glyphs or images.
fn internal_page_has_content(engine: *const ReflowEngine, cur: *const PrivCursor) bool {
    return (engine.glyph_count > cur.page_first_glyph) or
        (engine.image_box_count > cur.page_first_image);
}

/// Scale an intrinsic image size to fit the text column (no upscaling).
/// Caps width at the column, height at one page; preserves aspect ratio
/// with i64 products to avoid overflow.
fn internal_image_fit(iw: i32, ih: i32, col_w: i32, avail_h: i32, out_w: *i32, out_h: *i32) void {
    var bw: i32 = if (iw < col_w) iw else col_w;
    var bh: i32 = @intCast(@divTrunc(@as(i64, ih) * @as(i64, bw), @as(i64, iw)));
    if (bh > avail_h) {
        bh = avail_h;
        bw = @intCast(@divTrunc(@as(i64, iw) * @as(i64, bh), @as(i64, ih)));
    }
    out_w.* = if (bw < 1) 1 else bw;
    out_h.* = if (bh < 1) 1 else bh;
}

/// Resolve an image token to a column-fitted box size via the loader.
/// Calls the bound loader for the encoded bytes, probes the intrinsic
/// size (zero-alloc), and fits it to the column. Returns false when the
/// loader failed, the probe failed, or the viewport is too small.
fn internal_image_resolve_size(engine: *ReflowEngine, tok: *const ReflowToken, out_w: *i32, out_h: *i32) bool {
    const loader = engine.img_loader orelse return false;
    const href: [*]const u8 = @ptrCast(&engine.text_pool[tok.text_off]);
    var bytes: [*]const u8 = undefined;
    var blen: usize = 0;
    if (loader(engine.img_loader_ctx, href, tok.text_len, &bytes, &blen) != k_ra8_ok) {
        return false;
    }
    var iw: i32 = 0;
    var ih: i32 = 0;
    if (ra8_img_probe_size(bytes, blen, &iw, &ih) != k_ra8_ok or iw <= 0 or ih <= 0) {
        return false;
    }
    const col_w: i32 = @as(i32, engine.viewport_w) - (2 * @as(i32, k_reflow_margin_px));
    const avail_h: i32 = @as(i32, engine.viewport_h) - (2 * @as(i32, k_reflow_margin_px));
    if ((col_w < 1) or (avail_h < 1)) {
        return false;
    }
    internal_image_fit(iw, ih, col_w, avail_h, out_w, out_h);
    return true;
}

/// Record a laid-out image box and advance the cursor below it. Stores
/// the box at the left margin / current baseline tagged with the active
/// page, then drops the cursor past it.
fn internal_image_record(engine: *ReflowEngine, cur: *PrivCursor, tok: *const ReflowToken, bw: i32, bh: i32) void {
    const box = &engine.image_boxes[engine.image_box_count];
    box.x = @as(i32, k_reflow_margin_px);
    box.y = cur.y;
    box.w = bw;
    box.h = bh;
    box.src_off = tok.text_off;
    box.src_len = tok.text_len;
    box.page_index = engine.page_count;
    box.reserved = 0;
    engine.image_box_count += 1;

    cur.y += bh + @as(i32, k_reflow_paragraph_gap_px);
    cur.line_top = cur.y;
    cur.x = @as(i32, k_reflow_margin_px) + @as(i32, cur.indent_px);
    cur.line_has_content = 0;
}

/// Lay out a real `<img>` as a block: size it, page-break, record it.
/// Returns false (caller falls back to the placeholder) on a full image
/// pool, unresolved src, or a flush overflow.
fn internal_place_image(engine: *ReflowEngine, cur: *PrivCursor, tok: *const ReflowToken) bool {
    if (engine.image_box_count >= engine.image_boxes.len) {
        return false;
    }
    var bw: i32 = 0;
    var bh: i32 = 0;
    if (!internal_image_resolve_size(engine, tok, &bw, &bh)) {
        return false;
    }
    // Block-level: drop below the current line, then page-break if needed.
    if (cur.line_has_content != 0) {
        if (!lz.priv_reflow_layout_newline(engine, cur, false)) {
            return false;
        }
    }
    const bottom_limit: i32 = @as(i32, engine.viewport_h) - @as(i32, k_reflow_margin_px);
    if ((cur.y + bh > bottom_limit) and internal_page_has_content(engine, cur)) {
        if (!lz.priv_reflow_layout_finish_page(engine, cur)) {
            return false;
        }
    }
    internal_image_record(engine, cur, tok, bw, bh);
    // The record above just incremented image_box_count, so the page now
    // invariantly holds an image; the false arm of page_has_content is
    // unreachable here, mirroring the C note.
    if ((cur.y + @as(i32, cur.line_height_px) > bottom_limit) and internal_page_has_content(engine, cur)) {
        if (!lz.priv_reflow_layout_finish_page(engine, cur)) {
            return false;
        }
    }
    return true;
}

pub export fn priv_reflow_layout_apply_image(engine: *ReflowEngine, cur: *PrivCursor, tok: *const ReflowToken) callconv(.c) bool {
    if ((engine.img_loader != null) and (engine.img_arena != null) and (tok.text_len > 0)) {
        if (internal_place_image(engine, cur, tok)) {
            return true;
        }
        // Resolution / pool overflow: fall back to the placeholder advance.
    }
    return internal_apply_image_placeholder(engine, cur);
}
