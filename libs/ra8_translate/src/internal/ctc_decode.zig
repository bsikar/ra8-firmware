//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding CTC greedy decoder, per-script `TCHR` character table
//! validator/serializer, and peak-token confidence scorer for `ra8_translate`.

const std = @import("std");
const crc32 = @import("crc32");
const err_mod = @import("err");
const lang_mod = @import("lang");

pub const Error = err_mod.TranslateError;
pub const OcrModel = lang_mod.OcrModel;

pub const magic: u32 = 0x5248_4354; // Little-endian "TCHR".
pub const version: u16 = 1;
pub const header_bytes: usize = 36;
pub const max_classes: u32 = 65_536;
pub const max_glyph_bytes: usize = 64;
pub const index_none: u32 = 0xFFFF_FFFF;
pub const max_conf_bps: u16 = 10_000;
pub const default_low_conf_bps: u16 = 7_500;

pub const flag_skip_unk: u8 = 0x01;
pub const flag_valid_mask: u8 = 0x01;

/// Numeric interpretation of CTC model outputs per frame.
pub const ScoreMode = enum(u8) {
    logits = 0,
    probs = 1,

    /// Parse raw C-ABI score mode byte cleanly.
    pub fn fromRaw(raw: u8) ?ScoreMode {
        return switch (raw) {
            0 => .logits,
            1 => .probs,
            else => null,
        };
    }
};

/// ABI representation of `ra8_translate_ctc_charset_header_t`.
pub const CharsetHeader = extern struct {
    total_bytes: u32 = 0,
    container_crc32: u32 = 0,
    version: u16 = 0,
    flags: u16 = 0,
    class_count: u32 = 0,
    blank_index: u32 = 0,
    unk_index: u32 = index_none,
    string_pool_bytes: u32 = 0,
    ocr_model: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// ABI representation of `ra8_translate_ctc_decode_cfg_t`.
pub const DecodeCfg = extern struct {
    time_steps: u32 = 0,
    class_stride: u32 = 0,
    score_mode: u8 = 0,
    decode_flags: u8 = 0,
    low_confidence_thresh_bps: u16 = default_low_conf_bps,
    expected_ocr_model: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// ABI representation of `ra8_translate_ctc_result_t`.
pub const DecodeResult = extern struct {
    text_bytes: u32 = 0,
    emitted_tokens: u32 = 0,
    blank_frames: u32 = 0,
    unk_count: u32 = 0,
    confidence_bps: u16 = 0,
    min_token_confidence_bps: u16 = 0,
    low_confidence: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// Input options for `writeCharset()`.
pub const CharsetWriteOpts = struct {
    ocr_model: OcrModel,
    blank_index: u32 = 0,
    unk_index: u32 = index_none,
    flags: u16 = 0,
};

/// Borrowed zero-copy view over a validated `TCHR` per-script CTC character table.
pub const CharsetView = struct {
    bytes: []const u8,
    header: CharsetHeader,
    offsets_off: usize,
    pool_off: usize,

    /// Validate wire header, CRC-32 IEEE, class/blank/unk invariants, and UTF-8 glyphs.
    pub fn open(bytes: []const u8) Error!CharsetView {
        if (bytes.len < header_bytes) return Error.InvalidSize;
        if (readU32(bytes, 0) != magic) return Error.ValidationFailed;

        const ver = readU16(bytes, 4);
        const hdr_flags = readU16(bytes, 6);
        const total_bytes = readU32(bytes, 8);
        const recorded_crc = readU32(bytes, 12);
        const ocr_raw = bytes[16];
        const ocr_model = parseScriptOcrModel(ocr_raw) orelse return Error.ValidationFailed;
        _ = ocr_model;
        if (bytes[17] != 0 or bytes[18] != 0 or bytes[19] != 0) return Error.ValidationFailed;
        if (ver != version or hdr_flags != 0) return Error.ValidationFailed;
        if (total_bytes != bytes.len) return Error.InvalidSize;

        const computed_crc = crc32.crc32WithZeroWindow(bytes, 12, 4);
        if (computed_crc != recorded_crc) return Error.CrcMismatch;

        const class_count = readU32(bytes, 20);
        const blank_index = readU32(bytes, 24);
        const unk_index = readU32(bytes, 28);
        const string_pool_bytes = readU32(bytes, 32);

        if (class_count < 2 or class_count > max_classes) return Error.ValidationFailed;
        if (blank_index >= class_count) return Error.ValidationFailed;
        if (unk_index != index_none and (unk_index >= class_count or unk_index == blank_index)) {
            return Error.ValidationFailed;
        }

        const offset_entries = try checkedAdd(@as(usize, class_count), 1);
        const offset_bytes = try checkedMul(offset_entries, 4);
        const pool_off = try checkedAdd(header_bytes, offset_bytes);
        const expected_total = try checkedAdd(pool_off, string_pool_bytes);
        if (expected_total != total_bytes) return Error.InvalidSize;

        var prev_off: u32 = readU32(bytes, header_bytes);
        if (prev_off != 0) return Error.ValidationFailed;

        var idx: u32 = 0;
        while (idx < class_count) : (idx += 1) {
            const next_off = readU32(bytes, header_bytes + (@as(usize, idx) + 1) * 4);
            if (next_off < prev_off or next_off > string_pool_bytes) {
                return Error.ValidationFailed;
            }
            const glyph_len = next_off - prev_off;
            if (glyph_len > max_glyph_bytes) return Error.ValidationFailed;
            if (idx != blank_index and idx != unk_index and glyph_len == 0) {
                return Error.ValidationFailed;
            }
            const glyph_slice = bytes[pool_off + prev_off .. pool_off + next_off];
            if (!std.unicode.utf8ValidateSlice(glyph_slice)) {
                return Error.ValidationFailed;
            }
            prev_off = next_off;
        }
        if (prev_off != string_pool_bytes) return Error.ValidationFailed;

        return .{
            .bytes = bytes,
            .header = .{
                .total_bytes = total_bytes,
                .container_crc32 = recorded_crc,
                .version = ver,
                .flags = hdr_flags,
                .class_count = class_count,
                .blank_index = blank_index,
                .unk_index = unk_index,
                .string_pool_bytes = string_pool_bytes,
                .ocr_model = ocr_raw,
                .reserved = .{ 0, 0, 0 },
            },
            .offsets_off = header_bytes,
            .pool_off = pool_off,
        };
    }

    /// Return the borrowed UTF-8 glyph slice for `class_index`.
    pub fn glyphAt(self: CharsetView, class_index: u32) Error![]const u8 {
        if (class_index >= self.header.class_count) return Error.OutOfRange;
        return self.uncheckedGlyph(class_index);
    }

    fn uncheckedGlyph(self: CharsetView, class_index: u32) []const u8 {
        const entry_off = self.offsets_off + @as(usize, class_index) * 4;
        const start = readU32(self.bytes, entry_off);
        const end = readU32(self.bytes, entry_off + 4);
        return self.bytes[self.pool_off + start .. self.pool_off + end];
    }
};

/// Return the exact serialized byte size for a `TCHR` container.
pub fn requiredCharsetBytes(opts: CharsetWriteOpts, glyphs: []const []const u8) Error!usize {
    try validateWriteInputs(opts, glyphs);
    var pool_bytes: usize = 0;
    for (glyphs) |glyph| {
        pool_bytes = try checkedAdd(pool_bytes, glyph.len);
    }
    const offset_bytes = try checkedMul(glyphs.len + 1, 4);
    const total = try checkedAdd(header_bytes + offset_bytes, pool_bytes);
    if (total > std.math.maxInt(u32)) return Error.InvalidSize;
    return total;
}

/// Serialize a deterministic CRC-32-protected `TCHR` character table into `out`.
pub fn writeCharset(
    opts: CharsetWriteOpts,
    glyphs: []const []const u8,
    out: []u8,
) Error!usize {
    const total = try requiredCharsetBytes(opts, glyphs);
    if (out.len < total) return Error.InvalidSize;

    const class_count: u32 = @intCast(glyphs.len);
    const offsets_bytes = (glyphs.len + 1) * 4;
    const pool_off = header_bytes + offsets_bytes;

    var pool_cursor: u32 = 0;
    writeU32(out, header_bytes, 0);
    for (glyphs, 0..) |glyph, idx| {
        if (glyph.len > 0) {
            @memcpy(out[pool_off + pool_cursor .. pool_off + pool_cursor + glyph.len], glyph);
        }
        pool_cursor += @intCast(glyph.len);
        writeU32(out, header_bytes + (idx + 1) * 4, pool_cursor);
    }

    writeU32(out, 0, magic);
    writeU16(out, 4, version);
    writeU16(out, 6, opts.flags);
    writeU32(out, 8, @intCast(total));
    writeU32(out, 12, 0);
    out[16] = @backingInt(opts.ocr_model);
    out[17] = 0;
    out[18] = 0;
    out[19] = 0;
    writeU32(out, 20, class_count);
    writeU32(out, 24, opts.blank_index);
    writeU32(out, 28, opts.unk_index);
    writeU32(out, 32, pool_cursor);

    const sum = crc32.crc32(out[0..total]);
    writeU32(out, 12, sum);
    return total;
}

/// Greedy-decode quantized `int8` `[T, C]` CTC recognizer output to UTF-8.
pub fn decodeI8(
    charset: CharsetView,
    logits: []const i8,
    quant_scale: f32,
    zero_point: i8,
    cfg: DecodeCfg,
    out_utf8: []u8,
) Error!DecodeResult {
    const valid = try validateDecodeArgs(charset, logits.len, cfg);
    if (!std.math.isFinite(quant_scale) or quant_scale <= 0.0) return Error.InvalidArg;

    const src: I8FrameSource = .{
        .logits = logits,
        .quant_scale = quant_scale,
        .zero_point = zero_point,
    };
    return decodeGeneric(I8FrameSource, src, charset, valid, out_utf8);
}

/// Greedy-decode floating-point `[T, C]` CTC logits or probabilities to UTF-8.
pub fn decodeF32(
    charset: CharsetView,
    logits: []const f32,
    cfg: DecodeCfg,
    out_utf8: []u8,
) Error!DecodeResult {
    const valid = try validateDecodeArgs(charset, logits.len, cfg);

    const src: F32FrameSource = .{ .logits = logits };
    return decodeGeneric(F32FrameSource, src, charset, valid, out_utf8);
}

const ValidDecode = struct {
    time_steps: usize,
    class_count: usize,
    class_stride: usize,
    score_mode: ScoreMode,
    decode_flags: u8,
    low_thresh_bps: u16,
};

const FrameArgmax = struct {
    best_idx: u32,
    prob: f32,
};

const I8FrameSource = struct {
    logits: []const i8,
    quant_scale: f32,
    zero_point: i8,

    fn validateAndArgmax(self: I8FrameSource, row_off: usize, count: usize, mode: ScoreMode) Error!FrameArgmax {
        const row = self.logits[row_off .. row_off + count];
        var best_idx: u32 = 0;
        var best_val: i8 = row[0];
        for (row[1..], 1..) |q, c| {
            if (q > best_val) {
                best_val = q;
                best_idx = @intCast(c);
            }
        }
        const prob: f32 = switch (mode) {
            .logits => blk: {
                var sum_exp: f64 = 0.0;
                for (row) |q| {
                    const diff_steps: f32 = @floatFromInt(@as(i16, q) - @as(i16, best_val));
                    const d = diff_steps * self.quant_scale;
                    sum_exp += @as(f64, expNegF32(d));
                }
                const inv: f64 = if (sum_exp > 0.0) 1.0 / sum_exp else 0.0;
                break :blk std.math.clamp(@as(f32, @floatCast(inv)), 0.0, 1.0);
            },
            .probs => blk: {
                const raw = @as(f32, @floatFromInt(@as(i16, best_val) - @as(i16, self.zero_point))) *
                    self.quant_scale;
                break :blk std.math.clamp(raw, 0.0, 1.0);
            },
        };
        return .{ .best_idx = best_idx, .prob = prob };
    }

    fn fastArgmax(self: I8FrameSource, row_off: usize, count: usize) u32 {
        const row = self.logits[row_off .. row_off + count];
        var best_idx: u32 = 0;
        var best_val: i8 = row[0];
        for (row[1..], 1..) |q, c| {
            if (q > best_val) {
                best_val = q;
                best_idx = @intCast(c);
            }
        }
        return best_idx;
    }
};

const F32FrameSource = struct {
    logits: []const f32,

    fn validateAndArgmax(self: F32FrameSource, row_off: usize, count: usize, mode: ScoreMode) Error!FrameArgmax {
        const row = self.logits[row_off .. row_off + count];
        if (!std.math.isFinite(row[0])) return Error.InvalidArg;
        var best_idx: u32 = 0;
        var best_val: f32 = row[0];
        for (row[1..], 1..) |v, c| {
            if (!std.math.isFinite(v)) return Error.InvalidArg;
            if (v > best_val) {
                best_val = v;
                best_idx = @intCast(c);
            }
        }
        const prob: f32 = switch (mode) {
            .logits => blk: {
                var sum_exp: f64 = 0.0;
                for (row) |v| {
                    sum_exp += @as(f64, expNegF32(v - best_val));
                }
                const inv: f64 = if (sum_exp > 0.0) 1.0 / sum_exp else 0.0;
                break :blk std.math.clamp(@as(f32, @floatCast(inv)), 0.0, 1.0);
            },
            .probs => std.math.clamp(best_val, 0.0, 1.0),
        };
        return .{ .best_idx = best_idx, .prob = prob };
    }

    fn fastArgmax(self: F32FrameSource, row_off: usize, count: usize) u32 {
        const row = self.logits[row_off .. row_off + count];
        var best_idx: u32 = 0;
        var best_val: f32 = row[0];
        for (row[1..], 1..) |v, c| {
            if (v > best_val) {
                best_val = v;
                best_idx = @intCast(c);
            }
        }
        return best_idx;
    }
};

fn decodeGeneric(
    comptime Source: type,
    src: Source,
    charset: CharsetView,
    valid: ValidDecode,
    out_utf8: []u8,
) Error!DecodeResult {
    const blank_idx = charset.header.blank_index;
    const unk_idx = charset.header.unk_index;
    const skip_unk = (valid.decode_flags & flag_skip_unk) != 0;

    // Pass 1: validate all frames, compute collapsed metrics, and verify out_utf8 capacity
    // before writing any byte to caller memory.
    var prev_class: u32 = blank_idx;
    var active_run: bool = false;
    var active_run_peak: f32 = 0.0;

    var required_bytes: usize = 0;
    var emitted_tokens: u32 = 0;
    var blank_frames: u32 = 0;
    var unk_count: u32 = 0;
    var sum_peak_probs: f64 = 0.0;
    var min_peak_prob: f32 = 1.0;

    var t: usize = 0;
    while (t < valid.time_steps) : (t += 1) {
        const row_off = t * valid.class_stride;
        const step = try src.validateAndArgmax(row_off, valid.class_count, valid.score_mode);
        const c_t = step.best_idx;

        if (c_t == blank_idx) {
            if (active_run) {
                sum_peak_probs += @as(f64, active_run_peak);
                if (active_run_peak < min_peak_prob) min_peak_prob = active_run_peak;
                active_run = false;
            }
            blank_frames += 1;
            prev_class = blank_idx;
        } else if (c_t == prev_class and active_run) {
            if (step.prob > active_run_peak) active_run_peak = step.prob;
        } else {
            if (active_run) {
                sum_peak_probs += @as(f64, active_run_peak);
                if (active_run_peak < min_peak_prob) min_peak_prob = active_run_peak;
            }
            active_run = true;
            active_run_peak = step.prob;
            prev_class = c_t;
            emitted_tokens += 1;

            if (c_t == unk_idx) {
                unk_count += 1;
                if (!skip_unk) {
                    const glyph = charset.uncheckedGlyph(c_t);
                    required_bytes = try checkedAdd(required_bytes, glyph.len);
                }
            } else {
                const glyph = charset.uncheckedGlyph(c_t);
                required_bytes = try checkedAdd(required_bytes, glyph.len);
            }
        }
    }
    if (active_run) {
        sum_peak_probs += @as(f64, active_run_peak);
        if (active_run_peak < min_peak_prob) min_peak_prob = active_run_peak;
    }

    if (required_bytes > std.math.maxInt(u32) or required_bytes > out_utf8.len) {
        return Error.InvalidSize;
    }

    // Pass 2: materialize UTF-8 bytes after validation and bounds succeeded.
    var cursor: usize = 0;
    prev_class = blank_idx;
    t = 0;
    while (t < valid.time_steps) : (t += 1) {
        const c_t = src.fastArgmax(t * valid.class_stride, valid.class_count);
        if (c_t == blank_idx) {
            prev_class = blank_idx;
            continue;
        }
        if (c_t == prev_class) continue;
        prev_class = c_t;
        if (c_t == unk_idx and skip_unk) continue;
        const glyph = charset.uncheckedGlyph(c_t);
        if (glyph.len > 0) {
            @memcpy(out_utf8[cursor .. cursor + glyph.len], glyph);
            cursor += glyph.len;
        }
    }

    const conf_bps: u16 = if (emitted_tokens > 0)
        probToBps(@floatCast(sum_peak_probs / @as(f64, @floatFromInt(emitted_tokens))))
    else
        0;
    const min_conf_bps: u16 = if (emitted_tokens > 0) probToBps(min_peak_prob) else 0;
    const is_low: u8 = if (emitted_tokens == 0 or conf_bps < valid.low_thresh_bps) 1 else 0;

    return .{
        .text_bytes = @intCast(cursor),
        .emitted_tokens = emitted_tokens,
        .blank_frames = blank_frames,
        .unk_count = unk_count,
        .confidence_bps = conf_bps,
        .min_token_confidence_bps = min_conf_bps,
        .low_confidence = is_low,
        .reserved = .{ 0, 0, 0 },
    };
}

fn validateDecodeArgs(charset: CharsetView, elem_count: usize, cfg: DecodeCfg) Error!ValidDecode {
    if (cfg.time_steps == 0) return Error.InvalidArg;
    if (cfg.reserved[0] != 0 or cfg.reserved[1] != 0 or cfg.reserved[2] != 0) {
        return Error.InvalidArg;
    }
    const mode = ScoreMode.fromRaw(cfg.score_mode) orelse return Error.InvalidArg;
    if ((cfg.decode_flags & ~flag_valid_mask) != 0) return Error.InvalidArg;
    if (cfg.low_confidence_thresh_bps > max_conf_bps) return Error.InvalidArg;
    if (cfg.expected_ocr_model != 0) {
        _ = parseScriptOcrModel(cfg.expected_ocr_model) orelse return Error.InvalidArg;
        if (cfg.expected_ocr_model != charset.header.ocr_model) return Error.ValidationFailed;
    }

    const class_count: usize = @intCast(charset.header.class_count);
    const class_stride: usize = if (cfg.class_stride == 0)
        class_count
    else if (cfg.class_stride < charset.header.class_count)
        return Error.InvalidArg
    else
        @intCast(cfg.class_stride);

    const time_steps: usize = @intCast(cfg.time_steps);
    const prefix_stride = try checkedMul(time_steps - 1, class_stride);
    const needed_elems = try checkedAdd(prefix_stride, class_count);
    if (elem_count < needed_elems) return Error.InvalidSize;

    return .{
        .time_steps = time_steps,
        .class_count = class_count,
        .class_stride = class_stride,
        .score_mode = mode,
        .decode_flags = cfg.decode_flags,
        .low_thresh_bps = cfg.low_confidence_thresh_bps,
    };
}

fn validateWriteInputs(opts: CharsetWriteOpts, glyphs: []const []const u8) Error!void {
    if (opts.ocr_model == .none) return Error.InvalidArg;
    if (opts.flags != 0) return Error.InvalidArg;
    if (glyphs.len < 2 or glyphs.len > max_classes) return Error.InvalidArg;
    if (opts.blank_index >= glyphs.len) return Error.InvalidArg;
    if (opts.unk_index != index_none and
        (opts.unk_index >= glyphs.len or opts.unk_index == opts.blank_index))
    {
        return Error.InvalidArg;
    }
    for (glyphs, 0..) |glyph, idx| {
        if (glyph.len > max_glyph_bytes) return Error.InvalidArg;
        if (idx != opts.blank_index and idx != opts.unk_index and glyph.len == 0) {
            return Error.InvalidArg;
        }
        if (!std.unicode.utf8ValidateSlice(glyph)) return Error.InvalidArg;
    }
}

fn parseScriptOcrModel(raw: u8) ?OcrModel {
    return switch (raw) {
        @backingInt(OcrModel.korean) => .korean,
        @backingInt(OcrModel.japanese) => .japanese,
        @backingInt(OcrModel.chinese) => .chinese,
        else => null,
    };
}

/// Pure IEEE-754 `exp(d)` for `d <= 0.0` avoiding external `libm` calls on bare-metal targets.
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

fn probToBps(prob: f32) u16 {
    const clamped = std.math.clamp(prob, @as(f32, 0.0), @as(f32, 1.0));
    const scaled = std.math.clamp(
        clamped * @as(f32, @floatFromInt(max_conf_bps)) + @as(f32, 0.5),
        @as(f32, 0.0),
        @as(f32, @floatFromInt(max_conf_bps)),
    );
    return @intFromFloat(scaled);
}

fn checkedAdd(a: usize, b: usize) Error!usize {
    return std.math.add(usize, a, b) catch Error.InvalidSize;
}

fn checkedMul(a: usize, b: usize) Error!usize {
    return std.math.mul(usize, a, b) catch Error.InvalidSize;
}

fn readU16(buf: []const u8, off: usize) u16 {
    return std.mem.readInt(u16, buf[off..][0..2], .little);
}

fn readU32(buf: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, buf[off..][0..4], .little);
}

fn writeU16(buf: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, buf[off..][0..2], v, .little);
}

fn writeU32(buf: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, buf[off..][0..4], v, .little);
}
