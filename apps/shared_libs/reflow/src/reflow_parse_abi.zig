//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig port of apps/shared_libs/reflow/src/reflow_parse.c (RA8FW-1212):
//! the public parse entry point `reflow_parse_xhtml` behind the unchanged
//! C headers in inc/. The C TU validated its arguments, reset the token
//! and text pools plus the link-target table, and forwarded to the
//! markup walk, now ported to Zig in src/reflow_tokenize.zig (RA8FW-1213).
//!
//! The `ReflowEngine` mirror below is a comptime-faithful extern-struct
//! copy of `reflow_t` (inc/reflow_types.h) with its nested pool, face,
//! link and stylesheet types; the `ra8_img_arena_t` and `ra8_glyph_atlas_t`
//! members are pointer-only here, so they stay opaque. The host tests
//! assert the touched fields sit at the offsets the C compiler computes
//! for the same header.

const std = @import("std");

// Layout constants from inc/reflow_types.h and inc/reflow_css.h.
const k_reflow_max_tokens = 4096;
const k_reflow_text_pool_bytes = 65536;
const k_reflow_max_glyphs = 32768;
const k_reflow_max_pages = 256;
const k_reflow_max_images = 64;
const k_reflow_max_links = 255;
const k_reflow_max_link_rects = 512;
const k_reflow_max_anchors = 256;
const k_reflow_max_faces = 8;
const k_reflow_face_pad8 = 7;
const k_ra8_css_max_rules = 128;
const k_ra8_css_max_faces = 16;
const k_ra8_css_max_anc = 4;
const k_ra8_css_name_pool = 2048;

// Error codes from libs/ra8_core/inc/ra8_err.h (the C ABI keeps ra8_err_t
// a plain int-sized enum).
const k_ra8_ok: c_int = 0;
const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_not_initialized: c_int = 0x10F;
const k_ra8_err_null_ptr: c_int = 0x504;

pub const ReflowToken = extern struct {
    kind: u8,
    tag: u8,
    style: u8,
    reserved: u8,
    text_off: u32,
    text_len: u32,
    color: u32,
    css_font_px: u16,
    reserved16: u16,
};

const ReflowGlyph = extern struct {
    x: i32,
    y: i32,
    cp: i32,
    color: u32,
    font_px: u16,
    style: u8,
    reserved: u8,
};

const ReflowPage = extern struct {
    glyph_first: u32,
    glyph_count: u32,
};

const ReflowImageBox = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    src_off: u32,
    src_len: u32,
    page_index: u32,
    reserved: u32,
};

const ReflowLinkTarget = extern struct {
    href_off: u32,
    href_len: u32,
};

const ReflowLinkRect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    target: u32,
    page_index: u32,
};

const ReflowAnchor = extern struct {
    id_off: u32,
    id_len: u32,
    page_index: u32,
    y: i32,
};

const ReflowFace = extern struct {
    blob: ?[*]const u8,
    len: usize,
    css_face_idx: u8,
    pad8: [k_reflow_face_pad8]u8,
};

/// Mirror of `ra8_css_element_t` (inc/reflow_css.h): the identity of one
/// element, with the id / class slices pointing at caller-owned bytes.
pub const CssElement = extern struct {
    tag: u8,
    pad0: [7]u8,
    id: ?[*]const u8,
    id_len: u16,
    pad1: [6]u8,
    class_str: ?[*]const u8,
    class_len: u16,
    pad2: [6]u8,
};

pub const CssAnc = extern struct {
    tag: u8,
    pad8: u8,
    class_off: u16,
    class_len: u16,
    id_off: u16,
    id_len: u16,
};

pub const CssStyle = extern struct {
    set: u8,
    style: u8,
    @"align": u8,
    font_unit: u8,
    color: u32,
    font_val: u16,
    display: u8,
    pad: u8,
    family_off: u16,
    family_len: u16,
};

const CssRule = extern struct {
    sel_tag: u8,
    anc_count: u8,
    class_off: u16,
    class_len: u16,
    id_off: u16,
    id_len: u16,
    order: u16,
    anc: [k_ra8_css_max_anc]CssAnc,
    decl: CssStyle,
};

const CssFontFace = extern struct {
    family_off: u16,
    family_len: u16,
    src_off: u16,
    src_len: u16,
    weight_bold: u8,
    style_italic: u8,
    pad: u16,
};

pub const CssSheet = extern struct {
    rules: [k_ra8_css_max_rules]CssRule,
    faces: [k_ra8_css_max_faces]CssFontFace,
    rule_count: u16,
    face_count: u16,
    next_order: u16,
    names_used: u16,
    names: [k_ra8_css_name_pool]u8,
};

/// Mirror of `reflow_t` (inc/reflow_types.h). Only `in_use` and the three
/// pool counters are touched by this TU; the rest exists to hold the C
/// layout exactly.
pub const ReflowEngine = extern struct {
    viewport_w: u16,
    viewport_h: u16,
    font_px: u16,
    reserved16: u16,
    body_color: u32,
    link_color: u32,
    font_data: ?[*]const u8,
    font_len: usize,
    faces: [k_reflow_max_faces]ReflowFace,
    face_count: u8,
    xhtml_buf: ?[*]const u8,
    xhtml_len: usize,
    tokens: [k_reflow_max_tokens]ReflowToken,
    token_count: u32,
    text_pool: [k_reflow_text_pool_bytes]u8,
    text_pool_used: u32,
    glyphs: [k_reflow_max_glyphs]ReflowGlyph,
    glyph_count: u32,
    pages: [k_reflow_max_pages]ReflowPage,
    page_count: u32,
    img_loader: ?ReflowLoaderFn,
    img_loader_ctx: ?*anyopaque,
    img_arena: ?*anyopaque,
    css_loader: ?ReflowLoaderFn,
    css_loader_ctx: ?*anyopaque,
    image_boxes: [k_reflow_max_images]ReflowImageBox,
    image_box_count: u32,
    link_targets: [k_reflow_max_links]ReflowLinkTarget,
    link_target_count: u32,
    link_rects: [k_reflow_max_link_rects]ReflowLinkRect,
    link_rect_count: u32,
    anchors: [k_reflow_max_anchors]ReflowAnchor,
    anchor_count: u32,
    css: CssSheet,
    glyph_atlas: ?*anyopaque,
    in_use: u8,
    reserved8: [3]u8,
};

/// The image / stylesheet loader hooks (inc/reflow_types.h typedefs).
pub const ReflowLoaderFn = *const fn (?*anyopaque, [*]const u8, c_uint, ?*[*]const u8, ?*usize) callconv(.c) c_int;

/// The markup walk, ported to Zig in src/reflow_tokenize.zig (RA8FW-1213).
const walk = @import("reflow_tokenize.zig");

/// Validate the arguments, reset the token and text pools plus the
/// link-target table, and hand the buffer to the tokenizer. The layout
/// pools (glyphs/pages/images/link-rects/anchors) are cleared separately
/// by reflow_run_layout().
pub export fn reflow_parse_xhtml(engine: ?*ReflowEngine, xhtml_buf: ?[*]const u8, xhtml_len: usize) callconv(.c) c_int {
    if (engine == null or xhtml_buf == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (xhtml_len == 0) {
        return k_ra8_err_invalid_size;
    }

    engine.?.token_count = 0;
    engine.?.text_pool_used = 0;
    engine.?.link_target_count = 0;

    return walk.priv_reflow_xml_walk(engine.?, xhtml_buf.?, xhtml_len);
}

test "mirror layout sanity" {
    comptime {
        std.debug.assert(@offsetOf(ReflowEngine, "in_use") == @sizeOf(ReflowEngine) - 8);
        std.debug.assert(@offsetOf(ReflowEngine, "token_count") < @offsetOf(ReflowEngine, "text_pool"));
        std.debug.assert(@offsetOf(ReflowEngine, "text_pool_used") < @offsetOf(ReflowEngine, "link_targets"));
        std.debug.assert(@offsetOf(ReflowEngine, "link_target_count") < @offsetOf(ReflowEngine, "link_rects"));
    }
}

comptime {
    _ = k_ra8_ok;
}
