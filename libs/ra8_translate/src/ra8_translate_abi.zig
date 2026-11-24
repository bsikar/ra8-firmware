//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Public C ABI adapter for `libs/ra8_translate/inc/ra8_translate.h`.
//! Validates pointer contracts in deterministic precedence and maps all
//! recoverable Zig domain failures into canonical `ra8_err_t` codes.

const std = @import("std");
const err_mod = @import("err");
const sidecar = @import("sidecar");
const lang = @import("lang");
const glossary = @import("glossary");
const ocr = @import("ocr");
const tokenizer = @import("tokenizer");

pub const SidecarKey = sidecar.SidecarKey;
pub const SidecarHeader = sidecar.SidecarHeader;
pub const PageEntry = sidecar.PageEntry;
pub const BubbleRecord = sidecar.BubbleRecord;
pub const ScriptCounts = lang.ScriptCounts;
pub const LangSelection = lang.Selection;
pub const OcrRect = ocr.Rect;
pub const OcrImageSpec = ocr.ImageSpec;
pub const OcrPreprocessCfg = ocr.PreprocessCfg;
pub const OcrQuantParams = ocr.QuantParams;

/// Public C ABI representation of `ra8_translate_vocab_header_t`.
pub const VocabHeader = extern struct {
    total_bytes: u32,
    container_crc32: u32,
    vocab_size: u32,
    unk_id: u32,
    bos_id: u32,
    eos_id: u32,
    pad_id: u32,
    min_normal_score: f32,
    model_type: u8,
    flags: u8,
    reserved: [2]u8,
};

comptime {
    const assert = std.debug.assert;
    assert(@sizeOf(sidecar.Language) == 1);
    assert(@backingInt(sidecar.Language.unknown) == 0);
    assert(@backingInt(sidecar.Language.korean) == 1);
    assert(@backingInt(sidecar.Language.japanese) == 2);
    assert(@backingInt(sidecar.Language.chinese) == 3);

    assert(@sizeOf(sidecar.Shape) == 1);
    assert(@backingInt(sidecar.Shape.rect) == 0);
    assert(@backingInt(sidecar.Shape.ellipse) == 1);
    assert(@backingInt(sidecar.Shape.rounded_rect) == 2);
    assert(@backingInt(sidecar.Shape.cloud) == 3);

    assert(@sizeOf(sidecar.TextOrientation) == 1);
    assert(@backingInt(sidecar.TextOrientation.horizontal) == 0);
    assert(@backingInt(sidecar.TextOrientation.vertical_rl) == 1);
    assert(@backingInt(sidecar.TextOrientation.vertical_lr) == 2);

    assert(@sizeOf(SidecarKey) == 52);
    assert(@alignOf(SidecarKey) == 4);
    assert(@offsetOf(SidecarKey, "chapter_hash") == 0);
    assert(@offsetOf(SidecarKey, "detector_model_version") == 32);
    assert(@offsetOf(SidecarKey, "ocr_model_version") == 36);
    assert(@offsetOf(SidecarKey, "nmt_model_version") == 40);
    assert(@offsetOf(SidecarKey, "glossary_revision") == 44);
    assert(@offsetOf(SidecarKey, "source_lang") == 48);
    assert(@offsetOf(SidecarKey, "reserved") == 49);

    assert(@sizeOf(SidecarHeader) == 76);
    assert(@alignOf(SidecarHeader) == 4);
    assert(@offsetOf(SidecarHeader, "total_bytes") == 0);
    assert(@offsetOf(SidecarHeader, "file_crc32") == 4);
    assert(@offsetOf(SidecarHeader, "key") == 8);
    assert(@offsetOf(SidecarHeader, "page_count") == 60);
    assert(@offsetOf(SidecarHeader, "reserved") == 62);
    assert(@offsetOf(SidecarHeader, "bubble_count") == 64);
    assert(@offsetOf(SidecarHeader, "line_break_count") == 68);
    assert(@offsetOf(SidecarHeader, "text_pool_bytes") == 72);

    assert(@sizeOf(PageEntry) == 12);
    assert(@alignOf(PageEntry) == 4);
    assert(@offsetOf(PageEntry, "page_index") == 0);
    assert(@offsetOf(PageEntry, "bubble_count") == 2);
    assert(@offsetOf(PageEntry, "first_bubble") == 4);
    assert(@offsetOf(PageEntry, "page_width") == 8);
    assert(@offsetOf(PageEntry, "page_height") == 10);

    assert(@sizeOf(BubbleRecord) == 44);
    assert(@alignOf(BubbleRecord) == 4);
    assert(@offsetOf(BubbleRecord, "x") == 0);
    assert(@offsetOf(BubbleRecord, "y") == 2);
    assert(@offsetOf(BubbleRecord, "w") == 4);
    assert(@offsetOf(BubbleRecord, "h") == 6);
    assert(@offsetOf(BubbleRecord, "shape") == 8);
    assert(@offsetOf(BubbleRecord, "text_orientation") == 9);
    assert(@offsetOf(BubbleRecord, "corner_radius") == 10);
    assert(@offsetOf(BubbleRecord, "reading_order") == 12);
    assert(@offsetOf(BubbleRecord, "ocr_confidence_bps") == 14);
    assert(@offsetOf(BubbleRecord, "font_size_px") == 16);
    assert(@offsetOf(BubbleRecord, "line_count") == 18);
    assert(@offsetOf(BubbleRecord, "first_line_break") == 20);
    assert(@offsetOf(BubbleRecord, "source_text_off") == 24);
    assert(@offsetOf(BubbleRecord, "source_text_len") == 28);
    assert(@offsetOf(BubbleRecord, "english_text_off") == 32);
    assert(@offsetOf(BubbleRecord, "english_text_len") == 36);
    assert(@offsetOf(BubbleRecord, "bubble_flags") == 40);
    assert(@offsetOf(BubbleRecord, "reserved") == 41);

    assert(@sizeOf(lang.SelectionReason) == 1);
    assert(@backingInt(lang.SelectionReason.unknown) == 0);
    assert(@backingInt(lang.SelectionReason.script_confirmed) == 1);
    assert(@backingInt(lang.SelectionReason.deferred_to_hint) == 2);
    assert(@backingInt(lang.SelectionReason.script_override) == 3);

    assert(@sizeOf(lang.OcrModel) == 1);
    assert(@backingInt(lang.OcrModel.none) == 0);
    assert(@backingInt(lang.OcrModel.korean) == 1);
    assert(@backingInt(lang.OcrModel.japanese) == 2);
    assert(@backingInt(lang.OcrModel.chinese) == 3);

    assert(@sizeOf(lang.NmtModel) == 1);
    assert(@backingInt(lang.NmtModel.none) == 0);
    assert(@backingInt(lang.NmtModel.ko_en) == 1);
    assert(@backingInt(lang.NmtModel.ja_en) == 2);
    assert(@backingInt(lang.NmtModel.zh_en) == 3);

    assert(@sizeOf(ScriptCounts) == 16);
    assert(@alignOf(ScriptCounts) == 4);
    assert(@offsetOf(ScriptCounts, "hangul_count") == 0);
    assert(@offsetOf(ScriptCounts, "kana_count") == 4);
    assert(@offsetOf(ScriptCounts, "han_count") == 8);
    assert(@offsetOf(ScriptCounts, "other_count") == 12);

    assert(@sizeOf(LangSelection) == 24);
    assert(@alignOf(LangSelection) == 4);
    assert(@offsetOf(LangSelection, "language") == 0);
    assert(@offsetOf(LangSelection, "reason") == 1);
    assert(@offsetOf(LangSelection, "ocr_model") == 2);
    assert(@offsetOf(LangSelection, "nmt_model") == 3);
    assert(@offsetOf(LangSelection, "reserved") == 4);
    assert(@offsetOf(LangSelection, "counts") == 8);

    assert(@sizeOf(ocr.PixelFormat) == 1);
    assert(@backingInt(ocr.PixelFormat.gray8) == 1);
    assert(@backingInt(ocr.PixelFormat.rgb888) == 3);

    assert(@sizeOf(ocr.Rotation) == 1);
    assert(@backingInt(ocr.Rotation.none) == 0);
    assert(@backingInt(ocr.Rotation.ccw_90) == 1);
    assert(@backingInt(ocr.Rotation.cw_90) == 2);
    assert(@backingInt(ocr.Rotation.rot_180) == 3);

    assert(@sizeOf(ocr.CoordMode) == 1);
    assert(@backingInt(ocr.CoordMode.half_pixel) == 0);
    assert(@backingInt(ocr.CoordMode.align_corners) == 1);
    assert(@backingInt(ocr.CoordMode.asym_scale) == 2);

    assert(@sizeOf(OcrRect) == 8);
    assert(@alignOf(OcrRect) == 2);
    assert(@offsetOf(OcrRect, "x") == 0);
    assert(@offsetOf(OcrRect, "y") == 2);
    assert(@offsetOf(OcrRect, "width") == 4);
    assert(@offsetOf(OcrRect, "height") == 6);

    assert(@sizeOf(OcrImageSpec) == 12);
    assert(@alignOf(OcrImageSpec) == 4);
    assert(@offsetOf(OcrImageSpec, "stride_bytes") == 0);
    assert(@offsetOf(OcrImageSpec, "width") == 4);
    assert(@offsetOf(OcrImageSpec, "height") == 6);
    assert(@offsetOf(OcrImageSpec, "format") == 8);
    assert(@offsetOf(OcrImageSpec, "reserved") == 9);

    assert(@sizeOf(OcrPreprocessCfg) == 8);
    assert(@alignOf(OcrPreprocessCfg) == 2);
    assert(@offsetOf(OcrPreprocessCfg, "dst_width") == 0);
    assert(@offsetOf(OcrPreprocessCfg, "dst_height") == 2);
    assert(@offsetOf(OcrPreprocessCfg, "dst_format") == 4);
    assert(@offsetOf(OcrPreprocessCfg, "rotation") == 5);
    assert(@offsetOf(OcrPreprocessCfg, "coord_mode") == 6);
    assert(@offsetOf(OcrPreprocessCfg, "reserved") == 7);

    assert(@sizeOf(OcrQuantParams) == 36);
    assert(@alignOf(OcrQuantParams) == 4);
    assert(@offsetOf(OcrQuantParams, "mean") == 0);
    assert(@offsetOf(OcrQuantParams, "inv_std") == 12);
    assert(@offsetOf(OcrQuantParams, "input_scale") == 24);
    assert(@offsetOf(OcrQuantParams, "quant_scale") == 28);
    assert(@offsetOf(OcrQuantParams, "zero_point") == 32);
    assert(@offsetOf(OcrQuantParams, "reserved") == 33);

    assert(@sizeOf(tokenizer.ModelType) == 1);
    assert(@backingInt(tokenizer.ModelType.unigram) == 0);
    assert(@backingInt(tokenizer.ModelType.bpe) == 1);

    assert(@sizeOf(tokenizer.PieceKind) == 1);
    assert(@backingInt(tokenizer.PieceKind.normal) == 0);
    assert(@backingInt(tokenizer.PieceKind.unknown) == 1);
    assert(@backingInt(tokenizer.PieceKind.control) == 2);
    assert(@backingInt(tokenizer.PieceKind.user_defined) == 3);
    assert(@backingInt(tokenizer.PieceKind.byte) == 4);
    assert(@backingInt(tokenizer.PieceKind.unused) == 5);

    assert(@sizeOf(VocabHeader) == 36);
    assert(@alignOf(VocabHeader) == 4);
    assert(@offsetOf(VocabHeader, "total_bytes") == 0);
    assert(@offsetOf(VocabHeader, "container_crc32") == 4);
    assert(@offsetOf(VocabHeader, "vocab_size") == 8);
    assert(@offsetOf(VocabHeader, "unk_id") == 12);
    assert(@offsetOf(VocabHeader, "bos_id") == 16);
    assert(@offsetOf(VocabHeader, "eos_id") == 20);
    assert(@offsetOf(VocabHeader, "pad_id") == 24);
    assert(@offsetOf(VocabHeader, "min_normal_score") == 28);
    assert(@offsetOf(VocabHeader, "model_type") == 32);
    assert(@offsetOf(VocabHeader, "flags") == 33);
    assert(@offsetOf(VocabHeader, "reserved") == 34);
}

fn inputSlice(comptime T: type, ptr_opt: ?[*]const T, count: usize) ?[]const T {
    if (count == 0) return &.{};
    const ptr = ptr_opt orelse return null;
    return ptr[0..count];
}

/// C ABI export for `ra8_translate_sidecar_open()`.
pub export fn ra8_translate_sidecar_open(
    bytes_opt: ?[*]const u8,
    len: usize,
    out_header_opt: ?*SidecarHeader,
) callconv(.c) u16 {
    const out_header = out_header_opt orelse return err_mod.k_ra8_err_null_ptr;
    const bytes = inputSlice(u8, bytes_opt, len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = sidecar.SidecarView.open(bytes) catch |e| return err_mod.toRa8Err(e);
    out_header.* = view.header;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_sidecar_key_matches()`.
pub export fn ra8_translate_sidecar_key_matches(
    header_opt: ?*const SidecarHeader,
    expected_opt: ?*const SidecarKey,
    out_matches_opt: ?*bool,
) callconv(.c) u16 {
    const out_matches = out_matches_opt orelse return err_mod.k_ra8_err_null_ptr;
    const header = header_opt orelse return err_mod.k_ra8_err_null_ptr;
    const expected = expected_opt orelse return err_mod.k_ra8_err_null_ptr;
    const matched = sidecar.keysMatch(&header.key, expected) catch |e| return err_mod.toRa8Err(e);
    out_matches.* = matched;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_sidecar_page_at()`.
pub export fn ra8_translate_sidecar_page_at(
    bytes_opt: ?[*]const u8,
    len: usize,
    page_index: u16,
    out_page_opt: ?*PageEntry,
) callconv(.c) u16 {
    const out_page = out_page_opt orelse return err_mod.k_ra8_err_null_ptr;
    const bytes = inputSlice(u8, bytes_opt, len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = sidecar.SidecarView.open(bytes) catch |e| return err_mod.toRa8Err(e);
    const page = view.pageAt(page_index) catch |e| return err_mod.toRa8Err(e);
    out_page.* = page;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_sidecar_bubble_at()`.
pub export fn ra8_translate_sidecar_bubble_at(
    bytes_opt: ?[*]const u8,
    len: usize,
    bubble_index: u32,
    out_bubble_opt: ?*BubbleRecord,
) callconv(.c) u16 {
    const out_bubble = out_bubble_opt orelse return err_mod.k_ra8_err_null_ptr;
    const bytes = inputSlice(u8, bytes_opt, len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = sidecar.SidecarView.open(bytes) catch |e| return err_mod.toRa8Err(e);
    const bubble = view.bubbleAt(bubble_index) catch |e| return err_mod.toRa8Err(e);
    out_bubble.* = bubble;
    return err_mod.k_ra8_ok;
}

fn sidecarTextExport(
    bytes_opt: ?[*]const u8,
    len: usize,
    bubble_index: u32,
    out_ptr_opt: ?*?[*]const u8,
    out_len_opt: ?*u32,
    is_english: bool,
) u16 {
    const out_ptr = out_ptr_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_len = out_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const bytes = inputSlice(u8, bytes_opt, len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = sidecar.SidecarView.open(bytes) catch |e| return err_mod.toRa8Err(e);
    const span = if (is_english)
        view.englishText(bubble_index) catch |e| return err_mod.toRa8Err(e)
    else
        view.sourceText(bubble_index) catch |e| return err_mod.toRa8Err(e);
    out_ptr.* = span.ptr;
    out_len.* = @intCast(span.len);
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_sidecar_source_text()`.
pub export fn ra8_translate_sidecar_source_text(
    bytes_opt: ?[*]const u8,
    len: usize,
    bubble_index: u32,
    out_ptr_opt: ?*?[*]const u8,
    out_len_opt: ?*u32,
) callconv(.c) u16 {
    return sidecarTextExport(bytes_opt, len, bubble_index, out_ptr_opt, out_len_opt, false);
}

/// C ABI export for `ra8_translate_sidecar_english_text()`.
pub export fn ra8_translate_sidecar_english_text(
    bytes_opt: ?[*]const u8,
    len: usize,
    bubble_index: u32,
    out_ptr_opt: ?*?[*]const u8,
    out_len_opt: ?*u32,
) callconv(.c) u16 {
    return sidecarTextExport(bytes_opt, len, bubble_index, out_ptr_opt, out_len_opt, true);
}

/// C ABI export for `ra8_translate_sidecar_line_breaks()`.
pub export fn ra8_translate_sidecar_line_breaks(
    bytes_opt: ?[*]const u8,
    len: usize,
    bubble_index: u32,
    out_breaks_opt: ?[*]u16,
    break_cap: u16,
    out_count_opt: ?*u16,
) callconv(.c) u16 {
    const out_breaks = out_breaks_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_count = out_count_opt orelse return err_mod.k_ra8_err_null_ptr;
    const bytes = inputSlice(u8, bytes_opt, len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = sidecar.SidecarView.open(bytes) catch |e| return err_mod.toRa8Err(e);
    const count = view.lineBreaks(bubble_index, out_breaks[0..break_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_count.* = count;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_sidecar_required_bytes()`.
pub export fn ra8_translate_sidecar_required_bytes(
    page_count: u16,
    bubble_count: u32,
    line_break_count: u32,
    text_pool_bytes: u32,
    out_bytes_opt: ?*usize,
) callconv(.c) u16 {
    const out_bytes = out_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;
    const needed = sidecar.requiredBytes(page_count, bubble_count, line_break_count, text_pool_bytes) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_bytes.* = needed;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_sidecar_write()`.
pub export fn ra8_translate_sidecar_write(
    key_opt: ?*const SidecarKey,
    pages_opt: ?[*]const PageEntry,
    page_count: u16,
    bubbles_opt: ?[*]const BubbleRecord,
    bubble_count: u32,
    line_breaks_opt: ?[*]const u16,
    line_break_count: u32,
    text_pool_opt: ?[*]const u8,
    text_pool_bytes: u32,
    out_buf_opt: ?[*]u8,
    out_cap: usize,
    out_written_opt: ?*usize,
) callconv(.c) u16 {
    const out_buf = out_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_written = out_written_opt orelse return err_mod.k_ra8_err_null_ptr;
    const key = key_opt orelse return err_mod.k_ra8_err_null_ptr;
    const pages = inputSlice(PageEntry, pages_opt, page_count) orelse return err_mod.k_ra8_err_null_ptr;
    const bubbles = inputSlice(BubbleRecord, bubbles_opt, bubble_count) orelse return err_mod.k_ra8_err_null_ptr;
    const breaks = inputSlice(u16, line_breaks_opt, line_break_count) orelse return err_mod.k_ra8_err_null_ptr;
    const pool = inputSlice(u8, text_pool_opt, text_pool_bytes) orelse return err_mod.k_ra8_err_null_ptr;

    const written = sidecar.writeSidecar(
        key,
        pages,
        bubbles,
        breaks,
        pool,
        out_buf[0..out_cap],
    ) catch |e| return err_mod.toRa8Err(e);
    out_written.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_lang_from_hint()`.
pub export fn ra8_translate_lang_from_hint(
    hint_opt: ?[*]const u8,
    hint_len: usize,
    out_lang_opt: ?*u8,
) callconv(.c) u16 {
    const out_lang = out_lang_opt orelse return err_mod.k_ra8_err_null_ptr;
    const hint = inputSlice(u8, hint_opt, hint_len) orelse return err_mod.k_ra8_err_null_ptr;
    out_lang.* = @backingInt(lang.parseLanguageHint(hint));
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_lang_select()`.
pub export fn ra8_translate_lang_select(
    hint_raw: u8,
    sample_utf8_opt: ?[*]const u8,
    sample_len: usize,
    out_selection_opt: ?*LangSelection,
) callconv(.c) u16 {
    const out_selection = out_selection_opt orelse return err_mod.k_ra8_err_null_ptr;
    const sample = inputSlice(u8, sample_utf8_opt, sample_len) orelse return err_mod.k_ra8_err_null_ptr;
    const hint = sidecar.Language.fromRaw(hint_raw) orelse return err_mod.k_ra8_err_invalid_arg;
    const result = lang.selectLanguage(hint, sample) catch |e| return err_mod.toRa8Err(e);
    out_selection.* = result;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_glossary_init()`.
pub export fn ra8_translate_glossary_init(
    lang_raw: u8,
    revision: u32,
    out_buf_opt: ?[*]u8,
    out_cap: usize,
    out_len_opt: ?*usize,
) callconv(.c) u16 {
    const out_len = out_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_buf = out_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const source_lang = sidecar.Language.fromRaw(lang_raw) orelse return err_mod.k_ra8_err_invalid_arg;
    const written = glossary.initEmptyWire(source_lang, revision, out_buf[0..out_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_len.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_glossary_upsert()`.
pub export fn ra8_translate_glossary_upsert(
    tgl_opt: ?[*]const u8,
    tgl_len: usize,
    src_opt: ?[*]const u8,
    src_len: usize,
    eng_opt: ?[*]const u8,
    eng_len: usize,
    out_buf_opt: ?[*]u8,
    out_cap: usize,
    out_len_opt: ?*usize,
) callconv(.c) u16 {
    const out_len = out_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_buf = out_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tgl = inputSlice(u8, tgl_opt, tgl_len) orelse return err_mod.k_ra8_err_null_ptr;
    const src = inputSlice(u8, src_opt, src_len) orelse return err_mod.k_ra8_err_null_ptr;
    const eng = inputSlice(u8, eng_opt, eng_len) orelse return err_mod.k_ra8_err_null_ptr;
    const written = glossary.upsertWire(tgl, src, eng, out_buf[0..out_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_len.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_glossary_lookup()`.
pub export fn ra8_translate_glossary_lookup(
    tgl_opt: ?[*]const u8,
    tgl_len: usize,
    src_opt: ?[*]const u8,
    src_len: usize,
    out_eng_ptr_opt: ?*?[*]const u8,
    out_eng_len_opt: ?*usize,
) callconv(.c) u16 {
    const out_eng_ptr = out_eng_ptr_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_eng_len = out_eng_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tgl = inputSlice(u8, tgl_opt, tgl_len) orelse return err_mod.k_ra8_err_null_ptr;
    const src = inputSlice(u8, src_opt, src_len) orelse return err_mod.k_ra8_err_null_ptr;
    if (src.len == 0) return err_mod.k_ra8_err_invalid_arg;
    const view = glossary.TglView.open(tgl) catch |e| return err_mod.toRa8Err(e);
    const eng_opt = view.lookup(src) catch |e| return err_mod.toRa8Err(e);
    const eng = eng_opt orelse return err_mod.k_ra8_err_not_found;
    out_eng_ptr.* = eng.ptr;
    out_eng_len.* = eng.len;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_glossary_mask()`.
pub export fn ra8_translate_glossary_mask(
    tgl_opt: ?[*]const u8,
    tgl_len: usize,
    src_opt: ?[*]const u8,
    src_len: usize,
    out_masked_opt: ?[*]u8,
    masked_cap: usize,
    out_masked_len_opt: ?*usize,
    out_plan_opt: ?[*]u8,
    plan_cap: usize,
    out_plan_len_opt: ?*usize,
) callconv(.c) u16 {
    const out_masked_len = out_masked_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_plan_len = out_plan_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_masked = out_masked_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_plan = out_plan_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tgl = inputSlice(u8, tgl_opt, tgl_len) orelse return err_mod.k_ra8_err_null_ptr;
    const src = inputSlice(u8, src_opt, src_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = glossary.TglView.open(tgl) catch |e| return err_mod.toRa8Err(e);
    const res = view.maskTextWire(src, out_masked[0..masked_cap], out_plan[0..plan_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_masked_len.* = res.masked_len;
    out_plan_len.* = res.plan_len;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_glossary_restore()`.
pub export fn ra8_translate_glossary_restore(
    tgl_opt: ?[*]const u8,
    tgl_len: usize,
    plan_opt: ?[*]const u8,
    plan_len: usize,
    eng_opt: ?[*]const u8,
    eng_len: usize,
    out_restored_opt: ?[*]u8,
    restored_cap: usize,
    out_restored_len_opt: ?*usize,
) callconv(.c) u16 {
    const out_restored_len = out_restored_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_restored = out_restored_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tgl = inputSlice(u8, tgl_opt, tgl_len) orelse return err_mod.k_ra8_err_null_ptr;
    const plan_bytes = inputSlice(u8, plan_opt, plan_len) orelse return err_mod.k_ra8_err_null_ptr;
    const eng = inputSlice(u8, eng_opt, eng_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = glossary.TglView.open(tgl) catch |e| return err_mod.toRa8Err(e);
    const plan = glossary.SubstitutionPlan.deserialize(plan_bytes) catch |e| return err_mod.toRa8Err(e);
    const written = view.restoreText(&plan, eng, out_restored[0..restored_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_restored_len.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ocr_has_helium()`.
pub export fn ra8_translate_ocr_has_helium(out_has_helium_opt: ?*bool) callconv(.c) u16 {
    const out_has_helium = out_has_helium_opt orelse return err_mod.k_ra8_err_null_ptr;
    out_has_helium.* = ocr.hasHeliumMve();
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ocr_required_bytes()`.
pub export fn ra8_translate_ocr_required_bytes(
    dst_width: u16,
    dst_height: u16,
    dst_format_raw: u8,
    out_bytes_opt: ?*usize,
) callconv(.c) u16 {
    const out_bytes = out_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;
    const needed = ocr.requiredDstBytes(dst_width, dst_height, dst_format_raw) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_bytes.* = needed;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ocr_resize_u8()`.
pub export fn ra8_translate_ocr_resize_u8(
    src_opt: ?[*]const u8,
    src_len: usize,
    src_spec_opt: ?*const OcrImageSpec,
    crop_opt: ?*const OcrRect,
    cfg_opt: ?*const OcrPreprocessCfg,
    out_dst_opt: ?[*]u8,
    out_cap: usize,
    out_written_opt: ?*usize,
) callconv(.c) u16 {
    const out_written = out_written_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_dst = out_dst_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src = src_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src_spec = src_spec_opt orelse return err_mod.k_ra8_err_null_ptr;
    const crop = crop_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;

    const written = ocr.resizeCropRotateU8(
        src[0..src_len],
        src_spec.*,
        crop.*,
        cfg.*,
        out_dst[0..out_cap],
    ) catch |e| return err_mod.toRa8Err(e);
    out_written.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ocr_normalize_i8()`.
pub export fn ra8_translate_ocr_normalize_i8(
    src_opt: ?[*]const u8,
    src_len: usize,
    format_raw: u8,
    quant_opt: ?*const OcrQuantParams,
    out_dst_opt: ?[*]i8,
    out_cap: usize,
    out_written_opt: ?*usize,
) callconv(.c) u16 {
    const out_written = out_written_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_dst = out_dst_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src = src_opt orelse return err_mod.k_ra8_err_null_ptr;
    const quant = quant_opt orelse return err_mod.k_ra8_err_null_ptr;

    const written = ocr.normalizeU8ToI8(
        src[0..src_len],
        format_raw,
        quant.*,
        out_dst[0..out_cap],
    ) catch |e| return err_mod.toRa8Err(e);
    out_written.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ocr_preprocess_i8()`.
pub export fn ra8_translate_ocr_preprocess_i8(
    src_opt: ?[*]const u8,
    src_len: usize,
    src_spec_opt: ?*const OcrImageSpec,
    crop_opt: ?*const OcrRect,
    cfg_opt: ?*const OcrPreprocessCfg,
    quant_opt: ?*const OcrQuantParams,
    out_dst_opt: ?[*]i8,
    out_cap: usize,
    out_written_opt: ?*usize,
) callconv(.c) u16 {
    const out_written = out_written_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_dst = out_dst_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src = src_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src_spec = src_spec_opt orelse return err_mod.k_ra8_err_null_ptr;
    const crop = crop_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const quant = quant_opt orelse return err_mod.k_ra8_err_null_ptr;

    const written = ocr.preprocessCropRotateToI8(
        src[0..src_len],
        src_spec.*,
        crop.*,
        cfg.*,
        quant.*,
        out_dst[0..out_cap],
    ) catch |e| return err_mod.toRa8Err(e);
    out_written.* = written;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_tok_open()`.
pub export fn ra8_translate_tok_open(
    tvoc_opt: ?[*]const u8,
    tvoc_len: usize,
    out_header_opt: ?*VocabHeader,
) callconv(.c) u16 {
    const out_header = out_header_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tvoc = inputSlice(u8, tvoc_opt, tvoc_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = tokenizer.TvocView.open(tvoc) catch |e| return err_mod.toRa8Err(e);
    out_header.* = .{
        .total_bytes = view.header.total_bytes,
        .container_crc32 = view.header.container_crc32,
        .vocab_size = view.header.vocab_size,
        .unk_id = view.header.unk_id,
        .bos_id = view.header.bos_id,
        .eos_id = view.header.eos_id,
        .pad_id = view.header.pad_id,
        .min_normal_score = view.header.min_normal_score,
        .model_type = @backingInt(view.header.model_type),
        .flags = view.header.flags,
        .reserved = .{ 0, 0 },
    };
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_tok_piece_info()`.
pub export fn ra8_translate_tok_piece_info(
    tvoc_opt: ?[*]const u8,
    tvoc_len: usize,
    token_id: u32,
    out_piece_ptr_opt: ?*?[*]const u8,
    out_piece_len_opt: ?*u16,
    out_kind_opt: ?*u8,
    out_score_opt: ?*f32,
) callconv(.c) u16 {
    const out_piece_ptr = out_piece_ptr_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_piece_len = out_piece_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_kind = out_kind_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_score = out_score_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tvoc = inputSlice(u8, tvoc_opt, tvoc_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = tokenizer.TvocView.open(tvoc) catch |e| return err_mod.toRa8Err(e);
    const meta = view.pieceAt(token_id) catch |e| return err_mod.toRa8Err(e);
    out_piece_ptr.* = meta.text.ptr;
    out_piece_len.* = @intCast(meta.text.len);
    out_kind.* = @backingInt(meta.kind);
    out_score.* = meta.score;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_tok_work_bytes()`.
pub export fn ra8_translate_tok_work_bytes(
    text_len: usize,
    model_raw: u8,
    out_work_bytes_opt: ?*usize,
) callconv(.c) u16 {
    const out_work_bytes = out_work_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;
    const model_type = tokenizer.ModelType.fromRaw(model_raw) orelse return err_mod.k_ra8_err_invalid_arg;
    const needed = tokenizer.minWorkBufBytes(text_len, model_type) catch |e| return err_mod.toRa8Err(e);
    out_work_bytes.* = needed;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_tok_encode()`.
pub export fn ra8_translate_tok_encode(
    tvoc_opt: ?[*]const u8,
    tvoc_len: usize,
    text_opt: ?[*]const u8,
    text_len: usize,
    enc_flags: u8,
    work_opt: ?[*]u8,
    work_cap: usize,
    out_ids_opt: ?[*]u32,
    id_cap: usize,
    out_id_count_opt: ?*usize,
) callconv(.c) u16 {
    const out_id_count = out_id_count_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_ids = out_ids_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tvoc = inputSlice(u8, tvoc_opt, tvoc_len) orelse return err_mod.k_ra8_err_null_ptr;
    const text = inputSlice(u8, text_opt, text_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = tokenizer.TvocView.open(tvoc) catch |e| return err_mod.toRa8Err(e);
    const count = view.encode(text, enc_flags, work_buf[0..work_cap], out_ids[0..id_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_id_count.* = count;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_tok_decode()`.
pub export fn ra8_translate_tok_decode(
    tvoc_opt: ?[*]const u8,
    tvoc_len: usize,
    ids_opt: ?[*]const u32,
    id_count: usize,
    out_utf8_opt: ?[*]u8,
    out_cap: usize,
    out_len_opt: ?*usize,
) callconv(.c) u16 {
    const out_len = out_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_utf8 = out_utf8_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tvoc = inputSlice(u8, tvoc_opt, tvoc_len) orelse return err_mod.k_ra8_err_null_ptr;
    const ids = inputSlice(u32, ids_opt, id_count) orelse return err_mod.k_ra8_err_null_ptr;
    const view = tokenizer.TvocView.open(tvoc) catch |e| return err_mod.toRa8Err(e);
    const written = view.decode(ids, out_utf8[0..out_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_len.* = written;
    return err_mod.k_ra8_ok;
}
