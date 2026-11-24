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

pub const SidecarKey = sidecar.SidecarKey;
pub const SidecarHeader = sidecar.SidecarHeader;
pub const PageEntry = sidecar.PageEntry;
pub const BubbleRecord = sidecar.BubbleRecord;
pub const ScriptCounts = lang.ScriptCounts;
pub const LangSelection = lang.Selection;

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
