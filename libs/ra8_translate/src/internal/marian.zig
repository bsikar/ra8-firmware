//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding int8 Marian encoder-decoder transformer inference reading
//! quantized weights in place from a validated `TMNT` external-memory container.

const std = @import("std");
const ops = @import("marian_ops");
const cont = @import("marian_container");
const err_mod = @import("err");

pub const Error = err_mod.TranslateError;
pub const TmntView = cont.TmntView;
pub const MarianHeader = cont.MarianHeader;
pub const MarianStats = cont.MarianStats;
pub const TensorRef = cont.TensorRef;

/// Partitioned aligned scratch capacities required for single-sequence inference.
pub const WorkSizes = struct {
    max_src: usize,
    max_tgt: usize,
    d_model: usize,
    d_ff: usize,
    max_dim: usize,
    max_attn_len: usize,
    num_enc: usize,
    num_dec: usize,
    is_ssru: bool,
    enc_seq_elems: usize,
    cross_kv_elems: usize,
    self_kv_elems: usize,
    ssru_cell_elems: usize,
    total_f32_elems: usize,
    total_bytes: usize,
};

const WorkMem = struct {
    enc_x: []f32,
    enc_sub: []f32,
    enc_k: []f32,
    enc_v: []f32,
    cross_k: []f32,
    cross_v: []f32,
    self_k: []f32,
    self_v: []f32,
    ssru_cell: []f32,
    dec_x: []f32,
    tok_in: []f32,
    tok_q: []f32,
    tok_ctx: []f32,
    tok_out: []f32,
    ffn_tmp: []f32,
    f32_tmp_a: []f32,
    f32_tmp_b: []f32,
    attn_scores: []f32,
    q_tmp: []i8,
};

/// Compute required scratch byte capacity for encoding `max_src` and decoding `max_tgt`.
pub fn minWorkBytes(view: TmntView, max_src: usize, max_tgt: usize) Error!usize {
    const sizes = try planWorkspace(view.header, max_src, max_tgt);
    return sizes.total_bytes;
}

/// Plan partitioned workspace element counts with overflow checks.
pub fn planWorkspace(hdr: MarianHeader, max_src: usize, max_tgt: usize) Error!WorkSizes {
    if (max_src == 0 or max_tgt == 0) return Error.InvalidSize;
    if (max_src > hdr.max_seq or max_tgt > hdr.max_seq) return Error.OutOfRange;

    const d_model: usize = @intCast(hdr.d_model);
    const d_ff: usize = @intCast(hdr.d_ff);
    const num_enc: usize = @intCast(hdr.num_enc_layers);
    const num_dec: usize = @intCast(hdr.num_dec_layers);
    const max_dim = @max(d_model, d_ff);
    const max_attn_len = @max(max_src, max_tgt);

    const is_ssru = (hdr.model_flags & cont.flag_decoder_ssru) != 0;
    const enc_seq_elems = try checkedMul(max_src, d_model);
    const cross_kv_elems = try checkedMul(num_dec, enc_seq_elems);
    const tgt_seq_elems = try checkedMul(max_tgt, d_model);
    const self_kv_elems = if (is_ssru) 0 else try checkedMul(num_dec, tgt_seq_elems);
    const ssru_cell_elems = if (is_ssru) try checkedMul(num_dec, d_model) else 0;

    var total_f32: usize = 0;
    total_f32 = try checkedAdd(total_f32, try checkedMul(4, enc_seq_elems)); // enc_x, sub, k, v
    total_f32 = try checkedAdd(total_f32, try checkedMul(2, cross_kv_elems)); // cross_k, cross_v
    total_f32 = try checkedAdd(total_f32, try checkedMul(2, self_kv_elems)); // self_k, self_v
    total_f32 = try checkedAdd(total_f32, ssru_cell_elems); // ssru_cell
    total_f32 = try checkedAdd(total_f32, try checkedMul(6, d_model)); // dec_x, in, q, ctx, out, tmp_b
    total_f32 = try checkedAdd(total_f32, d_ff); // ffn_tmp
    total_f32 = try checkedAdd(total_f32, max_dim); // f32_tmp_a
    total_f32 = try checkedAdd(total_f32, max_attn_len); // attn_scores

    const f32_bytes = try checkedMul(total_f32, @sizeOf(f32));
    // Add 16 bytes alignment slack so an arbitrary byte slice base can align to 4 bytes.
    const total_bytes = try checkedAdd(16, try checkedAdd(f32_bytes, max_dim));

    return .{
        .max_src = max_src,
        .max_tgt = max_tgt,
        .d_model = d_model,
        .d_ff = d_ff,
        .max_dim = max_dim,
        .max_attn_len = max_attn_len,
        .num_enc = num_enc,
        .num_dec = num_dec,
        .is_ssru = is_ssru,
        .enc_seq_elems = enc_seq_elems,
        .cross_kv_elems = cross_kv_elems,
        .self_kv_elems = self_kv_elems,
        .ssru_cell_elems = ssru_cell_elems,
        .total_f32_elems = total_f32,
        .total_bytes = total_bytes,
    };
}

/// Evaluate decoder final-step raw vocabulary logits for a given source sequence and prefix.
pub fn evalStepLogits(
    view: TmntView,
    src_ids: []const u32,
    prefix_ids: []const u32,
    work_buf: []u8,
    out_logits: []f32,
) Error!void {
    const hdr = view.header;
    try validateTokenIds(src_ids, hdr.vocab_size, hdr.max_seq);
    try validateTokenIds(prefix_ids, hdr.vocab_size, hdr.max_seq);
    try validateOptionalFinalLnPairs(view);

    const vocab_size: usize = @intCast(hdr.vocab_size);
    if (out_logits.len < vocab_size) return Error.InvalidSize;

    const sizes = try planWorkspace(hdr, src_ids.len, prefix_ids.len);
    const mem = try bindWorkspace(work_buf, sizes);

    var bytes_read: u64 = 0;
    try runEncoderAndPrecomputeCross(view, sizes, &mem, src_ids, &bytes_read);
    if (sizes.is_ssru) @memset(mem.ssru_cell, 0.0);

    for (prefix_ids, 0..) |tok_id, pos| {
        try runDecoderStep(view, sizes, &mem, tok_id, pos, src_ids.len, &bytes_read);
    }

    try computeLogits(view, mem.dec_x, mem.q_tmp, out_logits[0..vocab_size], &bytes_read);
}

/// Run single-sequence greedy encoder-decoder generation starting from `hdr.bos_id`.
pub fn greedyDecode(
    view: TmntView,
    src_ids: []const u32,
    max_tgt_len: usize,
    work_buf: []u8,
    out_tgt_ids: []u32,
) Error!MarianStats {
    const hdr = view.header;
    try validateTokenIds(src_ids, hdr.vocab_size, hdr.max_seq);
    if (max_tgt_len == 0) return Error.InvalidSize;
    if (max_tgt_len > hdr.max_seq) return Error.OutOfRange;
    if (out_tgt_ids.len < max_tgt_len) return Error.InvalidSize;
    try validateOptionalFinalLnPairs(view);

    const sizes = try planWorkspace(hdr, src_ids.len, max_tgt_len);
    const mem = try bindWorkspace(work_buf, sizes);

    var bytes_read: u64 = 0;
    try runEncoderAndPrecomputeCross(view, sizes, &mem, src_ids, &bytes_read);
    if (sizes.is_ssru) @memset(mem.ssru_cell, 0.0);

    var cur_tok: u32 = hdr.bos_id;
    var generated: usize = 0;
    var stopped_eos = false;
    var step: usize = 0;
    while (step < max_tgt_len) : (step += 1) {
        try runDecoderStep(view, sizes, &mem, cur_tok, step, src_ids.len, &bytes_read);
        const next_tok = try greedyArgmaxLmHead(view, mem.dec_x, mem.q_tmp, &bytes_read);
        out_tgt_ids[step] = next_tok;
        generated = step + 1;
        if (next_tok == hdr.eos_id) {
            stopped_eos = true;
            break;
        }
        cur_tok = next_tok;
    }

    return .{
        .weight_bytes_read = bytes_read,
        .generated_tokens = @intCast(generated),
        .stopped_on_eos = if (stopped_eos) 1 else 0,
        .used_helium_mve = if (ops.hasHeliumMve()) 1 else 0,
        .reserved = .{ 0, 0 },
    };
}

fn validateTokenIds(ids: []const u32, vocab_size: u32, max_seq: u16) Error!void {
    if (ids.len == 0) return Error.InvalidSize;
    if (ids.len > max_seq) return Error.OutOfRange;
    for (ids) |tok| {
        if (tok >= vocab_size) return Error.OutOfRange;
    }
}

fn validateOptionalFinalLnPairs(view: TmntView) Error!void {
    const pairs = [2][2]usize{ .{ 4, 5 }, .{ 6, 7 } };
    for (pairs) |pair| {
        const g = try view.tensorAt(pair[0]);
        const b = try view.tensorAt(pair[1]);
        if ((g.dtype == .none) != (b.dtype == .none)) return Error.ValidationFailed;
    }
}

fn bindWorkspace(work_buf: []u8, sizes: WorkSizes) Error!WorkMem {
    if (work_buf.len < sizes.total_bytes) return Error.InvalidSize;

    const base_addr = @intFromPtr(work_buf.ptr);
    const aligned_addr = (base_addr + 3) & ~@as(usize, 3);
    const skip = aligned_addr - base_addr;
    const f32_bytes = sizes.total_f32_elems * @sizeOf(f32);
    if (work_buf.len < skip + f32_bytes + sizes.max_dim) return Error.InvalidSize;

    const f32_ptr: [*]f32 = @ptrCast(@alignCast(work_buf.ptr + skip));
    const f32_pool = f32_ptr[0..sizes.total_f32_elems];
    const q_bytes = work_buf[skip + f32_bytes .. skip + f32_bytes + sizes.max_dim];
    const q_ptr: [*]i8 = @ptrCast(q_bytes.ptr);

    var cursor: usize = 0;
    const enc_x = takeF32(f32_pool, &cursor, sizes.enc_seq_elems);
    const enc_sub = takeF32(f32_pool, &cursor, sizes.enc_seq_elems);
    const enc_k = takeF32(f32_pool, &cursor, sizes.enc_seq_elems);
    const enc_v = takeF32(f32_pool, &cursor, sizes.enc_seq_elems);
    const cross_k = takeF32(f32_pool, &cursor, sizes.cross_kv_elems);
    const cross_v = takeF32(f32_pool, &cursor, sizes.cross_kv_elems);
    const self_k = takeF32(f32_pool, &cursor, sizes.self_kv_elems);
    const self_v = takeF32(f32_pool, &cursor, sizes.self_kv_elems);
    const ssru_cell = takeF32(f32_pool, &cursor, sizes.ssru_cell_elems);
    const dec_x = takeF32(f32_pool, &cursor, sizes.d_model);
    const tok_in = takeF32(f32_pool, &cursor, sizes.d_model);
    const tok_q = takeF32(f32_pool, &cursor, sizes.d_model);
    const tok_ctx = takeF32(f32_pool, &cursor, sizes.d_model);
    const tok_out = takeF32(f32_pool, &cursor, sizes.d_model);
    const ffn_tmp = takeF32(f32_pool, &cursor, sizes.d_ff);
    const f32_tmp_a = takeF32(f32_pool, &cursor, sizes.max_dim);
    const f32_tmp_b = takeF32(f32_pool, &cursor, sizes.d_model);
    const attn_scores = takeF32(f32_pool, &cursor, sizes.max_attn_len);

    return .{
        .enc_x = enc_x,
        .enc_sub = enc_sub,
        .enc_k = enc_k,
        .enc_v = enc_v,
        .cross_k = cross_k,
        .cross_v = cross_v,
        .self_k = self_k,
        .self_v = self_v,
        .ssru_cell = ssru_cell,
        .dec_x = dec_x,
        .tok_in = tok_in,
        .tok_q = tok_q,
        .tok_ctx = tok_ctx,
        .tok_out = tok_out,
        .ffn_tmp = ffn_tmp,
        .f32_tmp_a = f32_tmp_a,
        .f32_tmp_b = f32_tmp_b,
        .attn_scores = attn_scores,
        .q_tmp = q_ptr[0..sizes.max_dim],
    };
}

fn takeF32(pool: []f32, cursor: *usize, count: usize) []f32 {
    const start = cursor.*;
    cursor.* = start + count;
    return pool[start..cursor.*];
}

fn runEncoderAndPrecomputeCross(
    view: TmntView,
    sizes: WorkSizes,
    mem: *const WorkMem,
    src_ids: []const u32,
    bytes_read: *u64,
) Error!void {
    const hdr = view.header;
    const d = sizes.d_model;
    const d_ff = sizes.d_ff;
    const src_len = src_ids.len;
    const pre_norm = (hdr.model_flags & cont.flag_pre_norm) != 0;
    const n_heads: usize = @intCast(hdr.num_heads);

    for (src_ids, 0..) |tok_id, s| {
        const dst = mem.enc_x[s * d .. (s + 1) * d];
        try embedInto(view, tok_id, s, dst, mem.f32_tmp_a[0..d], bytes_read);
    }

    var el: usize = 0;
    while (el < sizes.num_enc) : (el += 1) {
        const enc_base = cont.global_tensor_count + el * cont.enc_layer_tensor_count;

        // Pass 1: self-attention K, V across all source tokens.
        var s: usize = 0;
        while (s < src_len) : (s += 1) {
            try prepareBlockInput(
                view,
                mem.enc_x[s * d .. (s + 1) * d],
                mem.tok_in,
                pre_norm,
                enc_base + 8,
                enc_base + 9,
                hdr.ln_eps,
                mem,
                bytes_read,
            );
            try projectMatVec(
                view,
                enc_base + 2,
                enc_base + 3,
                mem.tok_in,
                mem.enc_k[s * d .. (s + 1) * d],
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
            try projectMatVec(
                view,
                enc_base + 4,
                enc_base + 5,
                mem.tok_in,
                mem.enc_v[s * d .. (s + 1) * d],
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
        }

        // Pass 2: Q projection, bidirectional attention, and O projection into `enc_sub`.
        s = 0;
        while (s < src_len) : (s += 1) {
            try prepareBlockInput(
                view,
                mem.enc_x[s * d .. (s + 1) * d],
                mem.tok_in,
                pre_norm,
                enc_base + 8,
                enc_base + 9,
                hdr.ln_eps,
                mem,
                bytes_read,
            );
            try projectMatVec(
                view,
                enc_base + 0,
                enc_base + 1,
                mem.tok_in,
                mem.tok_q,
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
            attendAllHeads(
                mem.tok_q,
                mem.enc_k[0 .. src_len * d],
                mem.enc_v[0 .. src_len * d],
                src_len,
                n_heads,
                d,
                mem.attn_scores[0..src_len],
                mem.tok_ctx,
            );
            try projectMatVec(
                view,
                enc_base + 6,
                enc_base + 7,
                mem.tok_ctx,
                mem.enc_sub[s * d .. (s + 1) * d],
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
        }

        // Residual update (+ post-norm if configured) and per-token FFN sublayer.
        const ffn_base = enc_base + 10;
        s = 0;
        while (s < src_len) : (s += 1) {
            const x_tok = mem.enc_x[s * d .. (s + 1) * d];
            addInPlace(x_tok, mem.enc_sub[s * d .. (s + 1) * d]);
            if (!pre_norm) {
                try applyLayerNormSlot(
                    view,
                    enc_base + 8,
                    enc_base + 9,
                    x_tok,
                    hdr.ln_eps,
                    mem.f32_tmp_a[0..d],
                    mem.f32_tmp_b,
                    bytes_read,
                );
            }

            try prepareBlockInput(
                view,
                x_tok,
                mem.tok_in,
                pre_norm,
                ffn_base + 4,
                ffn_base + 5,
                hdr.ln_eps,
                mem,
                bytes_read,
            );
            try projectMatVec(
                view,
                ffn_base + 0,
                ffn_base + 1,
                mem.tok_in,
                mem.ffn_tmp[0..d_ff],
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
            ops.reluInPlace(mem.ffn_tmp[0..d_ff]);
            try projectMatVec(
                view,
                ffn_base + 2,
                ffn_base + 3,
                mem.ffn_tmp[0..d_ff],
                mem.tok_out,
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
            addInPlace(x_tok, mem.tok_out);
            if (!pre_norm) {
                try applyLayerNormSlot(
                    view,
                    ffn_base + 4,
                    ffn_base + 5,
                    x_tok,
                    hdr.ln_eps,
                    mem.f32_tmp_a[0..d],
                    mem.f32_tmp_b,
                    bytes_read,
                );
            }
        }
    }

    // Optional encoder final LayerNorm (`slots 4, 5`).
    var s: usize = 0;
    while (s < src_len) : (s += 1) {
        try applyOptionalLayerNormPair(
            view,
            4,
            5,
            mem.enc_x[s * d .. (s + 1) * d],
            hdr.ln_eps,
            mem.f32_tmp_a[0..d],
            mem.f32_tmp_b,
            bytes_read,
        );
    }

    // Precompute cross-attention K/V projections once across all decoder layers.
    const dec_start = cont.global_tensor_count + sizes.num_enc * cont.enc_layer_tensor_count;
    const dec_stride = cont.decLayerStride(view.header.model_flags);
    const cross_off_in_layer: usize = if (sizes.is_ssru) 5 else 10;
    var dl: usize = 0;
    while (dl < sizes.num_dec) : (dl += 1) {
        const cross_base = dec_start + dl * dec_stride + cross_off_in_layer;
        const layer_off = dl * sizes.enc_seq_elems;
        s = 0;
        while (s < src_len) : (s += 1) {
            const enc_tok = mem.enc_x[s * d .. (s + 1) * d];
            try projectMatVec(
                view,
                cross_base + 2,
                cross_base + 3,
                enc_tok,
                mem.cross_k[layer_off + s * d .. layer_off + (s + 1) * d],
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
            try projectMatVec(
                view,
                cross_base + 4,
                cross_base + 5,
                enc_tok,
                mem.cross_v[layer_off + s * d .. layer_off + (s + 1) * d],
                mem.q_tmp,
                mem.f32_tmp_a,
                bytes_read,
            );
        }
    }
}

fn runDecoderStep(
    view: TmntView,
    sizes: WorkSizes,
    mem: *const WorkMem,
    tok_id: u32,
    pos: usize,
    src_len: usize,
    bytes_read: *u64,
) Error!void {
    const hdr = view.header;
    const d = sizes.d_model;
    const d_ff = sizes.d_ff;
    const pre_norm = (hdr.model_flags & cont.flag_pre_norm) != 0;
    const zero_bos = pos == 0 and (hdr.model_flags & cont.flag_zero_bos) != 0;
    const n_heads: usize = @intCast(hdr.num_heads);
    const dec_start = cont.global_tensor_count + sizes.num_enc * cont.enc_layer_tensor_count;
    const dec_stride = cont.decLayerStride(hdr.model_flags);

    if (zero_bos) {
        try embedZeroInto(view, pos, mem.dec_x, mem.f32_tmp_a[0..d], bytes_read);
    } else {
        try embedInto(view, tok_id, pos, mem.dec_x, mem.f32_tmp_a[0..d], bytes_read);
    }

    var dl: usize = 0;
    while (dl < sizes.num_dec) : (dl += 1) {
        const dec_base = dec_start + dl * dec_stride;
        const cross_base = dec_base + (if (sizes.is_ssru) @as(usize, 5) else @as(usize, 10));
        const ffn_base = cross_base + 10;

        if (sizes.is_ssru) {
            try runSsruSublayer(view, sizes, mem, dec_base, dl, pre_norm, hdr.ln_eps, bytes_read);
        } else {
            try runSelfAttnSublayer(view, sizes, mem, dec_base, dl, pos, n_heads, pre_norm, hdr.ln_eps, bytes_read);
        }
        try runCrossAttnSublayer(view, sizes, mem, cross_base, dl, src_len, n_heads, pre_norm, hdr.ln_eps, bytes_read);
        try runDecFfnSublayer(view, d, d_ff, mem, ffn_base, pre_norm, hdr.ln_eps, bytes_read);
    }

    // Optional decoder final LayerNorm (`slots 6, 7`).
    try applyOptionalLayerNormPair(
        view,
        6,
        7,
        mem.dec_x,
        hdr.ln_eps,
        mem.f32_tmp_a[0..d],
        mem.f32_tmp_b,
        bytes_read,
    );
}

fn runSsruSublayer(
    view: TmntView,
    sizes: WorkSizes,
    mem: *const WorkMem,
    dec_base: usize,
    dl: usize,
    pre_norm: bool,
    ln_eps: f32,
    bytes_read: *u64,
) Error!void {
    const d = sizes.d_model;
    try prepareBlockInput(view, mem.dec_x, mem.tok_in, pre_norm, dec_base + 3, dec_base + 4, ln_eps, mem, bytes_read);
    try projectMatVecOptBias(view, dec_base + 0, null, mem.tok_in, mem.tok_ctx, mem.q_tmp, mem.f32_tmp_a, bytes_read);
    try projectMatVecOptBias(view, dec_base + 1, dec_base + 2, mem.tok_in, mem.tok_out, mem.q_tmp, mem.f32_tmp_a, bytes_read);

    const cell = mem.ssru_cell[dl * d ..][0..d];
    var i: usize = 0;
    while (i < d) : (i += 1) {
        const gate = ops.sigmoidF32(mem.tok_out[i]);
        const c_next = gate * cell[i] + (1.0 - gate) * mem.tok_ctx[i];
        cell[i] = c_next;
        mem.dec_x[i] += if (c_next > 0.0) c_next else 0.0;
    }
    if (!pre_norm) {
        try applyLayerNormSlot(view, dec_base + 3, dec_base + 4, mem.dec_x, ln_eps, mem.f32_tmp_a[0..d], mem.f32_tmp_b, bytes_read);
    }
}

fn runSelfAttnSublayer(
    view: TmntView,
    sizes: WorkSizes,
    mem: *const WorkMem,
    self_base: usize,
    dl: usize,
    pos: usize,
    n_heads: usize,
    pre_norm: bool,
    ln_eps: f32,
    bytes_read: *u64,
) Error!void {
    const d = sizes.d_model;
    try prepareBlockInput(view, mem.dec_x, mem.tok_in, pre_norm, self_base + 8, self_base + 9, ln_eps, mem, bytes_read);
    try projectMatVec(view, self_base + 0, self_base + 1, mem.tok_in, mem.tok_q, mem.q_tmp, mem.f32_tmp_a, bytes_read);
    const self_layer_off = dl * sizes.max_tgt * d;
    const step_off = self_layer_off + pos * d;
    try projectMatVec(view, self_base + 2, self_base + 3, mem.tok_in, mem.self_k[step_off .. step_off + d], mem.q_tmp, mem.f32_tmp_a, bytes_read);
    try projectMatVec(view, self_base + 4, self_base + 5, mem.tok_in, mem.self_v[step_off .. step_off + d], mem.q_tmp, mem.f32_tmp_a, bytes_read);
    const kv_len = pos + 1;
    attendAllHeads(mem.tok_q, mem.self_k[self_layer_off .. self_layer_off + kv_len * d], mem.self_v[self_layer_off .. self_layer_off + kv_len * d], kv_len, n_heads, d, mem.attn_scores[0..kv_len], mem.tok_ctx);
    try projectMatVec(view, self_base + 6, self_base + 7, mem.tok_ctx, mem.tok_out, mem.q_tmp, mem.f32_tmp_a, bytes_read);
    addInPlace(mem.dec_x, mem.tok_out);
    if (!pre_norm) {
        try applyLayerNormSlot(view, self_base + 8, self_base + 9, mem.dec_x, ln_eps, mem.f32_tmp_a[0..d], mem.f32_tmp_b, bytes_read);
    }
}

fn runCrossAttnSublayer(
    view: TmntView,
    sizes: WorkSizes,
    mem: *const WorkMem,
    cross_base: usize,
    dl: usize,
    src_len: usize,
    n_heads: usize,
    pre_norm: bool,
    ln_eps: f32,
    bytes_read: *u64,
) Error!void {
    const d = sizes.d_model;
    try prepareBlockInput(view, mem.dec_x, mem.tok_in, pre_norm, cross_base + 8, cross_base + 9, ln_eps, mem, bytes_read);
    try projectMatVec(view, cross_base + 0, cross_base + 1, mem.tok_in, mem.tok_q, mem.q_tmp, mem.f32_tmp_a, bytes_read);
    const cross_layer_off = dl * sizes.enc_seq_elems;
    attendAllHeads(mem.tok_q, mem.cross_k[cross_layer_off .. cross_layer_off + src_len * d], mem.cross_v[cross_layer_off .. cross_layer_off + src_len * d], src_len, n_heads, d, mem.attn_scores[0..src_len], mem.tok_ctx);
    try projectMatVec(view, cross_base + 6, cross_base + 7, mem.tok_ctx, mem.tok_out, mem.q_tmp, mem.f32_tmp_a, bytes_read);
    addInPlace(mem.dec_x, mem.tok_out);
    if (!pre_norm) {
        try applyLayerNormSlot(view, cross_base + 8, cross_base + 9, mem.dec_x, ln_eps, mem.f32_tmp_a[0..d], mem.f32_tmp_b, bytes_read);
    }
}

fn runDecFfnSublayer(
    view: TmntView,
    d: usize,
    d_ff: usize,
    mem: *const WorkMem,
    ffn_base: usize,
    pre_norm: bool,
    ln_eps: f32,
    bytes_read: *u64,
) Error!void {
    try prepareBlockInput(view, mem.dec_x, mem.tok_in, pre_norm, ffn_base + 4, ffn_base + 5, ln_eps, mem, bytes_read);
    try projectMatVec(view, ffn_base + 0, ffn_base + 1, mem.tok_in, mem.ffn_tmp[0..d_ff], mem.q_tmp, mem.f32_tmp_a, bytes_read);
    ops.reluInPlace(mem.ffn_tmp[0..d_ff]);
    try projectMatVec(view, ffn_base + 2, ffn_base + 3, mem.ffn_tmp[0..d_ff], mem.tok_out, mem.q_tmp, mem.f32_tmp_a, bytes_read);
    addInPlace(mem.dec_x, mem.tok_out);
    if (!pre_norm) {
        try applyLayerNormSlot(view, ffn_base + 4, ffn_base + 5, mem.dec_x, ln_eps, mem.f32_tmp_a[0..d], mem.f32_tmp_b, bytes_read);
    }
}

fn computeLogits(
    view: TmntView,
    dec_x: []const f32,
    q_tmp: []i8,
    out_logits: []f32,
    bytes_read: *u64,
) Error!void {
    const w_ref = try view.tensorAt(2);
    const w_i8 = view.i8Slice(w_ref);
    const b_ref = try view.tensorAt(3);
    const vocab_size = w_ref.rows;
    ops.matVecI8(
        dec_x,
        w_i8,
        vocab_size,
        w_ref.cols,
        w_ref.scale,
        w_ref.zero_point,
        null,
        q_tmp,
        out_logits[0..vocab_size],
        bytes_read,
    );
    if (b_ref.dtype == .f32) {
        var r: usize = 0;
        while (r < vocab_size) : (r += 1) {
            out_logits[r] += view.f32At(b_ref, r);
        }
        bytes_read.* += @as(u64, vocab_size) * @sizeOf(f32);
    }
}

fn greedyArgmaxLmHead(
    view: TmntView,
    dec_x: []const f32,
    q_tmp: []i8,
    bytes_read: *u64,
) Error!u32 {
    const w_ref = try view.tensorAt(2);
    const w_i8 = view.i8Slice(w_ref);
    const b_ref = try view.tensorAt(3);
    const has_bias = b_ref.dtype == .f32;
    const rows = w_ref.rows;
    const cols = w_ref.cols;

    const q_info = ops.quantizeActSym(dec_x[0..cols], q_tmp[0..cols]);
    const combined_scale: f32 = q_info.scale * w_ref.scale;
    const zp_corr: i32 = @as(i32, w_ref.zero_point) *% q_info.sum_q;

    var best_id: usize = 0;
    var best_score: f32 = 0.0;
    var r: usize = 0;
    while (r < rows) : (r += 1) {
        const row_w = w_i8[r * cols .. (r + 1) * cols];
        const dot = ops.dotI8(q_tmp[0..cols], row_w);
        const corr_i32 = dot -% zp_corr;
        const b_val: f32 = if (has_bias) view.f32At(b_ref, r) else 0.0;
        const score = b_val + combined_scale * @as(f32, @floatFromInt(corr_i32));
        if (r == 0 or score > best_score) {
            best_score = score;
            best_id = r;
        }
    }
    bytes_read.* += @as(u64, rows) * @as(u64, cols);
    if (has_bias) {
        bytes_read.* += @as(u64, rows) * @sizeOf(f32);
    }
    return @intCast(best_id);
}

fn prepareBlockInput(
    view: TmntView,
    src_x: []const f32,
    dst_in: []f32,
    pre_norm: bool,
    ln_g_slot: usize,
    ln_b_slot: usize,
    ln_eps: f32,
    mem: *const WorkMem,
    bytes_read: *u64,
) Error!void {
    @memcpy(dst_in, src_x);
    if (pre_norm) {
        try applyLayerNormSlot(
            view,
            ln_g_slot,
            ln_b_slot,
            dst_in,
            ln_eps,
            mem.f32_tmp_a[0..dst_in.len],
            mem.f32_tmp_b,
            bytes_read,
        );
    }
}

fn embedInto(
    view: TmntView,
    tok_id: u32,
    pos: usize,
    out_vec: []f32,
    f32_tmp: []f32,
    bytes_read: *u64,
) Error!void {
    const d: usize = @intCast(view.header.d_model);
    const emb_ref = try view.tensorAt(0);
    const emb_w = view.i8Slice(emb_ref);
    const pos_ref = try view.tensorAt(1);
    const scale_emb = (view.header.model_flags & cont.flag_scale_embed) != 0;

    if (pos_ref.dtype == .f32) {
        var i: usize = 0;
        while (i < d) : (i += 1) {
            f32_tmp[i] = view.f32At(pos_ref, pos * d + i);
        }
        ops.embedToken(
            tok_id,
            0,
            emb_w,
            emb_ref.scale,
            emb_ref.zero_point,
            f32_tmp[0..d],
            d,
            scale_emb,
            out_vec,
            bytes_read,
        );
    } else {
        ops.embedToken(
            tok_id,
            pos,
            emb_w,
            emb_ref.scale,
            emb_ref.zero_point,
            null,
            d,
            scale_emb,
            out_vec,
            bytes_read,
        );
    }
}

fn embedZeroInto(
    view: TmntView,
    pos: usize,
    out_vec: []f32,
    f32_tmp: []f32,
    bytes_read: *u64,
) Error!void {
    const d: usize = @intCast(view.header.d_model);
    const pos_ref = try view.tensorAt(1);
    if (pos_ref.dtype == .f32) {
        var i: usize = 0;
        while (i < d) : (i += 1) {
            f32_tmp[i] = view.f32At(pos_ref, pos * d + i);
        }
        ops.embedZeroToken(0, f32_tmp[0..d], d, out_vec, bytes_read);
    } else {
        ops.embedZeroToken(pos, null, d, out_vec, bytes_read);
    }
}

fn projectMatVec(
    view: TmntView,
    w_slot: usize,
    b_slot: usize,
    x: []const f32,
    out: []f32,
    q_tmp: []i8,
    f32_tmp: []f32,
    bytes_read: *u64,
) Error!void {
    return projectMatVecOptBias(view, w_slot, b_slot, x, out, q_tmp, f32_tmp, bytes_read);
}

fn projectMatVecOptBias(
    view: TmntView,
    w_slot: usize,
    b_slot_opt: ?usize,
    x: []const f32,
    out: []f32,
    q_tmp: []i8,
    f32_tmp: []f32,
    bytes_read: *u64,
) Error!void {
    const w_ref = try view.tensorAt(w_slot);
    const w_i8 = view.i8Slice(w_ref);
    const b_opt = if (b_slot_opt) |b_slot| try readF32Opt(view, b_slot, f32_tmp) else null;
    ops.matVecI8(
        x,
        w_i8,
        w_ref.rows,
        w_ref.cols,
        w_ref.scale,
        w_ref.zero_point,
        b_opt,
        q_tmp,
        out,
        bytes_read,
    );
}

fn applyLayerNormSlot(
    view: TmntView,
    g_slot: usize,
    b_slot: usize,
    x: []f32,
    ln_eps: f32,
    g_tmp: []f32,
    b_tmp: []f32,
    bytes_read: *u64,
) Error!void {
    const gamma = try readF32Req(view, g_slot, g_tmp);
    const beta = try readF32Req(view, b_slot, b_tmp);
    ops.layerNorm(x, gamma, beta, ln_eps, bytes_read);
}

fn applyOptionalLayerNormPair(
    view: TmntView,
    g_slot: usize,
    b_slot: usize,
    x: []f32,
    ln_eps: f32,
    g_tmp: []f32,
    b_tmp: []f32,
    bytes_read: *u64,
) Error!void {
    const g_ref = try view.tensorAt(g_slot);
    const b_ref = try view.tensorAt(b_slot);
    if ((g_ref.dtype == .none) != (b_ref.dtype == .none)) return Error.ValidationFailed;
    if (g_ref.dtype == .none) return;
    try readF32Into(view, g_ref, g_tmp[0..x.len]);
    try readF32Into(view, b_ref, b_tmp[0..x.len]);
    ops.layerNorm(x, g_tmp[0..x.len], b_tmp[0..x.len], ln_eps, bytes_read);
}

fn readF32Opt(view: TmntView, slot: usize, dst: []f32) Error!?[]const f32 {
    const ref = try view.tensorAt(slot);
    if (ref.dtype == .none) return null;
    if (ref.dtype != .f32) return Error.ValidationFailed;
    const elems = ref.rows * ref.cols;
    try readF32Into(view, ref, dst[0..elems]);
    return dst[0..elems];
}

fn readF32Req(view: TmntView, slot: usize, dst: []f32) Error![]const f32 {
    const ref = try view.tensorAt(slot);
    if (ref.dtype != .f32) return Error.ValidationFailed;
    const elems = ref.rows * ref.cols;
    try readF32Into(view, ref, dst[0..elems]);
    return dst[0..elems];
}

fn readF32Into(view: TmntView, ref: TensorRef, dst: []f32) Error!void {
    const elems = ref.rows * ref.cols;
    if (dst.len < elems) return Error.InvalidSize;
    var i: usize = 0;
    while (i < elems) : (i += 1) {
        dst[i] = view.f32At(ref, i);
    }
}

fn attendAllHeads(
    q_vec: []const f32,
    k_seq: []const f32,
    v_seq: []const f32,
    kv_len: usize,
    num_heads: usize,
    d_model: usize,
    scores: []f32,
    out_ctx: []f32,
) void {
    const head_dim = d_model / num_heads;
    const inv_sqrt_hd: f32 = 1.0 / ops.sqrtF32(@floatFromInt(head_dim));
    var h: usize = 0;
    while (h < num_heads) : (h += 1) {
        const hoff = h * head_dim;
        const q_h = q_vec[hoff .. hoff + head_dim];

        var t: usize = 0;
        while (t < kv_len) : (t += 1) {
            const k_h = k_seq[t * d_model + hoff .. t * d_model + hoff + head_dim];
            var dot: f32 = 0.0;
            var i: usize = 0;
            while (i < head_dim) : (i += 1) {
                dot += q_h[i] * k_h[i];
            }
            scores[t] = dot * inv_sqrt_hd;
        }

        ops.softmaxInPlace(scores[0..kv_len]);

        const out_h = out_ctx[hoff .. hoff + head_dim];
        @memset(out_h, 0.0);
        t = 0;
        while (t < kv_len) : (t += 1) {
            const w = scores[t];
            const v_h = v_seq[t * d_model + hoff .. t * d_model + hoff + head_dim];
            var i: usize = 0;
            while (i < head_dim) : (i += 1) {
                out_h[i] += w * v_h[i];
            }
        }
    }
}

fn addInPlace(dst: []f32, src: []const f32) void {
    for (dst, src) |*d, s| {
        d.* += s;
    }
}

fn checkedMul(a: usize, b: usize) Error!usize {
    return std.math.mul(usize, a, b) catch Error.OutOfRange;
}

fn checkedAdd(a: usize, b: usize) Error!usize {
    return std.math.add(usize, a, b) catch Error.OutOfRange;
}
