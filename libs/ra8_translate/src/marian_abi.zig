//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C23 ABI adapter exports for freestanding int8 Marian NMT inference (`TMNT`).

const std = @import("std");
const marian = @import("marian");
const marian_batch = @import("marian_batch");
const err_mod = @import("err");

pub const MarianHeader = marian.MarianHeader;
pub const MarianStats = marian.MarianStats;
pub const MarianBatchCfg = marian_batch.MarianBatchCfg;
pub const MarianBatchJob = marian_batch.MarianBatchJob;
pub const MarianBatchStats = marian_batch.MarianBatchStats;

comptime {
    const assert = std.debug.assert;
    assert(@sizeOf(MarianHeader) == 64);
    assert(@alignOf(MarianHeader) == 4);
    assert(@offsetOf(MarianHeader, "total_bytes") == 0);
    assert(@offsetOf(MarianHeader, "meta_crc32") == 4);
    assert(@offsetOf(MarianHeader, "payload_crc32") == 8);
    assert(@offsetOf(MarianHeader, "vocab_size") == 12);
    assert(@offsetOf(MarianHeader, "unk_id") == 16);
    assert(@offsetOf(MarianHeader, "bos_id") == 20);
    assert(@offsetOf(MarianHeader, "eos_id") == 24);
    assert(@offsetOf(MarianHeader, "pad_id") == 28);
    assert(@offsetOf(MarianHeader, "shortlist_off") == 32);
    assert(@offsetOf(MarianHeader, "shortlist_bytes") == 36);
    assert(@offsetOf(MarianHeader, "ln_eps") == 40);
    assert(@offsetOf(MarianHeader, "version") == 44);
    assert(@offsetOf(MarianHeader, "model_flags") == 46);
    assert(@offsetOf(MarianHeader, "d_model") == 48);
    assert(@offsetOf(MarianHeader, "d_ff") == 50);
    assert(@offsetOf(MarianHeader, "max_seq") == 52);
    assert(@offsetOf(MarianHeader, "nmt_model") == 54);
    assert(@offsetOf(MarianHeader, "num_heads") == 55);
    assert(@offsetOf(MarianHeader, "num_enc_layers") == 56);
    assert(@offsetOf(MarianHeader, "num_dec_layers") == 57);
    assert(@offsetOf(MarianHeader, "reserved") == 58);

    assert(@sizeOf(MarianStats) == 16);
    assert(@alignOf(MarianStats) == 8);
    assert(@offsetOf(MarianStats, "weight_bytes_read") == 0);
    assert(@offsetOf(MarianStats, "generated_tokens") == 8);
    assert(@offsetOf(MarianStats, "stopped_on_eos") == 12);
    assert(@offsetOf(MarianStats, "used_helium_mve") == 13);
    assert(@offsetOf(MarianStats, "reserved") == 14);

    assert(@sizeOf(MarianBatchCfg) == 12);
    assert(@alignOf(MarianBatchCfg) == 2);
    assert(@offsetOf(MarianBatchCfg, "max_src_len") == 0);
    assert(@offsetOf(MarianBatchCfg, "max_tgt_len") == 2);
    assert(@offsetOf(MarianBatchCfg, "flags") == 4);
    assert(@offsetOf(MarianBatchCfg, "batch_size") == 6);
    assert(@offsetOf(MarianBatchCfg, "beam_width") == 7);
    assert(@offsetOf(MarianBatchCfg, "reserved") == 8);

    assert(@sizeOf(MarianBatchJob) == 24);
    assert(@alignOf(MarianBatchJob) == 4);
    assert(@offsetOf(MarianBatchJob, "src_off") == 0);
    assert(@offsetOf(MarianBatchJob, "src_len") == 4);
    assert(@offsetOf(MarianBatchJob, "tgt_off") == 8);
    assert(@offsetOf(MarianBatchJob, "tgt_cap") == 12);
    assert(@offsetOf(MarianBatchJob, "out_len") == 16);
    assert(@offsetOf(MarianBatchJob, "stopped_on_eos") == 20);
    assert(@offsetOf(MarianBatchJob, "completed") == 21);
    assert(@offsetOf(MarianBatchJob, "reserved") == 22);

    assert(@sizeOf(MarianBatchStats) == 24);
    assert(@alignOf(MarianBatchStats) == 8);
    assert(@offsetOf(MarianBatchStats, "total_weight_bytes_read") == 0);
    assert(@offsetOf(MarianBatchStats, "completed_jobs") == 8);
    assert(@offsetOf(MarianBatchStats, "decoder_steps") == 12);
    assert(@offsetOf(MarianBatchStats, "total_generated_tokens") == 16);
    assert(@offsetOf(MarianBatchStats, "used_helium_mve") == 20);
    assert(@offsetOf(MarianBatchStats, "reserved") == 21);
}

/// C ABI export for `ra8_translate_marian_open()`.
pub export fn ra8_translate_marian_open(
    tmnt_buf_opt: ?[*]const u8,
    tmnt_len: usize,
    open_flags: u8,
    out_header_opt: ?*MarianHeader,
) callconv(.c) u16 {
    const tmnt_ptr = tmnt_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_hdr = out_header_opt orelse return err_mod.k_ra8_err_null_ptr;

    const view = marian.TmntView.open(tmnt_ptr[0..tmnt_len], open_flags) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_hdr.* = view.header;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_marian_work_bytes()`.
pub export fn ra8_translate_marian_work_bytes(
    tmnt_buf_opt: ?[*]const u8,
    tmnt_len: usize,
    max_src_len: u16,
    max_tgt_len: u16,
    out_work_bytes_opt: ?*usize,
) callconv(.c) u16 {
    const tmnt_ptr = tmnt_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_bytes = out_work_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;

    const view = marian.TmntView.open(tmnt_ptr[0..tmnt_len], 0) catch |e| {
        return err_mod.toRa8Err(e);
    };
    const needed = marian.minWorkBytes(view, max_src_len, max_tgt_len) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_bytes.* = needed;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_marian_step_logits()`.
pub export fn ra8_translate_marian_step_logits(
    tmnt_buf_opt: ?[*]const u8,
    tmnt_len: usize,
    src_ids_opt: ?[*]const u32,
    src_len: usize,
    prefix_ids_opt: ?[*]const u32,
    prefix_len: usize,
    work_buf_opt: ?[*]u8,
    work_cap: usize,
    out_logits_opt: ?[*]f32,
    logits_cap: usize,
) callconv(.c) u16 {
    const tmnt_ptr = tmnt_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src_ptr = src_ids_opt orelse return err_mod.k_ra8_err_null_ptr;
    const prefix_ptr = prefix_ids_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_ptr = work_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_ptr = out_logits_opt orelse return err_mod.k_ra8_err_null_ptr;

    const view = marian.TmntView.open(tmnt_ptr[0..tmnt_len], 0) catch |e| {
        return err_mod.toRa8Err(e);
    };
    marian.evalStepLogits(
        view,
        src_ptr[0..src_len],
        prefix_ptr[0..prefix_len],
        work_ptr[0..work_cap],
        out_ptr[0..logits_cap],
    ) catch |e| {
        return err_mod.toRa8Err(e);
    };
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_marian_greedy_decode()`.
pub export fn ra8_translate_marian_greedy_decode(
    tmnt_buf_opt: ?[*]const u8,
    tmnt_len: usize,
    src_ids_opt: ?[*]const u32,
    src_len: usize,
    max_tgt_len: u16,
    work_buf_opt: ?[*]u8,
    work_cap: usize,
    out_tgt_ids_opt: ?[*]u32,
    tgt_cap: usize,
    out_stats_opt: ?*MarianStats,
) callconv(.c) u16 {
    const tmnt_ptr = tmnt_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src_ptr = src_ids_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_ptr = work_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_tgt_ptr = out_tgt_ids_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_stats = out_stats_opt orelse return err_mod.k_ra8_err_null_ptr;

    const view = marian.TmntView.open(tmnt_ptr[0..tmnt_len], 0) catch |e| {
        return err_mod.toRa8Err(e);
    };
    const stats = marian.greedyDecode(
        view,
        src_ptr[0..src_len],
        max_tgt_len,
        work_ptr[0..work_cap],
        out_tgt_ptr[0..tgt_cap],
    ) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_stats.* = stats;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_marian_batch_work_bytes()`.
pub export fn ra8_translate_marian_batch_work_bytes(
    tmnt_buf_opt: ?[*]const u8,
    tmnt_len: usize,
    cfg_opt: ?*const MarianBatchCfg,
    out_work_bytes_opt: ?*usize,
) callconv(.c) u16 {
    const tmnt_ptr = tmnt_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_bytes = out_work_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;

    const view = marian_batch.TmntView.open(tmnt_ptr[0..tmnt_len], 0) catch |e| {
        return err_mod.toRa8Err(e);
    };
    const needed = marian_batch.minBatchWorkBytes(view, cfg.*) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_bytes.* = needed;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_marian_batch_decode()`.
pub export fn ra8_translate_marian_batch_decode(
    tmnt_buf_opt: ?[*]const u8,
    tmnt_len: usize,
    cfg_opt: ?*const MarianBatchCfg,
    src_pool_opt: ?[*]const u32,
    src_pool_len: usize,
    jobs_opt: ?[*]MarianBatchJob,
    job_count: usize,
    work_buf_opt: ?[*]u8,
    work_cap: usize,
    out_tgt_pool_opt: ?[*]u32,
    tgt_pool_cap: usize,
    out_step_bytes_opt: ?[*]u64,
    step_bytes_cap: usize,
    out_stats_opt: ?*MarianBatchStats,
) callconv(.c) u16 {
    const tmnt_ptr = tmnt_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const src_ptr = src_pool_opt orelse return err_mod.k_ra8_err_null_ptr;
    const jobs_ptr = jobs_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_ptr = work_buf_opt orelse return err_mod.k_ra8_err_null_ptr;
    const tgt_ptr = out_tgt_pool_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_stats = out_stats_opt orelse return err_mod.k_ra8_err_null_ptr;

    const step_slice: ?[]u64 = if (out_step_bytes_opt) |sptr| blk: {
        if (step_bytes_cap == 0) return err_mod.k_ra8_err_invalid_size;
        break :blk sptr[0..step_bytes_cap];
    } else blk: {
        if (step_bytes_cap != 0) return err_mod.k_ra8_err_null_ptr;
        break :blk null;
    };

    const view = marian_batch.TmntView.open(tmnt_ptr[0..tmnt_len], 0) catch |e| {
        return err_mod.toRa8Err(e);
    };
    const stats = marian_batch.batchDecode(
        view,
        cfg.*,
        src_ptr[0..src_pool_len],
        jobs_ptr[0..job_count],
        work_ptr[0..work_cap],
        tgt_ptr[0..tgt_pool_cap],
        step_slice,
    ) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_stats.* = stats;
    return err_mod.k_ra8_ok;
}
