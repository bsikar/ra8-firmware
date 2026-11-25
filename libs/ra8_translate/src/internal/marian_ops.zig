//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding int8 Marian matmul (ARMv8.1-M Helium MVE + bit-identical scalar),
//! symmetric dynamic int8 activation quantization, LayerNorm, softmax, ReLU, and
//! zero-libm IEEE-754 transcendental kernels (`sqrt`, `exp`, `sin`, `cos`).

const builtin = @import("builtin");
const std = @import("std");

/// Report whether this target enables ARMv8.1-M Helium MVE floating-point SIMD.
pub fn hasHeliumMve() bool {
    return comptime (builtin.cpu.arch == .thumb) and
        std.Target.arm.featureSetHas(builtin.cpu.features, .mve_fp);
}

/// Exact scalar signed `i8` dot product accumulated in `i32`.
pub fn dotI8Scalar(a: []const i8, b: []const i8) i32 {
    var acc: i32 = 0;
    for (a, b) |va, vb| {
        acc +%= @as(i32, va) * @as(i32, vb);
    }
    return acc;
}

/// Helium MVE 16-lane SIMD signed `i8` dot product accumulated in `i32`
/// (exact same arithmetic result as `dotI8Scalar`, vectorized via `@Vector(16, i8)`).
pub fn dotI8Vector(a: []const i8, b: []const i8) i32 {
    const VecI8 = @Vector(16, i8);
    const VecI16 = @Vector(16, i16);
    const VecI32 = @Vector(4, i32);

    var acc_vec: VecI32 = @splat(0);
    var i: usize = 0;
    while (i + 16 <= a.len) : (i += 16) {
        const va8: VecI8 = a[i..][0..16].*;
        const vb8: VecI8 = b[i..][0..16].*;
        const va16: VecI16 = va8;
        const vb16: VecI16 = vb8;
        // Max magnitude |-128 * -128| = 16384 fits inside signed i16 without overflow.
        const prod16: VecI16 = va16 * vb16;
        const p0: VecI32 = .{ prod16[0], prod16[1], prod16[2], prod16[3] };
        const p1: VecI32 = .{ prod16[4], prod16[5], prod16[6], prod16[7] };
        const p2: VecI32 = .{ prod16[8], prod16[9], prod16[10], prod16[11] };
        const p3: VecI32 = .{ prod16[12], prod16[13], prod16[14], prod16[15] };
        acc_vec +%= p0 +% p1 +% p2 +% p3;
    }
    var sum: i32 = acc_vec[0] +% acc_vec[1] +% acc_vec[2] +% acc_vec[3];
    while (i < a.len) : (i += 1) {
        sum +%= @as(i32, a[i]) * @as(i32, b[i]);
    }
    return sum;
}

/// Dispatch `dotI8` to Helium MVE SIMD on Cortex-M85 targets or scalar on host.
pub fn dotI8(a: []const i8, b: []const i8) i32 {
    if (comptime hasHeliumMve()) {
        return dotI8Vector(a, b);
    }
    return dotI8Scalar(a, b);
}

/// Quantize a float32 activation vector into symmetric `int8` `[-127, 127]`
/// and return `(scale, sum_xq)` so matrix-vector multiply corrects affine `w_zp` in O(1).
pub fn quantizeActSym(x: []const f32, out_q: []i8) struct { scale: f32, sum_q: i32 } {
    var max_abs: f32 = 0.0;
    for (x) |v| {
        const av = @abs(v);
        if (av > max_abs) max_abs = av;
    }
    if (max_abs == 0.0) {
        @memset(out_q, 0);
        return .{ .scale = 0.0, .sum_q = 0 };
    }
    const scale: f32 = max_abs / 127.0;
    const inv_scale: f32 = 127.0 / max_abs;
    var sum_q: i32 = 0;
    for (x, 0..) |v, idx| {
        const s = v * inv_scale;
        const rounded: i32 = if (s >= 0.0)
            @intFromFloat(std.math.clamp(s + 0.5, 0.0, 127.0))
        else
            -@as(i32, @intFromFloat(std.math.clamp(-s + 0.5, 0.0, 127.0)));
        const qi: i8 = @intCast(std.math.clamp(rounded, -127, 127));
        out_q[idx] = qi;
        sum_q +%= qi;
    }
    return .{ .scale = scale, .sum_q = sum_q };
}

/// Quantized linear projection `out[r] = bias[r] + dot(x, W[r, :])` reading `i8`
/// weights sequentially in place from external memory and accounting exact bytes read.
pub fn matVecI8(
    x_f32: []const f32,
    w_i8: []const i8,
    rows: usize,
    cols: usize,
    w_scale: f32,
    w_zp: i8,
    bias_opt: ?[]const f32,
    scratch_q: []i8,
    out_f32: []f32,
    bytes_read: *u64,
) void {
    const q_info = quantizeActSym(x_f32[0..cols], scratch_q[0..cols]);
    const combined_scale: f32 = q_info.scale * w_scale;
    const zp_corr: i32 = @as(i32, w_zp) *% q_info.sum_q;

    var r: usize = 0;
    while (r < rows) : (r += 1) {
        const row_w = w_i8[r * cols .. (r + 1) * cols];
        const dot = dotI8(scratch_q[0..cols], row_w);
        const corr_i32 = dot -% zp_corr;
        const b_val: f32 = if (bias_opt) |b| b[r] else 0.0;
        out_f32[r] = b_val + combined_scale * @as(f32, @floatFromInt(corr_i32));
    }
    bytes_read.* += @as(u64, rows) * @as(u64, cols);
    if (bias_opt != null) {
        bytes_read.* += @as(u64, rows) * @sizeOf(f32);
    }
}

/// Batched quantized linear projection reading weights and optional bias ONCE for
/// `batch_count` activation vectors `[batch_count * cols] -> [batch_count * rows]`.
pub fn matVecI8Batch(
    x_batch: []const f32,
    batch_count: usize,
    w_i8: []const i8,
    rows: usize,
    cols: usize,
    w_scale: f32,
    w_zp: i8,
    bias_opt: ?[]const f32,
    scratch_q_batch: []i8,
    scratch_scales: []f32,
    scratch_zp_corrs: []i32,
    out_batch: []f32,
    bytes_read: *u64,
) void {
    var b: usize = 0;
    while (b < batch_count) : (b += 1) {
        const q_info = quantizeActSym(
            x_batch[b * cols .. (b + 1) * cols],
            scratch_q_batch[b * cols .. (b + 1) * cols],
        );
        scratch_scales[b] = q_info.scale * w_scale;
        scratch_zp_corrs[b] = @as(i32, w_zp) *% q_info.sum_q;
    }

    var r: usize = 0;
    while (r < rows) : (r += 1) {
        const row_w = w_i8[r * cols .. (r + 1) * cols];
        const b_val: f32 = if (bias_opt) |bias| bias[r] else 0.0;
        b = 0;
        while (b < batch_count) : (b += 1) {
            const dot = dotI8(scratch_q_batch[b * cols .. (b + 1) * cols], row_w);
            const corr_i32 = dot -% scratch_zp_corrs[b];
            out_batch[b * rows + r] = b_val + scratch_scales[b] * @as(f32, @floatFromInt(corr_i32));
        }
    }

    bytes_read.* += @as(u64, rows) * @as(u64, cols);
    if (bias_opt != null) {
        bytes_read.* += @as(u64, rows) * @sizeOf(f32);
    }
}

/// Same projection on an explicitly selected subset of output row IDs (`shortlist` support).
pub fn matVecI8Indexed(
    x_f32: []const f32,
    w_i8: []const i8,
    cols: usize,
    w_scale: f32,
    w_zp: i8,
    bias_opt: ?[]const f32,
    row_ids: []const u32,
    scratch_q: []i8,
    out_f32: []f32,
    bytes_read: *u64,
) void {
    const q_info = quantizeActSym(x_f32[0..cols], scratch_q[0..cols]);
    const combined_scale: f32 = q_info.scale * w_scale;
    const zp_corr: i32 = @as(i32, w_zp) *% q_info.sum_q;

    for (row_ids, 0..) |row_idx, out_idx| {
        const r: usize = @intCast(row_idx);
        const row_w = w_i8[r * cols .. (r + 1) * cols];
        const dot = dotI8(scratch_q[0..cols], row_w);
        const corr_i32 = dot -% zp_corr;
        const b_val: f32 = if (bias_opt) |b| b[r] else 0.0;
        out_f32[out_idx] = b_val + combined_scale * @as(f32, @floatFromInt(corr_i32));
    }
    bytes_read.* += @as(u64, row_ids.len) * @as(u64, cols);
    if (bias_opt != null) {
        bytes_read.* += @as(u64, row_ids.len) * @sizeOf(f32);
    }
}

/// Standard Transformer LayerNorm across a `[d_model]` vector.
pub fn layerNorm(
    x: []f32,
    gamma: []const f32,
    beta: []const f32,
    eps: f32,
    bytes_read: *u64,
) void {
    const d: f64 = @floatFromInt(x.len);
    var sum: f64 = 0.0;
    for (x) |v| {
        sum += @as(f64, v);
    }
    const mean: f32 = @floatCast(sum / d);

    var var_sum: f64 = 0.0;
    for (x) |v| {
        const diff = @as(f64, v - mean);
        var_sum += diff * diff;
    }
    const variance: f32 = @floatCast(var_sum / d);
    const inv_std: f32 = 1.0 / sqrtF32(variance + eps);

    for (x, 0..) |*v, i| {
        v.* = (v.* - mean) * inv_std * gamma[i] + beta[i];
    }
    bytes_read.* += @as(u64, x.len) * 2 * @sizeOf(f32);
}

/// In-place numerically stable softmax over `scores[0..len]`.
pub fn softmaxInPlace(scores: []f32) void {
    if (scores.len == 0) return;
    var max_v: f32 = scores[0];
    for (scores[1..]) |v| {
        if (v > max_v) max_v = v;
    }
    var sum: f64 = 0.0;
    for (scores) |*v| {
        const e = expNegF32(v.* - max_v);
        v.* = e;
        sum += @as(f64, e);
    }
    if (sum <= 0.0) return;
    const inv_sum: f32 = @floatCast(1.0 / sum);
    for (scores) |*v| {
        v.* *= inv_sum;
    }
}

/// In-place ReLU activation (`max(0, x)`).
pub fn reluInPlace(x: []f32) void {
    for (x) |*v| {
        if (v.* < 0.0) v.* = 0.0;
    }
}

/// Lookup token row `tok_id` from int8 embedding matrix and add Vaswani sinusoidal
/// (or explicit baked) positional encoding.
pub fn embedToken(
    tok_id: u32,
    pos: usize,
    embed_w: []const i8,
    embed_scale: f32,
    embed_zp: i8,
    pos_embed_opt: ?[]const f32,
    d_model: usize,
    scale_by_sqrt_d: bool,
    out_vec: []f32,
    bytes_read: *u64,
) void {
    const row_off: usize = @as(usize, tok_id) * d_model;
    const w_row = embed_w[row_off .. row_off + d_model];
    const amp: f32 = if (scale_by_sqrt_d) sqrtF32(@floatFromInt(d_model)) else 1.0;

    if (pos_embed_opt) |pos_tbl| {
        const p_row = pos_tbl[pos * d_model .. (pos + 1) * d_model];
        for (w_row, p_row, 0..) |q, pv, i| {
            const ef = @as(f32, @floatFromInt(@as(i16, q) - @as(i16, embed_zp))) * embed_scale;
            out_vec[i] = ef * amp + pv;
        }
        bytes_read.* += @as(u64, d_model) * @sizeOf(f32);
    } else {
        const half_d = d_model / 2;
        const pos_f: f32 = @floatFromInt(pos);
        // Marian / Vaswani sinusoidal positions: sin in first/even or paired dims.
        // Convention: dims [0 .. half_d) = sin(pos / 10000^(2k/d)), [half_d .. d_model) = cos(...),
        // matching HuggingFace MarianSinusoidalPositionalEmbedding.
        var i: usize = 0;
        while (i < d_model) : (i += 1) {
            const ef = @as(f32, @floatFromInt(@as(i16, w_row[i]) - @as(i16, embed_zp))) * embed_scale;
            var pval: f32 = 0.0;
            if (half_d > 0) {
                const pair_idx = if (i < half_d) i else i - half_d;
                const exponent = -9.210340371976184 * @as(f32, @floatFromInt(pair_idx * 2)) / @as(f32, @floatFromInt(d_model));
                const angle = pos_f * expNegF32(exponent);
                pval = if (i < half_d) sinF32(angle) else cosF32(angle);
            }
            out_vec[i] = ef * amp + pval;
        }
    }
    bytes_read.* += @as(u64, d_model);
}

/// Pure IEEE-754 `sqrt(x)` for `x >= 0.0` avoiding external `libm` calls.
pub fn sqrtF32(x: f32) f32 {
    if (x <= 0.0) return 0.0;
    const bits: u32 = @bitCast(x);
    const guess_bits: u32 = 0x1FBD_1DF5 +% (bits >> 1);
    var y: f32 = @bitCast(guess_bits);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    return y;
}

/// Pure IEEE-754 `exp(d)` for `d <= 0.0` avoiding external `libm` calls.
pub fn expNegF32(d: f32) f32 {
    if (d >= 0.0) return 1.0;
    if (d <= -87.3) return 0.0;
    const log2_e: f32 = 1.4426950408889634;
    const ln2_hi: f32 = 0.693145751953125;
    const ln2_lo: f32 = 1.4286067653301870e-6;
    const z: f32 = d * log2_e;
    const k: i32 = @intFromFloat(z - 0.5);
    const kf: f32 = @floatFromInt(k);
    const r: f32 = (d - kf * ln2_hi) - kf * ln2_lo;
    const poly: f32 = 1.0 + r * (1.0 + r * (0.5 + r * (1.0 / 6.0 + r * (1.0 / 24.0 + r * (1.0 / 120.0 + r * (1.0 / 720.0))))));
    const exp_bits: i32 = std.math.clamp(k + 127, 1, 254);
    const scale: f32 = @bitCast(@as(u32, @intCast(exp_bits)) << 23);
    return scale * poly;
}

/// Pure IEEE-754 `sin(x)` with quadrant reduction and Taylor polynomial (zero `libm`).
pub fn sinF32(x_in: f32) f32 {
    return trigReduceAndEval(x_in, false);
}

/// Pure IEEE-754 `cos(x)` with quadrant reduction and Taylor polynomial (zero `libm`).
pub fn cosF32(x_in: f32) f32 {
    return trigReduceAndEval(x_in, true);
}

/// Pure IEEE-754 `ln(x)` for `x > 0.0` avoiding external `libm` calls.
pub fn lnF32(x: f32) f32 {
    if (x <= 0.0) return -88.0;
    var val: f32 = x;
    var exp: i32 = 0;
    var bits: u32 = @bitCast(val);
    if (((bits >> 23) & 0xFF) == 0) {
        val *= 8388608.0;
        exp -= 23;
        bits = @bitCast(val);
    }
    const raw_exp: i32 = @intCast((bits >> 23) & 0xFF);
    exp += raw_exp - 127;
    var m: f64 = @floatCast(@as(f32, @bitCast((bits & 0x007F_FFFF) | 0x3F80_0000)));
    if (m > 1.4142135623730951) {
        m *= 0.5;
        exp += 1;
    }
    const u: f64 = (m - 1.0) / (m + 1.0);
    const u_sq: f64 = u * u;
    const poly: f64 = 1.0 + u_sq * (1.0 / 3.0 + u_sq * (1.0 / 5.0 + u_sq * (1.0 / 7.0 + u_sq * (1.0 / 9.0))));
    const ln_m: f64 = 2.0 * u * poly;
    return @floatCast(@as(f64, @floatFromInt(exp)) * 0.6931471805599453 + ln_m);
}

fn trigReduceAndEval(x_in: f32, is_cos: bool) f32 {
    const pi_over_2: f64 = 1.5707963267948966;
    const inv_pi_over_2: f64 = 0.6366197723675814;
    const xf: f64 = @floatCast(x_in);
    const qf: f64 = xf * inv_pi_over_2;
    const q: i64 = @intFromFloat(if (qf >= 0.0) qf + 0.5 else qf - 0.5);
    const r: f64 = xf - @as(f64, @floatFromInt(q)) * pi_over_2;
    const r2: f64 = r * r;

    const sin_r = r * (1.0 - r2 * (1.0 / 6.0 - r2 * (1.0 / 120.0 - r2 * (1.0 / 5040.0))));
    const cos_r = 1.0 - r2 * (0.5 - r2 * (1.0 / 24.0 - r2 * (1.0 / 720.0 - r2 * (1.0 / 40320.0))));

    const quad_raw = @mod(q + (if (is_cos) @as(i64, 1) else @as(i64, 0)), 4);
    const quad: u2 = @intCast(quad_raw);
    const val: f64 = switch (quad) {
        0 => sin_r,
        1 => cos_r,
        2 => -sin_r,
        3 => -cos_r,
    };
    return @floatCast(val);
}
