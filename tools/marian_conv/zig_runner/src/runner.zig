//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Marian `TMNT`/`TVOC`/`TSLT` host evaluation and parity trace generator.

const std = @import("std");
pub const c = @import("c_api.zig");

pub const Candidate = struct {
    id: u32,
    logit: f32,
};

pub const StepTrace = struct {
    step: usize,
    selected_id: u32,
    selected_logit: f32,
    selected_logit_bits: u32,
    logits_sha256: [64]u8,
    top_candidates: []Candidate,
};

pub const SentenceTrace = struct {
    source_text: []const u8,
    src_ids: []u32,
    greedy_ids: []u32,
    decoded_en: []u8,
    stopped_on_eos: bool,
    weight_bytes_read: u64,
    steps: []StepTrace,
};

pub const BatchCaseOut = struct {
    greedy_ids: []u32,
    stopped_on_eos: bool,
};

pub const BatchSummary = struct {
    completed_jobs: u32,
    decoder_steps: u32,
    total_weight_bytes_read: u64,
    cases: []BatchCaseOut,
};

pub const RunReport = struct {
    allocator: std.mem.Allocator,
    vocab: c.VocabHeader,
    marian: c.MarianHeader,
    cases: []SentenceTrace,
    batch_full: ?BatchSummary = null,
    batch_shortlist: ?BatchSummary = null,

    pub fn deinit(self: *RunReport) void {
        for (self.cases) |case| {
            self.allocator.free(case.source_text);
            self.allocator.free(case.src_ids);
            self.allocator.free(case.greedy_ids);
            self.allocator.free(case.decoded_en);
            for (case.steps) |s| self.allocator.free(s.top_candidates);
            self.allocator.free(case.steps);
        }
        self.allocator.free(self.cases);
        freeBatchSummary(self.allocator, self.batch_full);
        freeBatchSummary(self.allocator, self.batch_shortlist);
    }
};

fn freeBatchSummary(allocator: std.mem.Allocator, summary_opt: ?BatchSummary) void {
    const summary = summary_opt orelse return;
    for (summary.cases) |item| allocator.free(item.greedy_ids);
    allocator.free(summary.cases);
}

fn checkErr(rc: u16, err_tag: anyerror) !void {
    if (rc != c.ra8_ok) return err_tag;
}

pub fn encodeText(
    allocator: std.mem.Allocator,
    tvoc: []const u8,
    vocab_hdr: c.VocabHeader,
    text: []const u8,
) ![]u32 {
    var work_bytes: usize = 0;
    try checkErr(
        c.ra8_translate_tok_work_bytes(text.len, vocab_hdr.model_type, &work_bytes),
        error.TokWorkBytesFailed,
    );
    const work = try allocator.alloc(u8, @max(work_bytes, 64));
    defer allocator.free(work);

    const id_cap = @max(text.len + 16, 64);
    const ids = try allocator.alloc(u32, id_cap);
    errdefer allocator.free(ids);

    var id_count: usize = 0;
    try checkErr(
        c.ra8_translate_tok_encode(
            tvoc.ptr,
            tvoc.len,
            if (text.len > 0) text.ptr else null,
            text.len,
            c.tok_enc_add_eos,
            work.ptr,
            work.len,
            ids.ptr,
            ids.len,
            &id_count,
        ),
        error.TokEncodeFailed,
    );
    return try allocator.realloc(ids, id_count);
}

pub fn decodeTokens(allocator: std.mem.Allocator, tvoc: []const u8, ids: []const u32) ![]u8 {
    const out_cap = @max(ids.len * 48 + 64, 128);
    const buf = try allocator.alloc(u8, out_cap);
    errdefer allocator.free(buf);

    var out_len: usize = 0;
    try checkErr(
        c.ra8_translate_tok_decode(
            tvoc.ptr,
            tvoc.len,
            if (ids.len > 0) ids.ptr else null,
            ids.len,
            buf.ptr,
            buf.len,
            &out_len,
        ),
        error.TokDecodeFailed,
    );
    return try allocator.realloc(buf, out_len);
}

fn sha256LogitsHex(logits: []const f32) [64]u8 {
    var hasher = std.crypto.hash.sha2.Sha256.init(.{});
    var le_buf: [4]u8 = undefined;
    for (logits) |val| {
        std.mem.writeInt(u32, &le_buf, @bitCast(val), .little);
        hasher.update(&le_buf);
    }
    var digest: [32]u8 = undefined;
    hasher.final(&digest);
    return std.fmt.bytesToHex(digest, .lower);
}

fn topKCandidates(allocator: std.mem.Allocator, logits: []const f32, top_k: usize) ![]Candidate {
    const k = @min(top_k, logits.len);
    const out = try allocator.alloc(Candidate, k);
    errdefer allocator.free(out);
    for (out) |*slot| slot.* = .{ .id = 0, .logit = -std.math.inf(f32) };

    for (logits, 0..) |val, idx| {
        const uid: u32 = @intCast(idx);
        var pos: usize = k;
        while (pos > 0 and (val > out[pos - 1].logit or
            (val == out[pos - 1].logit and uid < out[pos - 1].id)))
        {
            if (pos < k) out[pos] = out[pos - 1];
            pos -= 1;
        }
        if (pos < k) out[pos] = .{ .id = uid, .logit = val };
    }
    return out;
}

fn collectSteps(
    allocator: std.mem.Allocator,
    tmnt: []const u8,
    hdr: c.MarianHeader,
    src_ids: []const u32,
    greedy_ids: []const u32,
    work: []u8,
    top_k: usize,
) ![]StepTrace {
    const steps = try allocator.alloc(StepTrace, greedy_ids.len);
    errdefer allocator.free(steps);
    var filled: usize = 0;
    errdefer for (steps[0..filled]) |s| allocator.free(s.top_candidates);

    const logits = try allocator.alloc(f32, hdr.vocab_size);
    defer allocator.free(logits);
    const prefix = try allocator.alloc(u32, greedy_ids.len + 1);
    defer allocator.free(prefix);
    prefix[0] = hdr.bos_id;

    for (greedy_ids, 0..) |chosen_id, step_idx| {
        const plen = step_idx + 1;
        try checkErr(
            c.ra8_translate_marian_step_logits(
                tmnt.ptr,
                tmnt.len,
                src_ids.ptr,
                src_ids.len,
                prefix.ptr,
                plen,
                work.ptr,
                work.len,
                logits.ptr,
                logits.len,
            ),
            error.MarianStepLogitsFailed,
        );
        const top = try topKCandidates(allocator, logits, @max(top_k, 1));
        if (top[0].id != chosen_id) {
            allocator.free(top);
            return error.StepLogitsGreedyMismatch;
        }
        steps[step_idx] = .{
            .step = step_idx,
            .selected_id = chosen_id,
            .selected_logit = logits[chosen_id],
            .selected_logit_bits = @bitCast(logits[chosen_id]),
            .logits_sha256 = sha256LogitsHex(logits),
            .top_candidates = top,
        };
        prefix[plen] = chosen_id;
        filled += 1;
    }
    return steps;
}

fn runSentence(
    allocator: std.mem.Allocator,
    tmnt: []const u8,
    tvoc: []const u8,
    v_hdr: c.VocabHeader,
    m_hdr: c.MarianHeader,
    text: []const u8,
    max_tgt_len: u16,
    top_k: usize,
) !SentenceTrace {
    const owned_text = try allocator.dupe(u8, text);
    errdefer allocator.free(owned_text);
    const src_ids = try encodeText(allocator, tvoc, v_hdr, text);
    errdefer allocator.free(src_ids);
    if (src_ids.len == 0 or src_ids.len > m_hdr.max_seq_len) return error.SourceLengthOutOfBounds;

    var work_bytes: usize = 0;
    const max_src_u16: u16 = @intCast(src_ids.len);
    try checkErr(
        c.ra8_translate_marian_work_bytes(tmnt.ptr, tmnt.len, max_src_u16, max_tgt_len, &work_bytes),
        error.MarianWorkBytesFailed,
    );
    const work = try allocator.alloc(u8, work_bytes);
    defer allocator.free(work);

    const out_buf = try allocator.alloc(u32, max_tgt_len);
    defer allocator.free(out_buf);
    var stats = c.MarianStats{};
    try checkErr(
        c.ra8_translate_marian_greedy_decode(
            tmnt.ptr,
            tmnt.len,
            src_ids.ptr,
            src_ids.len,
            max_tgt_len,
            work.ptr,
            work.len,
            out_buf.ptr,
            out_buf.len,
            &stats,
        ),
        error.MarianGreedyDecodeFailed,
    );

    const greedy_ids = try allocator.dupe(u32, out_buf[0..stats.generated_tokens]);
    errdefer allocator.free(greedy_ids);
    const decoded_en = try decodeTokens(allocator, tvoc, greedy_ids);
    errdefer allocator.free(decoded_en);
    const steps = try collectSteps(allocator, tmnt, m_hdr, src_ids, greedy_ids, work, top_k);

    return .{
        .source_text = owned_text,
        .src_ids = src_ids,
        .greedy_ids = greedy_ids,
        .decoded_en = decoded_en,
        .stopped_on_eos = stats.stopped_on_eos != 0,
        .weight_bytes_read = stats.weight_bytes_read,
        .steps = steps,
    };
}

const BatchPool = struct {
    src_pool: []u32,
    jobs: []c.MarianBatchJob,
    max_src: usize,
};

fn buildBatchPool(allocator: std.mem.Allocator, cases: []const SentenceTrace, max_tgt_len: u16) !BatchPool {
    var total_src: usize = 0;
    var max_src: usize = 1;
    for (cases) |case| {
        total_src += case.src_ids.len;
        max_src = @max(max_src, case.src_ids.len);
    }

    const src_pool = try allocator.alloc(u32, total_src);
    errdefer allocator.free(src_pool);
    const jobs = try allocator.alloc(c.MarianBatchJob, cases.len);
    errdefer allocator.free(jobs);

    var s_cursor: usize = 0;
    for (cases, 0..) |case, idx| {
        @memcpy(src_pool[s_cursor .. s_cursor + case.src_ids.len], case.src_ids);
        jobs[idx] = .{
            .src_off = @intCast(s_cursor),
            .src_len = @intCast(case.src_ids.len),
            .tgt_off = @intCast(idx * max_tgt_len),
            .tgt_cap = max_tgt_len,
        };
        s_cursor += case.src_ids.len;
    }
    return .{ .src_pool = src_pool, .jobs = jobs, .max_src = max_src };
}

fn runBatchPass(
    allocator: std.mem.Allocator,
    tmnt: []const u8,
    cases: []const SentenceTrace,
    max_tgt_len: u16,
    flags: u16,
) !BatchSummary {
    const pool = try buildBatchPool(allocator, cases, max_tgt_len);
    defer allocator.free(pool.src_pool);
    defer allocator.free(pool.jobs);

    const cfg = c.MarianBatchCfg{
        .max_src_len = @intCast(pool.max_src),
        .max_tgt_len = max_tgt_len,
        .flags = flags,
        .batch_size = @intCast(@min(cases.len, 4)),
        .beam_width = 1,
    };
    var work_bytes: usize = 0;
    try checkErr(
        c.ra8_translate_marian_batch_work_bytes(tmnt.ptr, tmnt.len, &cfg, &work_bytes),
        error.MarianBatchWorkBytesFailed,
    );
    const work = try allocator.alloc(u8, work_bytes);
    defer allocator.free(work);
    const tgt_pool = try allocator.alloc(u32, cases.len * max_tgt_len);
    defer allocator.free(tgt_pool);
    const step_bytes = try allocator.alloc(u64, cases.len * max_tgt_len);
    defer allocator.free(step_bytes);

    var b_stats = c.MarianBatchStats{};
    try checkErr(
        c.ra8_translate_marian_batch_decode(
            tmnt.ptr,
            tmnt.len,
            &cfg,
            pool.src_pool.ptr,
            pool.src_pool.len,
            pool.jobs.ptr,
            pool.jobs.len,
            work.ptr,
            work.len,
            tgt_pool.ptr,
            tgt_pool.len,
            step_bytes.ptr,
            step_bytes.len,
            &b_stats,
        ),
        error.MarianBatchDecodeFailed,
    );

    const out_cases = try allocator.alloc(BatchCaseOut, cases.len);
    errdefer allocator.free(out_cases);
    for (pool.jobs, 0..) |job, idx| {
        out_cases[idx] = .{
            .greedy_ids = try allocator.dupe(u32, tgt_pool[job.tgt_off .. job.tgt_off + job.out_len]),
            .stopped_on_eos = job.stopped_on_eos != 0,
        };
    }
    return .{
        .completed_jobs = b_stats.completed_jobs,
        .decoder_steps = b_stats.decoder_steps,
        .total_weight_bytes_read = b_stats.total_weight_bytes_read,
        .cases = out_cases,
    };
}

pub fn evaluateSentences(
    allocator: std.mem.Allocator,
    tmnt: []const u8,
    tvoc: []const u8,
    sentences: []const []const u8,
    max_tgt_len: u16,
    top_k: usize,
    verify_payload: bool,
    run_batch: bool,
) !RunReport {
    var v_hdr = c.VocabHeader{};
    try checkErr(c.ra8_translate_tok_open(tvoc.ptr, tvoc.len, &v_hdr), error.VocabOpenFailed);

    var m_hdr = c.MarianHeader{};
    const open_flags: u8 = if (verify_payload) c.marian_open_verify_payload else c.marian_open_meta_only;
    try checkErr(
        c.ra8_translate_marian_open(tmnt.ptr, tmnt.len, open_flags, &m_hdr),
        error.MarianOpenFailed,
    );

    const cases = try allocator.alloc(SentenceTrace, sentences.len);
    errdefer allocator.free(cases);
    var done: usize = 0;
    errdefer for (cases[0..done]) |case| {
        allocator.free(case.source_text);
        allocator.free(case.src_ids);
        allocator.free(case.greedy_ids);
        allocator.free(case.decoded_en);
        for (case.steps) |s| allocator.free(s.top_candidates);
        allocator.free(case.steps);
    };

    for (sentences, 0..) |text, idx| {
        cases[idx] = try runSentence(allocator, tmnt, tvoc, v_hdr, m_hdr, text, max_tgt_len, top_k);
        done += 1;
    }

    var report = RunReport{ .allocator = allocator, .vocab = v_hdr, .marian = m_hdr, .cases = cases };
    errdefer report.deinit();
    if (run_batch and cases.len > 0) {
        report.batch_full = try runBatchPass(
            allocator,
            tmnt,
            cases,
            max_tgt_len,
            c.marian_batch_flag_none,
        );
        if (m_hdr.shortlist_bytes > 0) {
            report.batch_shortlist = try runBatchPass(
                allocator,
                tmnt,
                cases,
                max_tgt_len,
                c.marian_batch_flag_use_shortlist,
            );
        }
    }
    return report;
}

pub fn evaluateTokenizerOnly(
    allocator: std.mem.Allocator,
    tvoc: []const u8,
    sentences: []const []const u8,
) !RunReport {
    var v_hdr = c.VocabHeader{};
    try checkErr(c.ra8_translate_tok_open(tvoc.ptr, tvoc.len, &v_hdr), error.VocabOpenFailed);

    const cases = try allocator.alloc(SentenceTrace, sentences.len);
    errdefer allocator.free(cases);
    var done: usize = 0;
    errdefer for (cases[0..done]) |case| {
        allocator.free(case.source_text);
        allocator.free(case.src_ids);
        allocator.free(case.greedy_ids);
        allocator.free(case.decoded_en);
        allocator.free(case.steps);
    };

    for (sentences, 0..) |text, idx| {
        const owned = try allocator.dupe(u8, text);
        errdefer allocator.free(owned);
        const ids = try encodeText(allocator, tvoc, v_hdr, text);
        errdefer allocator.free(ids);
        const roundtrip = try decodeTokens(allocator, tvoc, ids);
        errdefer allocator.free(roundtrip);
        cases[idx] = .{
            .source_text = owned,
            .src_ids = ids,
            .greedy_ids = try allocator.alloc(u32, 0),
            .decoded_en = roundtrip,
            .stopped_on_eos = false,
            .weight_bytes_read = 0,
            .steps = try allocator.alloc(StepTrace, 0),
        };
        done += 1;
    }

    var m_hdr = c.MarianHeader{};
    m_hdr.vocab_size = v_hdr.vocab_size;
    return .{ .allocator = allocator, .vocab = v_hdr, .marian = m_hdr, .cases = cases };
}
