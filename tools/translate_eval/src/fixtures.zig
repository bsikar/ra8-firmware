//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Deterministic builders for `TCHR`, `TVOC`, `TMNT`, and quantized/float
//! activation maps consumed by `libs/ra8_translate` C ABI calls.

const std = @import("std");
const c = @import("ra8_translate_c.zig");
const corpus = @import("corpus.zig");
const metrics = @import("metrics.zig");

/// Fixed transformer dimensions small enough for fast host sweeps while
/// exercising real Walsh-Hadamard multi-token decoding in `TMNT`.
pub const tmnt_d_model: usize = 16;
pub const tmnt_d_ff: usize = 16;
pub const tmnt_max_seq: usize = 16;
pub const tmnt_enc_layers: usize = 1;
pub const tmnt_dec_layers: usize = 1;

/// IEEE 802.3 CRC-32 matching `libs/ra8_translate/src/internal/crc32.zig`.
pub fn crc32Ieee(data: []const u8) u32 {
    var crc: u32 = 0xFFFF_FFFF;
    for (data) |byte| {
        crc ^= byte;
        var bit: u32 = 0;
        while (bit < 8) : (bit += 1) {
            const mask: u32 = if ((crc & 1) != 0) 0xEDB8_8320 else 0;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

fn writeU16(buf: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, buf[off..][0..2], v, .little);
}

fn writeU32(buf: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, buf[off..][0..4], v, .little);
}

fn writeF32(buf: []u8, off: usize, v: f32) void {
    writeU32(buf, off, @bitCast(v));
}

/// Quantize finite float value to affine `int8_t` centered around zero point.
pub fn quantizeF32ToI8(val: f32, scale: f32, zp: i8) i8 {
    const scaled = @round(val / scale) + @as(f32, @floatFromInt(zp));
    const clamped = std.math.clamp(@as(i32, @intFromFloat(scaled)), -128, 127);
    return @intCast(clamped);
}

/// Build float32 and quantized `int8_t` activation maps for a page.
pub const DetMaps = struct {
    f32_map: []f32,
    i8_map: []i8,
    text_scale: f32,
    text_zp: i8,

    pub fn deinit(self: *DetMaps, allocator: std.mem.Allocator) void {
        allocator.free(self.f32_map);
        allocator.free(self.i8_map);
    }
};

pub fn buildDetMaps(
    allocator: std.mem.Allocator,
    page: corpus.PageSpec,
) !DetMaps {
    const elem_count = @as(usize, page.det_map_width) * page.det_map_height;
    const f32_map = try allocator.alloc(f32, elem_count);
    errdefer allocator.free(f32_map);
    const i8_map = try allocator.alloc(i8, elem_count);
    errdefer allocator.free(i8_map);

    @memset(f32_map, 0.01);
    for (page.det_regions) |reg| {
        const y_end = @min(@as(usize, reg.y) + reg.h, page.det_map_height);
        const x_end = @min(@as(usize, reg.x) + reg.w, page.det_map_width);
        var y: usize = reg.y;
        while (y < y_end) : (y += 1) {
            var x: usize = reg.x;
            while (x < x_end) : (x += 1) {
                f32_map[y * page.det_map_width + x] = reg.prob;
            }
        }
    }

    // Affine scale chosen so strong activations quantize accurately while values
    // barely above the 0.300 detector cutoff (e.g. 0.304) round cleanly below
    // 0.300 after 8-bit quantization (23 * 0.013 = 0.299 < 0.300).
    const text_scale: f32 = 0.013;
    const text_zp: i8 = -128;
    for (f32_map, i8_map) |fv, *qv| {
        qv.* = quantizeF32ToI8(fv, text_scale, text_zp);
    }

    return .{
        .f32_map = f32_map,
        .i8_map = i8_map,
        .text_scale = text_scale,
        .text_zp = text_zp,
    };
}

/// Built `TCHR` charset + matching float/i8 CTC logit matrices `[T, C]`.
pub const OcrCase = struct {
    tchr_buf: []u8,
    logits_f32: []f32,
    logits_i8: []i8,
    time_steps: u32,
    class_count: u32,
    quant_scale: f32,
    zero_point: i8,

    pub fn deinit(self: *OcrCase, allocator: std.mem.Allocator) void {
        allocator.free(self.tchr_buf);
        allocator.free(self.logits_f32);
        allocator.free(self.logits_i8);
    }
};

fn ocrModelForLang(lang: []const u8) u8 {
    if (std.mem.eql(u8, lang, "ja")) return c.ocr_japanese;
    if (std.mem.eql(u8, lang, "zh")) return c.ocr_chinese;
    return c.ocr_korean;
}

pub fn nmtModelForLang(lang: []const u8) u8 {
    if (std.mem.eql(u8, lang, "ja")) return c.nmt_ja_en;
    if (std.mem.eql(u8, lang, "zh")) return c.nmt_zh_en;
    return c.nmt_ko_en;
}

/// Build `TCHR` container and `[T, C]` float32 + quantized int8 CTC logits for one line.
/// When `ocr_distractor_sub` is non-empty, class index 1 holds the confusable glyph so
/// an equal int8 quantized tie favors the lower class index (`>` argmax tie-breaking).
pub fn buildOcrCase(
    allocator: std.mem.Allocator,
    lang: []const u8,
    bubble: corpus.BubbleSpec,
) !OcrCase {
    const cps = try metrics.toCodepoints(allocator, bubble.source_text);
    defer allocator.free(cps);

    var glyphs: std.ArrayList([]const u8) = .empty;
    defer {
        for (glyphs.items) |g| allocator.free(g);
        glyphs.deinit(allocator);
    }

    // Class 0 = CTC blank "".
    try glyphs.append(allocator, try allocator.dupe(u8, ""));

    var distractor_class: ?usize = null;
    if (bubble.ocr_distractor_sub.len > 0) {
        distractor_class = glyphs.items.len;
        try glyphs.append(allocator, try allocator.dupe(u8, bubble.ocr_distractor_sub));
    }

    var target_classes = try allocator.alloc(usize, cps.len);
    defer allocator.free(target_classes);

    var tmp: [4]u8 = undefined;
    for (cps, 0..) |cp, i| {
        const n = std.unicode.utf8Encode(cp, &tmp) catch unreachable;
        const g_str = tmp[0..n];
        var found: ?usize = null;
        for (glyphs.items, 0..) |existing, gi| {
            if (std.mem.eql(u8, existing, g_str)) {
                found = gi;
                break;
            }
        }
        if (found) |gi| {
            target_classes[i] = gi;
        } else {
            target_classes[i] = glyphs.items.len;
            try glyphs.append(allocator, try allocator.dupe(u8, g_str));
        }
    }

    // Build TCHR wire container.
    const count: u32 = @intCast(glyphs.items.len);
    var pool_bytes: usize = 0;
    for (glyphs.items) |g| pool_bytes += g.len;
    const offsets_bytes = (glyphs.items.len + 1) * 4;
    const pool_off = c.ctc_header_bytes + offsets_bytes;
    const total = pool_off + pool_bytes;
    const tchr_buf = try allocator.alloc(u8, total);
    errdefer allocator.free(tchr_buf);
    @memset(tchr_buf, 0);

    writeU32(tchr_buf, c.ctc_header_bytes, 0);
    var cursor: usize = 0;
    for (glyphs.items, 0..) |g, gi| {
        if (g.len > 0) @memcpy(tchr_buf[pool_off + cursor ..][0..g.len], g);
        cursor += g.len;
        writeU32(tchr_buf, c.ctc_header_bytes + (gi + 1) * 4, @intCast(cursor));
    }

    writeU32(tchr_buf, 0, c.ctc_magic);
    writeU16(tchr_buf, 4, c.ctc_version);
    writeU16(tchr_buf, 6, 0);
    writeU32(tchr_buf, 8, @intCast(total));
    writeU32(tchr_buf, 12, 0);
    tchr_buf[16] = ocrModelForLang(lang);
    writeU32(tchr_buf, 20, count);
    writeU32(tchr_buf, 24, 0);
    writeU32(tchr_buf, 28, c.ctc_index_none);
    writeU32(tchr_buf, 32, @intCast(pool_bytes));
    writeU32(tchr_buf, 12, crc32Ieee(tchr_buf));

    // Build [T, C] CTC frames with a blank separator whenever two identical characters repeat.
    var seq: std.ArrayList(usize) = .empty;
    defer seq.deinit(allocator);
    for (target_classes, 0..) |cls, i| {
        if (i > 0 and cls == target_classes[i - 1]) {
            try seq.append(allocator, 0);
        }
        try seq.append(allocator, cls);
    }

    const time_steps: usize = seq.items.len;
    const logits_f32 = try allocator.alloc(f32, time_steps * count);
    errdefer allocator.free(logits_f32);
    const logits_i8 = try allocator.alloc(i8, time_steps * count);
    errdefer allocator.free(logits_i8);

    @memset(logits_f32, -1.0);
    for (seq.items, 0..) |cls, t| {
        const row = logits_f32[t * count ..][0..count];
        const peak: f32 = 0.504;
        row[cls] = peak;
        if (t == 0 and distractor_class != null and cls != 0 and distractor_class.? != cls) {
            row[distractor_class.?] = peak - bubble.ocr_margin;
        }
    }

    // Quantization step 0.030 maps both 0.504 and 0.496 to q=17 (0.504/0.030=16.8->17,
    // 0.496/0.030=16.533->17), exposing true 8-bit tie collapse on 0.008-margin steps.
    const quant_scale: f32 = 0.030;
    const zero_point: i8 = 0;
    for (logits_f32, logits_i8) |fv, *qv| {
        qv.* = quantizeF32ToI8(fv, quant_scale, zero_point);
    }

    return .{
        .tchr_buf = tchr_buf,
        .logits_f32 = logits_f32,
        .logits_i8 = logits_i8,
        .time_steps = @intCast(time_steps),
        .class_count = count,
        .quant_scale = quant_scale,
        .zero_point = zero_point,
    };
}

/// Piece entry for building a SentencePiece Unigram `TVOC` container.
const VocabPiece = struct {
    text: []const u8,
    score: f32,
    kind: u8,
};

/// Paired NMT sentence model holding serialized `TVOC`, `TMNT` int8 container,
/// plus pre-quantization float32 reference token prediction for float vs i8 delta.
pub const NmtCase = struct {
    tvoc_buf: []u8,
    tmnt_buf: []u8,
    src_ids: []u32,
    float_pred_ids: []u32,

    pub fn deinit(self: *NmtCase, allocator: std.mem.Allocator) void {
        allocator.free(self.tvoc_buf);
        allocator.free(self.tmnt_buf);
        allocator.free(self.src_ids);
        allocator.free(self.float_pred_ids);
    }
};

fn buildTvocFromPieces(
    allocator: std.mem.Allocator,
    pieces: []const VocabPiece,
    bos_id: u32,
    eos_id: u32,
) ![]u8 {
    const count = pieces.len;
    const hdr_bytes: usize = 56;
    const table_off = hdr_bytes;
    const sorted_off = table_off + count * 12;
    const pool_off = sorted_off + count * 4;
    var pool_len: usize = 0;
    for (pieces) |p| pool_len += p.text.len;

    const out = try allocator.alloc(u8, pool_off + pool_len);
    errdefer allocator.free(out);
    @memset(out, 0);

    var cursor: usize = 0;
    var min_score: f32 = -10.0;
    var saw_normal = false;
    for (pieces, 0..) |p, i| {
        if (p.kind == c.tok_piece_normal and (!saw_normal or p.score < min_score)) {
            min_score = p.score;
            saw_normal = true;
        }
        const rec = out[table_off + i * 12 ..][0..12];
        writeU32(rec, 0, @intCast(cursor));
        writeU16(rec, 4, @intCast(p.text.len));
        rec[6] = p.kind;
        rec[7] = 0;
        writeF32(rec, 8, p.score);
        if (p.text.len > 0) @memcpy(out[pool_off + cursor ..][0..p.text.len], p.text);
        cursor += p.text.len;
    }

    const sorted = try allocator.alloc(u32, count);
    defer allocator.free(sorted);
    for (sorted, 0..) |*s, i| s.* = @intCast(i);
    std.mem.sort(u32, sorted, pieces, struct {
        fn lessThan(ctx: []const VocabPiece, a: u32, b: u32) bool {
            const ord = std.mem.order(u8, ctx[a].text, ctx[b].text);
            return ord == .lt or (ord == .eq and a < b);
        }
    }.lessThan);
    for (sorted, 0..) |id, i| {
        writeU32(out, sorted_off + i * 4, id);
    }

    const total = pool_off + pool_len;
    const flags = c.tok_flag_add_dummy_prefix | c.tok_flag_remove_extra_ws | c.tok_flag_escape_ws;
    writeU32(out, 0, 0x434F_5654);
    writeU16(out, 4, 1);
    out[6] = c.tok_unigram;
    out[7] = flags;
    writeU32(out, 8, @intCast(total));
    writeU32(out, 16, @intCast(count));
    writeU32(out, 20, 0);
    writeU32(out, 24, bos_id);
    writeU32(out, 28, eos_id);
    writeU32(out, 32, c.tok_id_none);
    writeF32(out, 36, min_score);
    writeU32(out, 40, @intCast(table_off));
    writeU32(out, 44, @intCast(sorted_off));
    writeU32(out, 48, @intCast(pool_off));
    writeU32(out, 12, crc32Ieee(out));
    return out;
}

/// Walsh-Hadamard entry in `{-1.0, +1.0}` for `dim = 16` (`row * col` popcount parity).
fn walshSign(row: usize, col: usize) f32 {
    const bits: u32 = @intCast(row & col);
    return if ((@popCount(bits) & 1) == 0) 1.0 else -1.0;
}

const TensorSpec = struct {
    dtype: u8 = c.marian_dtype_none,
    rows: u32 = 0,
    cols: u32 = 0,
    scale: f32 = 1.0,
    zp: i8 = 0,
    i8_vals: []const i8 = &.{},
    f32_vals: []const f32 = &.{},
};

/// Build synthetic `TVOC` + `TMNT` container executing one real int8 Marian decode pass
/// for `bubble.english_ref` (`tok_encode` -> `marian_greedy_decode` -> `tok_decode`)
/// plus pre-quantization float32 decoding over the exact same weights.
pub fn buildNmtCase(
    allocator: std.mem.Allocator,
    lang: []const u8,
    bubble: corpus.BubbleSpec,
) !NmtCase {
    var pieces: std.ArrayList(VocabPiece) = .empty;
    defer {
        for (pieces.items) |p| allocator.free(p.text);
        pieces.deinit(allocator);
    }

    try pieces.append(allocator, .{ .text = try allocator.dupe(u8, "<unk>"), .score = 0.0, .kind = c.tok_piece_unknown });
    try pieces.append(allocator, .{ .text = try allocator.dupe(u8, "<s>"), .score = 0.0, .kind = c.tok_piece_control });
    try pieces.append(allocator, .{ .text = try allocator.dupe(u8, "</s>"), .score = 0.0, .kind = c.tok_piece_control });
    try pieces.append(allocator, .{ .text = try allocator.dupe(u8, "\xe2\x96\x81"), .score = -2.0, .kind = c.tok_piece_normal });

    const src_piece = try std.fmt.allocPrint(allocator, "\xe2\x96\x81{s}", .{bubble.source_text});
    try pieces.append(allocator, .{ .text = src_piece, .score = -0.1, .kind = c.tok_piece_normal });
    const src_token_id: u32 = @intCast(pieces.items.len - 1);

    // Tokenize reference English into SentencePiece whitespace-prefixed word pieces + trailing punctuation.
    var tgt_ids: std.ArrayList(u32) = .empty;
    defer tgt_ids.deinit(allocator);

    var it = std.mem.tokenizeScalar(u8, bubble.english_ref, ' ');
    while (it.next()) |word| {
        const piece_str = try std.fmt.allocPrint(allocator, "\xe2\x96\x81{s}", .{word});
        errdefer allocator.free(piece_str);
        var found: ?u32 = null;
        for (pieces.items, 0..) |existing, pi| {
            if (std.mem.eql(u8, existing.text, piece_str)) {
                found = @intCast(pi);
                break;
            }
        }
        if (found) |idx| {
            allocator.free(piece_str);
            try tgt_ids.append(allocator, idx);
        } else {
            const new_id: u32 = @intCast(pieces.items.len);
            try pieces.append(allocator, .{ .text = piece_str, .score = -0.5, .kind = c.tok_piece_normal });
            try tgt_ids.append(allocator, new_id);
        }
    }

    var alt_tok_id: ?u32 = null;
    if (bubble.nmt_alt_token.len > 0) {
        const alt_str = try std.fmt.allocPrint(allocator, "\xe2\x96\x81{s}", .{bubble.nmt_alt_token});
        var found: ?u32 = null;
        for (pieces.items, 0..) |existing, pi| {
            if (std.mem.eql(u8, existing.text, alt_str)) {
                found = @intCast(pi);
                break;
            }
        }
        if (found) |idx| {
            allocator.free(alt_str);
            alt_tok_id = idx;
        } else {
            alt_tok_id = @intCast(pieces.items.len);
            try pieces.append(allocator, .{ .text = alt_str, .score = -0.6, .kind = c.tok_piece_normal });
        }
    }

    try tgt_ids.append(allocator, 2); // EOS.

    const tvoc_buf = try buildTvocFromPieces(allocator, pieces.items, 1, 2);
    errdefer allocator.free(tvoc_buf);

    const vocab_size = pieces.items.len;
    const pos_count = tmnt_max_seq;
    const embed_w_i8 = try allocator.alloc(i8, vocab_size * tmnt_d_model);
    defer allocator.free(embed_w_i8);
    @memset(embed_w_i8, 0);

    // Walsh rows 1..15 carry zero-mean orthonormal positional directions in [-1.0, +1.0] * 0.25.
    const pos_f32 = try allocator.alloc(f32, pos_count * tmnt_d_model);
    defer allocator.free(pos_f32);
    for (0..pos_count) |pos| {
        const row_idx = pos + 1;
        for (0..tmnt_d_model) |col| {
            pos_f32[pos * tmnt_d_model + col] = if (pos < 15) 0.25 * walshSign(row_idx, col) else 0.0;
        }
    }

    const lm_w_f32 = try allocator.alloc(f32, vocab_size * tmnt_d_model);
    defer allocator.free(lm_w_f32);
    @memset(lm_w_f32, 0.0);
    const lm_b_f32 = try allocator.alloc(f32, vocab_size);
    defer allocator.free(lm_b_f32);
    @memset(lm_b_f32, -2.0);

    const lm_scale: f32 = 0.08;
    for (tgt_ids.items, 0..) |tok, step| {
        const row_idx = step + 1;
        const w_row = lm_w_f32[tok * tmnt_d_model ..][0..tmnt_d_model];
        const amp: f32 = if (step == 1 and alt_tok_id != null) 0.83 else 1.0;
        for (0..tmnt_d_model) |col| {
            w_row[col] += amp * 0.25 * walshSign(row_idx, col);
        }
        lm_b_f32[tok] = 0.0;
    }

    if (alt_tok_id) |alt_id| {
        // Step 1 competing token: float sum is slightly below amp=0.83 (`0.81 + 0.01 = 0.82 < 0.83`),
        // whereas int8 quantization at scale=0.08 maps both weights to the same bucket (10 * 0.08 = 0.80),
        // letting the +0.01 bias win in `host_i8`.
        const step: usize = 1;
        const row_idx = step + 1;
        const w_row = lm_w_f32[alt_id * tmnt_d_model ..][0..tmnt_d_model];
        const alt_amp: f32 = 0.83 - bubble.nmt_quant_margin;
        for (0..tmnt_d_model) |col| {
            w_row[col] = alt_amp * 0.25 * walshSign(row_idx, col);
        }
        lm_b_f32[alt_id] = 0.005;
    }

    const lm_w_i8 = try allocator.alloc(i8, vocab_size * tmnt_d_model);
    defer allocator.free(lm_w_i8);
    for (lm_w_f32, lm_w_i8) |fv, *qv| {
        qv.* = quantizeF32ToI8(fv, lm_scale, 0);
    }

    // Evaluate pre-quantization float32 reference greedy tokens directly over `pos_f32` and `lm_w_f32`.
    var float_pred: std.ArrayList(u32) = .empty;
    errdefer float_pred.deinit(allocator);
    for (0..tgt_ids.items.len) |step| {
        const p_row = pos_f32[step * tmnt_d_model ..][0..tmnt_d_model];
        var best_v: u32 = 0;
        var best_score: f32 = -std.math.inf(f32);
        for (0..vocab_size) |v| {
            const w_row = lm_w_f32[v * tmnt_d_model ..][0..tmnt_d_model];
            var dot: f32 = lm_b_f32[v];
            for (p_row, w_row) |px, wx| dot += px * wx;
            if (dot > best_score) {
                best_score = dot;
                best_v = @intCast(v);
            }
        }
        try float_pred.append(allocator, best_v);
        if (best_v == 2) break;
    }

    // Serialize TMNT container (slots: 8 global + 16 enc + 26 dec = 50 slots).
    const slots_count: usize = 8 + 16 + 26;
    var specs: [slots_count]TensorSpec = @splat(.{});
    specs[0] = .{ .dtype = c.marian_dtype_i8, .rows = @intCast(vocab_size), .cols = tmnt_d_model, .scale = 0.05, .i8_vals = embed_w_i8 };
    specs[1] = .{ .dtype = c.marian_dtype_f32, .rows = tmnt_max_seq, .cols = tmnt_d_model, .f32_vals = pos_f32 };
    specs[2] = .{ .dtype = c.marian_dtype_i8, .rows = @intCast(vocab_size), .cols = tmnt_d_model, .scale = lm_scale, .i8_vals = lm_w_i8 };
    specs[3] = .{ .dtype = c.marian_dtype_f32, .rows = @intCast(vocab_size), .cols = 1, .f32_vals = lm_b_f32 };

    const zero_mat_i8: [tmnt_d_model * tmnt_d_model]i8 = @splat(0);
    const one_ln: [tmnt_d_model]f32 = @splat(1.0);
    const zero_ln: [tmnt_d_model]f32 = @splat(0.0);

    // Encoder layer slots 8..23 (all zero projections, identity LayerNorm).
    for (0..4) |p| {
        specs[8 + p * 2] = .{ .dtype = c.marian_dtype_i8, .rows = tmnt_d_model, .cols = tmnt_d_model, .scale = 0.02, .i8_vals = &zero_mat_i8 };
    }
    specs[16] = .{ .dtype = c.marian_dtype_f32, .rows = tmnt_d_model, .cols = 1, .f32_vals = &one_ln };
    specs[17] = .{ .dtype = c.marian_dtype_f32, .rows = tmnt_d_model, .cols = 1, .f32_vals = &zero_ln };
    specs[18] = .{ .dtype = c.marian_dtype_i8, .rows = tmnt_d_ff, .cols = tmnt_d_model, .scale = 0.02, .i8_vals = &zero_mat_i8 };
    specs[20] = .{ .dtype = c.marian_dtype_i8, .rows = tmnt_d_model, .cols = tmnt_d_ff, .scale = 0.02, .i8_vals = &zero_mat_i8 };
    specs[22] = specs[16];
    specs[23] = specs[17];

    // Decoder layer slots 24..49 (self, cross, ffn).
    for (0..2) |blk| {
        const b = 24 + blk * 10;
        for (0..4) |p| {
            specs[b + p * 2] = .{ .dtype = c.marian_dtype_i8, .rows = tmnt_d_model, .cols = tmnt_d_model, .scale = 0.02, .i8_vals = &zero_mat_i8 };
        }
        specs[b + 8] = specs[16];
        specs[b + 9] = specs[17];
    }
    specs[44] = specs[18];
    specs[46] = specs[20];
    specs[48] = specs[16];
    specs[49] = specs[17];

    const weights_at = ((c.marian_header_bytes + slots_count * c.marian_tensor_desc_bytes) + 3) & ~@as(usize, 3);
    var total_bytes: usize = weights_at;
    for (specs) |sp| {
        if (sp.dtype == c.marian_dtype_none) continue;
        total_bytes = (total_bytes + 3) & ~@as(usize, 3);
        const elem_bytes: usize = if (sp.dtype == c.marian_dtype_i8) 1 else 4;
        total_bytes += @as(usize, sp.rows) * sp.cols * elem_bytes;
    }
    total_bytes = (total_bytes + 3) & ~@as(usize, 3);

    const tmnt_buf = try allocator.alloc(u8, total_bytes);
    errdefer allocator.free(tmnt_buf);
    @memset(tmnt_buf, 0);

    var cursor: usize = weights_at;
    for (specs, 0..) |sp, s| {
        const doff = c.marian_header_bytes + s * c.marian_tensor_desc_bytes;
        if (sp.dtype == c.marian_dtype_none) {
            writeF32(tmnt_buf, doff + 16, 1.0);
            continue;
        }
        cursor = (cursor + 3) & ~@as(usize, 3);
        const elems = @as(usize, sp.rows) * sp.cols;
        const elem_bytes: usize = if (sp.dtype == c.marian_dtype_i8) 1 else 4;
        const nbytes: u32 = @intCast(elems * elem_bytes);
        if (sp.dtype == c.marian_dtype_i8) {
            const dst = tmnt_buf[cursor..][0..elems];
            for (dst, sp.i8_vals) |*d, iv| d.* = @bitCast(iv);
        } else {
            for (sp.f32_vals, 0..) |fv, k| writeF32(tmnt_buf, cursor + k * 4, fv);
        }
        writeU32(tmnt_buf, doff + 0, @intCast(cursor));
        writeU32(tmnt_buf, doff + 4, nbytes);
        writeU32(tmnt_buf, doff + 8, sp.rows);
        writeU32(tmnt_buf, doff + 12, sp.cols);
        writeF32(tmnt_buf, doff + 16, sp.scale);
        tmnt_buf[doff + 20] = sp.dtype;
        tmnt_buf[doff + 21] = @bitCast(sp.zp);
        cursor += nbytes;
    }

    const dir_end = c.marian_header_bytes + slots_count * c.marian_tensor_desc_bytes;
    writeU32(tmnt_buf, 0, c.marian_magic);
    writeU16(tmnt_buf, 4, c.marian_version);
    writeU16(tmnt_buf, 6, c.marian_flag_pre_norm);
    writeU32(tmnt_buf, 8, @intCast(total_bytes));
    tmnt_buf[20] = nmtModelForLang(lang);
    tmnt_buf[21] = 2;
    tmnt_buf[22] = @intCast(tmnt_enc_layers);
    tmnt_buf[23] = @intCast(tmnt_dec_layers);
    writeU16(tmnt_buf, 24, @intCast(tmnt_d_model));
    writeU16(tmnt_buf, 26, @intCast(tmnt_d_ff));
    writeU16(tmnt_buf, 28, @intCast(tmnt_max_seq));
    writeU32(tmnt_buf, 32, @intCast(vocab_size));
    writeU32(tmnt_buf, 36, 0);
    writeU32(tmnt_buf, 40, 1);
    writeU32(tmnt_buf, 44, 2);
    writeU32(tmnt_buf, 48, 3);
    writeF32(tmnt_buf, 52, 1e-5);
    writeU32(tmnt_buf, 72, @intCast(slots_count));
    writeU32(tmnt_buf, 76, @intCast(weights_at));
    writeU32(tmnt_buf, 80, @intCast(total_bytes - weights_at));
    writeU32(tmnt_buf, 16, crc32Ieee(tmnt_buf[weights_at..total_bytes]));
    writeU32(tmnt_buf, 12, crc32Ieee(tmnt_buf[0..dir_end]));

    const src_ids = try allocator.alloc(u32, 2);
    src_ids[0] = src_token_id;
    src_ids[1] = 2;

    return .{
        .tvoc_buf = tvoc_buf,
        .tmnt_buf = tmnt_buf,
        .src_ids = src_ids,
        .float_pred_ids = try float_pred.toOwnedSlice(allocator),
    };
}
