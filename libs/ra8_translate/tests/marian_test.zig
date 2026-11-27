//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host unit tests verifying `TMNT` serialization/validation, pure IEEE math
//! approximations, `@Vector` vs scalar `dotI8` parity, PreNorm/PostNorm inference,
//! and independent `f64` Transformer parity for `RA8FW-676`.

const std = @import("std");
const ops = @import("marian_ops");
const cont = @import("marian_container");
const marian = @import("marian");
const err_mod = @import("err");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;
const expectApproxEqAbs = std.testing.expectApproxEqAbs;

fn detRandVal(seed: u32, idx: usize) f32 {
    var x: u32 = seed ^ @as(u32, @truncate(idx *% 0x9E37_79B1));
    x ^= x >> 16;
    x *%= 0x85EB_CA6B;
    x ^= x >> 13;
    x *%= 0xC2B2_AE35;
    x ^= x >> 16;
    const unit: i32 = @as(i32, @intCast(x & 0xFF)) - 128;
    return @as(f32, @floatFromInt(unit)) / 256.0;
}

const BuiltModel = struct {
    bytes: []u8,
    len: usize,
    allocator: std.mem.Allocator,

    fn deinit(self: *BuiltModel) void {
        self.allocator.free(self.bytes);
    }
};

fn quantizeRowI8(
    alloc: std.mem.Allocator,
    len: usize,
    seed: u32,
    zp: i8,
) ![]i8 {
    const buf = try alloc.alloc(i8, len);
    for (buf, 0..) |*dst, i| {
        const v = detRandVal(seed, i);
        const scaled = @as(i32, @intFromFloat(v * 90.0)) + @as(i32, zp);
        const clamped = std.math.clamp(scaled, -127, 127);
        dst.* = @intCast(clamped);
    }
    return buf;
}

fn fillF32(alloc: std.mem.Allocator, len: usize, seed: u32, offset: f32, amp: f32) ![]f32 {
    const buf = try alloc.alloc(f32, len);
    for (buf, 0..) |*dst, i| {
        dst.* = offset + detRandVal(seed, i) * amp;
    }
    return buf;
}

fn buildSyntheticModel(
    alloc: std.mem.Allocator,
    opts: cont.WriteOpts,
    with_pos_embed: bool,
    with_optional_bias: bool,
    with_final_ln: bool,
    seed_base: u32,
) !BuiltModel {
    const total_slots = cont.expectedTensorCount(opts.num_enc_layers, opts.num_dec_layers);
    var inputs = try alloc.alloc(cont.TensorInput, total_slots);
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

    // 0: embed_w, 1: pos_embed, 2: lm_head_w, 3: lm_head_b, 4..7: final LNs
    inputs[0] = .{
        .dtype = .i8,
        .rows = opts.vocab_size,
        .cols = opts.d_model,
        .scale = 0.04,
        .zero_point = -3,
        .i8_data = try quantizeRowI8(alloc, v * d, seed_base + 1, -3),
    };
    if (with_pos_embed) {
        inputs[1] = .{
            .dtype = .f32,
            .rows = opts.max_seq_len,
            .cols = opts.d_model,
            .f32_data = try fillF32(alloc, mseq * d, seed_base + 2, 0.0, 0.25),
        };
    }
    if ((opts.model_flags & cont.flag_tied_embed) != 0) {
        inputs[2] = .{
            .dtype = .i8,
            .rows = opts.vocab_size,
            .cols = opts.d_model,
            .scale = 0.04,
            .zero_point = -3,
            .alias_slot = 0,
        };
    } else {
        inputs[2] = .{
            .dtype = .i8,
            .rows = opts.vocab_size,
            .cols = opts.d_model,
            .scale = 0.035,
            .zero_point = 2,
            .i8_data = try quantizeRowI8(alloc, v * d, seed_base + 3, 2),
        };
    }
    if (with_optional_bias) {
        inputs[3] = .{
            .dtype = .f32,
            .rows = opts.vocab_size,
            .cols = 1,
            .f32_data = try fillF32(alloc, v, seed_base + 4, 0.05, 0.4),
        };
    }
    if (with_final_ln) {
        inputs[4] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try fillF32(alloc, d, seed_base + 5, 1.0, 0.1) };
        inputs[5] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try fillF32(alloc, d, seed_base + 6, 0.0, 0.05) };
        inputs[6] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try fillF32(alloc, d, seed_base + 7, 1.0, 0.1) };
        inputs[7] = .{ .dtype = .f32, .rows = opts.d_model, .cols = 1, .f32_data = try fillF32(alloc, d, seed_base + 8, 0.0, 0.05) };
    }

    var slot: usize = cont.global_tensor_count;
    var layer_seed: u32 = seed_base + 100;
    var el: u8 = 0;
    while (el < opts.num_enc_layers) : (el += 1) {
        try fillProjLn(alloc, inputs[slot .. slot + 10], opts.d_model, with_optional_bias, &layer_seed);
        slot += 10;
        try fillFfnLn(alloc, inputs[slot .. slot + 6], opts.d_model, opts.d_ff, ffn, d, with_optional_bias, &layer_seed);
        slot += 6;
    }
    var dl: u8 = 0;
    while (dl < opts.num_dec_layers) : (dl += 1) {
        try fillProjLn(alloc, inputs[slot .. slot + 10], opts.d_model, with_optional_bias, &layer_seed);
        slot += 10;
        try fillProjLn(alloc, inputs[slot .. slot + 10], opts.d_model, with_optional_bias, &layer_seed);
        slot += 10;
        try fillFfnLn(alloc, inputs[slot .. slot + 6], opts.d_model, opts.d_ff, ffn, d, with_optional_bias, &layer_seed);
        slot += 6;
    }

    const out = try alloc.alloc(u8, 512 * 1024);
    const used = try cont.writeContainer(opts, inputs, out);
    return .{ .bytes = out, .len = used, .allocator = alloc };
}

fn fillProjLn(
    alloc: std.mem.Allocator,
    slice: []cont.TensorInput,
    d_u16: u16,
    with_bias: bool,
    seed: *u32,
) !void {
    const d: usize = @intCast(d_u16);
    var p: usize = 0;
    while (p < 4) : (p += 1) {
        const zp: i8 = @intCast(@as(i32, @intCast(p)) - 2);
        slice[p * 2] = .{
            .dtype = .i8,
            .rows = d_u16,
            .cols = d_u16,
            .scale = 0.025,
            .zero_point = zp,
            .i8_data = try quantizeRowI8(alloc, d * d, seed.*, zp),
        };
        seed.* += 1;
        if (with_bias) {
            slice[p * 2 + 1] = .{
                .dtype = .f32,
                .rows = d_u16,
                .cols = 1,
                .f32_data = try fillF32(alloc, d, seed.*, 0.0, 0.1),
            };
        }
        seed.* += 1;
    }
    slice[8] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try fillF32(alloc, d, seed.*, 1.0, 0.15) };
    seed.* += 1;
    slice[9] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try fillF32(alloc, d, seed.*, 0.0, 0.05) };
    seed.* += 1;
}

fn fillFfnLn(
    alloc: std.mem.Allocator,
    slice: []cont.TensorInput,
    d_u16: u16,
    ffn_u16: u16,
    ffn: usize,
    d: usize,
    with_bias: bool,
    seed: *u32,
) !void {
    slice[0] = .{
        .dtype = .i8,
        .rows = ffn_u16,
        .cols = d_u16,
        .scale = 0.03,
        .zero_point = 1,
        .i8_data = try quantizeRowI8(alloc, ffn * d, seed.*, 1),
    };
    seed.* += 1;
    if (with_bias) {
        slice[1] = .{ .dtype = .f32, .rows = ffn_u16, .cols = 1, .f32_data = try fillF32(alloc, ffn, seed.*, 0.0, 0.1) };
    }
    seed.* += 1;
    slice[2] = .{
        .dtype = .i8,
        .rows = d_u16,
        .cols = ffn_u16,
        .scale = 0.025,
        .zero_point = -1,
        .i8_data = try quantizeRowI8(alloc, d * ffn, seed.*, -1),
    };
    seed.* += 1;
    if (with_bias) {
        slice[3] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try fillF32(alloc, d, seed.*, 0.0, 0.1) };
    }
    seed.* += 1;
    slice[4] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try fillF32(alloc, d, seed.*, 1.0, 0.1) };
    seed.* += 1;
    slice[5] = .{ .dtype = .f32, .rows = d_u16, .cols = 1, .f32_data = try fillF32(alloc, d, seed.*, 0.0, 0.05) };
    seed.* += 1;
}

// Independent `f64` reference transformer reading raw container bytes directly.
fn refStepLogitsF64(
    alloc: std.mem.Allocator,
    view: cont.TmntView,
    src_ids: []const u32,
    prefix_ids: []const u32,
    use_act_quant: bool,
) ![]f64 {
    const h = view.header;
    const d: usize = @intCast(h.d_model);
    const ffn: usize = @intCast(h.d_ff);
    const v: usize = @intCast(h.vocab_size);
    const heads: usize = @intCast(h.num_heads);
    const pre_norm = (h.model_flags & cont.flag_pre_norm) != 0;
    const scale_emb = (h.model_flags & cont.flag_scale_embed) != 0;

    var enc = try alloc.alloc(f64, src_ids.len * d);
    defer alloc.free(enc);
    for (src_ids, 0..) |tok, pos| {
        try refEmbed(view, tok, pos, d, scale_emb, enc[pos * d .. (pos + 1) * d]);
    }

    var el: usize = 0;
    while (el < h.num_enc_layers) : (el += 1) {
        const base = cont.global_tensor_count + el * cont.enc_layer_tensor_count;
        const norm_src = try alloc.alloc(f64, src_ids.len * d);
        defer alloc.free(norm_src);
        var s: usize = 0;
        while (s < src_ids.len) : (s += 1) {
            @memcpy(norm_src[s * d .. (s + 1) * d], enc[s * d .. (s + 1) * d]);
            if (pre_norm) try refLn(view, base + 8, base + 9, h.ln_eps, norm_src[s * d .. (s + 1) * d]);
        }
        const sub = try refSelfAttnSeq(alloc, view, base, norm_src, src_ids.len, d, heads, false, use_act_quant);
        defer alloc.free(sub);
        for (enc, sub) |*dst, sv| dst.* += sv;
        if (!pre_norm) {
            s = 0;
            while (s < src_ids.len) : (s += 1) {
                try refLn(view, base + 8, base + 9, h.ln_eps, enc[s * d .. (s + 1) * d]);
            }
        }

        const ffn_base = base + 10;
        const tmp_in = try alloc.alloc(f64, d);
        defer alloc.free(tmp_in);
        const h_ff = try alloc.alloc(f64, ffn);
        defer alloc.free(h_ff);
        const out_d = try alloc.alloc(f64, d);
        defer alloc.free(out_d);
        s = 0;
        while (s < src_ids.len) : (s += 1) {
            @memcpy(tmp_in, enc[s * d .. (s + 1) * d]);
            if (pre_norm) try refLn(view, ffn_base + 4, ffn_base + 5, h.ln_eps, tmp_in);
            try refMatVec(view, ffn_base + 0, ffn_base + 1, tmp_in, h_ff, use_act_quant);
            for (h_ff) |*fv| {
                if (fv.* < 0.0) fv.* = 0.0;
            }
            try refMatVec(view, ffn_base + 2, ffn_base + 3, h_ff, out_d, use_act_quant);
            for (enc[s * d .. (s + 1) * d], out_d) |*dst, ov| dst.* += ov;
            if (!pre_norm) try refLn(view, ffn_base + 4, ffn_base + 5, h.ln_eps, enc[s * d .. (s + 1) * d]);
        }
    }
    if ((try view.tensorAt(4)).dtype == .f32) {
        var s: usize = 0;
        while (s < src_ids.len) : (s += 1) {
            try refLn(view, 4, 5, h.ln_eps, enc[s * d .. (s + 1) * d]);
        }
    }

    var dec = try alloc.alloc(f64, prefix_ids.len * d);
    defer alloc.free(dec);
    for (prefix_ids, 0..) |tok, pos| {
        try refEmbed(view, tok, pos, d, scale_emb, dec[pos * d .. (pos + 1) * d]);
    }
    const dec_start = cont.global_tensor_count + @as(usize, h.num_enc_layers) * cont.enc_layer_tensor_count;
    var dl: usize = 0;
    while (dl < h.num_dec_layers) : (dl += 1) {
        const base = dec_start + dl * cont.dec_layer_tensor_count;
        const norm_dec = try alloc.alloc(f64, prefix_ids.len * d);
        defer alloc.free(norm_dec);
        var t: usize = 0;
        while (t < prefix_ids.len) : (t += 1) {
            @memcpy(norm_dec[t * d .. (t + 1) * d], dec[t * d .. (t + 1) * d]);
            if (pre_norm) try refLn(view, base + 8, base + 9, h.ln_eps, norm_dec[t * d .. (t + 1) * d]);
        }
        const s_sub = try refSelfAttnSeq(alloc, view, base, norm_dec, prefix_ids.len, d, heads, true, use_act_quant);
        defer alloc.free(s_sub);
        for (dec, s_sub) |*dst, sv| dst.* += sv;
        if (!pre_norm) {
            t = 0;
            while (t < prefix_ids.len) : (t += 1) try refLn(view, base + 8, base + 9, h.ln_eps, dec[t * d .. (t + 1) * d]);
        }

        const c_base = base + 10;
        const norm_c = try alloc.alloc(f64, prefix_ids.len * d);
        defer alloc.free(norm_c);
        t = 0;
        while (t < prefix_ids.len) : (t += 1) {
            @memcpy(norm_c[t * d .. (t + 1) * d], dec[t * d .. (t + 1) * d]);
            if (pre_norm) try refLn(view, c_base + 8, c_base + 9, h.ln_eps, norm_c[t * d .. (t + 1) * d]);
        }
        const c_sub = try refCrossAttnSeq(alloc, view, c_base, norm_c, prefix_ids.len, enc, src_ids.len, d, heads, use_act_quant);
        defer alloc.free(c_sub);
        for (dec, c_sub) |*dst, sv| dst.* += sv;
        if (!pre_norm) {
            t = 0;
            while (t < prefix_ids.len) : (t += 1) try refLn(view, c_base + 8, c_base + 9, h.ln_eps, dec[t * d .. (t + 1) * d]);
        }

        const f_base = base + 20;
        const f_in = try alloc.alloc(f64, d);
        defer alloc.free(f_in);
        const f_hid = try alloc.alloc(f64, ffn);
        defer alloc.free(f_hid);
        const f_out = try alloc.alloc(f64, d);
        defer alloc.free(f_out);
        t = 0;
        while (t < prefix_ids.len) : (t += 1) {
            @memcpy(f_in, dec[t * d .. (t + 1) * d]);
            if (pre_norm) try refLn(view, f_base + 4, f_base + 5, h.ln_eps, f_in);
            try refMatVec(view, f_base + 0, f_base + 1, f_in, f_hid, use_act_quant);
            for (f_hid) |*fv| {
                if (fv.* < 0.0) fv.* = 0.0;
            }
            try refMatVec(view, f_base + 2, f_base + 3, f_hid, f_out, use_act_quant);
            for (dec[t * d .. (t + 1) * d], f_out) |*dst, ov| dst.* += ov;
            if (!pre_norm) try refLn(view, f_base + 4, f_base + 5, h.ln_eps, dec[t * d .. (t + 1) * d]);
        }
    }

    const last = dec[(prefix_ids.len - 1) * d .. prefix_ids.len * d];
    if ((try view.tensorAt(6)).dtype == .f32) {
        try refLn(view, 6, 7, h.ln_eps, last);
    }
    const logits = try alloc.alloc(f64, v);
    try refMatVec(view, 2, 3, last, logits, use_act_quant);
    return logits;
}

fn refEmbed(view: cont.TmntView, tok: u32, pos: usize, d: usize, scale_emb: bool, out: []f64) !void {
    const emb_ref = try view.tensorAt(0);
    const emb_w = view.i8Slice(emb_ref);
    const pos_ref = try view.tensorAt(1);
    const amp: f64 = if (scale_emb) std.math.sqrt(@as(f64, @floatFromInt(d))) else 1.0;
    const half = d / 2;
    const row = emb_w[@as(usize, tok) * d ..][0..d];
    for (row, 0..) |q, i| {
        const ef = @as(f64, @floatFromInt(@as(i16, q) - @as(i16, emb_ref.zero_point))) * @as(f64, emb_ref.scale);
        var pv: f64 = 0.0;
        if (pos_ref.dtype == .f32) {
            pv = @as(f64, view.f32At(pos_ref, pos * d + i));
        } else if (half > 0) {
            const pair_idx = if (i < half) i else i - half;
            const exponent = -@as(f64, @floatFromInt(pair_idx * 2)) * @log(@as(f64, 10000.0)) / @as(f64, @floatFromInt(d));
            const angle = @as(f64, @floatFromInt(pos)) * std.math.exp(exponent);
            pv = if (i < half) std.math.sin(angle) else std.math.cos(angle);
        }
        out[i] = ef * amp + pv;
    }
}

fn refMatVec(view: cont.TmntView, w_slot: usize, b_slot: usize, x: []const f64, out: []f64, use_act_quant: bool) !void {
    const w_ref = try view.tensorAt(w_slot);
    const w_i8 = view.i8Slice(w_ref);
    const b_ref = try view.tensorAt(b_slot);
    const has_b = b_ref.dtype == .f32;
    var act_scale: f64 = 1.0;
    var sum_q: i64 = 0;
    var xq: [256]i8 = undefined;
    if (use_act_quant) {
        var amax: f64 = 0.0;
        for (x) |xv| amax = @max(amax, @abs(xv));
        if (amax > 0.0) {
            act_scale = amax / 127.0;
            const inv = 127.0 / amax;
            for (x, 0..) |xv, i| {
                const qi: i32 = @intFromFloat(@round(std.math.clamp(xv * inv, -127.0, 127.0)));
                xq[i] = @intCast(qi);
                sum_q += qi;
            }
        } else {
            @memset(xq[0..x.len], 0);
        }
    }

    var r: usize = 0;
    while (r < w_ref.rows) : (r += 1) {
        var acc: f64 = 0.0;
        if (use_act_quant) {
            var dot: i64 = 0;
            var c: usize = 0;
            while (c < w_ref.cols) : (c += 1) {
                dot += @as(i64, xq[c]) * @as(i64, w_i8[r * w_ref.cols + c]);
            }
            const corr = dot - @as(i64, w_ref.zero_point) * sum_q;
            acc = act_scale * @as(f64, w_ref.scale) * @as(f64, @floatFromInt(corr));
        } else {
            var c: usize = 0;
            while (c < w_ref.cols) : (c += 1) {
                const wv = @as(f64, @floatFromInt(@as(i16, w_i8[r * w_ref.cols + c]) - @as(i16, w_ref.zero_point))) * @as(f64, w_ref.scale);
                acc += x[c] * wv;
            }
        }
        const bv: f64 = if (has_b) @as(f64, view.f32At(b_ref, r)) else 0.0;
        out[r] = bv + acc;
    }
}

fn refLn(view: cont.TmntView, g_slot: usize, b_slot: usize, eps: f32, x: []f64) !void {
    const g_ref = try view.tensorAt(g_slot);
    const b_ref = try view.tensorAt(b_slot);
    var sum: f64 = 0.0;
    for (x) |xv| sum += xv;
    const mean = sum / @as(f64, @floatFromInt(x.len));
    var vs: f64 = 0.0;
    for (x) |xv| vs += (xv - mean) * (xv - mean);
    const inv = 1.0 / std.math.sqrt(vs / @as(f64, @floatFromInt(x.len)) + @as(f64, eps));
    for (x, 0..) |*xv, i| {
        xv.* = (xv.* - mean) * inv * @as(f64, view.f32At(g_ref, i)) + @as(f64, view.f32At(b_ref, i));
    }
}

fn refSelfAttnSeq(
    alloc: std.mem.Allocator,
    view: cont.TmntView,
    base: usize,
    seq: []const f64,
    len: usize,
    d: usize,
    heads: usize,
    causal: bool,
    use_act_quant: bool,
) ![]f64 {
    const q = try alloc.alloc(f64, len * d);
    defer alloc.free(q);
    const k = try alloc.alloc(f64, len * d);
    defer alloc.free(k);
    const v = try alloc.alloc(f64, len * d);
    defer alloc.free(v);
    var t: usize = 0;
    while (t < len) : (t += 1) {
        try refMatVec(view, base + 0, base + 1, seq[t * d ..][0..d], q[t * d ..][0..d], use_act_quant);
        try refMatVec(view, base + 2, base + 3, seq[t * d ..][0..d], k[t * d ..][0..d], use_act_quant);
        try refMatVec(view, base + 4, base + 5, seq[t * d ..][0..d], v[t * d ..][0..d], use_act_quant);
    }
    return refAttendAndOut(alloc, view, base + 6, base + 7, q, len, k, v, len, d, heads, causal, use_act_quant);
}

fn refCrossAttnSeq(
    alloc: std.mem.Allocator,
    view: cont.TmntView,
    base: usize,
    q_seq: []const f64,
    q_len: usize,
    kv_seq: []const f64,
    kv_len: usize,
    d: usize,
    heads: usize,
    use_act_quant: bool,
) ![]f64 {
    const q = try alloc.alloc(f64, q_len * d);
    defer alloc.free(q);
    const k = try alloc.alloc(f64, kv_len * d);
    defer alloc.free(k);
    const v = try alloc.alloc(f64, kv_len * d);
    defer alloc.free(v);
    var t: usize = 0;
    while (t < q_len) : (t += 1) {
        try refMatVec(view, base + 0, base + 1, q_seq[t * d ..][0..d], q[t * d ..][0..d], use_act_quant);
    }
    t = 0;
    while (t < kv_len) : (t += 1) {
        try refMatVec(view, base + 2, base + 3, kv_seq[t * d ..][0..d], k[t * d ..][0..d], use_act_quant);
        try refMatVec(view, base + 4, base + 5, kv_seq[t * d ..][0..d], v[t * d ..][0..d], use_act_quant);
    }
    return refAttendAndOut(alloc, view, base + 6, base + 7, q, q_len, k, v, kv_len, d, heads, false, use_act_quant);
}

fn refAttendAndOut(
    alloc: std.mem.Allocator,
    view: cont.TmntView,
    ow: usize,
    ob: usize,
    q: []const f64,
    q_len: usize,
    k: []const f64,
    v: []const f64,
    kv_len: usize,
    d: usize,
    heads: usize,
    causal: bool,
    use_act_quant: bool,
) ![]f64 {
    const out = try alloc.alloc(f64, q_len * d);
    const ctx = try alloc.alloc(f64, d);
    defer alloc.free(ctx);
    const scores = try alloc.alloc(f64, kv_len);
    defer alloc.free(scores);
    const hd = d / heads;
    const inv_sqrt = 1.0 / std.math.sqrt(@as(f64, @floatFromInt(hd)));

    var qi: usize = 0;
    while (qi < q_len) : (qi += 1) {
        const limit = if (causal) qi + 1 else kv_len;
        var h: usize = 0;
        while (h < heads) : (h += 1) {
            const hoff = h * hd;
            var mx: f64 = -std.math.inf(f64);
            var ki: usize = 0;
            while (ki < limit) : (ki += 1) {
                var dot: f64 = 0.0;
                var i: usize = 0;
                while (i < hd) : (i += 1) dot += q[qi * d + hoff + i] * k[ki * d + hoff + i];
                scores[ki] = dot * inv_sqrt;
                if (scores[ki] > mx) mx = scores[ki];
            }
            var sum_e: f64 = 0.0;
            ki = 0;
            while (ki < limit) : (ki += 1) {
                scores[ki] = std.math.exp(scores[ki] - mx);
                sum_e += scores[ki];
            }
            @memset(ctx[hoff .. hoff + hd], 0.0);
            ki = 0;
            while (ki < limit) : (ki += 1) {
                const p = scores[ki] / sum_e;
                var i: usize = 0;
                while (i < hd) : (i += 1) ctx[hoff + i] += p * v[ki * d + hoff + i];
            }
        }
        try refMatVec(view, ow, ob, ctx, out[qi * d ..][0..d], use_act_quant);
    }
    return out;
}

test "pure IEEE math helpers and Vector vs Scalar dotI8 parity" {
    const sq_inputs = [_]f32{ 0.0, 0.0625, 1.0, 2.0, 4.0, 17.5, 512.0 };
    for (sq_inputs) |x| {
        try expectApproxEqAbs(std.math.sqrt(x), ops.sqrtF32(x), 1e-4);
    }
    const exp_inputs = [_]f32{ 0.0, -0.5, -1.0, -4.5, -12.0, -30.0 };
    for (exp_inputs) |x| {
        try expectApproxEqAbs(std.math.exp(x), ops.expNegF32(x), 1e-4);
    }
    const sig_inputs = [_]f32{ -20.0, -4.0, -1.0, 0.0, 1.0, 4.0, 20.0 };
    for (sig_inputs) |x| {
        const ref_sig: f32 = @floatCast(1.0 / (1.0 + std.math.exp(-@as(f64, x))));
        try expectApproxEqAbs(ref_sig, ops.sigmoidF32(x), 1e-4);
    }
    const trig_inputs = [_]f32{ -10.0, -3.14159, -0.7, 0.0, 0.5, 2.5, 12.34, 57.2 };
    for (trig_inputs) |x| {
        try expectApproxEqAbs(std.math.sin(x), ops.sinF32(x), 5e-4);
        try expectApproxEqAbs(std.math.cos(x), ops.cosF32(x), 5e-4);
    }

    var a_buf: [67]i8 = undefined;
    var b_buf: [67]i8 = undefined;
    for (&a_buf, &b_buf, 0..) |*a, *b, i| {
        a.* = @intCast(@as(i32, @intCast((i * 19) % 255)) - 127);
        b.* = @intCast(@as(i32, @intCast((i * 37) % 255)) - 127);
    }
    const lens = [_]usize{ 0, 1, 7, 15, 16, 17, 32, 47, 67 };
    for (lens) |len| {
        try expectEqual(ops.dotI8Scalar(a_buf[0..len], b_buf[0..len]), ops.dotI8Vector(a_buf[0..len], b_buf[0..len]));
    }
}

test "TMNT CRC validation, payload guard, and unaligned load" {
    const alloc = std.testing.allocator;
    var model = try buildSyntheticModel(alloc, .{
        .num_heads = 2,
        .num_enc_layers = 1,
        .num_dec_layers = 1,
        .d_model = 8,
        .d_ff = 16,
        .max_seq_len = 12,
        .vocab_size = 10,
    }, false, true, false, 11);
    defer model.deinit();

    // Verify shifted unaligned copy (`raw[1..]`) parses and matches aligned logits.
    const unaligned = try alloc.alloc(u8, model.len + 1);
    defer alloc.free(unaligned);
    @memcpy(unaligned[1..], model.bytes[0..model.len]);

    const view_a = try cont.TmntView.open(model.bytes[0..model.len], cont.open_verify_payload);
    const view_u = try cont.TmntView.open(unaligned[1..], cont.open_verify_payload);

    const wbytes = try marian.minWorkBytes(view_a, 4, 4);
    const work = try alloc.alloc(u8, wbytes + 1);
    defer alloc.free(work);

    var logits_a: [10]f32 = undefined;
    var logits_u: [10]f32 = undefined;
    try marian.evalStepLogits(view_a, &.{ 4, 5, 6 }, &.{1}, work[0..wbytes], &logits_a);
    try marian.evalStepLogits(view_u, &.{ 4, 5, 6 }, &.{1}, work[1..], &logits_u);
    for (logits_a, logits_u) |a, b| try expectApproxEqAbs(a, b, 1e-6);

    // Weight byte corruption passes meta_only but fails open_verify_payload.
    model.bytes[model.len - 2] ^= 0x55;
    _ = try cont.TmntView.open(model.bytes[0..model.len], 0);
    try expectEqual(marian.Error.CrcMismatch, cont.TmntView.open(model.bytes[0..model.len], cont.open_verify_payload));

    // Header corruption always fails meta CRC.
    model.bytes[40] ^= 0x01;
    try expectEqual(marian.Error.CrcMismatch, cont.TmntView.open(model.bytes[0..model.len], 0));
}

test "Post-norm & Pre-norm inference matches independent f64 Transformer + greedy consistency" {
    const alloc = std.testing.allocator;
    const cases = [_]struct {
        opts: cont.WriteOpts,
        pos_emb: bool,
        bias: bool,
        final_ln: bool,
        seed: u32,
    }{
        .{
            .opts = .{
                .nmt_model = 2,
                .model_flags = cont.flag_tied_embed,
                .num_heads = 2,
                .num_enc_layers = 2,
                .num_dec_layers = 2,
                .d_model = 8,
                .d_ff = 16,
                .max_seq_len = 16,
                .vocab_size = 12,
                .bos_id = 1,
                .eos_id = 2,
            },
            .pos_emb = false,
            .bias = true,
            .final_ln = false,
            .seed = 42,
        },
        .{
            .opts = .{
                .nmt_model = 1,
                .model_flags = cont.flag_pre_norm | cont.flag_scale_embed,
                .num_heads = 2,
                .num_enc_layers = 2,
                .num_dec_layers = 2,
                .d_model = 8,
                .d_ff = 16,
                .max_seq_len = 16,
                .vocab_size = 12,
                .bos_id = 1,
                .eos_id = 2,
            },
            .pos_emb = true,
            .bias = false,
            .final_ln = true,
            .seed = 99,
        },
    };

    for (cases) |c| {
        var model = try buildSyntheticModel(alloc, c.opts, c.pos_emb, c.bias, c.final_ln, c.seed);
        defer model.deinit();
        const view = try cont.TmntView.open(model.bytes[0..model.len], cont.open_verify_payload);

        const src = [_]u32{ 4, 7, 9, 2 };
        const prefix = [_]u32{ 1, 5, 8 };
        const wbytes = try marian.minWorkBytes(view, 8, 8);
        const work = try alloc.alloc(u8, wbytes);
        defer alloc.free(work);

        var got_logits: [12]f32 = undefined;
        try marian.evalStepLogits(view, &src, &prefix, work, &got_logits);

        const ref_q = try refStepLogitsF64(alloc, view, &src, &prefix, true);
        defer alloc.free(ref_q);
        const ref_f64 = try refStepLogitsF64(alloc, view, &src, &prefix, false);
        defer alloc.free(ref_f64);

        var max_arg_got: usize = 0;
        var max_arg_f64: usize = 0;
        for (got_logits, ref_q, ref_f64, 0..) |g, rq, rf, idx| {
            try expectApproxEqAbs(@as(f32, @floatCast(rq)), g, 2e-3);
            if (g > got_logits[max_arg_got]) max_arg_got = idx;
            if (rf > ref_f64[max_arg_f64]) max_arg_f64 = idx;
        }
        try expectEqual(max_arg_f64, max_arg_got);

        // Step-by-step greedy decode matches per-step argmax(evalStepLogits).
        var out_ids: [5]u32 = undefined;
        const stats = try marian.greedyDecode(view, &src, 5, work, &out_ids);
        try expect(stats.generated_tokens >= 1 and stats.generated_tokens <= 5);
        try expect(stats.weight_bytes_read > 0);

        var check_prefix = [_]u32{ c.opts.bos_id, 0, 0, 0, 0, 0 };
        var step: usize = 0;
        while (step < stats.generated_tokens) : (step += 1) {
            try marian.evalStepLogits(view, &src, check_prefix[0 .. step + 1], work, &got_logits);
            var best: usize = 0;
            for (got_logits, 0..) |lv, li| {
                if (lv > got_logits[best]) best = li;
            }
            try expectEqual(@as(u32, @intCast(best)), out_ids[step]);
            check_prefix[step + 1] = out_ids[step];
        }
    }
}

test "fail-closed error guards keep outputs untouched" {
    const alloc = std.testing.allocator;
    var model = try buildSyntheticModel(alloc, .{
        .d_model = 4,
        .d_ff = 8,
        .max_seq_len = 8,
        .vocab_size = 6,
    }, false, true, false, 77);
    defer model.deinit();

    const view = try cont.TmntView.open(model.bytes[0..model.len], 0);
    const wbytes = try marian.minWorkBytes(view, 4, 4);
    const wbytes_small = try marian.minWorkBytes(view, 1, 1);
    const work = try alloc.alloc(u8, wbytes);
    defer alloc.free(work);

    var logits = [_]f32{ -99.0, -99.0, -99.0, -99.0, -99.0, -99.0 };
    try expectEqual(marian.Error.OutOfRange, marian.evalStepLogits(view, &.{ 1, 99 }, &.{1}, work, &logits));
    try expectEqual(marian.Error.InvalidSize, marian.evalStepLogits(view, &.{1}, &.{1}, work[0 .. wbytes_small - 1], &logits));
    for (logits) |lv| try expectEqual(@as(f32, -99.0), lv);
}
