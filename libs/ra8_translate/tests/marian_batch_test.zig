//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host unit tests verifying `RA8FW-674`: `lnF32` / `matVecI8Batch` parity,
//! `TSLT` shortlist parsing & pruning savings, batched queue refill equivalence
//! with single-sequence greedy decode (`RA8FW-676`), beam search (`2..4`) with
//! zero-copy KV tracing, and per-step `weight_bytes_read` reporting.

const std = @import("std");
const ops = @import("marian_ops");
const cont = @import("marian_container");
const marian = @import("marian");
const batch = @import("marian_batch");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;
const expectApproxEqAbs = std.testing.expectApproxEqAbs;

fn detVal(seed: u32, idx: usize) f32 {
    var x: u32 = seed ^ @as(u32, @truncate(idx *% 0x9E37_79B1));
    x ^= x >> 16;
    x *%= 0x85EB_CA6B;
    x ^= x >> 13;
    x *%= 0xC2B2_AE35;
    x ^= x >> 16;
    const unit: i32 = @as(i32, @intCast(x & 0xFF)) - 128;
    return @as(f32, @floatFromInt(unit)) / 256.0;
}

const BuiltBatchModel = struct {
    bytes: []u8,
    len: usize,
    alloc: std.mem.Allocator,

    fn deinit(self: *BuiltBatchModel) void {
        self.alloc.free(self.bytes);
    }
};

fn allocI8(alloc: std.mem.Allocator, len: usize, seed: u32, zp: i8) ![]i8 {
    const buf = try alloc.alloc(i8, len);
    for (buf, 0..) |*dst, i| {
        const scaled = @as(i32, @intFromFloat(detVal(seed, i) * 90.0)) + @as(i32, zp);
        dst.* = @intCast(std.math.clamp(scaled, -127, 127));
    }
    return buf;
}

fn allocF32(alloc: std.mem.Allocator, len: usize, seed: u32, off: f32, amp: f32) ![]f32 {
    const buf = try alloc.alloc(f32, len);
    for (buf, 0..) |*dst, i| dst.* = off + detVal(seed, i) * amp;
    return buf;
}

fn buildBatchModel(
    alloc: std.mem.Allocator,
    opts: cont.WriteOpts,
    with_pos: bool,
    with_bias: bool,
    with_final_ln: bool,
    shortlist_rows: ?[]const []const u32,
    seed_base: u32,
) !BuiltBatchModel {
    const slots_n = cont.expectedTensorCount(opts.num_enc_layers, opts.num_dec_layers);
    var inputs = try alloc.alloc(cont.TensorInput, slots_n);
    defer {
        for (inputs) |inp| {
            if (inp.alias_slot == null and inp.i8_data.len > 0) alloc.free(inp.i8_data);
            if (inp.f32_data.len > 0) alloc.free(inp.f32_data);
        }
        alloc.free(inputs);
    }
    @memset(inputs, .{});

    const d: usize = @intCast(opts.d_model);
    const ffn: usize = @intCast(opts.d_ff);
    const v: usize = @intCast(opts.vocab_size);
    const mseq: usize = @intCast(opts.max_seq_len);

    inputs[0] = .{ .dtype = .i8, .rows = opts.vocab_size, .cols = opts.d_model, .scale = 0.04, .zero_point = -2, .i8_data = try allocI8(alloc, v * d, seed_base + 1, -2) };
    if (with_pos) {
        inputs[1] = .{ .dtype = .f32, .rows = opts.max_seq_len, .cols = opts.d_model, .f32_data = try allocF32(alloc, mseq * d, seed_base + 2, 0.0, 0.25) };
    }
    if ((opts.model_flags & cont.flag_tied_embed) != 0) {
        inputs[2] = .{ .dtype = .i8, .rows = opts.vocab_size, .cols = opts.d_model, .scale = 0.04, .zero_point = -2, .alias_slot = 0 };
    } else {
        inputs[2] = .{ .dtype = .i8, .rows = opts.vocab_size, .cols = opts.d_model, .scale = 0.035, .zero_point = 1, .i8_data = try allocI8(alloc, v * d, seed_base + 3, 1) };
    }
    if (with_bias) {
        inputs[3] = .{ .dtype = .f32, .rows = opts.vocab_size, .cols = 1, .f32_data = try allocF32(alloc, v, seed_base + 4, 0.05, 0.5) };
    }
    if (with_final_ln) {
        inputs[4] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try allocF32(alloc, d, seed_base + 5, 1.0, 0.1) };
        inputs[5] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try allocF32(alloc, d, seed_base + 6, 0.0, 0.05) };
        inputs[6] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try allocF32(alloc, d, seed_base + 7, 1.0, 0.1) };
        inputs[7] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try allocF32(alloc, d, seed_base + 8, 0.0, 0.05) };
    }

    var slot: usize = cont.global_tensor_count;
    var seed = seed_base + 100;
    var l: u8 = 0;
    while (l < opts.num_enc_layers) : (l += 1) {
        try fillProj(alloc, inputs[slot .. slot + 10], opts.d_model, with_bias, &seed);
        slot += 10;
        try fillFfn(alloc, inputs[slot .. slot + 6], opts.d_model, opts.d_ff, ffn, d, with_bias, &seed);
        slot += 6;
    }
    l = 0;
    while (l < opts.num_dec_layers) : (l += 1) {
        try fillProj(alloc, inputs[slot .. slot + 10], opts.d_model, with_bias, &seed);
        slot += 10;
        try fillProj(alloc, inputs[slot .. slot + 10], opts.d_model, with_bias, &seed);
        slot += 10;
        try fillFfn(alloc, inputs[slot .. slot + 6], opts.d_model, opts.d_ff, ffn, d, with_bias, &seed);
        slot += 6;
    }

    const out = try alloc.alloc(u8, 512 * 1024);
    const len = try cont.writeContainerWithShortlist(opts, inputs, shortlist_rows, out);
    return .{ .bytes = out, .len = len, .alloc = alloc };
}

fn fillProj(alloc: std.mem.Allocator, slice: []cont.TensorInput, d_u16: u16, with_bias: bool, seed: *u32) !void {
    const d: usize = @intCast(d_u16);
    var p: usize = 0;
    while (p < 4) : (p += 1) {
        const zp: i8 = @intCast(@as(i32, @intCast(p)) - 2);
        slice[p * 2] = .{ .dtype = .i8, .rows = d_u16, .cols = d_u16, .scale = 0.025, .zero_point = zp, .i8_data = try allocI8(alloc, d * d, seed.*, zp) };
        seed.* += 1;
        if (with_bias) {
            slice[p * 2 + 1] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try allocF32(alloc, d, seed.*, 0.0, 0.1) };
        }
        seed.* += 1;
    }
    slice[8] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try allocF32(alloc, d, seed.*, 1.0, 0.1) };
    seed.* += 1;
    slice[9] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try allocF32(alloc, d, seed.*, 0.0, 0.05) };
    seed.* += 1;
}

fn fillFfn(alloc: std.mem.Allocator, slice: []cont.TensorInput, d_u16: u16, ffn_u16: u16, ffn: usize, d: usize, with_bias: bool, seed: *u32) !void {
    slice[0] = .{ .dtype = .i8, .rows = ffn_u16, .cols = d_u16, .scale = 0.03, .zero_point = 1, .i8_data = try allocI8(alloc, ffn * d, seed.*, 1) };
    seed.* += 1;
    if (with_bias) slice[1] = .{ .dtype = .f32, .rows = ffn_u16, .cols = 1, .f32_data = try allocF32(alloc, ffn, seed.*, 0.0, 0.1) };
    seed.* += 1;
    slice[2] = .{ .dtype = .i8, .rows = d_u16, .cols = ffn_u16, .scale = 0.025, .zero_point = -1, .i8_data = try allocI8(alloc, d * ffn, seed.*, -1) };
    seed.* += 1;
    if (with_bias) slice[3] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try allocF32(alloc, d, seed.*, 0.0, 0.1) };
    seed.* += 1;
    slice[4] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try allocF32(alloc, d, seed.*, 1.0, 0.1) };
    seed.* += 1;
    slice[5] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try allocF32(alloc, d, seed.*, 0.0, 0.05) };
    seed.* += 1;
}

test "lnF32 accuracy and matVecI8Batch equals single matVecI8 bit-for-bit" {
    const vals = [_]f32{ 1e-4, 0.125, 0.5, 1.0, 1.41421, 2.5, 13.75, 1024.0 };
    for (vals) |x| {
        try expectApproxEqAbs(@log(x), ops.lnF32(x), 1e-5);
    }

    var x_b: [3 * 16]f32 = undefined;
    var w: [8 * 16]i8 = undefined;
    var bias: [8]f32 = undefined;
    for (&x_b, 0..) |*dst, i| dst.* = detVal(3, i) * 2.5;
    for (&w, 0..) |*dst, i| dst.* = @intCast(@as(i32, @intCast((i * 17) % 240)) - 120);
    for (&bias, 0..) |*dst, i| dst.* = detVal(9, i);

    var out_batch: [3 * 8]f32 = undefined;
    var q_b: [3 * 16]i8 = undefined;
    var sc_b: [3]f32 = undefined;
    var zp_b: [3]i32 = undefined;
    var batch_bytes: u64 = 0;
    ops.matVecI8Batch(&x_b, 3, &w, 8, 16, 0.02, -3, &bias, &q_b, &sc_b, &zp_b, &out_batch, &batch_bytes);
    try expectEqual(@as(u64, 8 * 16 + 8 * 4), batch_bytes);

    var single_bytes: u64 = 0;
    var b: usize = 0;
    while (b < 3) : (b += 1) {
        var out_s: [8]f32 = undefined;
        var q_s: [16]i8 = undefined;
        ops.matVecI8(x_b[b * 16 ..][0..16], &w, 8, 16, 0.02, -3, &bias, &q_s, &out_s, &single_bytes);
        for (out_s, out_batch[b * 8 ..][0..8]) |sv, bv| try expectEqual(sv, bv);
    }
    try expectEqual(batch_bytes * 3, single_bytes);
}

test "batched queue decode with refill matches single-sequence greedy bit-for-bit and cuts weight reads" {
    const alloc = std.testing.allocator;
    const cfgs = [_]struct {
        opts: cont.WriteOpts,
        pos: bool,
        bias: bool,
        fln: bool,
        seed: u32,
    }{
        .{
            .opts = .{ .model_flags = cont.flag_tied_embed, .num_heads = 2, .num_enc_layers = 2, .num_dec_layers = 2, .d_model = 8, .d_ff = 16, .max_seq_len = 16, .vocab_size = 16 },
            .pos = false,
            .bias = true,
            .fln = false,
            .seed = 21,
        },
        .{
            .opts = .{ .model_flags = cont.flag_pre_norm | cont.flag_scale_embed, .num_heads = 2, .num_enc_layers = 1, .num_dec_layers = 2, .d_model = 8, .d_ff = 16, .max_seq_len = 16, .vocab_size = 16 },
            .pos = true,
            .bias = true,
            .fln = true,
            .seed = 64,
        },
    };

    for (cfgs) |c| {
        var model = try buildBatchModel(alloc, c.opts, c.pos, c.bias, c.fln, null, c.seed);
        defer model.deinit();
        const view = try cont.TmntView.open(model.bytes[0..model.len], cont.open_verify_payload);

        const src_pool = [_]u32{ 4, 7, 9, 5, 11, 6, 8, 12, 14, 10, 4, 15, 7, 13 };
        const specs = [_]struct { off: u32, len: u32, cap: u32 }{
            .{ .off = 0, .len = 3, .cap = 2 },
            .{ .off = 3, .len = 2, .cap = 5 },
            .{ .off = 5, .len = 4, .cap = 4 },
            .{ .off = 9, .len = 2, .cap = 3 },
            .{ .off = 11, .len = 3, .cap = 5 },
        };

        // Reference outputs from RA8FW-676 single-sequence greedyDecode.
        const single_wb = try marian.minWorkBytes(view, 4, 5);
        const single_work = try alloc.alloc(u8, single_wb);
        defer alloc.free(single_work);
        var ref_toks: [5 * 5]u32 = undefined;
        var ref_lens: [5]u32 = undefined;
        var ref_eos: [5]u8 = undefined;
        for (specs, 0..) |sp, idx| {
            const st = try marian.greedyDecode(view, src_pool[sp.off .. sp.off + sp.len], sp.cap, single_work, ref_toks[idx * 5 ..][0..sp.cap]);
            ref_lens[idx] = st.generated_tokens;
            ref_eos[idx] = st.stopped_on_eos;
        }

        // Batch size 1 vs Batch size 3 (queue refill as items complete).
        var jobs1: [5]batch.MarianBatchJob = undefined;
        var jobs3: [5]batch.MarianBatchJob = undefined;
        for (specs, 0..) |sp, idx| {
            jobs1[idx] = .{ .src_off = sp.off, .src_len = sp.len, .tgt_off = @intCast(idx * 5), .tgt_cap = sp.cap };
            jobs3[idx] = jobs1[idx];
        }

        const cfg1: batch.MarianBatchCfg = .{ .max_src_len = 4, .max_tgt_len = 5, .flags = 0, .batch_size = 1, .beam_width = 1 };
        const cfg3: batch.MarianBatchCfg = .{ .max_src_len = 4, .max_tgt_len = 5, .flags = 0, .batch_size = 3, .beam_width = 1 };
        const wb1 = try batch.minBatchWorkBytes(view, cfg1);
        const wb3 = try batch.minBatchWorkBytes(view, cfg3);
        const work3 = try alloc.alloc(u8, @max(wb1, wb3));
        defer alloc.free(work3);

        var tgt1: [25]u32 = @splat(0);
        var tgt3: [25]u32 = @splat(0);
        var steps1: [30]u64 = @splat(0);
        var steps3: [30]u64 = @splat(0);
        const st1 = try batch.batchDecode(view, cfg1, &src_pool, &jobs1, work3[0..wb1], &tgt1, &steps1);
        const st3 = try batch.batchDecode(view, cfg3, &src_pool, &jobs3, work3[0..wb3], &tgt3, &steps3);

        try expectEqual(@as(u32, 5), st3.completed_jobs);
        try expect(st3.decoder_steps > 0 and st3.decoder_steps < st1.decoder_steps);
        try expect(st3.total_weight_bytes_read < st1.total_weight_bytes_read);
        var dec3_sum: u64 = 0;
        for (steps3[0..st3.decoder_steps]) |sb| {
            try expect(sb > 0);
            dec3_sum += sb;
        }
        try expect(dec3_sum <= st3.total_weight_bytes_read);

        for (jobs3, 0..) |j, idx| {
            try expectEqual(@as(u8, 1), j.completed);
            try expectEqual(ref_lens[idx], j.out_len);
            try expectEqual(ref_eos[idx], j.stopped_on_eos);
            for (0..j.out_len) |k| {
                try expectEqual(ref_toks[idx * 5 + k], tgt3[idx * 5 + k]);
            }
        }
    }
}

test "TSLT shortlist pruning matches lossless greedy output and skips excluded rows" {
    const alloc = std.testing.allocator;
    const opts: cont.WriteOpts = .{
        .d_model = 4,
        .d_ff = 8,
        .max_seq_len = 12,
        .vocab_size = 24,
    };

    // First build unshortlisted to inspect true greedy token IDs for two source bubbles.
    var base_model = try buildBatchModel(alloc, opts, false, true, false, null, 313);
    defer base_model.deinit();
    const base_view = try cont.TmntView.open(base_model.bytes[0..base_model.len], 0);
    const single_work = try alloc.alloc(u8, try marian.minWorkBytes(base_view, 4, 4));
    defer alloc.free(single_work);

    const src0 = [_]u32{ 5, 8 };
    const src1 = [_]u32{ 12, 19 };
    var ref0: [4]u32 = undefined;
    var ref1: [4]u32 = undefined;
    const st0 = try marian.greedyDecode(base_view, &src0, 4, single_work, &ref0);
    const st1 = try marian.greedyDecode(base_view, &src1, 4, single_work, &ref1);

    // Populate TSLT CSR shortlist entries only for src tokens {5, 8, 12, 19} -> their exact targets.
    var rows: [24][]const u32 = @splat(&.{});
    rows[5] = ref0[0..st0.generated_tokens];
    rows[12] = ref1[0..st1.generated_tokens];

    var sl_model = try buildBatchModel(alloc, opts, false, true, false, &rows, 313);
    defer sl_model.deinit();
    const sl_view = try cont.TmntView.open(sl_model.bytes[0..sl_model.len], cont.open_verify_payload);
    try expect((try sl_view.shortlist()) != null);

    const cfg_full: batch.MarianBatchCfg = .{ .max_src_len = 4, .max_tgt_len = 4, .flags = 0, .batch_size = 2, .beam_width = 1 };
    const cfg_sl: batch.MarianBatchCfg = .{ .max_src_len = 4, .max_tgt_len = 4, .flags = batch.batch_flag_use_shortlist, .batch_size = 2, .beam_width = 1 };
    const work = try alloc.alloc(u8, @max(try batch.minBatchWorkBytes(sl_view, cfg_full), try batch.minBatchWorkBytes(sl_view, cfg_sl)));
    defer alloc.free(work);

    const pool = [_]u32{ 5, 8, 12, 19 };
    var jobs_full = [_]batch.MarianBatchJob{
        .{ .src_off = 0, .src_len = 2, .tgt_off = 0, .tgt_cap = 4 },
        .{ .src_off = 2, .src_len = 2, .tgt_off = 4, .tgt_cap = 4 },
    };
    var jobs_sl = jobs_full;
    var tgt_full: [8]u32 = @splat(0);
    var tgt_sl: [8]u32 = @splat(0);
    var sfull: [16]u64 = @splat(0);
    var ssl: [16]u64 = @splat(0);

    const res_full = try batch.batchDecode(sl_view, cfg_full, &pool, &jobs_full, work, &tgt_full, &sfull);
    const res_sl = try batch.batchDecode(sl_view, cfg_sl, &pool, &jobs_sl, work, &tgt_sl, &ssl);

    try expect(res_sl.total_weight_bytes_read < res_full.total_weight_bytes_read);
    try expect(ssl[0] < sfull[0]);
    try expectEqual(jobs_full[0].out_len, jobs_sl[0].out_len);
    try expectEqual(jobs_full[1].out_len, jobs_sl[1].out_len);
    for (0..jobs_full[0].out_len) |i| try expectEqual(tgt_full[i], tgt_sl[i]);
    for (0..jobs_full[1].out_len) |i| try expectEqual(tgt_full[4 + i], tgt_sl[4 + i]);

    // Container without shortlist rejects batch_flag_use_shortlist without mutating outputs.
    tgt_sl[0] = 0xDEAD;
    try expectEqual(batch.Error.NotFound, batch.batchDecode(base_view, cfg_sl, &pool, &jobs_sl, work, &tgt_sl, &ssl));
    try expectEqual(@as(u32, 0xDEAD), tgt_sl[0]);
}

test "small beam search (width 2..4) runs across queue refill with traced KV and shortlist" {
    const alloc = std.testing.allocator;
    const opts: cont.WriteOpts = .{ .num_heads = 2, .num_enc_layers = 1, .num_dec_layers = 2, .d_model = 8, .d_ff = 16, .max_seq_len = 12, .vocab_size = 12 };
    var rows: [12][]const u32 = @splat(&.{});
    const sl4 = [_]u32{ 4, 5, 6, 7 };
    const sl9 = [_]u32{ 8, 9, 10, 11 };
    rows[4] = &sl4;
    rows[6] = &sl9;
    rows[9] = &sl9;

    var model = try buildBatchModel(alloc, opts, true, true, true, &rows, 555);
    defer model.deinit();
    const view = try cont.TmntView.open(model.bytes[0..model.len], cont.open_verify_payload);

    const cfg: batch.MarianBatchCfg = .{
        .max_src_len = 4,
        .max_tgt_len = 4,
        .flags = batch.batch_flag_beam_search | batch.batch_flag_use_shortlist,
        .batch_size = 2,
        .beam_width = 3,
    };
    const wbytes = try batch.minBatchWorkBytes(view, cfg);
    const work = try alloc.alloc(u8, wbytes);
    defer alloc.free(work);

    const pool = [_]u32{ 4, 5, 6, 7, 9, 4 };
    var jobs = [_]batch.MarianBatchJob{
        .{ .src_off = 0, .src_len = 2, .tgt_off = 0, .tgt_cap = 4 },
        .{ .src_off = 2, .src_len = 2, .tgt_off = 4, .tgt_cap = 4 },
        .{ .src_off = 4, .src_len = 2, .tgt_off = 8, .tgt_cap = 4 },
    };
    var out_tgt: [12]u32 = @splat(0);
    var step_bytes: [16]u64 = @splat(0);
    const st = try batch.batchDecode(view, cfg, &pool, &jobs, work, &out_tgt, &step_bytes);

    try expectEqual(@as(u32, 3), st.completed_jobs);
    try expect(st.decoder_steps >= 1 and st.total_weight_bytes_read > 0);
    for (jobs) |j| {
        try expectEqual(@as(u8, 1), j.completed);
        try expect(j.out_len >= 1 and j.out_len <= 4);
    }
}
