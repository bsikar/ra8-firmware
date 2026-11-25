//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Line-to-bubble grouping, contour association, and manga/webtoon reading-order
//! sorting shared by `detector_post.zig`.

const std = @import("std");

/// Scaled candidate text line or bubble contour bounding box.
pub const CompBox = struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    conf_bps: u16,
    orient: u8,
    dropped: bool,
    group_root: u16,
    contour_idx: u16,
};

/// Grouping accumulator for one candidate speech bubble.
pub const BubbleAccum = struct {
    parent: u16,
    x: u16,
    y: u16,
    x1: u16,
    y1: u16,
    conf_sum: u32,
    line_count: u16,
    vert_votes: u16,
    horiz_votes: u16,
    from_bubble_map: bool,
    first_line: u16,
    reading_order: u16,
};

/// Return overlapping pixel span between `[a0, a0 + aw)` and `[b0, b0 + bw)`.
pub fn axisOverlap(a0: u16, aw: u16, b0: u16, bw: u16) u32 {
    const lo = @max(@as(u32, a0), b0);
    const hi = @min(@as(u32, a0) + aw, @as(u32, b0) + bw);
    return if (hi > lo) hi - lo else 0;
}

/// Return non-negative gap between `[a0, a0 + aw)` and `[b0, b0 + bw)`.
pub fn axisGap(a0: u16, aw: u16, b0: u16, bw: u16) u32 {
    const a1: u32 = @as(u32, a0) + aw;
    const b1: u32 = @as(u32, b0) + bw;
    if (a1 < b0) return b0 - a1;
    if (b1 < a0) return a0 - b1;
    return 0;
}

/// Suppress small furigana strips immediately adjacent to wider main text lines.
pub fn markFuriganaBoxes(
    boxes: []CompBox,
    max_ratio_pct: u16,
    max_gap_pct: u16,
) u32 {
    var dropped: u32 = 0;
    for (boxes, 0..) |cand, ci| {
        const c_vert = cand.h >= cand.w;
        const c_minor: u32 = @min(cand.w, cand.h);
        var suppress = false;

        for (boxes, 0..) |main, mi| {
            if (mi == ci) continue;
            const m_vert = main.h > main.w;
            const m_minor: u32 = @min(main.w, main.h);
            if (c_minor * 100 > m_minor * max_ratio_pct) continue;
            if (c_minor >= m_minor) continue;

            if (m_vert and c_vert) {
                const max_gap = (m_minor * max_gap_pct + 99) / 100;
                const x_gap = axisGap(cand.x, cand.w, main.x, main.w);
                const y_ov = axisOverlap(cand.y, cand.h, main.y, main.h);
                if (x_gap <= max_gap and y_ov * 100 >= @as(u32, cand.h) * 30 and cand.h <= main.h + main.w) {
                    suppress = true;
                    break;
                }
            } else if (!m_vert and !c_vert) {
                const max_gap = (m_minor * max_gap_pct + 99) / 100;
                const y_gap = axisGap(cand.y, cand.h, main.y, main.h);
                const x_ov = axisOverlap(cand.x, cand.w, main.x, main.w);
                if (y_gap <= max_gap and x_ov * 100 >= @as(u32, cand.w) * 30 and cand.w <= main.w + main.h) {
                    suppress = true;
                    break;
                }
            }
        }
        if (suppress) {
            boxes[ci].dropped = true;
            dropped += 1;
        }
    }
    return dropped;
}

/// Group surviving lines into speech bubbles via bubble contours and orientation-aware proximity.
pub fn groupIntoBubbles(
    boxes: []CompBox,
    active_lines: []const u16,
    contours: []const CompBox,
    group_gap_pct: u16,
    bubbles_out: []BubbleAccum,
    dsu: []u16,
    root_to_bubble: []u16,
) usize {
    assignContourIds(boxes, active_lines, contours);
    const n = active_lines.len;
    for (0..n) |i| {
        dsu[i] = @intCast(i);
    }

    for (0..n) |i| {
        const bi = boxes[active_lines[i]];
        const minor_i: u32 = @max(@min(bi.w, bi.h), 1);
        for (i + 1..n) |j| {
            const bj = boxes[active_lines[j]];
            if (shouldMergeLines(bi, minor_i, bj, group_gap_pct)) {
                unionSlice(dsu[0..n], @intCast(i), @intCast(j));
            }
        }
    }

    for (0..n) |i| {
        dsu[i] = findSliceRoot(dsu[0..n], @intCast(i));
    }

    var bubble_count: usize = 0;
    for (0..n) |i| {
        if (dsu[i] == i) {
            const b = boxes[active_lines[i]];
            bubbles_out[bubble_count] = .{
                .parent = @intCast(bubble_count),
                .x = b.x,
                .y = b.y,
                .x1 = b.x + b.w,
                .y1 = b.y + b.h,
                .conf_sum = 0,
                .line_count = 0,
                .vert_votes = 0,
                .horiz_votes = 0,
                .from_bubble_map = b.contour_idx != std.math.maxInt(u16),
                .first_line = 0,
                .reading_order = 0,
            };
            root_to_bubble[i] = @intCast(bubble_count);
            bubble_count += 1;
        }
    }

    for (0..n) |i| {
        const li = active_lines[i];
        const b = boxes[li];
        const bub_idx = root_to_bubble[dsu[i]];
        boxes[li].group_root = bub_idx;

        const acc = &bubbles_out[bub_idx];
        if (b.x < acc.x) acc.x = b.x;
        if (b.y < acc.y) acc.y = b.y;
        if (b.x + b.w > acc.x1) acc.x1 = b.x + b.w;
        if (b.y + b.h > acc.y1) acc.y1 = b.y + b.h;
        acc.conf_sum += b.conf_bps;
        acc.line_count += 1;
        if (b.orient == 0) acc.horiz_votes += 1 else acc.vert_votes += 1;

        if (b.contour_idx != std.math.maxInt(u16)) {
            const c = contours[b.contour_idx];
            if (c.x < acc.x) acc.x = c.x;
            if (c.y < acc.y) acc.y = c.y;
            if (c.x + c.w > acc.x1) acc.x1 = c.x + c.w;
            if (c.y + c.h > acc.y1) acc.y1 = c.y + c.h;
        }
    }

    return bubble_count;
}

/// Order grouped bubbles into horizontal page tiers (`manga_rtl` vs `webtoon_ttb`) and sort
/// lines cleanly within each bubble, writing into caller slices `out_lines` and `out_bubbles`.
pub fn sortAndMaterialize(
    comptime DetLine: type,
    comptime DetBubble: type,
    boxes: []const CompBox,
    active_lines: []const u16,
    bubbles: []BubbleAccum,
    order_idx: []u16,
    manga_rtl: bool,
    out_lines: []DetLine,
    out_bubbles: []DetBubble,
) void {
    const nb = bubbles.len;
    for (0..nb) |i| {
        order_idx[i] = @intCast(i);
    }

    const BubbleSortCtx = struct {
        bubs: []const BubbleAccum,
        fn lessY(ctx: @This(), a: u16, b: u16) bool {
            const ba = ctx.bubs[a];
            const bb = ctx.bubs[b];
            if (ba.y != bb.y) return ba.y < bb.y;
            if (ba.x1 != bb.x1) return ba.x1 > bb.x1;
            return a < b;
        }
        fn lessXAsc(ctx: @This(), a: u16, b: u16) bool {
            const ba = ctx.bubs[a];
            const bb = ctx.bubs[b];
            if (ba.x != bb.x) return ba.x < bb.x;
            return ba.y < bb.y;
        }
        fn lessRightDesc(ctx: @This(), a: u16, b: u16) bool {
            const ba = ctx.bubs[a];
            const bb = ctx.bubs[b];
            if (ba.x1 != bb.x1) return ba.x1 > bb.x1;
            return ba.y < bb.y;
        }
    };
    const ctx: BubbleSortCtx = .{ .bubs = bubbles };
    std.sort.insertion(u16, order_idx[0..nb], ctx, BubbleSortCtx.lessY);

    var start: usize = 0;
    while (start < nb) {
        const first_b = bubbles[order_idx[start]];
        const anchor_h: u32 = @max(first_b.y1 - first_b.y, 1);
        var tier_bot: u32 = first_b.y1;
        var end: usize = start + 1;
        while (end < nb) : (end += 1) {
            const cand_b = bubbles[order_idx[end]];
            const cand_h: u32 = @max(cand_b.y1 - cand_b.y, 1);
            const min_h = @min(anchor_h, cand_h);
            const overlap: i64 = @as(i64, @min(tier_bot, @as(u32, cand_b.y1))) - @as(i64, cand_b.y);
            if (overlap * 3 < @as(i64, min_h)) break;
            if (cand_b.y1 > tier_bot) tier_bot = cand_b.y1;
        }
        if (manga_rtl) {
            std.sort.insertion(u16, order_idx[start..end], ctx, BubbleSortCtx.lessRightDesc);
        } else {
            std.sort.insertion(u16, order_idx[start..end], ctx, BubbleSortCtx.lessXAsc);
        }
        start = end;
    }

    var line_cursor: usize = 0;
    for (order_idx[0..nb], 0..) |orig_bi, r_order| {
        const acc = &bubbles[orig_bi];
        const b_orient: u8 = if (acc.vert_votes > acc.horiz_votes) @as(u8, 1) else @as(u8, 0);

        const group_start = line_cursor;
        for (active_lines) |li| {
            if (boxes[li].group_root == orig_bi) {
                const b = boxes[li];
                out_lines[line_cursor] = .{
                    .x = b.x,
                    .y = b.y,
                    .w = b.w,
                    .h = b.h,
                    .bubble_index = @intCast(r_order),
                    .confidence_bps = b.conf_bps,
                    .text_orientation = b.orient,
                    .flags = 0,
                    .order_in_bubble = @intCast(line_cursor - group_start),
                };
                line_cursor += 1;
            }
        }

        const bub_lines = out_lines[group_start..line_cursor];
        const LineSortCtx = struct {
            orient: u8,
            fn lessFn(lctx: @This(), a: DetLine, b: DetLine) bool {
                if (lctx.orient == 1) {
                    const x_ov = axisOverlap(a.x, a.w, b.x, b.w);
                    const min_w = @min(@as(u32, a.w), b.w);
                    if (x_ov * 3 < min_w) return (a.x + a.w) > (b.x + b.w);
                    return a.y < b.y;
                }
                const y_ov = axisOverlap(a.y, a.h, b.y, b.h);
                const min_h = @min(@as(u32, a.h), b.h);
                if (y_ov * 3 < min_h) return a.y < b.y;
                return a.x < b.x;
            }
        };
        std.sort.insertion(DetLine, bub_lines, LineSortCtx{ .orient = b_orient }, LineSortCtx.lessFn);
        for (bub_lines, 0..) |*ln, k| {
            ln.order_in_bubble = @intCast(k);
        }

        const mean_conf: u16 = @intCast(acc.conf_sum / @max(@as(u32, acc.line_count), 1));
        out_bubbles[r_order] = .{
            .x = acc.x,
            .y = acc.y,
            .w = acc.x1 - acc.x,
            .h = acc.y1 - acc.y,
            .reading_order = @intCast(r_order),
            .confidence_bps = mean_conf,
            .first_line = @intCast(group_start),
            .line_count = @intCast(line_cursor - group_start),
            .shape = if (acc.from_bubble_map) 1 else 0,
            .text_orientation = b_orient,
            .flags = if (acc.from_bubble_map) 1 else 0,
            .reserved = 0,
        };
    }
}

fn assignContourIds(boxes: []CompBox, active_lines: []const u16, contours: []const CompBox) void {
    for (active_lines) |li| {
        var b = &boxes[li];
        b.contour_idx = std.math.maxInt(u16);
        if (contours.len == 0) continue;

        const cx = @as(u32, b.x) + @as(u32, b.w) / 2;
        const cy = @as(u32, b.y) + @as(u32, b.h) / 2;
        var best_idx: usize = std.math.maxInt(usize);
        var best_ov: u32 = 0;

        for (contours, 0..) |c, ci| {
            const x_ov = axisOverlap(b.x, b.w, c.x, c.w);
            const y_ov = axisOverlap(b.y, b.h, c.y, c.h);
            const ov_area = x_ov * y_ov;
            const center_in = cx >= c.x and cx < @as(u32, c.x) + c.w and cy >= c.y and cy < @as(u32, c.y) + c.h;
            if (center_in or ov_area * 2 >= @as(u32, b.w) * b.h) {
                if (ov_area >= best_ov) {
                    best_ov = ov_area;
                    best_idx = ci;
                }
            }
        }
        if (best_idx < std.math.maxInt(u16)) {
            b.contour_idx = @intCast(best_idx);
        }
    }
}

fn shouldMergeLines(bi: CompBox, minor_i: u32, bj: CompBox, group_gap_pct: u16) bool {
    if (bi.contour_idx != std.math.maxInt(u16) and bi.contour_idx == bj.contour_idx) {
        return true;
    }
    if (bi.contour_idx != std.math.maxInt(u16) or bj.contour_idx != std.math.maxInt(u16)) {
        return false;
    }

    const minor_j: u32 = @max(@min(bj.w, bj.h), 1);
    const minor_avg = (minor_i + minor_j) / 2;
    const max_gap = (minor_avg * group_gap_pct + 99) / 100;
    const xg = axisGap(bi.x, bi.w, bj.x, bj.w);
    const yg = axisGap(bi.y, bi.h, bj.y, bj.h);
    const y_ov = axisOverlap(bi.y, bi.h, bj.y, bj.h);
    const x_ov = axisOverlap(bi.x, bi.w, bj.x, bj.w);

    // Group sibling parallel columns for vertical text, or stacked rows for horizontal text.
    if (bi.orient != 0 and bj.orient != 0) {
        if (y_ov > 0 and xg <= max_gap) return true;
        return x_ov > 0 and yg <= minor_avg / 2;
    }
    if (bi.orient == 0 and bj.orient == 0) {
        if (x_ov > 0 and yg <= max_gap) return true;
        return y_ov > 0 and xg <= minor_avg / 2;
    }
    return xg <= minor_avg / 2 and yg <= minor_avg / 2;
}

fn findSliceRoot(dsu: []u16, start: u16) u16 {
    var cur = start;
    while (dsu[cur] != cur) {
        cur = dsu[cur];
    }
    return cur;
}

fn unionSlice(dsu: []u16, a: u16, b: u16) void {
    const ra = findSliceRoot(dsu, a);
    const rb = findSliceRoot(dsu, b);
    if (ra != rb) {
        const keep = @min(ra, rb);
        const drop = @max(ra, rb);
        dsu[drop] = keep;
    }
}
