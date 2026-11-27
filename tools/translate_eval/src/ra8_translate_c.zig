//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C23 ABI extern bindings matching `libs/ra8_translate/inc/*.h`.

/// `ra8_err_t` value indicating success (`k_ra8_ok`).
pub const ra8_ok: u16 = 0;

/// Detector constants from `ra8_translate_detector.h`.
pub const det_order_manga_rtl: u8 = 0;
pub const det_order_webtoon_ttb: u8 = 1;
pub const det_flag_drop_furigana: u8 = 1 << 0;
pub const det_def_txt_bps: u16 = 3000;
pub const det_def_bub_bps: u16 = 4000;
pub const det_def_gap_pct: u16 = 150;
pub const det_def_furi_pct: u16 = 55;
pub const det_def_fgap_pct: u16 = 120;

/// OCR / CTC constants matching `ra8_translate_ctc.h` and `ra8_translate.h`.
pub const ocr_korean: u8 = 1;
pub const ocr_japanese: u8 = 2;
pub const ocr_chinese: u8 = 3;
pub const ctc_magic: u32 = 0x52484354;
pub const ctc_version: u16 = 1;
pub const ctc_header_bytes: usize = 36;
pub const ctc_index_none: u32 = 0xFFFF_FFFF;
pub const ctc_score_logits: u8 = 0;
pub const ctc_score_probs: u8 = 1;
pub const ctc_flag_none: u8 = 0;
pub const ctc_default_low_conf_bps: u16 = 7500;

/// SentencePiece `TVOC` constants matching `ra8_translate.h`.
pub const tok_unigram: u8 = 0;
pub const tok_piece_normal: u8 = 0;
pub const tok_piece_unknown: u8 = 1;
pub const tok_piece_control: u8 = 2;
pub const tok_flag_add_dummy_prefix: u8 = 0x01;
pub const tok_flag_remove_extra_ws: u8 = 0x02;
pub const tok_flag_escape_ws: u8 = 0x04;
pub const tok_enc_none: u8 = 0x00;
pub const tok_enc_add_eos: u8 = 0x02;
pub const tok_id_none: u32 = 0xFFFF_FFFF;

/// Marian `TMNT` constants matching `ra8_translate_marian.h`.
pub const marian_magic: u32 = 0x544E_4D54;
pub const marian_version: u16 = 1;
pub const marian_header_bytes: usize = 84;
pub const marian_tensor_desc_bytes: usize = 24;
pub const marian_flag_pre_norm: u16 = 1 << 0;
pub const marian_open_verify_payload: u8 = 1 << 0;
pub const marian_dtype_none: u8 = 0;
pub const marian_dtype_i8: u8 = 1;
pub const marian_dtype_f32: u8 = 2;
pub const nmt_ko_en: u8 = 1;
pub const nmt_ja_en: u8 = 2;
pub const nmt_zh_en: u8 = 3;

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

pub const DetCfg = extern struct {
    map_width: u16,
    map_height: u16,
    row_stride: u16 = 0,
    elem_stride: u16 = 0,
    page_width: u16,
    page_height: u16,
    text_thresh_bps: u16 = det_def_txt_bps,
    bubble_thresh_bps: u16 = det_def_bub_bps,
    min_comp_area: u16 = 1,
    box_pad_pct: u16 = 0,
    group_gap_pct: u16 = det_def_gap_pct,
    furigana_max_ratio_pct: u16 = det_def_furi_pct,
    furigana_max_gap_pct: u16 = det_def_fgap_pct,
    reading_order: u8,
    flags: u8,
    max_components: u16,
    reserved: [2]u8 = .{ 0, 0 },
};

pub const DetSummary = extern struct {
    raw_components: u32 = 0,
    dropped_furigana: u32 = 0,
    line_count: u32 = 0,
    bubble_count: u32 = 0,
};

pub const CtcCharsetHeader = extern struct {
    total_bytes: u32 = 0,
    container_crc32: u32 = 0,
    version: u16 = 0,
    flags: u16 = 0,
    class_count: u32 = 0,
    blank_index: u32 = 0,
    unk_index: u32 = 0,
    string_pool_bytes: u32 = 0,
    ocr_model: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

pub const CtcDecodeCfg = extern struct {
    time_steps: u32,
    class_stride: u32 = 0,
    score_mode: u8 = ctc_score_logits,
    decode_flags: u8 = ctc_flag_none,
    low_confidence_thresh_bps: u16 = ctc_default_low_conf_bps,
    expected_ocr_model: u8,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

pub const CtcResult = extern struct {
    text_bytes: u32 = 0,
    emitted_tokens: u32 = 0,
    blank_frames: u32 = 0,
    unk_count: u32 = 0,
    confidence_bps: u16 = 0,
    min_token_confidence_bps: u16 = 0,
    low_confidence: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

comptime {
    if (@sizeOf(CtcCharsetHeader) != 32 or @offsetOf(CtcCharsetHeader, "class_count") != 12) {
        @compileError("CtcCharsetHeader ABI layout mismatch");
    }
    if (@sizeOf(CtcResult) != 24 or @offsetOf(CtcResult, "unk_count") != 12) {
        @compileError("CtcResult ABI layout mismatch");
    }
}

pub const VocabHeader = extern struct {
    total_bytes: u32 = 0,
    container_crc32: u32 = 0,
    vocab_size: u32 = 0,
    unk_id: u32 = 0,
    bos_id: u32 = 0,
    eos_id: u32 = 0,
    pad_id: u32 = 0,
    min_normal_score: f32 = 0.0,
    model_type: u8 = 0,
    flags: u8 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

pub const MarianHeader = extern struct {
    total_bytes: u32 = 0,
    meta_crc32: u32 = 0,
    payload_crc32: u32 = 0,
    vocab_size: u32 = 0,
    unk_id: u32 = 0,
    bos_id: u32 = 0,
    eos_id: u32 = 0,
    pad_id: u32 = 0,
    shortlist_off: u32 = 0,
    shortlist_bytes: u32 = 0,
    ln_eps: f32 = 0.0,
    version: u16 = 0,
    model_flags: u16 = 0,
    d_model: u16 = 0,
    d_ff: u16 = 0,
    max_seq_len: u16 = 0,
    nmt_model: u8 = 0,
    num_heads: u8 = 0,
    num_enc_layers: u8 = 0,
    num_dec_layers: u8 = 0,
    reserved: [6]u8 = .{ 0, 0, 0, 0, 0, 0 },
};

pub const MarianStats = extern struct {
    weight_bytes_read: u64 = 0,
    generated_tokens: u32 = 0,
    stopped_on_eos: u8 = 0,
    used_helium_mve: u8 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

pub extern fn ra8_translate_det_work_bytes(
    map_width: u16,
    max_components: u16,
    out_work_bytes: *usize,
) callconv(.c) u16;

pub extern fn ra8_translate_det_post_i8(
    text_map: [*]const i8,
    text_elem_count: usize,
    text_scale: f32,
    text_zp: i8,
    bubble_map: ?[*]const i8,
    bubble_elem_count: usize,
    bubble_scale: f32,
    bubble_zp: i8,
    cfg: *const DetCfg,
    work_buf: [*]u8,
    work_cap: usize,
    out_lines: [*]DetLine,
    line_cap: usize,
    out_bubbles: [*]DetBubble,
    bubble_cap: usize,
    out_summary: *DetSummary,
) callconv(.c) u16;

pub extern fn ra8_translate_det_post_f32(
    text_map: [*]const f32,
    text_elem_count: usize,
    bubble_map: ?[*]const f32,
    bubble_elem_count: usize,
    cfg: *const DetCfg,
    work_buf: [*]u8,
    work_cap: usize,
    out_lines: [*]DetLine,
    line_cap: usize,
    out_bubbles: [*]DetBubble,
    bubble_cap: usize,
    out_summary: *DetSummary,
) callconv(.c) u16;

pub extern fn ra8_translate_ctc_charset_open(
    tchr_buf: [*]const u8,
    tchr_len: usize,
    out_header: *CtcCharsetHeader,
) callconv(.c) u16;

pub extern fn ra8_translate_ctc_decode_i8(
    tchr_buf: [*]const u8,
    tchr_len: usize,
    logits_i8: [*]const i8,
    logit_elem_count: usize,
    quant_scale: f32,
    zero_point: i8,
    cfg: *const CtcDecodeCfg,
    out_utf8: [*]u8,
    out_cap: usize,
    out_result: *CtcResult,
) callconv(.c) u16;

pub extern fn ra8_translate_ctc_decode_f32(
    tchr_buf: [*]const u8,
    tchr_len: usize,
    logits_f32: [*]const f32,
    logit_elem_count: usize,
    cfg: *const CtcDecodeCfg,
    out_utf8: [*]u8,
    out_cap: usize,
    out_result: *CtcResult,
) callconv(.c) u16;

pub extern fn ra8_translate_tok_open(
    tvoc_buf: [*]const u8,
    tvoc_len: usize,
    out_header: *VocabHeader,
) callconv(.c) u16;

pub extern fn ra8_translate_tok_work_bytes(
    text_len: usize,
    model_type: u8,
    out_work_bytes: *usize,
) callconv(.c) u16;

pub extern fn ra8_translate_tok_encode(
    tvoc_buf: [*]const u8,
    tvoc_len: usize,
    text_utf8: ?[*]const u8,
    text_len: usize,
    enc_flags: u8,
    work_buf: [*]u8,
    work_cap: usize,
    out_ids: [*]u32,
    id_cap: usize,
    out_id_count: *usize,
) callconv(.c) u16;

pub extern fn ra8_translate_tok_decode(
    tvoc_buf: [*]const u8,
    tvoc_len: usize,
    ids: ?[*]const u32,
    id_count: usize,
    out_utf8: [*]u8,
    out_cap: usize,
    out_len: *usize,
) callconv(.c) u16;

pub extern fn ra8_translate_marian_open(
    tmnt_buf: [*]const u8,
    tmnt_len: usize,
    open_flags: u8,
    out_header: *MarianHeader,
) callconv(.c) u16;

pub extern fn ra8_translate_marian_work_bytes(
    tmnt_buf: [*]const u8,
    tmnt_len: usize,
    max_src_len: u16,
    max_tgt_len: u16,
    out_work_bytes: *usize,
) callconv(.c) u16;

pub extern fn ra8_translate_marian_greedy_decode(
    tmnt_buf: [*]const u8,
    tmnt_len: usize,
    src_ids: [*]const u32,
    src_len: usize,
    max_tgt_len: u16,
    work_buf: [*]u8,
    work_cap: usize,
    out_tgt_ids: [*]u32,
    tgt_cap: usize,
    out_stats: *MarianStats,
) callconv(.c) u16;
