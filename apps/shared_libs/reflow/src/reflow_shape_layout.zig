//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Paragraph and table-cell bidirectional layout orchestration bridging
//! `reflow_shape` (script shaping + UAX #9/29) with `reflow_layout` glyph push.

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lz = @import("reflow_layout.zig");
const u = @import("reflow_unicode.zig");
const bidi = @import("reflow_bidi.zig");
const ot = @import("reflow_opentype.zig");
const sh = @import("reflow_shape.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowToken = abi.ReflowToken;
const PrivCursor = lz.PrivCursor;
const RawChar = sh.RawChar;
const FaceEntry = sh.FaceEntry;
const ShapedItem = sh.ShapedItem;

const k_reflow_margin_px: i32 = 16;
const k_reflow_align_right: u8 = 1;
const k_reflow_tok_text: u8 = 2;
const k_reflow_tok_break: u8 = 3;
const k_reflow_color_inherit: u32 = 0xFFFFFFFF;
const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;

var s_raw_buf: [sh.k_shape_cap]RawChar = undefined;

/// Extract UTF-8 byte slice belonging to `tok` from `engine.text_pool`.
pub fn tokenBytes(engine: *const ReflowEngine, tok: *const ReflowToken) []const u8 {
    if (tok.text_len == 0) return &.{};
    const off: usize = tok.text_off;
    const end: usize = off + tok.text_len;
    if (end > engine.text_pool.len) return &.{};
    return engine.text_pool[off..end];
}

/// True iff any token in an inline slice requires shaping or bidi handling.
pub fn tokensNeedShaping(
    engine: *const ReflowEngine,
    block_dir: bidi.BaseDir,
    tokens: []const ReflowToken,
    faces: []const FaceEntry,
) bool {
    if (block_dir == .rtl or block_dir == .ltr) return true;
    const default_tables = if (faces.len > 0) sh.tablesForBlob(faces[0].data) else ot.loadTables(&.{}, 0);
    for (tokens) |tok| {
        if (tok.kind == k_reflow_tok_text) {
            if (((tok.reserved16 >> 8) & 0x03) != 0) return true;
            if (sh.needsComplexPipeline(tokenBytes(engine, &tok), block_dir, default_tables)) return true;
        }
    }
    return false;
}

/// Compute P2-P3 base paragraph level directly over the inline token stream.
pub fn resolveTokensBaseLevel(engine: *const ReflowEngine, block_dir: bidi.BaseDir, tokens: []const ReflowToken) u8 {
    if (block_dir == .ltr) return 0;
    if (block_dir == .rtl) return 1;
    var iso_depth: u32 = 0;
    for (tokens) |tok| {
        if (tok.kind != k_reflow_tok_text) continue;
        const inline_dir: u8 = @intCast((tok.reserved16 >> 8) & 0x03);
        // Isolate spans (explicit dir attribute) are skipped at top paragraph P2 level.
        if (inline_dir != 0) continue;
        const bytes = tokenBytes(engine, &tok);
        var i: usize = 0;
        while (i < bytes.len) {
            const cp = sh.decodeUtf8Next(bytes, &i);
            const c = u.bidiClass(cp);
            if (c.isIsolateInitiator()) {
                iso_depth += 1;
            } else if (c == .pdi) {
                if (iso_depth > 0) iso_depth -= 1;
            } else if (iso_depth == 0) {
                if (c == .l) return 0;
                if (c == .r or c == .al) return 1;
            }
        }
    }
    return 0;
}

fn emitVisualLine(
    engine: *ReflowEngine,
    cur: *PrivCursor,
    items: []ShapedItem,
    para_level: u8,
) bool {
    const vis = sh.reorderLineForLayout(items, para_level);
    var pen_x: i32 = cur.x;
    var last_base_x: i32 = pen_x;
    var last_base_y: i32 = cur.y;
    for (vis) |it| {
        var gx: i32 = pen_x;
        var gy: i32 = cur.y;
        if (it.is_mark) {
            gx = last_base_x + it.dx;
            gy = last_base_y + it.dy;
        } else {
            pen_x += it.advance;
            last_base_x = gx;
            last_base_y = gy;
        }
        const eff_style: u8 = (it.style & 0x0F) | @as(u8, @intCast((@as(u32, it.face_slot) & 0x0F) << 4));
        if (!lz.priv_reflow_layout_push_glyph(engine, gx, gy, it.cp, it.font_px, eff_style, it.color, it.link_id)) {
            return false;
        }
        cur.line_has_content = 1;
    }
    cur.x = pen_x;
    return true;
}

fn nextBreakUnitEnd(items: []const ShapedItem, start: usize) usize {
    if (start >= items.len) return start;
    if (items[start].is_space or items[start].is_break) return start + 1;
    const is_thai = u.isThai(items[start].orig_cp);
    var end = start + 1;
    while (end < items.len) : (end += 1) {
        const it = items[end];
        if (it.is_space or it.is_break) break;
        if (is_thai and it.wrap_id != items[start].wrap_id) break;
        if (!is_thai and u.isThai(it.orig_cp) and it.wrap_id != items[end - 1].wrap_id) break;
    }
    return end;
}

fn layShapedItems(
    engine: *ReflowEngine,
    cur: *PrivCursor,
    items: []ShapedItem,
    para_level: u8,
    right_limit: i32,
    flush_tail: bool,
) c_int {
    var pos: usize = 0;
    while (pos < items.len) {
        if (items[pos].is_break) {
            if (!lz.priv_reflow_layout_newline(engine, cur, false)) return k_ra8_err_no_mem;
            pos += 1;
            continue;
        }
        while (pos < items.len and cur.line_has_content == 0 and items[pos].is_space) : (pos += 1) {}
        if (pos >= items.len) break;

        var line_end = pos;
        var sim_x: i32 = cur.x;
        var hit_br = false;
        while (line_end < items.len) {
            if (items[line_end].is_break) {
                hit_br = true;
                break;
            }
            const unit_end = nextBreakUnitEnd(items, line_end);
            var unit_w: i32 = 0;
            for (items[line_end..unit_end]) |u_it| unit_w += u_it.advance;
            if ((sim_x + unit_w > right_limit) and (line_end > pos or cur.line_has_content != 0)) {
                break;
            }
            sim_x += unit_w;
            line_end = unit_end;
        }

        var emit_end = line_end;
        if (!hit_br and line_end < items.len) {
            while (emit_end > pos and items[emit_end - 1].is_space) : (emit_end -= 1) {}
        } else if (para_level == 1) {
            while (emit_end > pos and items[emit_end - 1].is_space) : (emit_end -= 1) {}
        }

        if (emit_end > pos) {
            if (!emitVisualLine(engine, cur, items[pos..emit_end], para_level)) return k_ra8_err_no_mem;
        }
        pos = line_end;
        if (hit_br) {
            if (!lz.priv_reflow_layout_newline(engine, cur, false)) return k_ra8_err_no_mem;
            pos += 1;
        } else if (pos < items.len) {
            if (!lz.priv_reflow_layout_newline(engine, cur, true)) return k_ra8_err_no_mem;
        } else if (flush_tail and cur.line_has_content != 0) {
            if (!lz.priv_reflow_layout_newline(engine, cur, true)) return k_ra8_err_no_mem;
        }
    }
    return k_ra8_ok;
}

/// Lay out a contiguous inline token slice (text / `<br>`) within a block,
/// applying shaping, UAX #29 clustering, mark placement, and UAX #9 bidi reordering.
pub fn layComplexBlockRun(
    engine: *ReflowEngine,
    cur: *PrivCursor,
    faces: []const FaceEntry,
    block_dir: bidi.BaseDir,
    has_explicit_align: bool,
    tokens: []const ReflowToken,
) c_int {
    const para_level = resolveTokensBaseLevel(engine, block_dir, tokens);
    if (!has_explicit_align and (block_dir == .rtl or para_level == 1)) {
        cur.@"align" = k_reflow_align_right;
    }
    const right_limit = @as(i32, engine.viewport_w) - k_reflow_margin_px;
    const eff_dir: bidi.BaseDir = if (para_level == 1) .rtl else .ltr;

    var ti: usize = 0;
    var boff: usize = 0;
    var active_inline_dir: u8 = 0;

    while (ti < tokens.len) {
        var raw_n: usize = 0;
        var last_ws_pos: ?usize = null;
        var save_ti = ti;
        var save_boff = boff;
        var save_dir = active_inline_dir;

        // Reserve space for expanding ligatures/matras cleanly inside k_shape_cap.
        const max_chunk = sh.k_shape_cap / 2;
        while (ti < tokens.len and raw_n < max_chunk - 12) {
            const tok = &tokens[ti];
            if (tok.kind == k_reflow_tok_break) {
                if (active_inline_dir != 0) {
                    s_raw_buf[raw_n] = .{ .cp = 0x2069, .font_px = cur.active_font_px, .style = tok.style, .link_id = tok.reserved, .color = engine.body_color };
                    raw_n += 1;
                    active_inline_dir = 0;
                }
                s_raw_buf[raw_n] = .{ .cp = '\n', .font_px = cur.active_font_px, .style = tok.style, .link_id = tok.reserved, .color = engine.body_color };
                raw_n += 1;
                last_ws_pos = raw_n;
                ti += 1;
                boff = 0;
                save_ti = ti;
                save_boff = 0;
                save_dir = active_inline_dir;
                continue;
            }
            if (tok.kind != k_reflow_tok_text) {
                ti += 1;
                boff = 0;
                continue;
            }
            const t_dir: u8 = @intCast((tok.reserved16 >> 8) & 0x03);
            const t_face: u8 = @intCast(tok.reserved16 & 0x0F);
            const link_id = tok.reserved;
            const col: u32 = if (tok.color != k_reflow_color_inherit)
                tok.color
            else if (link_id != 0)
                engine.link_color
            else
                engine.body_color;
            const t_style = tok.style;

            if (boff == 0 and t_dir != active_inline_dir) {
                if (active_inline_dir != 0 and raw_n < max_chunk - 4) {
                    s_raw_buf[raw_n] = .{ .cp = 0x2069, .font_px = cur.active_font_px, .style = t_style, .link_id = link_id, .color = col, .face_slot = t_face };
                    raw_n += 1;
                }
                active_inline_dir = t_dir;
                if (t_dir != 0 and raw_n < max_chunk - 4) {
                    const iso_cp: u32 = if (t_dir == 2) 0x2067 else 0x2066;
                    s_raw_buf[raw_n] = .{ .cp = iso_cp, .font_px = cur.active_font_px, .style = t_style, .link_id = link_id, .color = col, .face_slot = t_face };
                    raw_n += 1;
                }
            }

            const bytes = tokenBytes(engine, tok);
            while (boff < bytes.len and raw_n < max_chunk - 12) {
                const cp = sh.decodeUtf8Next(bytes, &boff);
                s_raw_buf[raw_n] = .{
                    .cp = cp,
                    .font_px = cur.active_font_px,
                    .style = t_style,
                    .link_id = link_id,
                    .color = col,
                    .face_slot = t_face,
                };
                raw_n += 1;
                if (cp == ' ') {
                    last_ws_pos = raw_n;
                    save_ti = ti;
                    save_boff = boff;
                    save_dir = active_inline_dir;
                }
            }
            if (boff >= bytes.len) {
                ti += 1;
                boff = 0;
            }
        }
        const more_follows = (ti < tokens.len);
        if (more_follows) {
            if (last_ws_pos) |wpos| {
                raw_n = wpos;
                ti = save_ti;
                boff = save_boff;
                active_inline_dir = save_dir;
            }
        }
        if (active_inline_dir != 0 and raw_n < max_chunk) {
            s_raw_buf[raw_n] = .{ .cp = 0x2069, .font_px = cur.active_font_px, .color = engine.body_color };
            raw_n += 1;
            active_inline_dir = 0;
        }
        var unused_lvl: u8 = 0;
        const shaped = sh.shapeMultiFaceSequence(s_raw_buf[0..raw_n], eff_dir, faces, &unused_lvl);
        const err = layShapedItems(engine, cur, shaped, para_level, right_limit, more_follows);
        if (err != k_ra8_ok) return err;
    }
    return k_ra8_ok;
}

/// Lay out one table cell text token using shaping + bidi inside `[cell_x, cell_right)`.
pub fn layComplexCellText(
    engine: *ReflowEngine,
    font_opaque: *const lz.StbttFontinfo,
    font_data: []const u8,
    tok: *const ReflowToken,
    cell_x: i32,
    cell_right: i32,
    font_px: u16,
    color: u32,
    cx: *i32,
    cy: *i32,
    lines: *u32,
) void {
    const font: *const sh.StbttFontinfo = @ptrCast(@alignCast(font_opaque));
    const bytes = tokenBytes(engine, tok);
    if (bytes.len == 0) return;
    var raw_n: usize = 0;
    var i: usize = 0;
    while (i < bytes.len and raw_n < sh.k_shape_cap / 2) {
        s_raw_buf[raw_n] = .{
            .cp = sh.decodeUtf8Next(bytes, &i),
            .font_px = font_px,
            .color = color,
        };
        raw_n += 1;
    }
    const line_h: i32 = @intCast(lz.priv_reflow_layout_line_height(font_px));
    var para_level: u8 = 0;
    const single = [_]FaceEntry{.{ .font = font, .data = font_data }};
    const items = sh.shapeMultiFaceSequence(s_raw_buf[0..raw_n], .auto, &single, &para_level);

    var pos: usize = 0;
    while (pos < items.len) {
        while (pos < items.len and cx.* == cell_x and items[pos].is_space) : (pos += 1) {}
        if (pos >= items.len) break;
        var line_end = pos;
        var sim_x = cx.*;
        while (line_end < items.len) {
            const unit_end = nextBreakUnitEnd(items, line_end);
            var unit_w: i32 = 0;
            for (items[line_end..unit_end]) |u_it| unit_w += u_it.advance;
            if ((sim_x + unit_w > cell_right) and (line_end > pos or cx.* > cell_x)) break;
            sim_x += unit_w;
            line_end = unit_end;
        }
        var emit_end = line_end;
        while (emit_end > pos and items[emit_end - 1].is_space) : (emit_end -= 1) {}
        const vis = sh.reorderLineForLayout(items[pos..emit_end], para_level);
        var line_w: i32 = 0;
        for (vis) |v| line_w += v.advance;
        var pen_x = if (para_level == 1 and cell_right - cell_x > line_w) cell_right - line_w else cx.*;
        var last_bx = pen_x;
        var last_by = cy.*;
        for (vis) |v| {
            var gx = pen_x;
            var gy = cy.*;
            if (v.is_mark) {
                gx = last_bx + v.dx;
                gy = last_by + v.dy;
            } else {
                pen_x += v.advance;
                last_bx = gx;
                last_by = gy;
            }
            _ = lz.priv_reflow_layout_push_glyph(engine, gx, gy, v.cp, font_px, 0, color, 0);
        }
        cx.* = if (para_level == 1) cell_x else pen_x;
        pos = line_end;
        if (pos < items.len) {
            cx.* = cell_x;
            cy.* += line_h;
            lines.* += 1;
        }
    }
}
