//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C23 ABI adapter exports for freestanding int8 Marian NMT inference (`TMNT`).

const std = @import("std");
const marian = @import("marian");
const err_mod = @import("err");

pub const MarianHeader = marian.MarianHeader;
pub const MarianStats = marian.MarianStats;

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
