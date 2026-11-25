//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure evaluation metrics for manga/webtoon translation benchmarking:
//! IoU box detection precision/recall/F1, UTF-8 codepoint Levenshtein CER,
//! and character n-gram F-score (chrF).

const std = @import("std");

/// Axis-aligned bounding box in page coordinates.
pub const Box = struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
};

/// Detection precision, recall, F1, and match counts on a page or aggregate.
pub const DetMetrics = struct {
    tp: u32 = 0,
    fp: u32 = 0,
    fn_count: u32 = 0,

    pub fn precision(self: DetMetrics) f64 {
        const denom = self.tp + self.fp;
        if (denom == 0) return if (self.fn_count == 0) 1.0 else 0.0;
        return @as(f64, @floatFromInt(self.tp)) / @as(f64, @floatFromInt(denom));
    }

    pub fn recall(self: DetMetrics) f64 {
        const denom = self.tp + self.fn_count;
        if (denom == 0) return if (self.fp == 0) 1.0 else 0.0;
        return @as(f64, @floatFromInt(self.tp)) / @as(f64, @floatFromInt(denom));
    }

    pub fn f1(self: DetMetrics) f64 {
        const p = self.precision();
        const r = self.recall();
        if (p + r == 0.0) return 0.0;
        return (2.0 * p * r) / (p + r);
    }

    pub fn add(self: *DetMetrics, other: DetMetrics) void {
        self.tp += other.tp;
        self.fp += other.fp;
        self.fn_count += other.fn_count;
    }
};

/// Compute Intersection-over-Union of two axis-aligned boxes.
pub fn boxIoU(a: Box, b: Box) f64 {
    const ax0: u32 = a.x;
    const ay0: u32 = a.y;
    const ax1: u32 = @as(u32, a.x) + a.w;
    const ay1: u32 = @as(u32, a.y) + a.h;

    const bx0: u32 = b.x;
    const by0: u32 = b.y;
    const bx1: u32 = @as(u32, b.x) + b.w;
    const by1: u32 = @as(u32, b.y) + b.h;

    const ix0 = @max(ax0, bx0);
    const iy0 = @max(ay0, by0);
    const ix1 = @min(ax1, bx1);
    const iy1 = @min(ay1, by1);

    const iw: u64 = if (ix1 > ix0) ix1 - ix0 else 0;
    const ih: u64 = if (iy1 > iy0) iy1 - iy0 else 0;
    const inter: u64 = iw * ih;
    if (inter == 0) return 0.0;

    const area_a: u64 = @as(u64, a.w) * a.h;
    const area_b: u64 = @as(u64, b.w) * b.h;
    const union_area = area_a + area_b - inter;
    if (union_area == 0) return 0.0;
    return @as(f64, @floatFromInt(inter)) / @as(f64, @floatFromInt(union_area));
}

/// Score greedy 1-to-1 box matches against ground truth at `iou_threshold`.
pub fn scoreDetection(
    allocator: std.mem.Allocator,
    preds: []const Box,
    refs: []const Box,
    iou_threshold: f64,
) !DetMetrics {
    const matched = try allocator.alloc(bool, refs.len);
    defer allocator.free(matched);
    @memset(matched, false);

    var out = DetMetrics{};
    for (preds) |pred| {
        var best_iou: f64 = -1.0;
        var best_idx: ?usize = null;
        for (refs, 0..) |ref, ri| {
            if (matched[ri]) continue;
            const iou = boxIoU(pred, ref);
            if (iou >= iou_threshold and iou > best_iou) {
                best_iou = iou;
                best_idx = ri;
            }
        }
        if (best_idx) |idx| {
            matched[idx] = true;
            out.tp += 1;
        } else {
            out.fp += 1;
        }
    }
    for (matched) |m| {
        if (!m) out.fn_count += 1;
    }
    return out;
}

/// Decode a UTF-8 slice into Unicode scalar codepoints, replacing invalid bytes
/// with U+FFFD so metric calculation never fails on corrupted model bytes.
pub fn toCodepoints(allocator: std.mem.Allocator, utf8: []const u8) ![]u21 {
    var list: std.ArrayList(u21) = .empty;
    errdefer list.deinit(allocator);

    var i: usize = 0;
    while (i < utf8.len) {
        const seq_len = std.unicode.utf8ByteSequenceLength(utf8[i]) catch {
            try list.append(allocator, 0xFFFD);
            i += 1;
            continue;
        };
        if (i + seq_len > utf8.len) {
            try list.append(allocator, 0xFFFD);
            break;
        }
        const cp = std.unicode.utf8Decode(utf8[i .. i + seq_len]) catch {
            try list.append(allocator, 0xFFFD);
            i += 1;
            continue;
        };
        try list.append(allocator, cp);
        i += seq_len;
    }
    return list.toOwnedSlice(allocator);
}

/// Compute Levenshtein edit distance over UTF-8 codepoint sequences and
/// return (edits, reference_length).
pub fn codepointEdits(
    allocator: std.mem.Allocator,
    hypothesis_utf8: []const u8,
    reference_utf8: []const u8,
) !struct { edits: u32, ref_len: u32 } {
    const hyp = try toCodepoints(allocator, hypothesis_utf8);
    defer allocator.free(hyp);
    const ref = try toCodepoints(allocator, reference_utf8);
    defer allocator.free(ref);

    if (ref.len == 0) {
        return .{ .edits = @intCast(hyp.len), .ref_len = 0 };
    }
    if (hyp.len == 0) {
        return .{ .edits = @intCast(ref.len), .ref_len = @intCast(ref.len) };
    }

    var prev = try allocator.alloc(u32, ref.len + 1);
    defer allocator.free(prev);
    var curr = try allocator.alloc(u32, ref.len + 1);
    defer allocator.free(curr);

    for (0..ref.len + 1) |j| {
        prev[j] = @intCast(j);
    }

    for (hyp) |hcp| {
        curr[0] = prev[0] + 1;
        for (ref, 0..) |rcp, j| {
            const cost: u32 = if (hcp == rcp) 0 else 1;
            const del_cost = prev[j + 1] + 1;
            const ins_cost = curr[j] + 1;
            const sub_cost = prev[j] + cost;
            curr[j + 1] = @min(@min(del_cost, ins_cost), sub_cost);
        }
        @memcpy(prev, curr);
    }

    return .{ .edits = prev[ref.len], .ref_len = @intCast(ref.len) };
}

/// Compute Character Error Rate (edits / max(1, ref_len)).
pub fn computeCer(
    allocator: std.mem.Allocator,
    hypothesis_utf8: []const u8,
    reference_utf8: []const u8,
) !f64 {
    const res = try codepointEdits(allocator, hypothesis_utf8, reference_utf8);
    if (res.ref_len == 0) return if (res.edits == 0) 0.0 else 1.0;
    return @as(f64, @floatFromInt(res.edits)) / @as(f64, @floatFromInt(res.ref_len));
}

fn stripWhitespaceCodepoints(allocator: std.mem.Allocator, utf8: []const u8) ![]u21 {
    const all = try toCodepoints(allocator, utf8);
    defer allocator.free(all);

    var out: std.ArrayList(u21) = .empty;
    errdefer out.deinit(allocator);
    for (all) |cp| {
        if (cp <= 0x7F and std.ascii.isWhitespace(@intCast(cp))) continue;
        try out.append(allocator, cp);
    }
    return out.toOwnedSlice(allocator);
}

fn countNgramOverlap(
    allocator: std.mem.Allocator,
    hyp: []const u21,
    ref: []const u21,
    n: usize,
) !struct { matches: u32, hyp_total: u32, ref_total: u32 } {
    if (hyp.len < n or ref.len < n) {
        return .{
            .matches = 0,
            .hyp_total = if (hyp.len >= n) @intCast(hyp.len - n + 1) else 0,
            .ref_total = if (ref.len >= n) @intCast(ref.len - n + 1) else 0,
        };
    }

    const hyp_count = hyp.len - n + 1;
    const ref_count = ref.len - n + 1;
    const used = try allocator.alloc(bool, ref_count);
    defer allocator.free(used);
    @memset(used, false);

    var matches: u32 = 0;
    for (0..hyp_count) |i| {
        const h_slice = hyp[i .. i + n];
        for (0..ref_count) |j| {
            if (used[j]) continue;
            if (std.mem.eql(u21, h_slice, ref[j .. j + n])) {
                used[j] = true;
                matches += 1;
                break;
            }
        }
    }

    return .{
        .matches = matches,
        .hyp_total = @intCast(hyp_count),
        .ref_total = @intCast(ref_count),
    };
}

/// Corpus-level chrF accumulator (Popovic 2015, n=1..6, beta=2.0 -> 0..100%).
pub const ChrFAccum = struct {
    pub const max_n: usize = 6;
    pub const beta: f64 = 2.0;

    matches: [max_n]u64 = @splat(0),
    hyp_totals: [max_n]u64 = @splat(0),
    ref_totals: [max_n]u64 = @splat(0),

    pub fn addSentence(
        self: *ChrFAccum,
        allocator: std.mem.Allocator,
        hypothesis_utf8: []const u8,
        reference_utf8: []const u8,
    ) !void {
        const hyp = try stripWhitespaceCodepoints(allocator, hypothesis_utf8);
        defer allocator.free(hyp);
        const ref = try stripWhitespaceCodepoints(allocator, reference_utf8);
        defer allocator.free(ref);

        for (1..max_n + 1) |n| {
            const ov = try countNgramOverlap(allocator, hyp, ref, n);
            self.matches[n - 1] += ov.matches;
            self.hyp_totals[n - 1] += ov.hyp_total;
            self.ref_totals[n - 1] += ov.ref_total;
        }
    }

    pub fn score(self: ChrFAccum) f64 {
        var sum_p: f64 = 0.0;
        var sum_r: f64 = 0.0;
        var active_orders: usize = 0;

        for (0..max_n) |idx| {
            if (self.hyp_totals[idx] == 0 and self.ref_totals[idx] == 0) continue;
            active_orders += 1;
            if (self.hyp_totals[idx] > 0) {
                sum_p += @as(f64, @floatFromInt(self.matches[idx])) /
                    @as(f64, @floatFromInt(self.hyp_totals[idx]));
            }
            if (self.ref_totals[idx] > 0) {
                sum_r += @as(f64, @floatFromInt(self.matches[idx])) /
                    @as(f64, @floatFromInt(self.ref_totals[idx]));
            }
        }
        if (active_orders == 0) return 0.0;
        const orders_f: f64 = @floatFromInt(active_orders);
        const avg_p = sum_p / orders_f;
        const avg_r = sum_r / orders_f;
        const beta_sq = beta * beta;
        const denom = beta_sq * avg_p + avg_r;
        if (denom == 0.0) return 0.0;
        return 100.0 * ((1.0 + beta_sq) * avg_p * avg_r) / denom;
    }
};

/// Compute single-sentence chrF6 score in [0.0, 100.0].
pub fn computeChrF(
    allocator: std.mem.Allocator,
    hypothesis_utf8: []const u8,
    reference_utf8: []const u8,
) !f64 {
    var acc = ChrFAccum{};
    try acc.addSentence(allocator, hypothesis_utf8, reference_utf8);
    return acc.score();
}
