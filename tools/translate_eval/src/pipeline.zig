//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host evaluation pipeline running float reference, quantized int8 (`libra8_translate`),
//! and optional emulator prediction scoring across `ja`, `zh`, and `ko` corpus pages.

const std = @import("std");
const c = @import("ra8_translate_c.zig");
const corpus = @import("corpus.zig");
const fixtures = @import("fixtures.zig");
const metrics = @import("metrics.zig");

/// Per-language, per-mode evaluation scores.
pub const LangSummary = struct {
    lang: []const u8,
    pages: u32 = 0,
    bubbles: u32 = 0,
    det: metrics.DetMetrics = .{},
    ocr_edits: u64 = 0,
    ocr_ref_cps: u64 = 0,
    chrf: metrics.ChrFAccum = .{},
    weight_bytes_read: u64 = 0,
    furigana_dropped: u32 = 0,

    pub fn ocrCer(self: LangSummary) f64 {
        if (self.ocr_ref_cps == 0) return if (self.ocr_edits == 0) 0.0 else 1.0;
        return @as(f64, @floatFromInt(self.ocr_edits)) / @as(f64, @floatFromInt(self.ocr_ref_cps));
    }
};

pub const Mode = enum {
    float_ref,
    host_i8,
    emulator,

    pub fn label(self: Mode) []const u8 {
        return switch (self) {
            .float_ref => "float",
            .host_i8 => "host_i8",
            .emulator => "emulator",
        };
    }
};

pub const EvalRow = struct {
    lang: []const u8,
    mode: Mode,
    pages: u32,
    bubbles: u32,
    det_precision: f64,
    det_recall: f64,
    det_f1: f64,
    ocr_cer: f64,
    nmt_chrf: f64,
    weight_bytes: u64,
    furigana_dropped: u32,
};

pub const EvalReport = struct {
    allocator: std.mem.Allocator,
    has_emulator: bool,
    rows: []EvalRow,

    pub fn deinit(self: *EvalReport) void {
        self.allocator.free(self.rows);
    }
};

fn ocrModelForLang(lang: []const u8) u8 {
    if (std.mem.eql(u8, lang, "ja")) return c.ocr_japanese;
    if (std.mem.eql(u8, lang, "zh")) return c.ocr_chinese;
    return c.ocr_korean;
}

fn evalPageDet(
    allocator: std.mem.Allocator,
    page: corpus.PageSpec,
    mode: Mode,
) !struct { det: metrics.DetMetrics, furigana_dropped: u32 } {
    var maps = try fixtures.buildDetMaps(allocator, page);
    defer maps.deinit(allocator);

    const max_comps: u16 = 32;
    var work_bytes: usize = 0;
    const w_rc = c.ra8_translate_det_work_bytes(page.det_map_width, max_comps, &work_bytes);
    if (w_rc != c.ra8_ok) return error.TranslateDetWorkFailed;

    const work_buf = try allocator.alloc(u8, work_bytes);
    defer allocator.free(work_buf);
    @memset(work_buf, 0);

    const reading_order: u8 = if (std.mem.eql(u8, page.layout, "manga_rtl"))
        c.det_order_manga_rtl
    else
        c.det_order_webtoon_ttb;

    const cfg = c.DetCfg{
        .map_width = page.det_map_width,
        .map_height = page.det_map_height,
        .page_width = page.page_width,
        .page_height = page.page_height,
        .min_comp_area = 2,
        .reading_order = reading_order,
        .flags = c.det_flag_drop_furigana,
        .max_components = max_comps,
    };

    var lines: [max_comps]c.DetLine = @splat(c.DetLine{});
    var bubbles: [max_comps]c.DetBubble = @splat(c.DetBubble{});
    var summary = c.DetSummary{};

    const rc = switch (mode) {
        .float_ref => c.ra8_translate_det_post_f32(
            maps.f32_map.ptr,
            maps.f32_map.len,
            null,
            0,
            &cfg,
            work_buf.ptr,
            work_buf.len,
            &lines,
            max_comps,
            &bubbles,
            max_comps,
            &summary,
        ),
        .host_i8 => c.ra8_translate_det_post_i8(
            maps.i8_map.ptr,
            maps.i8_map.len,
            maps.text_scale,
            maps.text_zp,
            null,
            0,
            1.0,
            0,
            &cfg,
            work_buf.ptr,
            work_buf.len,
            &lines,
            max_comps,
            &bubbles,
            max_comps,
            &summary,
        ),
        .emulator => unreachable,
    };
    if (rc != c.ra8_ok) return error.TranslateDetFailed;

    var pred_boxes: std.ArrayList(metrics.Box) = .empty;
    defer pred_boxes.deinit(allocator);
    for (bubbles[0..summary.bubble_count]) |b| {
        try pred_boxes.append(allocator, .{ .x = b.x, .y = b.y, .w = b.w, .h = b.h });
    }

    var ref_boxes: std.ArrayList(metrics.Box) = .empty;
    defer ref_boxes.deinit(allocator);
    for (page.bubbles) |b| try ref_boxes.append(allocator, b.box());

    const det_res = try metrics.scoreDetection(allocator, pred_boxes.items, ref_boxes.items, 0.5);
    return .{ .det = det_res, .furigana_dropped = summary.dropped_furigana };
}

fn evalBubbleOcrAndNmt(
    allocator: std.mem.Allocator,
    lang: []const u8,
    bubble: corpus.BubbleSpec,
    mode: Mode,
    summary: *LangSummary,
) !void {
    // 1. OCR stage via `ra8_translate_ctc_*`.
    var ocr_case = try fixtures.buildOcrCase(allocator, lang, bubble);
    defer ocr_case.deinit(allocator);

    var hdr = c.CtcCharsetHeader{};
    if (c.ra8_translate_ctc_charset_open(ocr_case.tchr_buf.ptr, ocr_case.tchr_buf.len, &hdr) != c.ra8_ok) {
        return error.TranslateCtcOpenFailed;
    }

    const ctc_cfg = c.CtcDecodeCfg{
        .time_steps = ocr_case.time_steps,
        .expected_ocr_model = ocrModelForLang(lang),
    };
    var ocr_utf8: [512]u8 = undefined;
    var ocr_res = c.CtcResult{};

    const ctc_rc = switch (mode) {
        .float_ref => c.ra8_translate_ctc_decode_f32(
            ocr_case.tchr_buf.ptr,
            ocr_case.tchr_buf.len,
            ocr_case.logits_f32.ptr,
            ocr_case.logits_f32.len,
            &ctc_cfg,
            &ocr_utf8,
            ocr_utf8.len,
            &ocr_res,
        ),
        .host_i8 => c.ra8_translate_ctc_decode_i8(
            ocr_case.tchr_buf.ptr,
            ocr_case.tchr_buf.len,
            ocr_case.logits_i8.ptr,
            ocr_case.logits_i8.len,
            ocr_case.quant_scale,
            ocr_case.zero_point,
            &ctc_cfg,
            &ocr_utf8,
            ocr_utf8.len,
            &ocr_res,
        ),
        .emulator => unreachable,
    };
    if (ctc_rc != c.ra8_ok) return error.TranslateCtcDecodeFailed;

    const ocr_hyp = ocr_utf8[0..ocr_res.text_bytes];
    const edits = try metrics.codepointEdits(allocator, ocr_hyp, bubble.source_text);
    summary.ocr_edits += edits.edits;
    summary.ocr_ref_cps += edits.ref_len;

    // 2. NMT stage via `ra8_translate_tok_*` + `ra8_translate_marian_*`.
    var nmt_case = try fixtures.buildNmtCase(allocator, lang, bubble);
    defer nmt_case.deinit(allocator);

    var tok_hdr = c.VocabHeader{};
    if (c.ra8_translate_tok_open(nmt_case.tvoc_buf.ptr, nmt_case.tvoc_buf.len, &tok_hdr) != c.ra8_ok) {
        return error.TranslateTokOpenFailed;
    }

    // Verify source text tokenization round-trip through the real Unigram encoder.
    var tok_work_bytes: usize = 0;
    if (c.ra8_translate_tok_work_bytes(ocr_hyp.len, c.tok_unigram, &tok_work_bytes) != c.ra8_ok) {
        return error.TranslateTokWorkFailed;
    }
    const tok_work = try allocator.alloc(u8, @max(tok_work_bytes, 64));
    defer allocator.free(tok_work);
    var enc_ids: [32]u32 = undefined;
    var enc_count: usize = 0;
    if (c.ra8_translate_tok_encode(
        nmt_case.tvoc_buf.ptr,
        nmt_case.tvoc_buf.len,
        ocr_hyp.ptr,
        ocr_hyp.len,
        c.tok_enc_add_eos,
        tok_work.ptr,
        tok_work.len,
        &enc_ids,
        enc_ids.len,
        &enc_count,
    ) != c.ra8_ok) {
        return error.TranslateTokEncodeFailed;
    }
    const model_src_ids = if (enc_count > 0) enc_ids[0..enc_count] else nmt_case.src_ids;

    var out_english_buf: [512]u8 = undefined;
    var out_english_len: usize = 0;

    switch (mode) {
        .float_ref => {
            if (c.ra8_translate_tok_decode(
                nmt_case.tvoc_buf.ptr,
                nmt_case.tvoc_buf.len,
                nmt_case.float_pred_ids.ptr,
                nmt_case.float_pred_ids.len,
                &out_english_buf,
                out_english_buf.len,
                &out_english_len,
            ) != c.ra8_ok) {
                return error.TranslateTokDecodeFailed;
            }
        },
        .host_i8 => {
            var m_hdr = c.MarianHeader{};
            if (c.ra8_translate_marian_open(
                nmt_case.tmnt_buf.ptr,
                nmt_case.tmnt_buf.len,
                c.marian_open_verify_payload,
                &m_hdr,
            ) != c.ra8_ok) {
                return error.TranslateMarianOpenFailed;
            }
            var m_work_bytes: usize = 0;
            const max_src: u16 = @intCast(@min(model_src_ids.len, fixtures.tmnt_max_seq));
            const max_tgt: u16 = @intCast(fixtures.tmnt_max_seq);
            if (c.ra8_translate_marian_work_bytes(
                nmt_case.tmnt_buf.ptr,
                nmt_case.tmnt_buf.len,
                max_src,
                max_tgt,
                &m_work_bytes,
            ) != c.ra8_ok) {
                return error.TranslateMarianWorkFailed;
            }
            const m_work = try allocator.alloc(u8, m_work_bytes);
            defer allocator.free(m_work);

            var out_ids: [fixtures.tmnt_max_seq]u32 = undefined;
            var stats = c.MarianStats{};
            if (c.ra8_translate_marian_greedy_decode(
                nmt_case.tmnt_buf.ptr,
                nmt_case.tmnt_buf.len,
                model_src_ids.ptr,
                max_src,
                max_tgt,
                m_work.ptr,
                m_work.len,
                &out_ids,
                out_ids.len,
                &stats,
            ) != c.ra8_ok) {
                return error.TranslateMarianDecodeFailed;
            }
            summary.weight_bytes_read += stats.weight_bytes_read;

            if (c.ra8_translate_tok_decode(
                nmt_case.tvoc_buf.ptr,
                nmt_case.tvoc_buf.len,
                &out_ids,
                stats.generated_tokens,
                &out_english_buf,
                out_english_buf.len,
                &out_english_len,
            ) != c.ra8_ok) {
                return error.TranslateTokDecodeFailed;
            }
        },
        .emulator => unreachable,
    }

    try summary.chrf.addSentence(allocator, out_english_buf[0..out_english_len], bubble.english_ref);
}

fn findEmuPage(emu: corpus.EmuTrace, page_id: []const u8) ?corpus.EmuPagePred {
    for (emu.pages) |p| {
        if (std.mem.eql(u8, p.page_id, page_id)) return p;
    }
    return null;
}

fn evalPageEmulator(
    allocator: std.mem.Allocator,
    page: corpus.PageSpec,
    emu: corpus.EmuTrace,
    summary: *LangSummary,
) !void {
    const pred_page = findEmuPage(emu, page.page_id) orelse return error.MissingEmulatorPage;
    var ref_boxes: std.ArrayList(metrics.Box) = .empty;
    defer ref_boxes.deinit(allocator);
    for (page.bubbles) |b| try ref_boxes.append(allocator, b.box());

    const det = try metrics.scoreDetection(allocator, pred_page.boxes, ref_boxes.items, 0.5);
    summary.det.add(det);

    const n = @min(page.bubbles.len, pred_page.bubbles.len);
    for (0..n) |i| {
        const edits = try metrics.codepointEdits(
            allocator,
            pred_page.bubbles[i].source_text,
            page.bubbles[i].source_text,
        );
        summary.ocr_edits += edits.edits;
        summary.ocr_ref_cps += edits.ref_len;
        try summary.chrf.addSentence(
            allocator,
            pred_page.bubbles[i].english_text,
            page.bubbles[i].english_ref,
        );
    }
    if (n < page.bubbles.len) {
        for (page.bubbles[n..]) |gt| {
            const edits = try metrics.codepointEdits(allocator, "", gt.source_text);
            summary.ocr_edits += edits.edits;
            summary.ocr_ref_cps += edits.ref_len;
            try summary.chrf.addSentence(allocator, "", gt.english_ref);
        }
    }
}

fn toRow(lang: []const u8, mode: Mode, s: LangSummary) EvalRow {
    return .{
        .lang = lang,
        .mode = mode,
        .pages = s.pages,
        .bubbles = s.bubbles,
        .det_precision = s.det.precision(),
        .det_recall = s.det.recall(),
        .det_f1 = s.det.f1(),
        .ocr_cer = s.ocrCer(),
        .nmt_chrf = s.chrf.score(),
        .weight_bytes = s.weight_bytes_read,
        .furigana_dropped = s.furigana_dropped,
    };
}

/// Evaluate every language and aggregate row across enabled execution modes.
pub fn evaluate(
    allocator: std.mem.Allocator,
    eval_corpus: corpus.Corpus,
    emu_trace: ?corpus.EmuTrace,
) !EvalReport {
    const langs = eval_corpus.languages;
    const modes_buf = [_]Mode{ .float_ref, .host_i8, .emulator };
    const active_modes: []const Mode = if (emu_trace != null) modes_buf[0..3] else modes_buf[0..2];

    var rows: std.ArrayList(EvalRow) = .empty;
    errdefer rows.deinit(allocator);

    for (active_modes) |mode| {
        var overall = LangSummary{ .lang = "overall" };

        for (langs) |lang| {
            var lang_sum = LangSummary{ .lang = lang };

            for (eval_corpus.pages) |page| {
                if (!std.mem.eql(u8, page.lang, lang)) continue;
                lang_sum.pages += 1;
                lang_sum.bubbles += @intCast(page.bubbles.len);

                if (mode == .emulator) {
                    try evalPageEmulator(allocator, page, emu_trace.?, &lang_sum);
                } else {
                    const det = try evalPageDet(allocator, page, mode);
                    lang_sum.det.add(det.det);
                    lang_sum.furigana_dropped += det.furigana_dropped;

                    for (page.bubbles) |b| {
                        try evalBubbleOcrAndNmt(allocator, lang, b, mode, &lang_sum);
                    }
                }
            }

            overall.pages += lang_sum.pages;
            overall.bubbles += lang_sum.bubbles;
            overall.det.add(lang_sum.det);
            overall.ocr_edits += lang_sum.ocr_edits;
            overall.ocr_ref_cps += lang_sum.ocr_ref_cps;
            overall.weight_bytes_read += lang_sum.weight_bytes_read;
            overall.furigana_dropped += lang_sum.furigana_dropped;
            for (0..metrics.ChrFAccum.max_n) |k| {
                overall.chrf.matches[k] += lang_sum.chrf.matches[k];
                overall.chrf.hyp_totals[k] += lang_sum.chrf.hyp_totals[k];
                overall.chrf.ref_totals[k] += lang_sum.chrf.ref_totals[k];
            }

            try rows.append(allocator, toRow(lang, mode, lang_sum));
        }

        try rows.append(allocator, toRow("overall", mode, overall));
    }

    return .{
        .allocator = allocator,
        .has_emulator = emu_trace != null,
        .rows = try rows.toOwnedSlice(allocator),
    };
}
