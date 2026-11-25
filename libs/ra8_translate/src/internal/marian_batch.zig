//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Batched Marian NMT bubble queue orchestrator: shared-weight streaming per
//! decoder step, independent slot completion + chapter queue refill, optional
//! `TSLT` shortlist pruning, optional beam search (`2..4`), and weight-bytes-per-step
//! reporting (`RA8FW-674`).

const ops = @import("marian_ops");
const bp = @import("marian_batch_plan");
const step = @import("marian_batch_step");

pub const Error = bp.Error;
pub const TmntView = bp.TmntView;
pub const MarianBatchCfg = bp.MarianBatchCfg;
pub const MarianBatchJob = bp.MarianBatchJob;
pub const MarianBatchStats = bp.MarianBatchStats;
pub const batch_flag_use_shortlist = bp.batch_flag_use_shortlist;
pub const batch_flag_beam_search = bp.batch_flag_beam_search;

/// Compute required scratch byte capacity for batched Marian NMT queue decoding.
pub fn minBatchWorkBytes(view: TmntView, cfg: MarianBatchCfg) Error!usize {
    const plan = try bp.planBatchWork(view, cfg);
    return plan.total_bytes;
}

/// Drain all queued source bubbles using shared-weight batched decoding with
/// immediate queue refill as sequences finish independently.
pub fn batchDecode(
    view: TmntView,
    cfg: MarianBatchCfg,
    src_pool: []const u32,
    jobs: []MarianBatchJob,
    work_buf: []u8,
    out_tgt_pool: []u32,
    out_step_bytes_opt: ?[]u64,
) Error!MarianBatchStats {
    const plan = try bp.planBatchWork(view, cfg);
    const total_steps_bound = try bp.validateJobs(view.header, cfg, src_pool, jobs, out_tgt_pool);
    if (out_step_bytes_opt) |step_arr| {
        if (step_arr.len < total_steps_bound) return Error.InvalidSize;
    }
    try bp.validateOptionalFinalLns(view);

    const mem = try bp.bindBatchMem(work_buf, plan);
    const sl_view_opt = if (plan.use_shortlist) (try view.shortlist() orelse return Error.NotFound) else null;

    var slots: [bp.max_batch]bp.SlotState = @splat(.{});
    var next_job: usize = 0;
    var completed_jobs: usize = 0;
    var decoder_steps: usize = 0;
    var total_gen_tokens: usize = 0;
    var total_bytes: u64 = 0;

    while (completed_jobs < jobs.len) {
        var s: usize = 0;
        while (s < plan.batch_size and next_job < jobs.len) : (s += 1) {
            if (!slots[s].active) {
                const job = jobs[next_job];
                const s_off: usize = @intCast(job.src_off);
                const s_len: usize = @intCast(job.src_len);
                try step.initSlot(
                    view,
                    plan,
                    &mem,
                    sl_view_opt,
                    s,
                    next_job,
                    src_pool[s_off .. s_off + s_len],
                    @min(plan.max_tgt, @as(usize, @intCast(job.tgt_cap))),
                    &slots[s],
                    &total_bytes,
                );
                next_job += 1;
            }
        }

        var lanes: [bp.max_batch * bp.max_beam]bp.LaneRef = undefined;
        var lane_count: usize = 0;
        s = 0;
        while (s < plan.batch_size) : (s += 1) {
            if (!slots[s].active) continue;
            var b: usize = 0;
            while (b < slots[s].alive_beams) : (b += 1) {
                lanes[lane_count] = .{ .slot = s, .beam = b };
                lane_count += 1;
            }
        }
        if (lane_count == 0) break;

        var step_bytes: u64 = 0;
        try step.runBatchDecoderStep(view, plan, &mem, &slots, lanes[0..lane_count], &step_bytes);

        var best_toks: [bp.max_batch * bp.max_beam]u32 = @splat(0);
        var cand_counts: [bp.max_batch]usize = @splat(0);
        if (sl_view_opt != null) {
            try step.scoreLmHeadShortlist(
                view,
                plan,
                &mem,
                &slots,
                lanes[0..lane_count],
                &best_toks,
                &cand_counts,
                &step_bytes,
            );
        } else {
            try step.scoreLmHeadFull(
                view,
                plan,
                &mem,
                lanes[0..lane_count],
                &best_toks,
                &step_bytes,
            );
            s = 0;
            while (s < plan.batch_size) : (s += 1) {
                cand_counts[s] = plan.vocab_size;
            }
        }

        total_bytes += step_bytes;
        if (out_step_bytes_opt) |step_arr| {
            step_arr[decoder_steps] = step_bytes;
        }
        decoder_steps += 1;

        var lane_cursor: usize = 0;
        s = 0;
        while (s < plan.batch_size) : (s += 1) {
            if (!slots[s].active) continue;
            const n_beams = slots[s].alive_beams;
            const done_info = if (plan.beam_k == 1)
                step.advanceGreedySlot(view.header, plan, &mem, s, &slots[s], best_toks[lane_cursor])
            else
                step.advanceBeamSlot(view.header, plan, &mem, s, &slots[s], lane_cursor, cand_counts[s]);
            lane_cursor += n_beams;

            if (done_info.done) {
                const jidx = slots[s].job_idx;
                const tgt_start: usize = @intCast(jobs[jidx].tgt_off);
                const tok_src = if (plan.beam_k == 1)
                    mem.beam_hist[step.slotHistOff(plan, s, 0)..][0..done_info.out_len]
                else
                    mem.finished_toks[s * plan.max_tgt ..][0..done_info.out_len];
                @memcpy(out_tgt_pool[tgt_start .. tgt_start + done_info.out_len], tok_src);
                jobs[jidx].out_len = @intCast(done_info.out_len);
                jobs[jidx].stopped_on_eos = if (done_info.stopped_eos) 1 else 0;
                jobs[jidx].completed = 1;
                total_gen_tokens += done_info.out_len;
                completed_jobs += 1;
                slots[s].active = false;
            }
        }
    }

    return .{
        .total_weight_bytes_read = total_bytes,
        .completed_jobs = @intCast(completed_jobs),
        .decoder_steps = @intCast(decoder_steps),
        .total_generated_tokens = @intCast(total_gen_tokens),
        .used_helium_mve = if (ops.hasHeliumMve()) 1 else 0,
        .reserved = .{ 0, 0, 0 },
    };
}
