//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding DB-style detector post-processing: thresholding, streaming
//! two-row union-find connected components, adjacent furigana suppression,
//! line-to-bubble grouping, and manga/webtoon reading-order sorting.

const std = @import("std");
const err_mod = @import("err");
const grp = @import("detector_group");

pub const Error = err_mod.TranslateError;

pub const max_map_dim: u16 = 2048;
pub const max_comps: u16 = 4096;
pub const max_bps: u16 = 10_000;
pub const def_txt_bps: u16 = 3000;
pub const def_bub_bps: u16 = 4000;
pub const def_gap_pct: u16 = 150;
pub const def_furi_pct: u16 = 55;
pub const def_fgap_pct: u16 = 120;

pub const flag_drop_furigana: u8 = 0x01;
pub const flag_4_connected: u8 = 0x02;
pub const flag_valid_mask: u8 = 0x03;

/// Page-level reading order policy for grouped speech bubbles.
pub const ReadingOrder = enum(u8) {
    manga_rtl = 0,
    webtoon_ttb = 1,

    /// Decode C ABI byte into `ReadingOrder`.
    pub fn fromRaw(raw: u8) ?ReadingOrder {
        return switch (raw) {
            0 => .manga_rtl,
            1 => .webtoon_ttb,
            else => null,
        };
    }
};

/// C ABI layout matching `ra8_translate_det_line_t`.
pub const DetLine = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    w: u16 = 0,
    h: u16 = 0,
    bubble_index: u16 = 0,
    confidence_bps: u16 = 0,
    text_orientation: u8 = 0,
    flags: u8 = 0,
    order_in_bubble: u16 = 0,
};

/// C ABI layout matching `ra8_translate_det_bubble_t`.
pub const DetBubble = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    w: u16 = 0,
    h: u16 = 0,
    reading_order: u16 = 0,
    confidence_bps: u16 = 0,
    first_line: u16 = 0,
    line_count: u16 = 0,
    shape: u8 = 0,
    text_orientation: u8 = 0,
    flags: u8 = 0,
    reserved: u8 = 0,
};

/// C ABI layout matching `ra8_translate_det_cfg_t`.
pub const DetCfg = extern struct {
    map_width: u16 = 0,
    map_height: u16 = 0,
    row_stride: u16 = 0,
    elem_stride: u16 = 0,
    page_width: u16 = 0,
    page_height: u16 = 0,
    text_thresh_bps: u16 = def_txt_bps,
    bubble_thresh_bps: u16 = def_bub_bps,
    min_comp_area: u16 = 1,
    box_pad_pct: u16 = 0,
    group_gap_pct: u16 = def_gap_pct,
    furigana_max_ratio_pct: u16 = def_furi_pct,
    furigana_max_gap_pct: u16 = def_fgap_pct,
    reading_order: u8 = 0,
    flags: u8 = flag_drop_furigana,
    max_components: u16 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

/// C ABI layout matching `ra8_translate_det_summary_t`.
pub const DetSummary = extern struct {
    raw_components: u32 = 0,
    dropped_furigana: u32 = 0,
    line_count: u32 = 0,
    bubble_count: u32 = 0,
};

const CompAccum = struct {
    parent: u16,
    x0: u16,
    y0: u16,
    x1: u16,
    y1: u16,
    area: u32,
    score_sum: u32,
};

const CompBox = grp.CompBox;
const BubbleAccum = grp.BubbleAccum;

const ScratchView = struct {
    row_prev: []u16,
    row_curr: []u16,
    pool: []CompAccum,
    remap: []u16,
    alive: []u8,
    text_boxes: []CompBox,
    contour_boxes: []CompBox,
    bubbles: []BubbleAccum,
    order_idx: []u16,
    line_idx: []u16,
    dsu_buf: []u16,
};

const ScratchSizes = struct {
    pool_cap: usize,
    raw_cap: usize,
    row_bytes: usize,
    pool_bytes: usize,
    remap_bytes: usize,
    alive_bytes: usize,
    boxes_bytes: usize,
    bubbles_bytes: usize,
    idx_bytes: usize,
    total_bytes: usize,
};

fn planScratch(map_width: u16, max_components: u16) Error!ScratchSizes {
    if (map_width == 0 or map_width > max_map_dim) return Error.InvalidArg;
    if (max_components == 0 or max_components > max_comps) return Error.InvalidArg;

    const w: usize = @intCast(map_width);
    const raw_cap: usize = @intCast(max_components);
    const pool_cap: usize = 2 * w + 8;

    const row_bytes = alignUp4(w * @sizeOf(u16));
    const pool_bytes = alignUp4(pool_cap * @sizeOf(CompAccum));
    const remap_bytes = alignUp4(pool_cap * @sizeOf(u16));
    const alive_bytes = alignUp4(pool_cap);
    const boxes_bytes = alignUp4(raw_cap * @sizeOf(CompBox));
    const bubbles_bytes = alignUp4(raw_cap * @sizeOf(BubbleAccum));
    const idx_bytes = alignUp4(raw_cap * @sizeOf(u16));

    // Align base buffer by up to 4 bytes in case caller pointer is unaligned.
    var total: usize = 4;
    total = try checkedAdd(total, row_bytes * 2);
    total = try checkedAdd(total, pool_bytes);
    total = try checkedAdd(total, remap_bytes);
    total = try checkedAdd(total, alive_bytes);
    total = try checkedAdd(total, boxes_bytes * 2);
    total = try checkedAdd(total, bubbles_bytes);
    total = try checkedAdd(total, idx_bytes * 3);

    return .{
        .pool_cap = pool_cap,
        .raw_cap = raw_cap,
        .row_bytes = row_bytes,
        .pool_bytes = pool_bytes,
        .remap_bytes = remap_bytes,
        .alive_bytes = alive_bytes,
        .boxes_bytes = boxes_bytes,
        .bubbles_bytes = bubbles_bytes,
        .idx_bytes = idx_bytes,
        .total_bytes = total,
    };
}

fn sliceScratch(work_buf: []u8, sizes: ScratchSizes) Error!ScratchView {
    if (work_buf.len < sizes.total_bytes) return Error.InvalidSize;
    const base_addr = @intFromPtr(work_buf.ptr);
    const misalign = base_addr & 3;
    var cursor: usize = if (misalign == 0) 0 else 4 - misalign;

    const row_prev = sliceTyped(u16, work_buf, cursor, sizes.row_bytes, sizes.row_bytes / 2);
    cursor += sizes.row_bytes;
    const row_curr = sliceTyped(u16, work_buf, cursor, sizes.row_bytes, sizes.row_bytes / 2);
    cursor += sizes.row_bytes;
    const pool = sliceTyped(CompAccum, work_buf, cursor, sizes.pool_bytes, sizes.pool_cap);
    cursor += sizes.pool_bytes;
    const remap = sliceTyped(u16, work_buf, cursor, sizes.remap_bytes, sizes.pool_cap);
    cursor += sizes.remap_bytes;
    const alive = work_buf[cursor .. cursor + sizes.pool_cap];
    cursor += sizes.alive_bytes;
    const text_boxes = sliceTyped(CompBox, work_buf, cursor, sizes.boxes_bytes, sizes.raw_cap);
    cursor += sizes.boxes_bytes;
    const contour_boxes = sliceTyped(CompBox, work_buf, cursor, sizes.boxes_bytes, sizes.raw_cap);
    cursor += sizes.boxes_bytes;
    const bubbles = sliceTyped(BubbleAccum, work_buf, cursor, sizes.bubbles_bytes, sizes.raw_cap);
    cursor += sizes.bubbles_bytes;
    const order_idx = sliceTyped(u16, work_buf, cursor, sizes.idx_bytes, sizes.raw_cap);
    cursor += sizes.idx_bytes;
    const line_idx = sliceTyped(u16, work_buf, cursor, sizes.idx_bytes, sizes.raw_cap);
    cursor += sizes.idx_bytes;
    const dsu_buf = sliceTyped(u16, work_buf, cursor, sizes.idx_bytes, sizes.raw_cap);

    return .{
        .row_prev = row_prev,
        .row_curr = row_curr,
        .pool = pool,
        .remap = remap,
        .alive = alive,
        .text_boxes = text_boxes,
        .contour_boxes = contour_boxes,
        .bubbles = bubbles,
        .order_idx = order_idx,
        .line_idx = line_idx,
        .dsu_buf = dsu_buf,
    };
}

/// Return conservative byte capacity required for `postProcessI8` / `postProcessF32`.
pub fn minWorkBytes(map_width: u16, max_components: u16) Error!usize {
    const sizes = try planScratch(map_width, max_components);
    return sizes.total_bytes;
}

/// Post-process quantized `int8` detector maps into grouped, reading-ordered lines and bubbles.
pub fn postProcessI8(
    text_map: []const i8,
    text_scale: f32,
    text_zp: i8,
    bubble_map_opt: ?[]const i8,
    bubble_scale: f32,
    bubble_zp: i8,
    cfg: DetCfg,
    work_buf: []u8,
    out_lines: []DetLine,
    out_bubbles: []DetBubble,
) Error!DetSummary {
    const valid = try validateCfg(
        cfg,
        text_map.len,
        if (bubble_map_opt) |b| b.len else null,
        out_lines.len,
    );
    if (!std.math.isFinite(text_scale) or text_scale <= 0.0) return Error.InvalidArg;
    if (bubble_map_opt != null and (!std.math.isFinite(bubble_scale) or bubble_scale <= 0.0)) {
        return Error.InvalidArg;
    }

    const sizes = try planScratch(cfg.map_width, valid.raw_cap);
    const scratch = try sliceScratch(work_buf, sizes);

    const txt_src: I8Map = .{ .data = text_map, .scale = text_scale, .zp = text_zp, .geom = valid };
    const text_count = try extractComponents(I8Map, txt_src, cfg.text_thresh_bps, cfg, scratch, scratch.text_boxes);

    var contour_count: usize = 0;
    if (bubble_map_opt) |bmap| {
        const bub_src: I8Map = .{ .data = bmap, .scale = bubble_scale, .zp = bubble_zp, .geom = valid };
        contour_count = try extractComponents(I8Map, bub_src, cfg.bubble_thresh_bps, cfg, scratch, scratch.contour_boxes);
    }

    return finalizePipeline(cfg, valid, scratch, text_count, contour_count, out_lines, out_bubbles);
}

/// Post-process `float` `[0, 1]` detector maps into grouped, reading-ordered lines and bubbles.
pub fn postProcessF32(
    text_map: []const f32,
    bubble_map_opt: ?[]const f32,
    cfg: DetCfg,
    work_buf: []u8,
    out_lines: []DetLine,
    out_bubbles: []DetBubble,
) Error!DetSummary {
    const valid = try validateCfg(
        cfg,
        text_map.len,
        if (bubble_map_opt) |b| b.len else null,
        out_lines.len,
    );
    const sizes = try planScratch(cfg.map_width, valid.raw_cap);
    const scratch = try sliceScratch(work_buf, sizes);

    const txt_src: F32Map = .{ .data = text_map, .geom = valid };
    try txt_src.validateFinite();
    const text_count = try extractComponents(F32Map, txt_src, cfg.text_thresh_bps, cfg, scratch, scratch.text_boxes);

    var contour_count: usize = 0;
    if (bubble_map_opt) |bmap| {
        const bub_src: F32Map = .{ .data = bmap, .geom = valid };
        try bub_src.validateFinite();
        contour_count = try extractComponents(F32Map, bub_src, cfg.bubble_thresh_bps, cfg, scratch, scratch.contour_boxes);
    }

    return finalizePipeline(cfg, valid, scratch, text_count, contour_count, out_lines, out_bubbles);
}

const ValidGeom = struct {
    map_w: usize,
    map_h: usize,
    row_stride: usize,
    elem_stride: usize,
    raw_cap: u16,
    order: ReadingOrder,
    drop_furigana: bool,
    eight_connected: bool,
};

const I8Map = struct {
    data: []const i8,
    scale: f32,
    zp: i8,
    geom: ValidGeom,

    fn probBps(self: I8Map, x: usize, y: usize) u16 {
        const q = self.data[y * self.geom.row_stride + x * self.geom.elem_stride];
        const val = @as(f32, @floatFromInt(@as(i16, q) - @as(i16, self.zp))) * self.scale;
        return floatToBps(val);
    }
};

const F32Map = struct {
    data: []const f32,
    geom: ValidGeom,

    fn validateFinite(self: F32Map) Error!void {
        var y: usize = 0;
        while (y < self.geom.map_h) : (y += 1) {
            var x: usize = 0;
            while (x < self.geom.map_w) : (x += 1) {
                const v = self.data[y * self.geom.row_stride + x * self.geom.elem_stride];
                if (!std.math.isFinite(v)) return Error.InvalidArg;
            }
        }
    }

    fn probBps(self: F32Map, x: usize, y: usize) u16 {
        const val = self.data[y * self.geom.row_stride + x * self.geom.elem_stride];
        return floatToBps(val);
    }
};

fn extractComponents(
    comptime MapType: type,
    map: MapType,
    thresh_bps: u16,
    cfg: DetCfg,
    scratch: ScratchView,
    out_boxes: []CompBox,
) Error!usize {
    const w = map.geom.map_w;
    const h = map.geom.map_h;
    @memset(scratch.row_prev[0..w], 0);
    var active_count: u16 = 0;
    var out_count: usize = 0;

    var y: usize = 0;
    while (y < h) : (y += 1) {
        @memset(scratch.row_curr[0..w], 0);
        var next_id: u16 = active_count + 1;

        var x: usize = 0;
        while (x < w) : (x += 1) {
            const p_bps = map.probBps(x, y);
            if (p_bps < thresh_bps) continue;

            var merged_root: u16 = 0;
            if (x > 0) {
                merged_root = mergeNeighbor(scratch.pool, merged_root, scratch.row_curr[x - 1]);
            }
            merged_root = mergeNeighbor(scratch.pool, merged_root, scratch.row_prev[x]);
            if (map.geom.eight_connected) {
                if (x > 0) merged_root = mergeNeighbor(scratch.pool, merged_root, scratch.row_prev[x - 1]);
                if (x + 1 < w) merged_root = mergeNeighbor(scratch.pool, merged_root, scratch.row_prev[x + 1]);
            }

            const xu16: u16 = @intCast(x);
            const yu16: u16 = @intCast(y);
            if (merged_root == 0) {
                const id = next_id;
                next_id += 1;
                scratch.pool[id] = .{
                    .parent = id,
                    .x0 = xu16,
                    .y0 = yu16,
                    .x1 = xu16,
                    .y1 = yu16,
                    .area = 1,
                    .score_sum = p_bps,
                };
                scratch.row_curr[x] = id;
            } else {
                const acc = &scratch.pool[merged_root];
                if (xu16 < acc.x0) acc.x0 = xu16;
                if (xu16 > acc.x1) acc.x1 = xu16;
                if (yu16 < acc.y0) acc.y0 = yu16;
                if (yu16 > acc.y1) acc.y1 = yu16;
                acc.area +|= 1;
                acc.score_sum +|= p_bps;
                scratch.row_curr[x] = merged_root;
            }
        }

        // Compress row_curr to live roots and mark alive flags.
        const max_used: usize = @intCast(next_id);
        @memset(scratch.alive[0..max_used], 0);
        for (scratch.row_curr[0..w]) |*cell| {
            if (cell.* != 0) {
                const r = findRoot(scratch.pool, cell.*);
                cell.* = r;
                scratch.alive[r] = 1;
            }
        }

        // Flush any closed roots that have no pixel on this row, and compact live roots.
        var new_active: u16 = 0;
        var id: usize = 1;
        while (id < max_used) : (id += 1) {
            if (scratch.pool[id].parent != id) continue;
            if (scratch.alive[id] == 0) {
                if (scratch.pool[id].area >= cfg.min_comp_area) {
                    if (out_count >= out_boxes.len) return Error.InvalidSize;
                    out_boxes[out_count] = scaleComponentBox(scratch.pool[id], cfg);
                    out_count += 1;
                }
            } else {
                new_active += 1;
                scratch.remap[id] = new_active;
            }
        }

        // Shift compacted active labels into slots 1..new_active.
        id = 1;
        while (id < max_used) : (id += 1) {
            if (scratch.pool[id].parent == id and scratch.alive[id] != 0) {
                const dst_id = scratch.remap[id];
                var item = scratch.pool[id];
                item.parent = dst_id;
                scratch.pool[dst_id] = item;
            }
        }
        for (0..w) |col| {
            const c = scratch.row_curr[col];
            scratch.row_prev[col] = if (c != 0) scratch.remap[c] else 0;
        }
        active_count = new_active;
    }

    // Flush remaining components touching the bottom edge.
    var id: usize = 1;
    while (id <= active_count) : (id += 1) {
        if (scratch.pool[id].area >= cfg.min_comp_area) {
            if (out_count >= out_boxes.len) return Error.InvalidSize;
            out_boxes[out_count] = scaleComponentBox(scratch.pool[id], cfg);
            out_count += 1;
        }
    }
    return out_count;
}

fn finalizePipeline(
    cfg: DetCfg,
    valid: ValidGeom,
    scratch: ScratchView,
    text_count: usize,
    contour_count: usize,
    out_lines: []DetLine,
    out_bubbles: []DetBubble,
) Error!DetSummary {
    const raw_count: u32 = @intCast(text_count);
    if (text_count == 0) {
        return .{ .raw_components = 0, .dropped_furigana = 0, .line_count = 0, .bubble_count = 0 };
    }

    const boxes = scratch.text_boxes[0..text_count];
    for (boxes) |*b| {
        b.orient = inferBoxOrient(b.*, valid.order);
    }

    var dropped_furi: u32 = 0;
    if (valid.drop_furigana) {
        dropped_furi = grp.markFuriganaBoxes(boxes, cfg.furigana_max_ratio_pct, cfg.furigana_max_gap_pct);
    }

    // Compact surviving boxes into front of line_idx.
    var live_lines: usize = 0;
    for (boxes, 0..) |b, i| {
        if (!b.dropped) {
            scratch.line_idx[live_lines] = @intCast(i);
            live_lines += 1;
        }
    }
    if (live_lines == 0) {
        return .{ .raw_components = raw_count, .dropped_furigana = dropped_furi, .line_count = 0, .bubble_count = 0 };
    }

    const active_lines = scratch.line_idx[0..live_lines];
    const contours = scratch.contour_boxes[0..contour_count];
    const bubble_count = grp.groupIntoBubbles(
        boxes,
        active_lines,
        contours,
        cfg.group_gap_pct,
        scratch.bubbles,
        scratch.dsu_buf,
        scratch.order_idx,
    );

    if (live_lines > out_lines.len or bubble_count > out_bubbles.len) {
        return Error.InvalidSize;
    }

    grp.sortAndMaterialize(
        DetLine,
        DetBubble,
        boxes,
        active_lines,
        scratch.bubbles[0..bubble_count],
        scratch.order_idx[0..bubble_count],
        valid.order == .manga_rtl,
        out_lines,
        out_bubbles,
    );
    return .{
        .raw_components = raw_count,
        .dropped_furigana = dropped_furi,
        .line_count = @intCast(live_lines),
        .bubble_count = @intCast(bubble_count),
    };
}

fn validateCfg(
    cfg: DetCfg,
    text_len: usize,
    bubble_len_opt: ?usize,
    out_line_cap: usize,
) Error!ValidGeom {
    if (cfg.map_width == 0 or cfg.map_width > max_map_dim) return Error.InvalidArg;
    if (cfg.map_height == 0 or cfg.map_height > max_map_dim) return Error.InvalidArg;
    if (cfg.page_width == 0 or cfg.page_height == 0) return Error.InvalidArg;
    if (cfg.text_thresh_bps == 0 or cfg.text_thresh_bps > max_bps) return Error.InvalidArg;
    if (cfg.bubble_thresh_bps == 0 or cfg.bubble_thresh_bps > max_bps) return Error.InvalidArg;
    if (cfg.min_comp_area == 0) return Error.InvalidArg;
    if (cfg.box_pad_pct > 100 or cfg.group_gap_pct > 1000) return Error.InvalidArg;
    if (cfg.furigana_max_ratio_pct == 0 or cfg.furigana_max_ratio_pct >= 100) return Error.InvalidArg;
    if (cfg.furigana_max_gap_pct > 500) return Error.InvalidArg;
    const order = ReadingOrder.fromRaw(cfg.reading_order) orelse return Error.InvalidArg;
    if ((cfg.flags & ~flag_valid_mask) != 0) return Error.InvalidArg;
    if (cfg.max_components > max_comps) return Error.InvalidArg;
    if (cfg.reserved[0] != 0 or cfg.reserved[1] != 0) return Error.InvalidArg;

    const elem_stride: usize = if (cfg.elem_stride == 0) 1 else @intCast(cfg.elem_stride);
    const map_w: usize = @intCast(cfg.map_width);
    const map_h: usize = @intCast(cfg.map_height);
    const packed_row = try checkedMul(map_w, elem_stride);
    const row_stride: usize = if (cfg.row_stride == 0)
        packed_row
    else if (cfg.row_stride < packed_row)
        return Error.InvalidArg
    else
        @intCast(cfg.row_stride);

    const last_row_off = try checkedMul(map_h - 1, row_stride);
    const needed_elems = try checkedAdd(last_row_off, (map_w - 1) * elem_stride + 1);
    if (text_len < needed_elems) return Error.InvalidSize;
    if (bubble_len_opt) |bl| {
        if (bl < needed_elems) return Error.InvalidSize;
    }

    const raw_cap: u16 = if (cfg.max_components != 0)
        cfg.max_components
    else
        @intCast(@min(@as(usize, max_comps), @max(out_line_cap, 1)));
    return .{
        .map_w = map_w,
        .map_h = map_h,
        .row_stride = row_stride,
        .elem_stride = elem_stride,
        .raw_cap = raw_cap,
        .order = order,
        .drop_furigana = (cfg.flags & flag_drop_furigana) != 0,
        .eight_connected = (cfg.flags & flag_4_connected) == 0,
    };
}

fn scaleComponentBox(acc: CompAccum, cfg: DetCfg) CompBox {
    const mw: u32 = cfg.map_width;
    const mh: u32 = cfg.map_height;
    const pw: u32 = cfg.page_width;
    const ph: u32 = cfg.page_height;

    const raw_x0 = (@as(u32, acc.x0) * pw) / mw;
    const raw_y0 = (@as(u32, acc.y0) * ph) / mh;
    const raw_x1 = (@as(u32, acc.x1) + 1) * pw;
    const raw_y1 = (@as(u32, acc.y1) + 1) * ph;
    var x1 = @min((raw_x1 + mw - 1) / mw, pw);
    var y1 = @min((raw_y1 + mh - 1) / mh, ph);
    var x0 = raw_x0;
    var y0 = raw_y0;

    if (cfg.box_pad_pct > 0) {
        const bw = @max(x1 -| x0, 1);
        const bh = @max(y1 -| y0, 1);
        const pad_x = (bw * cfg.box_pad_pct + 99) / 100;
        const pad_y = (bh * cfg.box_pad_pct + 99) / 100;
        x0 -|= pad_x;
        y0 -|= pad_y;
        x1 = @min(x1 + pad_x, pw);
        y1 = @min(y1 + pad_y, ph);
    }

    const w_px: u16 = @intCast(@max(x1 -| x0, 1));
    const h_px: u16 = @intCast(@max(y1 -| y0, 1));
    const x_px: u16 = @intCast(@min(x0, pw - 1));
    const y_px: u16 = @intCast(@min(y0, ph - 1));
    const conf: u16 = @intCast(@min(acc.score_sum / @max(acc.area, 1), @as(u32, max_bps)));

    return .{
        .x = x_px,
        .y = y_px,
        .w = w_px,
        .h = h_px,
        .conf_bps = conf,
        .orient = 0,
        .dropped = false,
        .group_root = 0,
        .contour_idx = std.math.maxInt(u16),
    };
}

fn inferBoxOrient(b: CompBox, order: ReadingOrder) u8 {
    if (order == .manga_rtl and b.h * 5 > b.w * 6) return 1; // Vertical RL
    return 0; // Horizontal
}

fn findRoot(pool: []CompAccum, start: u16) u16 {
    var cur = start;
    while (pool[cur].parent != cur) {
        cur = pool[cur].parent;
    }
    var walk = start;
    while (pool[walk].parent != cur) {
        const nxt = pool[walk].parent;
        pool[walk].parent = cur;
        walk = nxt;
    }
    return cur;
}

fn mergeNeighbor(pool: []CompAccum, current_root: u16, neighbor_id: u16) u16 {
    if (neighbor_id == 0) return current_root;
    const nr = findRoot(pool, neighbor_id);
    if (current_root == 0) return nr;
    if (current_root == nr) return current_root;
    const keep = @min(current_root, nr);
    const drop = @max(current_root, nr);
    pool[drop].parent = keep;
    if (pool[drop].x0 < pool[keep].x0) pool[keep].x0 = pool[drop].x0;
    if (pool[drop].y0 < pool[keep].y0) pool[keep].y0 = pool[drop].y0;
    if (pool[drop].x1 > pool[keep].x1) pool[keep].x1 = pool[drop].x1;
    if (pool[drop].y1 > pool[keep].y1) pool[keep].y1 = pool[drop].y1;
    pool[keep].area +|= pool[drop].area;
    pool[keep].score_sum +|= pool[drop].score_sum;
    return keep;
}

fn floatToBps(val: f32) u16 {
    const clamped = std.math.clamp(val, @as(f32, 0.0), @as(f32, 1.0));
    const scaled = std.math.clamp(
        clamped * @as(f32, @floatFromInt(max_bps)) + @as(f32, 0.5),
        @as(f32, 0.0),
        @as(f32, @floatFromInt(max_bps)),
    );
    return @intFromFloat(scaled);
}

fn alignUp4(v: usize) usize {
    return (v + 3) & ~@as(usize, 3);
}

fn sliceTyped(comptime T: type, buf: []u8, offset: usize, byte_len: usize, count: usize) []T {
    const raw: [*]align(4) u8 = @alignCast(buf[offset .. offset + byte_len].ptr);
    const typed: [*]T = @ptrCast(raw);
    return typed[0..count];
}

fn checkedAdd(a: usize, b: usize) Error!usize {
    return std.math.add(usize, a, b) catch Error.InvalidSize;
}

fn checkedMul(a: usize, b: usize) Error!usize {
    return std.math.mul(usize, a, b) catch Error.InvalidSize;
}
