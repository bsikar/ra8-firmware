//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C23 ABI extern bindings for `ra8_translate` tokenizer and Marian NMT APIs.

const std = @import("std");

pub const ra8_ok: u16 = 0;

pub const tok_unigram: u8 = 0;
pub const tok_enc_none: u8 = 0x00;
pub const tok_enc_add_bos: u8 = 0x01;
pub const tok_enc_add_eos: u8 = 0x02;

pub const marian_open_meta_only: u8 = 0;
pub const marian_open_verify_payload: u8 = 1 << 0;
pub const marian_flag_decoder_ssru: u16 = 1 << 3;
pub const marian_flag_zero_bos: u16 = 1 << 4;

pub const marian_batch_flag_none: u16 = 0;
pub const marian_batch_flag_use_shortlist: u16 = 1 << 0;
pub const marian_batch_flag_beam_search: u16 = 1 << 1;

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

pub const MarianBatchCfg = extern struct {
    max_src_len: u16,
    max_tgt_len: u16,
    flags: u16 = marian_batch_flag_none,
    batch_size: u8,
    beam_width: u8 = 1,
    reserved: [4]u8 = .{ 0, 0, 0, 0 },
};

pub const MarianBatchJob = extern struct {
    src_off: u32 = 0,
    src_len: u32 = 0,
    tgt_off: u32 = 0,
    tgt_cap: u32 = 0,
    out_len: u32 = 0,
    stopped_on_eos: u8 = 0,
    completed: u8 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

pub const MarianBatchStats = extern struct {
    total_weight_bytes_read: u64 = 0,
    completed_jobs: u32 = 0,
    decoder_steps: u32 = 0,
    total_generated_tokens: u32 = 0,
    used_helium_mve: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

comptime {
    std.debug.assert(@sizeOf(VocabHeader) == 36);
    std.debug.assert(@sizeOf(MarianHeader) == 64);
    std.debug.assert(@sizeOf(MarianStats) == 16);
    std.debug.assert(@sizeOf(MarianBatchCfg) == 12);
    std.debug.assert(@offsetOf(MarianBatchCfg, "flags") == 4);
    std.debug.assert(@offsetOf(MarianBatchCfg, "beam_width") == 7);
    std.debug.assert(@sizeOf(MarianBatchJob) == 24);
    std.debug.assert(@sizeOf(MarianBatchStats) == 24);
}

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

pub extern fn ra8_translate_marian_step_logits(
    tmnt_buf: [*]const u8,
    tmnt_len: usize,
    src_ids: [*]const u32,
    src_len: usize,
    prefix_ids: [*]const u32,
    prefix_len: usize,
    work_buf: [*]u8,
    work_cap: usize,
    out_logits: [*]f32,
    logits_cap: usize,
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

pub extern fn ra8_translate_marian_batch_work_bytes(
    tmnt_buf: [*]const u8,
    tmnt_len: usize,
    cfg: *const MarianBatchCfg,
    out_work_bytes: *usize,
) callconv(.c) u16;

pub extern fn ra8_translate_marian_batch_decode(
    tmnt_buf: [*]const u8,
    tmnt_len: usize,
    cfg: *const MarianBatchCfg,
    src_pool: [*]const u32,
    src_pool_len: usize,
    jobs: [*]MarianBatchJob,
    job_count: usize,
    work_buf: [*]u8,
    work_cap: usize,
    out_tgt_pool: [*]u32,
    tgt_pool_cap: usize,
    out_step_bytes: ?[*]u64,
    step_bytes_cap: usize,
    out_stats: *MarianBatchStats,
) callconv(.c) u16;
