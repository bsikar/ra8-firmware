//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C23 ABI adapter exports for `libs/ra8_translate` CTC character tables and
//! greedy OCR recognizer decoding.

const std = @import("std");
const ctc = @import("ctc_decode");
const err_mod = @import("err");

pub const CharsetHeader = ctc.CharsetHeader;
pub const DecodeCfg = ctc.DecodeCfg;
pub const DecodeResult = ctc.DecodeResult;

comptime {
    const assert = std.debug.assert;
    assert(@sizeOf(ctc.ScoreMode) == 1);
    assert(@sizeOf(CharsetHeader) == 32);
    assert(@alignOf(CharsetHeader) == 4);
    assert(@offsetOf(CharsetHeader, "total_bytes") == 0);
    assert(@offsetOf(CharsetHeader, "container_crc32") == 4);
    assert(@offsetOf(CharsetHeader, "version") == 8);
    assert(@offsetOf(CharsetHeader, "flags") == 10);
    assert(@offsetOf(CharsetHeader, "class_count") == 12);
    assert(@offsetOf(CharsetHeader, "blank_index") == 16);
    assert(@offsetOf(CharsetHeader, "unk_index") == 20);
    assert(@offsetOf(CharsetHeader, "string_pool_bytes") == 24);
    assert(@offsetOf(CharsetHeader, "ocr_model") == 28);
    assert(@offsetOf(CharsetHeader, "reserved") == 29);

    assert(@sizeOf(DecodeCfg) == 16);
    assert(@alignOf(DecodeCfg) == 4);
    assert(@offsetOf(DecodeCfg, "time_steps") == 0);
    assert(@offsetOf(DecodeCfg, "class_stride") == 4);
    assert(@offsetOf(DecodeCfg, "score_mode") == 8);
    assert(@offsetOf(DecodeCfg, "decode_flags") == 9);
    assert(@offsetOf(DecodeCfg, "low_confidence_thresh_bps") == 10);
    assert(@offsetOf(DecodeCfg, "expected_ocr_model") == 12);
    assert(@offsetOf(DecodeCfg, "reserved") == 13);

    assert(@sizeOf(DecodeResult) == 24);
    assert(@alignOf(DecodeResult) == 4);
    assert(@offsetOf(DecodeResult, "text_bytes") == 0);
    assert(@offsetOf(DecodeResult, "emitted_tokens") == 4);
    assert(@offsetOf(DecodeResult, "blank_frames") == 8);
    assert(@offsetOf(DecodeResult, "unk_count") == 12);
    assert(@offsetOf(DecodeResult, "confidence_bps") == 16);
    assert(@offsetOf(DecodeResult, "min_token_confidence_bps") == 18);
    assert(@offsetOf(DecodeResult, "low_confidence") == 20);
    assert(@offsetOf(DecodeResult, "reserved") == 21);
}

fn inputSlice(comptime T: type, ptr_opt: ?[*]const T, len: usize) ?[]const T {
    if (len == 0) return &.{};
    const ptr = ptr_opt orelse return null;
    return ptr[0..len];
}

/// C ABI export for `ra8_translate_ctc_charset_open()`.
pub export fn ra8_translate_ctc_charset_open(
    tchr_opt: ?[*]const u8,
    tchr_len: usize,
    out_header_opt: ?*CharsetHeader,
) callconv(.c) u16 {
    const out_header = out_header_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tchr = inputSlice(u8, tchr_opt, tchr_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = ctc.CharsetView.open(tchr) catch |e| return err_mod.toRa8Err(e);
    out_header.* = view.header;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ctc_charset_glyph()`.
pub export fn ra8_translate_ctc_charset_glyph(
    tchr_opt: ?[*]const u8,
    tchr_len: usize,
    class_index: u32,
    out_utf8_ptr_opt: ?*?[*]const u8,
    out_utf8_len_opt: ?*u16,
) callconv(.c) u16 {
    const out_ptr = out_utf8_ptr_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_len = out_utf8_len_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tchr = inputSlice(u8, tchr_opt, tchr_len) orelse return err_mod.k_ra8_err_null_ptr;
    const view = ctc.CharsetView.open(tchr) catch |e| return err_mod.toRa8Err(e);
    const glyph = view.glyphAt(class_index) catch |e| return err_mod.toRa8Err(e);
    out_ptr.* = glyph.ptr;
    out_len.* = @intCast(glyph.len);
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ctc_decode_i8()`.
pub export fn ra8_translate_ctc_decode_i8(
    tchr_opt: ?[*]const u8,
    tchr_len: usize,
    logits_opt: ?[*]const i8,
    logit_elem_count: usize,
    quant_scale: f32,
    zero_point: i8,
    cfg_opt: ?*const DecodeCfg,
    out_utf8_opt: ?[*]u8,
    out_cap: usize,
    out_result_opt: ?*DecodeResult,
) callconv(.c) u16 {
    const out_result = out_result_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_utf8 = out_utf8_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tchr = inputSlice(u8, tchr_opt, tchr_len) orelse return err_mod.k_ra8_err_null_ptr;
    const logits = inputSlice(i8, logits_opt, logit_elem_count) orelse {
        return err_mod.k_ra8_err_null_ptr;
    };
    const view = ctc.CharsetView.open(tchr) catch |e| return err_mod.toRa8Err(e);
    const res = ctc.decodeI8(view, logits, quant_scale, zero_point, cfg.*, out_utf8[0..out_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_result.* = res;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_ctc_decode_f32()`.
pub export fn ra8_translate_ctc_decode_f32(
    tchr_opt: ?[*]const u8,
    tchr_len: usize,
    logits_opt: ?[*]const f32,
    logit_elem_count: usize,
    cfg_opt: ?*const DecodeCfg,
    out_utf8_opt: ?[*]u8,
    out_cap: usize,
    out_result_opt: ?*DecodeResult,
) callconv(.c) u16 {
    const out_result = out_result_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_utf8 = out_utf8_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tchr = inputSlice(u8, tchr_opt, tchr_len) orelse return err_mod.k_ra8_err_null_ptr;
    const logits = inputSlice(f32, logits_opt, logit_elem_count) orelse {
        return err_mod.k_ra8_err_null_ptr;
    };
    const view = ctc.CharsetView.open(tchr) catch |e| return err_mod.toRa8Err(e);
    const res = ctc.decodeF32(view, logits, cfg.*, out_utf8[0..out_cap]) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_result.* = res;
    return err_mod.k_ra8_ok;
}
