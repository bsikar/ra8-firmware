// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! `<table>` equal-column grid layout for the reflow engine
//! (src/reflow_layout_table.c, RA8FW-1223).
//!
//! Zig port of the table-layout sub-responsibility split out of
//! reflow_layout.c so each translation unit stays under the project
//! file-size cap. The grid is an equal-column model: the content width is
//! divided evenly by the maximum per-row cell count, each cell flows its
//! plain-text tokens greedily within its column box, and rows page-break as
//! a unit (rolling the tentative glyphs back and re-laying the row at the
//! top of a fresh page when it would overflow).
//!
//! The driver enters this module through priv_reflow_layout_table (kept on
//! C linkage so the C layout driver links against it exactly as before);
//! the core inline-flow helpers it reuses (line height, glyph advance/push,
//! line wrap, page flush) come from the layout core module, which is Zig
//! since RA8FW-1220.
//!
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");
const lz = @import("reflow_layout.zig");
const shape = @import("reflow_shape.zig");

const ReflowEngine = abi.ReflowEngine;
const ReflowToken = abi.ReflowToken;
const PrivCursor = lz.PrivCursor;
const StbttFontinfo = lz.StbttFontinfo;

// ---- Constants (inc/reflow_types.h, src/reflow_layout_internal.h) ----

const k_reflow_margin_px: u16 = 16;
const k_reflow_paragraph_gap_px: u16 = 8;

const k_reflow_tag_tr: u8 = 21;
const k_reflow_tag_td: u8 = 22;
const k_reflow_tag_th: u8 = 23;

const k_reflow_tok_block_start: u8 = 0;
const k_reflow_tok_block_end: u8 = 1;
const k_reflow_tok_text: u8 = 2;

const k_reflow_align_left: u8 = 0;

const k_priv_cell_pad_px: u16 = 6;
const k_priv_row_gap_px: u16 = 4;

const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;

/// Find the block-end token matching the block-start at `start`. Scans
/// forward matching only tokens whose tag equals the tag at `start`; each
/// nested block-start increments a depth counter, each block-end decrements
/// it, and the index of the token that brings the depth to zero is
/// returned. `limit` is returned when the scan runs out without a match
/// (malformed input), so callers treat the whole remaining range as the
/// cell or row content.
fn match_block_end(engine: *ReflowEngine, start: u32, limit: u32) u32 {
    const tag = engine.tokens[start].tag;
    var depth: i32 = 1;
    var i: u32 = start + 1;
    while (i < limit) : (i += 1) {
        const tok = &engine.tokens[i];
        if (tok.tag != tag) {
            continue;
        }
        if (tok.kind == k_reflow_tok_block_start) {
            depth += 1;
        } else if (tok.kind == k_reflow_tok_block_end) {
            depth -= 1;
            if (depth == 0) {
                return i;
            }
        }
    }
    return limit;
}

/// True iff token `i` opens a table cell (`<td>` or `<th>`).
fn is_cell_start(engine: *ReflowEngine, i: u32) bool {
    const tok = &engine.tokens[i];
    return (tok.kind == k_reflow_tok_block_start) and
        ((tok.tag == k_reflow_tag_td) or (tok.tag == k_reflow_tag_th));
}

/// True iff token `i` opens a table row (`<tr>`).
fn is_row_start(engine: *ReflowEngine, i: u32) bool {
    const tok = &engine.tokens[i];
    return (tok.kind == k_reflow_tok_block_start) and (tok.tag == k_reflow_tag_tr);
}

/// Column count = the maximum number of cells in any row of the table.
/// The scan advances by full row spans so deeply nested same-tag elements
/// do not distort the count; zero for a table with no cells.
fn table_columns(engine: *ReflowEngine, start: u32, end: u32) u32 {
    var cols: u32 = 0;
    var i: u32 = start + 1;
    while (i < end) {
        if (!is_row_start(engine, i)) {
            i += 1;
            continue;
        }
        const tr_end = match_block_end(engine, i, end);
        var cells: u32 = 0;
        var j: u32 = i + 1;
        while (j < tr_end) : (j += 1) {
            if (is_cell_start(engine, j)) {
                cells += 1;
            }
        }
        if (cells > cols) {
            cols = cells;
        }
        i = tr_end + 1;
    }
    return cols;
}

/// Flow one text token inside a cell box, wrapping at the column edge.
/// Greedy word-wrap within `[cell_x, cell_right)` from the running pen
/// `(*cx, *cy)`: spaces are emitted only if the pen is past the left edge
/// and the space fits before the right edge (the stash run collapses
/// leading whitespace, so a cell text token never begins with a space);
/// each word is pre-measured and wraps to `cell_x` on the next line when
/// it would cross the right edge with a non-empty pen. Glyphs land with
/// style 0 and link id 0 (table cells are always plain body text in v1).
fn cell_text(engine: *ReflowEngine, font: *const StbttFontinfo, tok: *const ReflowToken, cell_x: i32, cell_right: i32, font_px: u16, color: u32, cx: *i32, cy: *i32, lines: *u32) void {
    const line_h: i32 = @intCast(lz.priv_reflow_layout_line_height(font_px));
    const base = engine.text_pool[tok.text_off..];
    const len: u32 = tok.text_len;
    const font_data: []const u8 = if (engine.font_data) |fd| fd[0..engine.font_len] else &.{};
    if (len > 0 and shape.needsComplexPipeline(base[0..len], .auto, shape.tablesForBlob(font_data))) {
        shape.layComplexCellText(engine, font, font_data, tok, cell_x, cell_right, font_px, color, cx, cy, lines);
        return;
    }
    var i: u32 = 0;
    while (i < len) {
        if (base[i] == ' ') {
            const adv = lz.priv_reflow_layout_glyph_advance(font, font_px, ' ');
            if ((cx.* > cell_x) and ((cx.* + adv) <= cell_right)) {
                _ = lz.priv_reflow_layout_push_glyph(engine, cx.*, cy.*, ' ', font_px, 0, color, 0);
                cx.* += adv;
            }
            i += 1;
            continue;
        }
        var word_end: u32 = i;
        var word_w: i32 = 0;
        while ((word_end < len) and (base[word_end] != ' ')) {
            word_w += lz.priv_reflow_layout_glyph_advance(font, font_px, base[word_end]);
            word_end += 1;
        }
        if (((cx.* + word_w) > cell_right) and (cx.* > cell_x)) {
            cx.* = cell_x;
            cy.* += line_h;
            lines.* += 1;
        }
        while (i < word_end) {
            const adv = lz.priv_reflow_layout_glyph_advance(font, font_px, base[i]);
            _ = lz.priv_reflow_layout_push_glyph(engine, cx.*, cy.*, base[i], font_px, 0, color, 0);
            cx.* += adv;
            i += 1;
        }
    }
}

/// Lay out one cell's text tokens within a fixed column box (left-flow).
/// Only `text` tokens flow (v1 table cells hold plain text); the running
/// pen and line counter are local to this call and the return value conveys
/// the height consumed so the row can allocate vertical space for the
/// tallest cell. Empty cells floor at one line.
fn layout_cell(engine: *ReflowEngine, font: *const StbttFontinfo, cell_x: i32, cell_w: i32, top_y: i32, color: u32, tstart: u32, tend: u32) u32 {
    const cell_right = cell_x + cell_w;
    const font_px = engine.font_px;
    var cx: i32 = cell_x;
    var cy: i32 = top_y;
    var lines: u32 = 1;
    var t: u32 = tstart;
    while (t < tend) : (t += 1) {
        const tok = &engine.tokens[t];
        if (tok.kind == k_reflow_tok_text) {
            cell_text(engine, font, tok, cell_x, cell_right, font_px, color, &cx, &cy, &lines);
        }
    }
    return lines;
}

/// Lay out every cell of one row at `row_y`; return the row's line count.
/// Each cell maps to a zero-based column index; the content area starts at
/// `margin + col * col_w + pad` and has width `col_w - 2 * pad`.
fn row_cells(engine: *ReflowEngine, font: *const StbttFontinfo, tr_start: u32, tr_end: u32, col_w: i32, row_y: i32) u32 {
    const pad: i32 = @intCast(k_priv_cell_pad_px);
    const color = engine.body_color;
    var rows: u32 = 1;
    var col: u32 = 0;
    var i: u32 = tr_start + 1;
    while (i < tr_end) {
        if (!is_cell_start(engine, i)) {
            i += 1;
            continue;
        }
        const cell_end = match_block_end(engine, i, tr_end);
        const cell_x: i32 = @as(i32, k_reflow_margin_px) + (@as(i32, @intCast(col)) * col_w) + pad;
        const cell_w: i32 = col_w - (2 * pad);
        const lines = layout_cell(engine, font, cell_x, cell_w, row_y, color, i + 1, cell_end);
        if (lines > rows) {
            rows = lines;
        }
        col += 1;
        i = cell_end + 1;
    }
    return rows;
}

/// Lay out one table row, page-breaking before it if it would overflow.
/// The row is laid tentatively at `cur.y`; if its bottom crosses the page
/// bottom margin and there is content above to preserve, the tentative
/// glyphs are rolled back by restoring `engine.glyph_count`, a page flush
/// is issued and the row is re-laid from the top of the fresh page.
fn layout_row(engine: *ReflowEngine, cur: *PrivCursor, font: *const StbttFontinfo, tr_start: u32, table_end: u32, col_w: i32) u32 {
    const tr_end = match_block_end(engine, tr_start, table_end);
    const line_h: i32 = @intCast(lz.priv_reflow_layout_line_height(engine.font_px));
    const bottom_limit: i32 = @as(i32, engine.viewport_h) - @as(i32, k_reflow_margin_px);

    const glyphs_before = engine.glyph_count;
    var row_lines = row_cells(engine, font, tr_start, tr_end, col_w, cur.y);
    var row_h: i32 = @as(i32, @intCast(row_lines)) * line_h;

    if (((cur.y + row_h) > bottom_limit) and (cur.y > @as(i32, k_reflow_margin_px))) {
        engine.glyph_count = glyphs_before; // roll the row back ...
        _ = lz.priv_reflow_layout_finish_page(engine, cur); // ... flush the page ...
        // ... and re-lay it at the top of the fresh page.
        row_lines = row_cells(engine, font, tr_start, tr_end, col_w, cur.y);
        row_h = @as(i32, @intCast(row_lines)) * line_h;
    }
    cur.y += row_h + @as(i32, @intCast(k_priv_row_gap_px));
    return tr_end + 1;
}

/// Lay out a `<table>` range as an equal-column grid: `end + 1` in
/// `out_next`, the cursor reset for linear flow below the table.
pub export fn priv_reflow_layout_table(engine: *ReflowEngine, cur: *PrivCursor, font: *const StbttFontinfo, start: u32, out_next: *u32) callconv(.c) c_int {
    if (cur.line_has_content != 0) {
        if (!lz.priv_reflow_layout_newline(engine, cur, false)) {
            return k_ra8_err_no_mem;
        }
    }
    const end = match_block_end(engine, start, engine.token_count);
    const cols = table_columns(engine, start, end);
    if (cols == 0) {
        out_next.* = if (end < engine.token_count) (end + 1) else engine.token_count;
        return k_ra8_ok;
    }
    const content_w: i32 = @as(i32, engine.viewport_w) - (2 * @as(i32, k_reflow_margin_px));
    const col_w: i32 = @divTrunc(content_w, @as(i32, @intCast(cols)));

    var i: u32 = start + 1;
    while (i < end) {
        if (is_row_start(engine, i)) {
            i = layout_row(engine, cur, font, i, end, col_w);
        } else {
            i += 1;
        }
    }

    // Resume linear flow below the table.
    cur.x = @as(i32, k_reflow_margin_px) + @as(i32, cur.indent_px);
    cur.@"align" = k_reflow_align_left;
    cur.line_has_content = 0;
    cur.y += @as(i32, k_reflow_paragraph_gap_px);
    cur.line_top = cur.y;
    cur.line_first_glyph = engine.glyph_count;
    out_next.* = if (end < engine.token_count) (end + 1) else engine.token_count;
    return k_ra8_ok;
}
