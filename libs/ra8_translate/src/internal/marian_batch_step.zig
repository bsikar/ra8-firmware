//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Batched Marian NMT execution kernels: encoder cross-KV setup, `TSLT` CSR
//! shortlist extraction, weight-shared decoder step, shortlist/full LM head
//! scoring, and greedy or zero-copy beam progression (`RA8FW-674`).

const std = @import("std");
const ops = @import("marian_ops");
const cont = @import("marian_container");
const bp = @import("marian_batch_plan");

pub const Error = bp.Error;
pub const TmntView = cont.TmntView;
pub const ShortlistView = cont.ShortlistView;
pub const MarianHeader = cont.MarianHeader;
pub const BatchPlan = bp.BatchPlan;
pub const BatchMem = bp.BatchMem;
pub const SlotState = bp.SlotState;
pub const LaneRef = bp.LaneRef;

/// Outcome of advancing one batch slot by a single step.
pub const AdvanceResult = struct {
    done: bool,
    out_len: usize,
    stopped_eos: bool,
};

const TopCand = struct {
    parent: usize = 0,
    tok: u32 = 0,
    score: f32 = 0.0,
};

/// Initialize one idle batch slot by encoding `src_ids` into cross-KV and extracting
/// its optional sorted `TSLT` shortlist.
pub fn initSlot(
    view: TmntView,
    plan: BatchPlan,
    mem: *const BatchMem,
    sl_view_opt: ?ShortlistView,
    slot_idx: usize,
    job_idx: usize,
    src_ids: []const u32,
    tgt_limit: usize,
    slot: *SlotState,
    bytes_read: *u64,
) Error!void {
    try encodeSingleToCross(view, plan, mem, slot_idx, src_ids, bytes_read);
    const sl_len = if (sl_view_opt) |slv|
        try buildSlotShortlist(
            view.header,
            slv,
            src_ids,
            mem.shortlist_ids[slot_idx * plan.max_sl_cap .. (slot_idx + 1) * plan.max_sl_cap],
            bytes_read,
        )
    else
        0;

    slot.* = .{
        .active = true,
        .job_idx = job_idx,
        .src_len = src_ids.len,
        .tgt_limit = tgt_limit,
        .dec_pos = 0,
        .alive_beams = 1,
        .sl_len = sl_len,
        .has_finished = false,
        .finished_len = 0,
        .best_finished_score = 0.0,
    };
    slot.cur_toks[0] = view.header.bos_id;
}

/// Run one batched decoder step across all active `(slot, beam)` `lanes`, streaming
/// each layer weight matrix once across every active vector.
pub fn runBatchDecoderStep(
    view: TmntView,
    plan: BatchPlan,
    mem: *const BatchMem,
    slots: *[bp.max_batch]SlotState,
    lanes: []const LaneRef,
    bytes_read: *u64,
) Error!void {
    const hdr = view.header;
    const d = plan.d_model;
    const d_ff = plan.d_ff;
    const count = lanes.len;
    const pre_norm = (hdr.model_flags & cont.flag_pre_norm) != 0;
    const n_heads: usize = @intCast(hdr.num_heads);
    const dec_start = cont.global_tensor_count + plan.num_enc * cont.enc_layer_tensor_count;

    for (lanes, 0..) |lr, m| {
        const sl = slots[lr.slot];
        try embedInto(view, sl.cur_toks[lr.beam], sl.dec_pos, mem.bat_x[m * d ..][0..d], mem.f32_tmp_a[0..d], bytes_read);
        const g_lane = lr.slot * plan.beam_k + lr.beam;
        mem.kv_parent[g_lane * plan.max_tgt + sl.dec_pos] = sl.prev_lanes[lr.beam];
    }

    var dl: usize = 0;
    while (dl < plan.num_dec) : (dl += 1) {
        const dec_base = dec_start + dl * cont.dec_layer_tensor_count;
        const self_base = dec_base;
        const cross_base = dec_base + 10;
        const ffn_base = dec_base + 20;

        // 1. Causal self-attention sublayer.
        try prepBatch(view, mem.bat_x[0 .. count * d], mem.bat_in[0 .. count * d], count, d, pre_norm, self_base + 8, self_base + 9, mem, bytes_read);
        try projBatch(view, self_base + 0, self_base + 1, mem.bat_in[0 .. count * d], count, d, d, mem.bat_q[0 .. count * d], mem, bytes_read);
        try projBatch(view, self_base + 2, self_base + 3, mem.bat_in[0 .. count * d], count, d, d, mem.bat_kv_k[0 .. count * d], mem, bytes_read);
        try projBatch(view, self_base + 4, self_base + 5, mem.bat_in[0 .. count * d], count, d, d, mem.bat_kv_v[0 .. count * d], mem, bytes_read);

        for (lanes, 0..) |lr, m| {
            const g_lane = lr.slot * plan.beam_k + lr.beam;
            const pos = slots[lr.slot].dec_pos;
            const dst_off = g_lane * plan.self_lane_elems + dl * plan.max_tgt * d + pos * d;
            @memcpy(mem.self_k[dst_off .. dst_off + d], mem.bat_kv_k[m * d ..][0..d]);
            @memcpy(mem.self_v[dst_off .. dst_off + d], mem.bat_kv_v[m * d ..][0..d]);
            attendTracedSelf(plan, mem, lr, pos, dl, n_heads, mem.bat_q[m * d ..][0..d], mem.bat_ctx[m * d ..][0..d]);
        }

        try projBatch(view, self_base + 6, self_base + 7, mem.bat_ctx[0 .. count * d], count, d, d, mem.bat_out[0 .. count * d], mem, bytes_read);
        addVec(mem.bat_x[0 .. count * d], mem.bat_out[0 .. count * d]);
        if (!pre_norm) try applyBatchLn(view, self_base + 8, self_base + 9, mem.bat_x[0 .. count * d], count, d, mem, bytes_read);

        // 2. Encoder-decoder cross-attention sublayer.
        try prepBatch(view, mem.bat_x[0 .. count * d], mem.bat_in[0 .. count * d], count, d, pre_norm, cross_base + 8, cross_base + 9, mem, bytes_read);
        try projBatch(view, cross_base + 0, cross_base + 1, mem.bat_in[0 .. count * d], count, d, d, mem.bat_q[0 .. count * d], mem, bytes_read);
        for (lanes, 0..) |lr, m| {
            const src_len = slots[lr.slot].src_len;
            const coff = lr.slot * plan.cross_slot_elems + dl * plan.enc_seq_elems;
            attendSeq(mem.bat_q[m * d ..][0..d], mem.cross_k[coff .. coff + src_len * d], mem.cross_v[coff .. coff + src_len * d], src_len, n_heads, d, mem.attn_scores[0..src_len], mem.bat_ctx[m * d ..][0..d]);
        }
        try projBatch(view, cross_base + 6, cross_base + 7, mem.bat_ctx[0 .. count * d], count, d, d, mem.bat_out[0 .. count * d], mem, bytes_read);
        addVec(mem.bat_x[0 .. count * d], mem.bat_out[0 .. count * d]);
        if (!pre_norm) try applyBatchLn(view, cross_base + 8, cross_base + 9, mem.bat_x[0 .. count * d], count, d, mem, bytes_read);

        // 3. Position-wise feed-forward sublayer.
        try prepBatch(view, mem.bat_x[0 .. count * d], mem.bat_in[0 .. count * d], count, d, pre_norm, ffn_base + 4, ffn_base + 5, mem, bytes_read);
        try projBatch(view, ffn_base + 0, ffn_base + 1, mem.bat_in[0 .. count * d], count, d_ff, d, mem.bat_ffn[0 .. count * d_ff], mem, bytes_read);
        ops.reluInPlace(mem.bat_ffn[0 .. count * d_ff]);
        try projBatch(view, ffn_base + 2, ffn_base + 3, mem.bat_ffn[0 .. count * d_ff], count, d, d_ff, mem.bat_out[0 .. count * d], mem, bytes_read);
        addVec(mem.bat_x[0 .. count * d], mem.bat_out[0 .. count * d]);
        if (!pre_norm) try applyBatchLn(view, ffn_base + 4, ffn_base + 5, mem.bat_x[0 .. count * d], count, d, mem, bytes_read);
    }

    const g6 = try view.tensorAt(6);
    if (g6.dtype == .f32) {
        try applyBatchLn(view, 6, 7, mem.bat_x[0 .. count * d], count, d, mem, bytes_read);
    }
}

/// Score full vocabulary rows streaming `lm_head` once across all active lanes.
pub fn scoreLmHeadFull(
    view: TmntView,
    plan: BatchPlan,
    mem: *const BatchMem,
    lanes: []const LaneRef,
    best_toks: *[bp.max_batch * bp.max_beam]u32,
    bytes_read: *u64,
) Error!void {
    const w_ref = try view.tensorAt(2);
    const w_i8 = view.i8Slice(w_ref);
    const b_ref = try view.tensorAt(3);
    const has_bias = b_ref.dtype == .f32;
    const cols = plan.d_model;
    const rows = plan.vocab_size;
    const count = lanes.len;

    quantizeLanes(w_ref, mem, count, cols);

    var best_scores: [bp.max_batch * bp.max_beam]f32 = @splat(0.0);
    var r: usize = 0;
    while (r < rows) : (r += 1) {
        const row_w = w_i8[r * cols .. (r + 1) * cols];
        const b_val: f32 = if (has_bias) view.f32At(b_ref, r) else 0.0;
        var m: usize = 0;
        while (m < count) : (m += 1) {
            const dot = ops.dotI8(mem.q_tmp_batch[m * cols ..][0..cols], row_w);
            const score = b_val + mem.bat_scales[m] * @as(f32, @floatFromInt(dot -% mem.bat_zp_corrs[m]));
            if (plan.beam_k > 1) {
                mem.beam_logits[m * plan.cand_cap + r] = score;
            } else if (r == 0 or score > best_scores[m]) {
                best_scores[m] = score;
                best_toks[m] = @intCast(r);
            }
        }
    }
    bytes_read.* += @as(u64, rows) * @as(u64, cols);
    if (has_bias) bytes_read.* += @as(u64, rows) * @sizeOf(f32);
}

/// Score only candidate token rows in active slots' union `TSLT` shortlist, reading each
/// candidate weight row once per decoder step.
pub fn scoreLmHeadShortlist(
    view: TmntView,
    plan: BatchPlan,
    mem: *const BatchMem,
    slots: *const [bp.max_batch]SlotState,
    lanes: []const LaneRef,
    best_toks: *[bp.max_batch * bp.max_beam]u32,
    cand_counts: *[bp.max_batch]usize,
    bytes_read: *u64,
) Error!void {
    const w_ref = try view.tensorAt(2);
    const w_i8 = view.i8Slice(w_ref);
    const b_ref = try view.tensorAt(3);
    const has_bias = b_ref.dtype == .f32;
    const cols = plan.d_model;
    const count = lanes.len;

    quantizeLanes(w_ref, mem, count, cols);

    var sl_cursors: [bp.max_batch]usize = @splat(0);
    var best_scores: [bp.max_batch * bp.max_beam]f32 = @splat(-std.math.inf(f32));
    var slot_cand_idx: [bp.max_batch]usize = @splat(0);

    while (true) {
        var have_min = false;
        var min_tok: u32 = 0;
        var s: usize = 0;
        while (s < plan.batch_size) : (s += 1) {
            if (!slots[s].active) continue;
            if (sl_cursors[s] < slots[s].sl_len) {
                const tok = mem.shortlist_ids[s * plan.max_sl_cap + sl_cursors[s]];
                if (!have_min or tok < min_tok) {
                    have_min = true;
                    min_tok = tok;
                }
            }
        }
        if (!have_min) break;

        const r: usize = @intCast(min_tok);
        const row_w = w_i8[r * cols .. (r + 1) * cols];
        const b_val: f32 = if (has_bias) view.f32At(b_ref, r) else 0.0;
        bytes_read.* += @as(u64, cols) + (if (has_bias) @as(u64, @sizeOf(f32)) else 0);

        for (lanes, 0..) |lr, m| {
            const sl = slots[lr.slot];
            if (sl_cursors[lr.slot] >= sl.sl_len) continue;
            if (mem.shortlist_ids[lr.slot * plan.max_sl_cap + sl_cursors[lr.slot]] != min_tok) continue;
            const dot = ops.dotI8(mem.q_tmp_batch[m * cols ..][0..cols], row_w);
            const score = b_val + mem.bat_scales[m] * @as(f32, @floatFromInt(dot -% mem.bat_zp_corrs[m]));
            if (plan.beam_k > 1) {
                mem.beam_logits[m * plan.cand_cap + slot_cand_idx[lr.slot]] = score;
            } else if (score > best_scores[m]) {
                best_scores[m] = score;
                best_toks[m] = min_tok;
            }
        }
        s = 0;
        while (s < plan.batch_size) : (s += 1) {
            if (slots[s].active and sl_cursors[s] < slots[s].sl_len) {
                if (mem.shortlist_ids[s * plan.max_sl_cap + sl_cursors[s]] == min_tok) {
                    sl_cursors[s] += 1;
                    cand_counts[s] = slot_cand_idx[s] + 1;
                    slot_cand_idx[s] += 1;
                }
            }
        }
    }
}

/// Advance greedy (`beam_k == 1`) slot state by one token and detect EOS/limit stop.
pub fn advanceGreedySlot(
    hdr: MarianHeader,
    plan: BatchPlan,
    mem: *const BatchMem,
    slot_idx: usize,
    slot: *SlotState,
    next_tok: u32,
) AdvanceResult {
    mem.beam_hist[slotHistOff(plan, slot_idx, 0) + slot.dec_pos] = next_tok;
    const len = slot.dec_pos + 1;
    if (next_tok == hdr.eos_id) {
        return .{ .done = true, .out_len = len, .stopped_eos = true };
    }
    if (len >= slot.tgt_limit) {
        return .{ .done = true, .out_len = len, .stopped_eos = false };
    }
    slot.dec_pos = len;
    slot.cur_toks[0] = next_tok;
    slot.prev_lanes[0] = 0;
    return .{ .done = false, .out_len = 0, .stopped_eos = false };
}

/// Advance small-beam (`beam_k` in `2..4`) slot hypotheses using pure IEEE log-softmax.
pub fn advanceBeamSlot(
    hdr: MarianHeader,
    plan: BatchPlan,
    mem: *const BatchMem,
    slot_idx: usize,
    slot: *SlotState,
    lane_base: usize,
    cand_count: usize,
) AdvanceResult {
    const pos = slot.dec_pos;
    const len = pos + 1;
    var best_alive: [bp.max_beam]TopCand = @splat(.{});
    var alive_n: usize = 0;

    var b: usize = 0;
    while (b < slot.alive_beams) : (b += 1) {
        const logits = mem.beam_logits[(lane_base + b) * plan.cand_cap ..][0..cand_count];
        var mx: f32 = logits[0];
        for (logits[1..]) |v| {
            if (v > mx) mx = v;
        }
        var sum_e: f64 = 0.0;
        for (logits) |v| {
            sum_e += @as(f64, ops.expNegF32(v - mx));
        }
        const log_norm: f32 = mx + ops.lnF32(@floatCast(sum_e));

        var ci: usize = 0;
        while (ci < cand_count) : (ci += 1) {
            const tok: u32 = if (plan.use_shortlist)
                mem.shortlist_ids[slot_idx * plan.max_sl_cap + ci]
            else
                @intCast(ci);
            const score = slot.beam_scores[b] + (logits[ci] - log_norm);
            if (tok == hdr.eos_id) {
                if (!slot.has_finished or score > slot.best_finished_score) {
                    slot.has_finished = true;
                    slot.best_finished_score = score;
                    slot.finished_len = len;
                    const dst = mem.finished_toks[slot_idx * plan.max_tgt ..];
                    const hsrc = mem.beam_hist[slotHistOff(plan, slot_idx, b)..];
                    @memcpy(dst[0..pos], hsrc[0..pos]);
                    dst[pos] = hdr.eos_id;
                }
                continue;
            }
            insertTopCand(&best_alive, &alive_n, plan.beam_k, .{ .parent = b, .tok = tok, .score = score });
        }
    }

    if (len >= slot.tgt_limit or (slot.has_finished and (alive_n == 0 or best_alive[0].score <= slot.best_finished_score))) {
        if (slot.has_finished) {
            return .{ .done = true, .out_len = slot.finished_len, .stopped_eos = true };
        }
        if (alive_n > 0) {
            const dst = mem.finished_toks[slot_idx * plan.max_tgt ..];
            const hsrc = mem.beam_hist[slotHistOff(plan, slot_idx, best_alive[0].parent)..];
            @memcpy(dst[0..pos], hsrc[0..pos]);
            dst[pos] = best_alive[0].tok;
            return .{ .done = true, .out_len = len, .stopped_eos = false };
        }
        return .{ .done = true, .out_len = len, .stopped_eos = true };
    }

    var nb: usize = 0;
    while (nb < alive_n) : (nb += 1) {
        const cand = best_alive[nb];
        const tmp_row = mem.hist_scratch[nb * plan.max_tgt ..];
        const parent_row = mem.beam_hist[slotHistOff(plan, slot_idx, cand.parent)..];
        @memcpy(tmp_row[0..pos], parent_row[0..pos]);
        tmp_row[pos] = cand.tok;
    }
    nb = 0;
    while (nb < alive_n) : (nb += 1) {
        const dst = mem.beam_hist[slotHistOff(plan, slot_idx, nb)..];
        const tmp_row = mem.hist_scratch[nb * plan.max_tgt ..];
        @memcpy(dst[0..len], tmp_row[0..len]);
        slot.cur_toks[nb] = best_alive[nb].tok;
        slot.beam_scores[nb] = best_alive[nb].score;
        slot.prev_lanes[nb] = @intCast(best_alive[nb].parent);
    }
    slot.alive_beams = alive_n;
    slot.dec_pos = len;
    return .{ .done = false, .out_len = 0, .stopped_eos = false };
}

/// Flattened token history offset for `(slot_idx, beam)`.
pub fn slotHistOff(plan: BatchPlan, slot_idx: usize, beam: usize) usize {
    return (slot_idx * plan.beam_k + beam) * plan.max_tgt;
}

fn buildSlotShortlist(
    hdr: MarianHeader,
    sl_view: ShortlistView,
    src_ids: []const u32,
    out_ids: []u32,
    bytes_read: *u64,
) Error!usize {
    var count: usize = 0;
    const specials = [4]u32{ hdr.unk_id, hdr.bos_id, hdr.eos_id, hdr.pad_id };
    for (specials) |sp| appendUnique(&count, out_ids, sp);
    for (src_ids) |src_tok| {
        const range = try sl_view.rowRange(src_tok);
        bytes_read.* += @as(u64, range.end - range.start) * @sizeOf(u32);
        var idx = range.start;
        while (idx < range.end) : (idx += 1) {
            appendUnique(&count, out_ids, sl_view.entryAt(idx));
        }
    }
    std.sort.pdq(u32, out_ids[0..count], {}, std.sort.asc(u32));
    return count;
}

fn appendUnique(count: *usize, out_ids: []u32, tok: u32) void {
    for (out_ids[0..count.*]) |existing| {
        if (existing == tok) return;
    }
    if (count.* < out_ids.len) {
        out_ids[count.*] = tok;
        count.* += 1;
    }
}

fn encodeSingleToCross(
    view: TmntView,
    plan: BatchPlan,
    mem: *const BatchMem,
    slot_idx: usize,
    src_ids: []const u32,
    bytes_read: *u64,
) Error!void {
    const hdr = view.header;
    const d = plan.d_model;
    const d_ff = plan.d_ff;
    const src_len = src_ids.len;
    const pre_norm = (hdr.model_flags & cont.flag_pre_norm) != 0;
    const n_heads: usize = @intCast(hdr.num_heads);

    for (src_ids, 0..) |tok, s| {
        try embedInto(view, tok, s, mem.enc_x[s * d .. (s + 1) * d], mem.f32_tmp_a[0..d], bytes_read);
    }

    var el: usize = 0;
    while (el < plan.num_enc) : (el += 1) {
        const base = cont.global_tensor_count + el * cont.enc_layer_tensor_count;
        var s: usize = 0;
        while (s < src_len) : (s += 1) {
            try prepTok(view, mem.enc_x[s * d ..][0..d], mem.bat_in[0..d], pre_norm, base + 8, base + 9, mem, bytes_read);
            try projSingle(view, base + 2, base + 3, mem.bat_in[0..d], mem.enc_k[s * d ..][0..d], mem, bytes_read);
            try projSingle(view, base + 4, base + 5, mem.bat_in[0..d], mem.enc_v[s * d ..][0..d], mem, bytes_read);
        }
        s = 0;
        while (s < src_len) : (s += 1) {
            try prepTok(view, mem.enc_x[s * d ..][0..d], mem.bat_in[0..d], pre_norm, base + 8, base + 9, mem, bytes_read);
            try projSingle(view, base + 0, base + 1, mem.bat_in[0..d], mem.bat_q[0..d], mem, bytes_read);
            attendSeq(mem.bat_q[0..d], mem.enc_k[0 .. src_len * d], mem.enc_v[0 .. src_len * d], src_len, n_heads, d, mem.attn_scores[0..src_len], mem.bat_ctx[0..d]);
            try projSingle(view, base + 6, base + 7, mem.bat_ctx[0..d], mem.enc_sub[s * d ..][0..d], mem, bytes_read);
        }
        const ffn_base = base + 10;
        s = 0;
        while (s < src_len) : (s += 1) {
            const x_tok = mem.enc_x[s * d ..][0..d];
            addVec(x_tok, mem.enc_sub[s * d ..][0..d]);
            if (!pre_norm) try applyLn(view, base + 8, base + 9, x_tok, mem, bytes_read);
            try prepTok(view, x_tok, mem.bat_in[0..d], pre_norm, ffn_base + 4, ffn_base + 5, mem, bytes_read);
            try projSingle(view, ffn_base + 0, ffn_base + 1, mem.bat_in[0..d], mem.bat_ffn[0..d_ff], mem, bytes_read);
            ops.reluInPlace(mem.bat_ffn[0..d_ff]);
            try projSingle(view, ffn_base + 2, ffn_base + 3, mem.bat_ffn[0..d_ff], mem.bat_out[0..d], mem, bytes_read);
            addVec(x_tok, mem.bat_out[0..d]);
            if (!pre_norm) try applyLn(view, ffn_base + 4, ffn_base + 5, x_tok, mem, bytes_read);
        }
    }

    var s: usize = 0;
    while (s < src_len) : (s += 1) {
        try applyOptLn(view, 4, 5, mem.enc_x[s * d ..][0..d], mem, bytes_read);
    }

    const dec_start = cont.global_tensor_count + plan.num_enc * cont.enc_layer_tensor_count;
    const slot_base = slot_idx * plan.cross_slot_elems;
    var dl: usize = 0;
    while (dl < plan.num_dec) : (dl += 1) {
        const cbase = dec_start + dl * cont.dec_layer_tensor_count + 10;
        const doff = slot_base + dl * plan.enc_seq_elems;
        s = 0;
        while (s < src_len) : (s += 1) {
            const enc_tok = mem.enc_x[s * d ..][0..d];
            try projSingle(view, cbase + 2, cbase + 3, enc_tok, mem.cross_k[doff + s * d ..][0..d], mem, bytes_read);
            try projSingle(view, cbase + 4, cbase + 5, enc_tok, mem.cross_v[doff + s * d ..][0..d], mem, bytes_read);
        }
    }
}

fn quantizeLanes(w_ref: cont.TensorRef, mem: *const BatchMem, count: usize, cols: usize) void {
    var m: usize = 0;
    while (m < count) : (m += 1) {
        const q_info = ops.quantizeActSym(mem.bat_x[m * cols ..][0..cols], mem.q_tmp_batch[m * cols ..][0..cols]);
        mem.bat_scales[m] = q_info.scale * w_ref.scale;
        mem.bat_zp_corrs[m] = @as(i32, w_ref.zero_point) *% q_info.sum_q;
    }
}

fn insertTopCand(top: *[bp.max_beam]TopCand, n: *usize, k: usize, item: TopCand) void {
    var pos: usize = n.*;
    if (pos == k and item.score <= top[k - 1].score) return;
    if (pos < k) {
        n.* = pos + 1;
    } else {
        pos = k - 1;
    }
    while (pos > 0 and item.score > top[pos - 1].score) : (pos -= 1) {
        top[pos] = top[pos - 1];
    }
    top[pos] = item;
}

fn attendTracedSelf(
    plan: BatchPlan,
    mem: *const BatchMem,
    lr: LaneRef,
    pos: usize,
    dl: usize,
    n_heads: usize,
    q_vec: []const f32,
    out_ctx: []f32,
) void {
    const trace = mem.trace_buf[0 .. pos + 1];
    var lane_at: u8 = @intCast(lr.beam);
    var t: isize = @intCast(pos);
    while (t >= 0) : (t -= 1) {
        const tu: usize = @intCast(t);
        trace[tu] = lane_at;
        const g_lane = lr.slot * plan.beam_k + @as(usize, lane_at);
        lane_at = mem.kv_parent[g_lane * plan.max_tgt + tu];
    }

    const d = plan.d_model;
    const head_dim = d / n_heads;
    const inv_sqrt_hd = 1.0 / ops.sqrtF32(@floatFromInt(head_dim));
    const kv_len = pos + 1;
    var h: usize = 0;
    while (h < n_heads) : (h += 1) {
        const hoff = h * head_dim;
        const q_h = q_vec[hoff .. hoff + head_dim];
        var step: usize = 0;
        while (step < kv_len) : (step += 1) {
            const g_step = lr.slot * plan.beam_k + @as(usize, trace[step]);
            const k_off = g_step * plan.self_lane_elems + dl * plan.max_tgt * d + step * d + hoff;
            const k_h = mem.self_k[k_off .. k_off + head_dim];
            var dot: f32 = 0.0;
            for (q_h, k_h) |qv, kv| dot += qv * kv;
            mem.attn_scores[step] = dot * inv_sqrt_hd;
        }
        ops.softmaxInPlace(mem.attn_scores[0..kv_len]);
        const out_h = out_ctx[hoff .. hoff + head_dim];
        @memset(out_h, 0.0);
        step = 0;
        while (step < kv_len) : (step += 1) {
            const w = mem.attn_scores[step];
            const g_step = lr.slot * plan.beam_k + @as(usize, trace[step]);
            const v_off = g_step * plan.self_lane_elems + dl * plan.max_tgt * d + step * d + hoff;
            const v_h = mem.self_v[v_off .. v_off + head_dim];
            for (out_h, v_h) |*ov, vv| ov.* += w * vv;
        }
    }
}

fn attendSeq(
    q_vec: []const f32,
    k_seq: []const f32,
    v_seq: []const f32,
    kv_len: usize,
    n_heads: usize,
    d_model: usize,
    scores: []f32,
    out_ctx: []f32,
) void {
    const head_dim = d_model / n_heads;
    const inv_sqrt_hd = 1.0 / ops.sqrtF32(@floatFromInt(head_dim));
    var h: usize = 0;
    while (h < n_heads) : (h += 1) {
        const hoff = h * head_dim;
        const q_h = q_vec[hoff .. hoff + head_dim];
        var t: usize = 0;
        while (t < kv_len) : (t += 1) {
            const k_h = k_seq[t * d_model + hoff ..][0..head_dim];
            var dot: f32 = 0.0;
            for (q_h, k_h) |qv, kv| dot += qv * kv;
            scores[t] = dot * inv_sqrt_hd;
        }
        ops.softmaxInPlace(scores[0..kv_len]);
        const out_h = out_ctx[hoff .. hoff + head_dim];
        @memset(out_h, 0.0);
        t = 0;
        while (t < kv_len) : (t += 1) {
            const w = scores[t];
            const v_h = v_seq[t * d_model + hoff ..][0..head_dim];
            for (out_h, v_h) |*ov, vv| ov.* += w * vv;
        }
    }
}

fn embedInto(view: TmntView, tok: u32, pos: usize, out_vec: []f32, tmp: []f32, bytes_read: *u64) Error!void {
    const d: usize = @intCast(view.header.d_model);
    const emb_ref = try view.tensorAt(0);
    const pos_ref = try view.tensorAt(1);
    const scale_emb = (view.header.model_flags & cont.flag_scale_embed) != 0;
    if (pos_ref.dtype == .f32) {
        var i: usize = 0;
        while (i < d) : (i += 1) tmp[i] = view.f32At(pos_ref, pos * d + i);
        ops.embedToken(tok, 0, view.i8Slice(emb_ref), emb_ref.scale, emb_ref.zero_point, tmp[0..d], d, scale_emb, out_vec, bytes_read);
    } else {
        ops.embedToken(tok, pos, view.i8Slice(emb_ref), emb_ref.scale, emb_ref.zero_point, null, d, scale_emb, out_vec, bytes_read);
    }
}

fn prepTok(view: TmntView, src_x: []const f32, dst: []f32, pre_norm: bool, gs: usize, bs: usize, mem: *const BatchMem, br: *u64) Error!void {
    @memcpy(dst, src_x);
    if (pre_norm) try applyLn(view, gs, bs, dst, mem, br);
}

fn prepBatch(view: TmntView, src_x: []const f32, dst: []f32, count: usize, d: usize, pre_norm: bool, gs: usize, bs: usize, mem: *const BatchMem, br: *u64) Error!void {
    @memcpy(dst, src_x);
    if (pre_norm) try applyBatchLn(view, gs, bs, dst, count, d, mem, br);
}

fn applyLn(view: TmntView, gs: usize, bs: usize, x: []f32, mem: *const BatchMem, br: *u64) Error!void {
    const g = try readF32Req(view, gs, mem.f32_tmp_a[0..x.len]);
    const b = try readF32Req(view, bs, mem.f32_tmp_b[0..x.len]);
    ops.layerNorm(x, g, b, view.header.ln_eps, br);
}

fn applyBatchLn(view: TmntView, gs: usize, bs: usize, xs: []f32, count: usize, d: usize, mem: *const BatchMem, br: *u64) Error!void {
    const g = try readF32Req(view, gs, mem.f32_tmp_a[0..d]);
    const b = try readF32Req(view, bs, mem.f32_tmp_b[0..d]);
    var m: usize = 0;
    var dummy: u64 = 0;
    while (m < count) : (m += 1) {
        ops.layerNorm(xs[m * d .. (m + 1) * d], g, b, view.header.ln_eps, if (m == 0) br else &dummy);
    }
}

fn applyOptLn(view: TmntView, gs: usize, bs: usize, x: []f32, mem: *const BatchMem, br: *u64) Error!void {
    const gref = try view.tensorAt(gs);
    if (gref.dtype == .none) return;
    try applyLn(view, gs, bs, x, mem, br);
}

fn projSingle(view: TmntView, ws: usize, bs: usize, x: []const f32, out: []f32, mem: *const BatchMem, br: *u64) Error!void {
    const wref = try view.tensorAt(ws);
    const bopt = try readF32Opt(view, bs, mem.f32_tmp_a);
    ops.matVecI8(x, view.i8Slice(wref), wref.rows, wref.cols, wref.scale, wref.zero_point, bopt, mem.q_tmp_batch, out, br);
}

fn projBatch(view: TmntView, ws: usize, bs: usize, xs: []const f32, count: usize, rows: usize, cols: usize, out: []f32, mem: *const BatchMem, br: *u64) Error!void {
    const wref = try view.tensorAt(ws);
    const bopt = try readF32Opt(view, bs, mem.f32_tmp_a);
    ops.matVecI8Batch(xs, count, view.i8Slice(wref), rows, cols, wref.scale, wref.zero_point, bopt, mem.q_tmp_batch, mem.bat_scales, mem.bat_zp_corrs, out, br);
}

fn readF32Opt(view: TmntView, slot: usize, dst: []f32) Error!?[]const f32 {
    const ref = try view.tensorAt(slot);
    if (ref.dtype == .none) return null;
    if (ref.dtype != .f32) return Error.ValidationFailed;
    const elems = ref.rows * ref.cols;
    var i: usize = 0;
    while (i < elems) : (i += 1) dst[i] = view.f32At(ref, i);
    return dst[0..elems];
}

fn readF32Req(view: TmntView, slot: usize, dst: []f32) Error![]const f32 {
    return (try readF32Opt(view, slot, dst)) orelse Error.ValidationFailed;
}

fn addVec(dst: []f32, src: []const f32) void {
    for (dst, src) |*d, s| d.* += s;
}
