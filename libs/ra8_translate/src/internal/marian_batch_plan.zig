//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI data structures, workspace planning, and transactional validation for
//! batched Marian NMT inference (`RA8FW-674`).

const std = @import("std");
const cont = @import("marian_container");
const err_mod = @import("err");

pub const Error = err_mod.TranslateError;
pub const TmntView = cont.TmntView;
pub const ShortlistView = cont.ShortlistView;
pub const MarianHeader = cont.MarianHeader;

pub const max_batch: usize = 16;
pub const max_beam: usize = 4;
pub const batch_flag_use_shortlist: u16 = 0x01;
pub const batch_flag_beam_search: u16 = 0x02;
pub const batch_flag_valid_mask: u16 = 0x03;

/// Extern ABI configuration matching `ra8_translate_marian_batch_cfg_t`.
pub const MarianBatchCfg = extern struct {
    max_src_len: u16 = 0,
    max_tgt_len: u16 = 0,
    flags: u16 = 0,
    batch_size: u8 = 1,
    beam_width: u8 = 1,
    reserved: [4]u8 = .{ 0, 0, 0, 0 },
};

/// Extern ABI job descriptor matching `ra8_translate_marian_batch_job_t`.
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

/// Extern ABI summary matching `ra8_translate_marian_batch_stats_t`.
pub const MarianBatchStats = extern struct {
    total_weight_bytes_read: u64 = 0,
    completed_jobs: u32 = 0,
    decoder_steps: u32 = 0,
    total_generated_tokens: u32 = 0,
    used_helium_mve: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// Partitioned aligned workspace plan for batched decoding.
pub const BatchPlan = struct {
    batch_size: usize,
    beam_k: usize,
    max_lanes: usize,
    max_src: usize,
    max_tgt: usize,
    d_model: usize,
    d_ff: usize,
    max_dim: usize,
    num_enc: usize,
    num_dec: usize,
    is_ssru: bool,
    vocab_size: usize,
    use_shortlist: bool,
    max_sl_cap: usize,
    cand_cap: usize,
    enc_seq_elems: usize,
    cross_slot_elems: usize,
    self_lane_elems: usize,
    ssru_lane_elems: usize,
    ssru_scratch_elems: usize,
    total_f32: usize,
    total_u32: usize,
    total_i32: usize,
    total_i8: usize,
    total_u8: usize,
    total_bytes: usize,
};

/// Borrowed typed views over caller-supplied `work_buf`.
pub const BatchMem = struct {
    enc_x: []f32,
    enc_sub: []f32,
    enc_k: []f32,
    enc_v: []f32,
    cross_k: []f32,
    cross_v: []f32,
    self_k: []f32,
    self_v: []f32,
    ssru_cell: []f32,
    ssru_scratch: []f32,
    bat_x: []f32,
    bat_in: []f32,
    bat_q: []f32,
    bat_kv_k: []f32,
    bat_kv_v: []f32,
    bat_ctx: []f32,
    bat_out: []f32,
    bat_ffn: []f32,
    bat_scales: []f32,
    f32_tmp_a: []f32,
    f32_tmp_b: []f32,
    attn_scores: []f32,
    beam_logits: []f32,
    bat_zp_corrs: []i32,
    shortlist_ids: []u32,
    beam_hist: []u32,
    hist_scratch: []u32,
    finished_toks: []u32,
    q_tmp_batch: []i8,
    kv_parent: []u8,
    trace_buf: []u8,
};

/// Active queue-slot execution state for in-flight sequences and beams.
pub const SlotState = struct {
    active: bool = false,
    job_idx: usize = 0,
    src_len: usize = 0,
    tgt_limit: usize = 0,
    dec_pos: usize = 0,
    alive_beams: usize = 0,
    sl_len: usize = 0,
    has_finished: bool = false,
    finished_len: usize = 0,
    best_finished_score: f32 = 0.0,
    cur_toks: [max_beam]u32 = .{ 0, 0, 0, 0 },
    beam_scores: [max_beam]f32 = .{ 0.0, 0.0, 0.0, 0.0 },
    prev_lanes: [max_beam]u8 = .{ 0, 0, 0, 0 },
};

/// Pair identifying one active beam hypothesis within one active batch slot.
pub const LaneRef = struct {
    slot: usize,
    beam: usize,
};

/// Compute required scratch workspace dimensions and total bytes with overflow checks.
pub fn planBatchWork(view: TmntView, cfg: MarianBatchCfg) Error!BatchPlan {
    const hdr = view.header;
    if ((cfg.flags & ~batch_flag_valid_mask) != 0) return Error.InvalidArg;
    if (cfg.max_src_len == 0 or cfg.max_tgt_len == 0) return Error.InvalidSize;
    if (cfg.max_src_len > hdr.max_seq or cfg.max_tgt_len > hdr.max_seq) return Error.OutOfRange;
    if (cfg.batch_size == 0 or cfg.batch_size > max_batch) return Error.OutOfRange;

    const beam_enabled = (cfg.flags & batch_flag_beam_search) != 0;
    const beam_k: usize = if (beam_enabled) blk: {
        if (cfg.beam_width < 2 or cfg.beam_width > max_beam) return Error.OutOfRange;
        break :blk @intCast(cfg.beam_width);
    } else blk: {
        if (cfg.beam_width > 1) return Error.InvalidArg;
        break :blk 1;
    };

    const use_shortlist = (cfg.flags & batch_flag_use_shortlist) != 0;
    const vocab_size: usize = @intCast(hdr.vocab_size);
    var max_sl_cap: usize = 0;
    if (use_shortlist) {
        const sl_view = (try view.shortlist()) orelse return Error.NotFound;
        var max_row: usize = 0;
        var tok: u32 = 0;
        while (tok < hdr.vocab_size) : (tok += 1) {
            const r = try sl_view.rowRange(tok);
            max_row = @max(max_row, @as(usize, r.end - r.start));
        }
        const seed_top = @min(vocab_size, @as(usize, sl_view.first_k));
        const row_bound = try checkedMul(@as(usize, cfg.max_src_len), max_row);
        const bound = try checkedAdd(4, try checkedAdd(seed_top, row_bound));
        max_sl_cap = @min(vocab_size, bound);
    }

    const batch_size: usize = @intCast(cfg.batch_size);
    const max_lanes = try checkedMul(batch_size, beam_k);
    const max_src: usize = @intCast(cfg.max_src_len);
    const max_tgt: usize = @intCast(cfg.max_tgt_len);
    const d_model: usize = @intCast(hdr.d_model);
    const d_ff: usize = @intCast(hdr.d_ff);
    const max_dim = @max(d_model, d_ff);
    const num_enc: usize = @intCast(hdr.num_enc_layers);
    const num_dec: usize = @intCast(hdr.num_dec_layers);
    const is_ssru = (hdr.model_flags & cont.flag_decoder_ssru) != 0;
    const cand_cap: usize = if (beam_k > 1) (if (use_shortlist) max_sl_cap else vocab_size) else 0;

    const enc_seq_elems = try checkedMul(max_src, d_model);
    const cross_slot_elems = try checkedMul(num_dec, enc_seq_elems);
    const self_lane_elems = if (is_ssru) 0 else try checkedMul(num_dec, try checkedMul(max_tgt, d_model));
    const ssru_lane_elems = if (is_ssru) try checkedMul(num_dec, d_model) else 0;
    const ssru_scratch_elems = if (is_ssru and beam_k > 1) try checkedMul(beam_k, ssru_lane_elems) else 0;

    var tf32: usize = 0;
    tf32 = try checkedAdd(tf32, try checkedMul(4, enc_seq_elems));
    tf32 = try checkedAdd(tf32, try checkedMul(2, try checkedMul(batch_size, cross_slot_elems)));
    tf32 = try checkedAdd(tf32, try checkedMul(2, try checkedMul(max_lanes, self_lane_elems)));
    tf32 = try checkedAdd(tf32, try checkedAdd(try checkedMul(max_lanes, ssru_lane_elems), ssru_scratch_elems));
    tf32 = try checkedAdd(tf32, try checkedMul(7, try checkedMul(max_lanes, d_model)));
    tf32 = try checkedAdd(tf32, try checkedMul(max_lanes, d_ff));
    tf32 = try checkedAdd(tf32, try checkedAdd(max_lanes, max_dim));
    tf32 = try checkedAdd(tf32, try checkedAdd(d_model, @max(max_src, max_tgt)));
    tf32 = try checkedAdd(tf32, try checkedMul(max_lanes, cand_cap));

    const tu32 = try checkedAdd(
        try checkedMul(batch_size, max_sl_cap),
        try checkedAdd(
            try checkedMul(max_lanes, max_tgt),
            try checkedAdd(try checkedMul(beam_k, max_tgt), try checkedMul(batch_size, max_tgt)),
        ),
    );
    const ti32 = max_lanes;
    const ti8 = try checkedMul(max_lanes, max_dim);
    const tu8 = try checkedAdd(try checkedMul(max_lanes, max_tgt), max_tgt);

    var total_bytes: usize = 16;
    total_bytes = try checkedAdd(total_bytes, try checkedMul(tf32, 4));
    total_bytes = try checkedAdd(total_bytes, try checkedMul(tu32, 4));
    total_bytes = try checkedAdd(total_bytes, try checkedMul(ti32, 4));
    total_bytes = try checkedAdd(total_bytes, ti8);
    total_bytes = try checkedAdd(total_bytes, tu8);

    return .{
        .batch_size = batch_size,
        .beam_k = beam_k,
        .max_lanes = max_lanes,
        .max_src = max_src,
        .max_tgt = max_tgt,
        .d_model = d_model,
        .d_ff = d_ff,
        .max_dim = max_dim,
        .num_enc = num_enc,
        .num_dec = num_dec,
        .is_ssru = is_ssru,
        .vocab_size = vocab_size,
        .use_shortlist = use_shortlist,
        .max_sl_cap = max_sl_cap,
        .cand_cap = cand_cap,
        .enc_seq_elems = enc_seq_elems,
        .cross_slot_elems = cross_slot_elems,
        .self_lane_elems = self_lane_elems,
        .ssru_lane_elems = ssru_lane_elems,
        .ssru_scratch_elems = ssru_scratch_elems,
        .total_f32 = tf32,
        .total_u32 = tu32,
        .total_i32 = ti32,
        .total_i8 = ti8,
        .total_u8 = tu8,
        .total_bytes = total_bytes,
    };
}

/// Bind aligned scratch sub-slices inside caller-owned `work_buf`.
pub fn bindBatchMem(work_buf: []u8, plan: BatchPlan) Error!BatchMem {
    if (work_buf.len < plan.total_bytes) return Error.InvalidSize;
    const base_addr = @intFromPtr(work_buf.ptr);
    const aligned_addr = (base_addr + 3) & ~@as(usize, 3);
    const skip = aligned_addr - base_addr;
    const words32 = plan.total_f32 + plan.total_u32 + plan.total_i32;
    const needed_end = skip + words32 * 4 + plan.total_i8 + plan.total_u8;
    if (work_buf.len < needed_end) return Error.InvalidSize;

    const f_base: [*]f32 = @ptrCast(@alignCast(work_buf.ptr + skip));
    const f_pool = f_base[0..plan.total_f32];
    const u32_off = skip + plan.total_f32 * 4;
    const u_base: [*]u32 = @ptrCast(@alignCast(work_buf.ptr + u32_off));
    const u_pool = u_base[0..plan.total_u32];
    const i32_off = u32_off + plan.total_u32 * 4;
    const i_base: [*]i32 = @ptrCast(@alignCast(work_buf.ptr + i32_off));
    const i_pool = i_base[0..plan.total_i32];
    const i8_off = i32_off + plan.total_i32 * 4;
    const i8_ptr: [*]i8 = @ptrCast(work_buf.ptr + i8_off);
    const u8_off = i8_off + plan.total_i8;
    const kv_u8_len = plan.max_lanes * plan.max_tgt;

    var fc: usize = 0;
    var uc: usize = 0;

    return .{
        .enc_x = takeSlice(f32, f_pool, &fc, plan.enc_seq_elems),
        .enc_sub = takeSlice(f32, f_pool, &fc, plan.enc_seq_elems),
        .enc_k = takeSlice(f32, f_pool, &fc, plan.enc_seq_elems),
        .enc_v = takeSlice(f32, f_pool, &fc, plan.enc_seq_elems),
        .cross_k = takeSlice(f32, f_pool, &fc, plan.batch_size * plan.cross_slot_elems),
        .cross_v = takeSlice(f32, f_pool, &fc, plan.batch_size * plan.cross_slot_elems),
        .self_k = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.self_lane_elems),
        .self_v = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.self_lane_elems),
        .ssru_cell = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.ssru_lane_elems),
        .ssru_scratch = takeSlice(f32, f_pool, &fc, plan.ssru_scratch_elems),
        .bat_x = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_in = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_q = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_kv_k = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_kv_v = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_ctx = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_out = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_model),
        .bat_ffn = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.d_ff),
        .bat_scales = takeSlice(f32, f_pool, &fc, plan.max_lanes),
        .f32_tmp_a = takeSlice(f32, f_pool, &fc, plan.max_dim),
        .f32_tmp_b = takeSlice(f32, f_pool, &fc, plan.d_model),
        .attn_scores = takeSlice(f32, f_pool, &fc, @max(plan.max_src, plan.max_tgt)),
        .beam_logits = takeSlice(f32, f_pool, &fc, plan.max_lanes * plan.cand_cap),
        .bat_zp_corrs = i_pool,
        .shortlist_ids = takeSlice(u32, u_pool, &uc, plan.batch_size * plan.max_sl_cap),
        .beam_hist = takeSlice(u32, u_pool, &uc, plan.max_lanes * plan.max_tgt),
        .hist_scratch = takeSlice(u32, u_pool, &uc, plan.beam_k * plan.max_tgt),
        .finished_toks = takeSlice(u32, u_pool, &uc, plan.batch_size * plan.max_tgt),
        .q_tmp_batch = i8_ptr[0..plan.total_i8],
        .kv_parent = work_buf[u8_off .. u8_off + kv_u8_len],
        .trace_buf = work_buf[u8_off + kv_u8_len .. u8_off + plan.total_u8],
    };
}

/// Validate all caller queue descriptors, source IDs, and destination capacities upfront.
pub fn validateJobs(
    hdr: MarianHeader,
    cfg: MarianBatchCfg,
    src_pool: []const u32,
    jobs: []const MarianBatchJob,
    out_tgt_pool: []const u32,
) Error!usize {
    if (jobs.len == 0) return Error.InvalidSize;
    var steps_bound: usize = 0;
    for (jobs) |job| {
        if (job.src_len == 0 or job.tgt_cap == 0) return Error.InvalidSize;
        if (job.src_len > cfg.max_src_len) return Error.OutOfRange;
        const s_end = try checkedAdd(@as(usize, job.src_off), @as(usize, job.src_len));
        if (s_end > src_pool.len) return Error.OutOfRange;
        const t_end = try checkedAdd(@as(usize, job.tgt_off), @as(usize, job.tgt_cap));
        if (t_end > out_tgt_pool.len) return Error.InvalidSize;
        for (src_pool[job.src_off..s_end]) |tok| {
            if (tok >= hdr.vocab_size) return Error.OutOfRange;
        }
        steps_bound = try checkedAdd(steps_bound, @min(@as(usize, cfg.max_tgt_len), @as(usize, job.tgt_cap)));
    }
    return steps_bound;
}

/// Reject mismatched optional final LayerNorm pairs before mutating any output.
pub fn validateOptionalFinalLns(view: TmntView) Error!void {
    const pairs = [2][2]usize{ .{ 4, 5 }, .{ 6, 7 } };
    for (pairs) |pair| {
        const g = try view.tensorAt(pair[0]);
        const b = try view.tensorAt(pair[1]);
        if ((g.dtype == .none) != (b.dtype == .none)) return Error.ValidationFailed;
    }
}

fn takeSlice(comptime T: type, pool: []T, cursor: *usize, n: usize) []T {
    const start = cursor.*;
    cursor.* = start + n;
    return pool[start..cursor.*];
}

fn checkedMul(a: usize, b: usize) Error!usize {
    return std.math.mul(usize, a, b) catch Error.OutOfRange;
}

fn checkedAdd(a: usize, b: usize) Error!usize {
    return std.math.add(usize, a, b) catch Error.OutOfRange;
}
