//! Deterministic OCR image preprocessing for translation detection and recognition.
//!
//! Performs bounded crop, orthogonal rotation (including vertical CJK column
//! rotation into horizontal reading order), bilinear resizing in fixed-point
//! Q16.16 arithmetic, channel conversion (`gray8` <-> `rgb888`), and affine
//! int8 tensor quantization with a Cortex-M85 Helium MVE path and bit-identical
//! scalar fallback. Zero heap allocation; all functions are pure over slices.

const std = @import("std");
const builtin = @import("builtin");
const err_mod = @import("err");

/// Stack staging capacity (bytes) for fused crop+resize+quantize streaming.
/// Multiple of 12 and 16 so every full chunk saturates SIMD vector loops.
const stage_bytes: usize = 192;

/// Maximum unsigned 8-bit sample value for bilinear clamp invariant check.
const fp_shift: u6 = 16;
const fp_unit: u64 = @as(u64, 1) << fp_shift;
const fp_mask: u64 = fp_unit - 1;
const fp_norm_round: u64 = @as(u64, 1) << 31;
const fp_norm_shift: u6 = 32;

/// Pixel layout supported by OCR preprocessing inputs and outputs.
pub const PixelFormat = enum(u8) {
    gray8 = 1,
    rgb888 = 3,

    /// Decode a raw byte into a known pixel format, rejecting invalid values.
    pub fn fromRaw(raw: u8) ?PixelFormat {
        return switch (raw) {
            1 => .gray8,
            3 => .rgb888,
            else => null,
        };
    }

    /// Return byte count per pixel for tightly packed storage.
    pub fn channels(self: PixelFormat) u8 {
        return @backingInt(self);
    }
};

/// Orthogonal rotation applied after cropping and before destination scaling.
pub const Rotation = enum(u8) {
    none = 0,
    ccw_90 = 1,
    cw_90 = 2,
    rot_180 = 3,

    /// Decode a raw byte into a valid rotation mode.
    pub fn fromRaw(raw: u8) ?Rotation {
        return switch (raw) {
            0 => .none,
            1 => .ccw_90,
            2 => .cw_90,
            3 => .rot_180,
            else => null,
        };
    }

    /// Return the logical source dimensions (rot_w, rot_h) seen by the resizer.
    pub fn rotatedDims(self: Rotation, rect: Rect) struct { w: u16, h: u16 } {
        return switch (self) {
            .none, .rot_180 => .{ .w = rect.width, .h = rect.height },
            .ccw_90, .cw_90 => .{ .w = rect.height, .h = rect.width },
        };
    }
};

/// Coordinate mapping convention for bilinear source sampling centers.
pub const CoordMode = enum(u8) {
    half_pixel = 0,
    align_corners = 1,
    asym_scale = 2,

    /// Decode a raw byte into a known bilinear coordinate mode.
    pub fn fromRaw(raw: u8) ?CoordMode {
        return switch (raw) {
            0 => .half_pixel,
            1 => .align_corners,
            2 => .asym_scale,
            else => null,
        };
    }
};

/// Axis-aligned pixel rectangle inside a decoded source image.
pub const Rect = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    width: u16 = 0,
    height: u16 = 0,
};

/// Source image geometry and row stride metadata (pointer-free for ABI parity).
pub const ImageSpec = extern struct {
    stride_bytes: u32 = 0,
    width: u16 = 0,
    height: u16 = 0,
    format: u8 = @backingInt(PixelFormat.gray8),
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// Target tensor resize, channel conversion, and rotation parameters.
pub const PreprocessCfg = extern struct {
    dst_width: u16 = 0,
    dst_height: u16 = 0,
    dst_format: u8 = @backingInt(PixelFormat.gray8),
    rotation: u8 = @backingInt(Rotation.none),
    coord_mode: u8 = @backingInt(CoordMode.half_pixel),
    reserved: u8 = 0,
};

/// Floating-point input normalization and TFLite affine int8 quantization parameters.
pub const QuantParams = extern struct {
    mean: [3]f32 = .{ 0.0, 0.0, 0.0 },
    inv_std: [3]f32 = .{ 1.0, 1.0, 1.0 },
    input_scale: f32 = 1.0 / 255.0,
    quant_scale: f32 = 1.0 / 128.0,
    zero_point: i8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

const ValidatedSpec = struct {
    stride_bytes: u32,
    width: u16,
    height: u16,
    format: PixelFormat,
};

const ValidatedCfg = struct {
    dst_width: u16,
    dst_height: u16,
    dst_format: PixelFormat,
    rotation: Rotation,
    coord_mode: CoordMode,
};

/// Precomputed per-channel affine constants: `q = clamp(round(x * gain[c] + bias[c]), -128, 127)`.
pub const AffineQuant = struct {
    gain: [3]f32,
    bias: [3]f32,
};

/// Report whether this target enables ARMv8.1-M Helium MVE floating-point SIMD instructions.
pub fn hasHeliumMve() bool {
    return comptime (builtin.cpu.arch == .thumb) and
        std.Target.arm.featureSetHas(builtin.cpu.features, .mve_fp);
}

/// Suggest 90-degree CCW rotation when a cropped text region is taller than wide.
pub fn suggestVerticalRotation(rect: Rect) Rotation {
    return if (rect.height > rect.width) .ccw_90 else .none;
}

/// Compute exact packed destination buffer size for `width x height x format`.
pub fn requiredDstBytes(
    width: u16,
    height: u16,
    format_raw: u8,
) err_mod.TranslateError!usize {
    if (width == 0 or height == 0) return error.InvalidArg;
    const fmt = PixelFormat.fromRaw(format_raw) orelse return error.InvalidArg;
    const px = std.math.mul(usize, @as(usize, width), @as(usize, height)) catch {
        return error.OutOfRange;
    };
    return std.math.mul(usize, px, @as(usize, fmt.channels())) catch error.OutOfRange;
}

fn validateSpec(spec: ImageSpec, buf_len: usize) err_mod.TranslateError!ValidatedSpec {
    if (!std.mem.eql(u8, &spec.reserved, &.{ 0, 0, 0 })) return error.InvalidArg;
    if (spec.width == 0 or spec.height == 0) return error.InvalidArg;
    const fmt = PixelFormat.fromRaw(spec.format) orelse return error.InvalidArg;
    const min_row: u32 = @as(u32, spec.width) * @as(u32, fmt.channels());
    if (spec.stride_bytes < min_row) return error.InvalidArg;
    const need_u64: u64 = (@as(u64, spec.height - 1) * @as(u64, spec.stride_bytes)) +
        @as(u64, min_row);
    if (@as(u64, buf_len) < need_u64) return error.InvalidSize;
    return .{
        .stride_bytes = spec.stride_bytes,
        .width = spec.width,
        .height = spec.height,
        .format = fmt,
    };
}

fn validateRect(spec: ValidatedSpec, rect: Rect) err_mod.TranslateError!void {
    if (rect.width == 0 or rect.height == 0) return error.InvalidArg;
    const end_x: u32 = @as(u32, rect.x) + @as(u32, rect.width);
    const end_y: u32 = @as(u32, rect.y) + @as(u32, rect.height);
    if (end_x > spec.width or end_y > spec.height) return error.OutOfRange;
}

fn validateCfg(cfg: PreprocessCfg) err_mod.TranslateError!ValidatedCfg {
    if (cfg.reserved != 0) return error.InvalidArg;
    if (cfg.dst_width == 0 or cfg.dst_height == 0) return error.InvalidArg;
    const dst_fmt = PixelFormat.fromRaw(cfg.dst_format) orelse return error.InvalidArg;
    const rot = Rotation.fromRaw(cfg.rotation) orelse return error.InvalidArg;
    const coord = CoordMode.fromRaw(cfg.coord_mode) orelse return error.InvalidArg;
    return .{
        .dst_width = cfg.dst_width,
        .dst_height = cfg.dst_height,
        .dst_format = dst_fmt,
        .rotation = rot,
        .coord_mode = coord,
    };
}

/// Validate quantization parameters and fold them into per-channel affine constants.
pub fn buildAffineQuant(
    quant: QuantParams,
    fmt: PixelFormat,
) err_mod.TranslateError!AffineQuant {
    if (!std.mem.eql(u8, &quant.reserved, &.{ 0, 0, 0 })) return error.InvalidArg;
    if (!std.math.isFinite(quant.input_scale) or quant.input_scale <= 0.0) {
        return error.InvalidArg;
    }
    if (!std.math.isFinite(quant.quant_scale) or quant.quant_scale <= 0.0) {
        return error.InvalidArg;
    }

    var gain = [_]f32{ 0.0, 0.0, 0.0 };
    var bias = [_]f32{ 0.0, 0.0, 0.0 };
    const zp_f: f32 = @floatFromInt(quant.zero_point);
    const ch_count: usize = fmt.channels();
    for (0..ch_count) |c| {
        if (!std.math.isFinite(quant.mean[c])) return error.InvalidArg;
        if (!std.math.isFinite(quant.inv_std[c]) or quant.inv_std[c] <= 0.0) {
            return error.InvalidArg;
        }
        gain[c] = (quant.input_scale * quant.inv_std[c]) / quant.quant_scale;
        bias[c] = ((-quant.mean[c] * quant.inv_std[c]) / quant.quant_scale) + zp_f;
        if (!std.math.isFinite(gain[c]) or !std.math.isFinite(bias[c])) {
            return error.OutOfRange;
        }
    }
    return .{ .gain = gain, .bias = bias };
}

fn mapCoordQ16(dst_idx: u16, dst_len: u16, rot_len: u16, mode: CoordMode) u64 {
    if (rot_len <= 1) return 0;
    const max_fp: u64 = @as(u64, rot_len - 1) << fp_shift;
    switch (mode) {
        .half_pixel => {
            const num: i64 = ((@as(i64, dst_idx) * 2 + 1) * @as(i64, rot_len)) -
                @as(i64, dst_len);
            if (num <= 0) return 0;
            const scaled: u64 = (@as(u64, @intCast(num)) << (fp_shift - 1)) /
                @as(u64, dst_len);
            return @min(scaled, max_fp);
        },
        .align_corners => {
            if (dst_len <= 1) return 0;
            return (@as(u64, dst_idx) * max_fp) / @as(u64, dst_len - 1);
        },
        .asym_scale => {
            const raw: u64 = ((@as(u64, dst_idx) * @as(u64, rot_len)) << fp_shift) /
                @as(u64, dst_len);
            return @min(raw, max_fp);
        },
    }
}

fn rotateSamplePointQ16(
    u_fp: u64,
    v_fp: u64,
    rect: Rect,
    rot: Rotation,
) struct { x_fp: u64, y_fp: u64 } {
    const max_lx_fp: u64 = @as(u64, rect.width - 1) << fp_shift;
    const max_ly_fp: u64 = @as(u64, rect.height - 1) << fp_shift;
    const LocalPt = struct { x: u64, y: u64 };
    const local: LocalPt = switch (rot) {
        .none => .{ .x = u_fp, .y = v_fp },
        .ccw_90 => .{ .x = max_lx_fp - v_fp, .y = u_fp },
        .cw_90 => .{ .x = v_fp, .y = max_ly_fp - u_fp },
        .rot_180 => .{ .x = max_lx_fp - u_fp, .y = max_ly_fp - v_fp },
    };
    return .{
        .x_fp = (@as(u64, rect.x) << fp_shift) + local.x,
        .y_fp = (@as(u64, rect.y) << fp_shift) + local.y,
    };
}

fn loadSampleValue(
    src: []const u8,
    byte_off: usize,
    src_fmt: PixelFormat,
    dst_fmt: PixelFormat,
    dst_ch: u8,
) u64 {
    if (src_fmt == .gray8) return src[byte_off];
    if (dst_fmt == .rgb888) return src[byte_off + dst_ch];
    const r: u64 = src[byte_off];
    const g: u64 = src[byte_off + 1];
    const b: u64 = src[byte_off + 2];
    return (77 * r + 150 * g + 29 * b + 128) >> 8;
}

fn bilinearSampleCh(
    src: []const u8,
    spec: ValidatedSpec,
    rect: Rect,
    sx_fp: u64,
    sy_fp: u64,
    dst_fmt: PixelFormat,
    dst_ch: u8,
) u8 {
    const max_x: u16 = rect.x + rect.width - 1;
    const max_y: u16 = rect.y + rect.height - 1;
    const sx0: u16 = @intCast(sx_fp >> fp_shift);
    const sy0: u16 = @intCast(sy_fp >> fp_shift);
    const sx1: u16 = if (sx0 < max_x) sx0 + 1 else max_x;
    const sy1: u16 = if (sy0 < max_y) sy0 + 1 else max_y;
    const fx: u64 = sx_fp & fp_mask;
    const fy: u64 = sy_fp & fp_mask;
    const ifx: u64 = fp_unit - fx;
    const ify: u64 = fp_unit - fy;

    const src_ch: usize = spec.format.channels();
    const row0: usize = @as(usize, sy0) * spec.stride_bytes;
    const row1: usize = @as(usize, sy1) * spec.stride_bytes;
    const col0: usize = @as(usize, sx0) * src_ch;
    const col1: usize = @as(usize, sx1) * src_ch;

    const p00 = loadSampleValue(src, row0 + col0, spec.format, dst_fmt, dst_ch);
    const p10 = loadSampleValue(src, row0 + col1, spec.format, dst_fmt, dst_ch);
    const p01 = loadSampleValue(src, row1 + col0, spec.format, dst_fmt, dst_ch);
    const p11 = loadSampleValue(src, row1 + col1, spec.format, dst_fmt, dst_ch);

    const acc: u64 = (p00 * ifx * ify) + (p10 * fx * ify) + (p01 * ifx * fy) + (p11 * fx * fy);
    return @intCast((acc + fp_norm_round) >> fp_norm_shift);
}

fn samplePixel(
    src: []const u8,
    spec: ValidatedSpec,
    rect: Rect,
    cfg: ValidatedCfg,
    dx: u16,
    dy: u16,
    out_px: []u8,
) void {
    const rot_dims = cfg.rotation.rotatedDims(rect);
    const u_fp = mapCoordQ16(dx, cfg.dst_width, rot_dims.w, cfg.coord_mode);
    const v_fp = mapCoordQ16(dy, cfg.dst_height, rot_dims.h, cfg.coord_mode);
    const sp = rotateSamplePointQ16(u_fp, v_fp, rect, cfg.rotation);
    if (cfg.dst_format == .gray8) {
        out_px[0] = bilinearSampleCh(src, spec, rect, sp.x_fp, sp.y_fp, .gray8, 0);
        return;
    }
    if (spec.format == .gray8) {
        const luma = bilinearSampleCh(src, spec, rect, sp.x_fp, sp.y_fp, .gray8, 0);
        out_px[0] = luma;
        out_px[1] = luma;
        out_px[2] = luma;
        return;
    }
    for (0..3) |c| {
        out_px[c] = bilinearSampleCh(src, spec, rect, sp.x_fp, sp.y_fp, .rgb888, @intCast(c));
    }
}

/// Crop, orthogonally rotate, convert channels, and bilinearly resize `src` into `dst_u8`.
pub fn resizeCropRotateU8(
    src: []const u8,
    spec: ImageSpec,
    rect: Rect,
    cfg: PreprocessCfg,
    dst_u8: []u8,
) err_mod.TranslateError!usize {
    const vspec = try validateSpec(spec, src.len);
    try validateRect(vspec, rect);
    const vcfg = try validateCfg(cfg);
    const needed = try requiredDstBytes(vcfg.dst_width, vcfg.dst_height, @backingInt(vcfg.dst_format));
    if (dst_u8.len < needed) return error.InvalidSize;

    const dst_ch: usize = vcfg.dst_format.channels();
    var off: usize = 0;
    for (0..vcfg.dst_height) |dy| {
        for (0..vcfg.dst_width) |dx| {
            samplePixel(
                src,
                vspec,
                rect,
                vcfg,
                @intCast(dx),
                @intCast(dy),
                dst_u8[off .. off + dst_ch],
            );
            off += dst_ch;
        }
    }
    return needed;
}

fn quantizeScalar(val: u8, gain: f32, bias: f32) i8 {
    const scaled: f32 = (@as(f32, @floatFromInt(val)) * gain) + bias;
    const clamped: f32 = std.math.clamp(scaled, @as(f32, -128.0), @as(f32, 127.0));
    const rounded_i32: i32 = @intFromFloat(@round(clamped));
    return @intCast(rounded_i32);
}

fn quantizeVec4(
    src_four: *const [4]u8,
    gain: @Vector(4, f32),
    bias: @Vector(4, f32),
) [4]i8 {
    const in_u8: @Vector(4, u8) = src_four.*;
    const in_f32: @Vector(4, f32) = @floatFromInt(in_u8);
    const scaled = (in_f32 * gain) + bias;
    const lo: @Vector(4, f32) = @splat(-128.0);
    const hi: @Vector(4, f32) = @splat(127.0);
    const clamped = @min(@max(scaled, lo), hi);
    const as_i32: @Vector(4, i32) = @intFromFloat(@round(clamped));
    const out_i8: @Vector(4, i8) = @intCast(as_i32);
    return out_i8;
}

fn normalizeGrayVector(src: []const u8, aq: AffineQuant, dst: []i8) void {
    const gain_v: @Vector(4, f32) = @splat(aq.gain[0]);
    const bias_v: @Vector(4, f32) = @splat(aq.bias[0]);
    var i: usize = 0;
    while (i + 16 <= src.len) : (i += 16) {
        inline for (0..4) |k| {
            const off = i + k * 4;
            dst[off..][0..4].* = quantizeVec4(src[off..][0..4], gain_v, bias_v);
        }
    }
    while (i + 4 <= src.len) : (i += 4) {
        dst[i..][0..4].* = quantizeVec4(src[i..][0..4], gain_v, bias_v);
    }
    while (i < src.len) : (i += 1) {
        dst[i] = quantizeScalar(src[i], aq.gain[0], aq.bias[0]);
    }
}

fn normalizeRgbVector(src: []const u8, aq: AffineQuant, dst: []i8) void {
    const g0: @Vector(4, f32) = .{ aq.gain[0], aq.gain[1], aq.gain[2], aq.gain[0] };
    const b0: @Vector(4, f32) = .{ aq.bias[0], aq.bias[1], aq.bias[2], aq.bias[0] };
    const g1: @Vector(4, f32) = .{ aq.gain[1], aq.gain[2], aq.gain[0], aq.gain[1] };
    const b1: @Vector(4, f32) = .{ aq.bias[1], aq.bias[2], aq.bias[0], aq.bias[1] };
    const g2: @Vector(4, f32) = .{ aq.gain[2], aq.gain[0], aq.gain[1], aq.gain[2] };
    const b2: @Vector(4, f32) = .{ aq.bias[2], aq.bias[0], aq.bias[1], aq.bias[2] };

    var i: usize = 0;
    while (i + 12 <= src.len) : (i += 12) {
        dst[i..][0..4].* = quantizeVec4(src[i..][0..4], g0, b0);
        dst[i + 4 ..][0..4].* = quantizeVec4(src[i + 4 ..][0..4], g1, b1);
        dst[i + 8 ..][0..4].* = quantizeVec4(src[i + 8 ..][0..4], g2, b2);
    }
    while (i < src.len) : (i += 3) {
        dst[i] = quantizeScalar(src[i], aq.gain[0], aq.bias[0]);
        dst[i + 1] = quantizeScalar(src[i + 1], aq.gain[1], aq.bias[1]);
        dst[i + 2] = quantizeScalar(src[i + 2], aq.gain[2], aq.bias[2]);
    }
}

fn normalizeScalarOnly(src: []const u8, fmt: PixelFormat, aq: AffineQuant, dst: []i8) void {
    if (fmt == .gray8) {
        for (src, 0..) |px, i| {
            dst[i] = quantizeScalar(px, aq.gain[0], aq.bias[0]);
        }
        return;
    }
    var i: usize = 0;
    while (i < src.len) : (i += 3) {
        dst[i] = quantizeScalar(src[i], aq.gain[0], aq.bias[0]);
        dst[i + 1] = quantizeScalar(src[i + 1], aq.gain[1], aq.bias[1]);
        dst[i + 2] = quantizeScalar(src[i + 2], aq.gain[2], aq.bias[2]);
    }
}

/// Normalize and int8-quantize packed `src_u8` into `dst_i8` with explicit SIMD mode.
pub fn normalizeU8ToI8WithMode(
    src_u8: []const u8,
    format_raw: u8,
    quant: QuantParams,
    dst_i8: []i8,
    comptime use_simd: bool,
) err_mod.TranslateError!usize {
    const fmt = PixelFormat.fromRaw(format_raw) orelse return error.InvalidArg;
    const ch: usize = fmt.channels();
    if (src_u8.len == 0 or (src_u8.len % ch) != 0) return error.InvalidArg;
    if (dst_i8.len < src_u8.len) return error.InvalidSize;
    const aq = try buildAffineQuant(quant, fmt);

    if (use_simd) {
        if (fmt == .gray8) {
            normalizeGrayVector(src_u8, aq, dst_i8[0..src_u8.len]);
        } else {
            normalizeRgbVector(src_u8, aq, dst_i8[0..src_u8.len]);
        }
    } else {
        normalizeScalarOnly(src_u8, fmt, aq, dst_i8[0..src_u8.len]);
    }
    return src_u8.len;
}

/// Normalize and int8-quantize packed `src_u8` using Helium MVE when enabled on target.
pub fn normalizeU8ToI8(
    src_u8: []const u8,
    format_raw: u8,
    quant: QuantParams,
    dst_i8: []i8,
) err_mod.TranslateError!usize {
    return normalizeU8ToI8WithMode(src_u8, format_raw, quant, dst_i8, comptime hasHeliumMve());
}

fn normalizeBlockWithAffine(
    src_u8: []const u8,
    fmt: PixelFormat,
    aq: AffineQuant,
    dst_i8: []i8,
    comptime use_simd: bool,
) void {
    if (use_simd) {
        if (fmt == .gray8) {
            normalizeGrayVector(src_u8, aq, dst_i8);
        } else {
            normalizeRgbVector(src_u8, aq, dst_i8);
        }
    } else {
        normalizeScalarOnly(src_u8, fmt, aq, dst_i8);
    }
}

/// Fused crop, rotation, bilinear resize, channel conversion, and int8 quantization.
pub fn preprocessCropRotateToI8(
    src: []const u8,
    spec: ImageSpec,
    rect: Rect,
    cfg: PreprocessCfg,
    quant: QuantParams,
    dst_i8: []i8,
) err_mod.TranslateError!usize {
    const vspec = try validateSpec(spec, src.len);
    try validateRect(vspec, rect);
    const vcfg = try validateCfg(cfg);
    const needed = try requiredDstBytes(vcfg.dst_width, vcfg.dst_height, @backingInt(vcfg.dst_format));
    if (dst_i8.len < needed) return error.InvalidSize;
    const aq = try buildAffineQuant(quant, vcfg.dst_format);

    const dst_ch: usize = vcfg.dst_format.channels();
    const total_px: usize = needed / dst_ch;
    const chunk_max_px: usize = stage_bytes / dst_ch;
    var stage: [stage_bytes]u8 = undefined;
    var px_idx: usize = 0;

    while (px_idx < total_px) {
        const chunk_px = @min(chunk_max_px, total_px - px_idx);
        const chunk_bytes = chunk_px * dst_ch;
        for (0..chunk_px) |k| {
            const p = px_idx + k;
            const dx: u16 = @intCast(p % vcfg.dst_width);
            const dy: u16 = @intCast(p / vcfg.dst_width);
            const off = k * dst_ch;
            samplePixel(src, vspec, rect, vcfg, dx, dy, stage[off .. off + dst_ch]);
        }
        const dst_off = px_idx * dst_ch;
        normalizeBlockWithAffine(
            stage[0..chunk_bytes],
            vcfg.dst_format,
            aq,
            dst_i8[dst_off .. dst_off + chunk_bytes],
            comptime hasHeliumMve(),
        );
        px_idx += chunk_px;
    }
    return needed;
}
