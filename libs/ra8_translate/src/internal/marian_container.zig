//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `TMNT` int8 Marian NMT container reader, CRC-32 validator, shortlist CSR
//! validator, and deterministic test/tool serializer.

const std = @import("std");
const crc32 = @import("crc32");
const err_mod = @import("err");

pub const Error = err_mod.TranslateError;

pub const magic: u32 = 0x544E_4D54; // Little-endian "TMNT".
pub const version: u16 = 1;
pub const header_bytes: usize = 84;
pub const tensor_desc_bytes: usize = 24;
pub const max_layers: u8 = 32;
pub const max_d_model: u16 = 2048;
pub const max_d_ff: u16 = 8192;
pub const max_seq_len: u16 = 2048;

pub const flag_pre_norm: u16 = 0x01;
pub const flag_scale_embed: u16 = 0x02;
pub const flag_tied_embed: u16 = 0x04;
pub const flag_valid_mask: u16 = 0x07;

pub const open_verify_payload: u8 = 0x01;
pub const open_valid_mask: u8 = 0x01;

pub const global_tensor_count: usize = 8;
pub const enc_layer_tensor_count: usize = 16;
pub const dec_layer_tensor_count: usize = 26;

/// Wire element encoding inside `TMNT` tensor descriptors.
pub const DType = enum(u8) {
    none = 0,
    i8 = 1,
    f32 = 2,

    /// Parse raw byte into `DType`.
    pub fn fromRaw(raw: u8) ?DType {
        return switch (raw) {
            0 => .none,
            1 => .i8,
            2 => .f32,
            else => null,
        };
    }
};

/// ABI metadata struct matching `ra8_translate_marian_header_t`.
pub const MarianHeader = extern struct {
    total_bytes: u32 = 0,
    meta_crc32: u32 = 0,
    payload_crc32: u32 = 0,
    vocab_size: u32 = 0,
    unk_id: u32 = 0,
    bos_id: u32 = 0,
    eos_id: u32 = 0,
    pad_id: u32 = 0,
    shortlist_off: u32 = 0,
    shortlist_bytes: u32 = 0,
    ln_eps: f32 = 1e-5,
    version: u16 = 0,
    model_flags: u16 = 0,
    d_model: u16 = 0,
    d_ff: u16 = 0,
    max_seq: u16 = 0,
    nmt_model: u8 = 0,
    num_heads: u8 = 0,
    num_enc_layers: u8 = 0,
    num_dec_layers: u8 = 0,
    reserved: [6]u8 = .{ 0, 0, 0, 0, 0, 0 },
};

/// ABI execution stats struct matching `ra8_translate_marian_stats_t`.
pub const MarianStats = extern struct {
    weight_bytes_read: u64 = 0,
    generated_tokens: u32 = 0,
    stopped_on_eos: u8 = 0,
    used_helium_mve: u8 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

/// Parsed tensor slice metadata referencing bytes inside a `TMNT` container.
pub const TensorRef = struct {
    dtype: DType,
    data_off: usize,
    data_bytes: usize,
    rows: usize,
    cols: usize,
    scale: f32,
    zero_point: i8,
};

/// Builder input descriptor for writing deterministic test/tool `TMNT` containers.
pub const TensorInput = struct {
    dtype: DType = .none,
    rows: u32 = 0,
    cols: u32 = 0,
    scale: f32 = 1.0,
    zero_point: i8 = 0,
    i8_data: []const i8 = &.{},
    f32_data: []const f32 = &.{},
    alias_slot: ?usize = null,
};

/// Builder options for `writeContainer()`.
pub const WriteOpts = struct {
    nmt_model: u8 = 1,
    model_flags: u16 = 0,
    num_heads: u8 = 1,
    num_enc_layers: u8 = 1,
    num_dec_layers: u8 = 1,
    d_model: u16 = 4,
    d_ff: u16 = 8,
    max_seq_len: u16 = 16,
    vocab_size: u32 = 8,
    unk_id: u32 = 0,
    bos_id: u32 = 1,
    eos_id: u32 = 2,
    pad_id: u32 = 3,
    ln_eps: f32 = 1e-5,
};

/// Zero-copy validated view over a `TMNT` Marian NMT container in external memory.
pub const TmntView = struct {
    bytes: []const u8,
    header: MarianHeader,
    dir_off: usize,
    tensor_count: usize,
    weights_off: usize,
    weights_bytes: usize,

    /// Open and validate a `TMNT` container (`open_flags` controls full payload CRC-32 check).
    pub fn open(bytes: []const u8, open_flags: u8) Error!TmntView {
        if ((open_flags & ~open_valid_mask) != 0) return Error.InvalidArg;
        if (bytes.len < header_bytes) return Error.InvalidSize;
        if (readU32(bytes, 0) != magic) return Error.ValidationFailed;

        const ver = readU16(bytes, 4);
        const model_flags = readU16(bytes, 6);
        const total_bytes = readU32(bytes, 8);
        const meta_crc = readU32(bytes, 12);
        const payload_crc = readU32(bytes, 16);
        const nmt_model = bytes[20];
        const num_heads = bytes[21];
        const num_enc = bytes[22];
        const num_dec = bytes[23];
        const d_model = readU16(bytes, 24);
        const d_ff = readU16(bytes, 26);
        const max_seq = readU16(bytes, 28);
        const reserved0 = readU16(bytes, 30);
        const vocab_size = readU32(bytes, 32);
        const unk_id = readU32(bytes, 36);
        const bos_id = readU32(bytes, 40);
        const eos_id = readU32(bytes, 44);
        const pad_id = readU32(bytes, 48);
        const ln_eps = readF32(bytes, 52);
        const vocab_off = readU32(bytes, 56);
        const vocab_bytes = readU32(bytes, 60);
        const sl_off = readU32(bytes, 64);
        const sl_bytes = readU32(bytes, 68);
        const tensor_count = readU32(bytes, 72);
        const weights_off = readU32(bytes, 76);
        const weights_bytes = readU32(bytes, 80);

        if (ver != version or reserved0 != 0) return Error.ValidationFailed;
        if ((model_flags & ~flag_valid_mask) != 0) return Error.ValidationFailed;
        if (total_bytes != bytes.len) return Error.InvalidSize;
        if (nmt_model < 1 or nmt_model > 3) return Error.ValidationFailed;
        if (num_heads == 0 or d_model == 0 or d_model > max_d_model or (d_model % num_heads) != 0) {
            return Error.ValidationFailed;
        }
        if (d_ff == 0 or d_ff > max_d_ff) return Error.ValidationFailed;
        if (num_enc == 0 or num_enc > max_layers or num_dec == 0 or num_dec > max_layers) {
            return Error.ValidationFailed;
        }
        if (max_seq < 2 or max_seq > max_seq_len) return Error.ValidationFailed;
        if (vocab_size < 4) return Error.ValidationFailed;
        if (unk_id >= vocab_size or bos_id >= vocab_size or eos_id >= vocab_size or pad_id >= vocab_size) {
            return Error.ValidationFailed;
        }
        if (!std.math.isFinite(ln_eps) or ln_eps <= 0.0) return Error.ValidationFailed;

        const expected_tensors = expectedTensorCount(num_enc, num_dec);
        if (tensor_count != expected_tensors) return Error.ValidationFailed;

        const dir_bytes = try checkedMul(@as(usize, tensor_count), tensor_desc_bytes);
        const dir_end = try checkedAdd(header_bytes, dir_bytes);
        const aligned_weights_off = alignUp4(dir_end);
        if (weights_off != aligned_weights_off) return Error.ValidationFailed;
        if (dir_end > bytes.len or weights_off > total_bytes) return Error.InvalidSize;
        const weights_end = try checkedAdd(@as(usize, weights_off), weights_bytes);
        if (weights_end > total_bytes) return Error.InvalidSize;

        // Verify metadata CRC-32 unconditionally (`[0..dir_end)` with field `[12..16)` zeroed).
        const got_meta_crc = crc32.crc32WithZeroWindow(bytes[0..dir_end], 12, 4);
        if (got_meta_crc != meta_crc) return Error.CrcMismatch;

        // Verify full payload CRC-32 (`[weights_off .. total_bytes)`) when requested.
        if ((open_flags & open_verify_payload) != 0) {
            const got_payload_crc = crc32.crc32(bytes[weights_off..total_bytes]);
            if (got_payload_crc != payload_crc) return Error.CrcMismatch;
        }

        // Optional trailing sections must fit cleanly inside `[weights_end .. total_bytes]`.
        try validateSpan(vocab_off, vocab_bytes, weights_end, total_bytes);
        try validateSpan(sl_off, sl_bytes, weights_end, total_bytes);

        const view: TmntView = .{
            .bytes = bytes,
            .header = .{
                .total_bytes = total_bytes,
                .meta_crc32 = meta_crc,
                .payload_crc32 = payload_crc,
                .vocab_size = vocab_size,
                .unk_id = unk_id,
                .bos_id = bos_id,
                .eos_id = eos_id,
                .pad_id = pad_id,
                .shortlist_off = sl_off,
                .shortlist_bytes = sl_bytes,
                .ln_eps = ln_eps,
                .version = ver,
                .model_flags = model_flags,
                .d_model = d_model,
                .d_ff = d_ff,
                .max_seq = max_seq,
                .nmt_model = nmt_model,
                .num_heads = num_heads,
                .num_enc_layers = num_enc,
                .num_dec_layers = num_dec,
                .reserved = .{ 0, 0, 0, 0, 0, 0 },
            },
            .dir_off = header_bytes,
            .tensor_count = @intCast(tensor_count),
            .weights_off = @intCast(weights_off),
            .weights_bytes = @intCast(weights_bytes),
        };
        try view.validateAllTensors();
        return view;
    }

    /// Read validated tensor metadata at `slot`.
    pub fn tensorAt(self: TmntView, slot: usize) Error!TensorRef {
        if (slot >= self.tensor_count) return Error.OutOfRange;
        const off = self.dir_off + slot * tensor_desc_bytes;
        const data_off = readU32(self.bytes, off + 0);
        const data_bytes = readU32(self.bytes, off + 4);
        const rows = readU32(self.bytes, off + 8);
        const cols = readU32(self.bytes, off + 12);
        const scale = readF32(self.bytes, off + 16);
        const dtype = DType.fromRaw(self.bytes[off + 20]) orelse return Error.ValidationFailed;
        const zp: i8 = @bitCast(self.bytes[off + 21]);
        if (self.bytes[off + 22] != 0 or self.bytes[off + 23] != 0) return Error.ValidationFailed;

        return .{
            .dtype = dtype,
            .data_off = @intCast(data_off),
            .data_bytes = @intCast(data_bytes),
            .rows = @intCast(rows),
            .cols = @intCast(cols),
            .scale = scale,
            .zero_point = zp,
        };
    }

    /// Borrow signed `int8` payload slice for a validated `.i8` tensor.
    pub fn i8Slice(self: TmntView, ref: TensorRef) []const i8 {
        const u8s = self.bytes[ref.data_off .. ref.data_off + ref.data_bytes];
        const ptr: [*]const i8 = @ptrCast(u8s.ptr);
        return ptr[0..u8s.len];
    }

    /// Alignment-safe little-endian `f32` read from a validated `.f32` tensor element index.
    pub fn f32At(self: TmntView, ref: TensorRef, idx: usize) f32 {
        return readF32(self.bytes, ref.data_off + idx * 4);
    }

    fn validateAllTensors(self: TmntView) Error!void {
        const h = self.header;
        const d: usize = @intCast(h.d_model);
        const ffn: usize = @intCast(h.d_ff);
        const v: usize = @intCast(h.vocab_size);
        const mseq: usize = @intCast(h.max_seq);

        // Globals (0..7)
        try self.expectTensor(0, true, .i8, v, d);
        try self.expectTensor(1, false, .f32, mseq, d);
        try self.expectTensor(2, true, .i8, v, d);
        try self.expectTensor(3, false, .f32, v, 1);
        try self.expectTensor(4, false, .f32, d, 1);
        try self.expectTensor(5, false, .f32, d, 1);
        try self.expectTensor(6, false, .f32, d, 1);
        try self.expectTensor(7, false, .f32, d, 1);

        var slot: usize = global_tensor_count;
        var el: u8 = 0;
        while (el < h.num_enc_layers) : (el += 1) {
            try self.expectProjAndLnBlock(slot, d, d);
            slot += 10;
            try self.expectFfnAndLnBlock(slot, d, ffn);
            slot += 6;
        }
        var dl: u8 = 0;
        while (dl < h.num_dec_layers) : (dl += 1) {
            try self.expectProjAndLnBlock(slot, d, d);
            slot += 10;
            try self.expectProjAndLnBlock(slot, d, d);
            slot += 10;
            try self.expectFfnAndLnBlock(slot, d, ffn);
            slot += 6;
        }
    }

    fn expectProjAndLnBlock(self: TmntView, base: usize, rows: usize, cols: usize) Error!void {
        try self.expectTensor(base + 0, true, .i8, rows, cols); // q_w
        try self.expectTensor(base + 1, false, .f32, rows, 1); // q_b
        try self.expectTensor(base + 2, true, .i8, rows, cols); // k_w
        try self.expectTensor(base + 3, false, .f32, rows, 1); // k_b
        try self.expectTensor(base + 4, true, .i8, rows, cols); // v_w
        try self.expectTensor(base + 5, false, .f32, rows, 1); // v_b
        try self.expectTensor(base + 6, true, .i8, rows, cols); // o_w
        try self.expectTensor(base + 7, false, .f32, rows, 1); // o_b
        try self.expectTensor(base + 8, true, .f32, cols, 1); // ln_g
        try self.expectTensor(base + 9, true, .f32, cols, 1); // ln_b
    }

    fn expectFfnAndLnBlock(self: TmntView, base: usize, d: usize, ffn: usize) Error!void {
        try self.expectTensor(base + 0, true, .i8, ffn, d); // fc1_w
        try self.expectTensor(base + 1, false, .f32, ffn, 1); // fc1_b
        try self.expectTensor(base + 2, true, .i8, d, ffn); // fc2_w
        try self.expectTensor(base + 3, false, .f32, d, 1); // fc2_b
        try self.expectTensor(base + 4, true, .f32, d, 1); // ln_g
        try self.expectTensor(base + 5, true, .f32, d, 1); // ln_b
    }

    fn expectTensor(
        self: TmntView,
        slot: usize,
        required: bool,
        want_dtype: DType,
        want_rows: usize,
        want_cols: usize,
    ) Error!void {
        const ref = try self.tensorAt(slot);
        if (ref.dtype == .none) {
            if (required or ref.data_bytes != 0) return Error.ValidationFailed;
            return;
        }
        if (ref.dtype != want_dtype) return Error.ValidationFailed;
        if (ref.rows != want_rows or ref.cols != want_cols) return Error.ValidationFailed;
        const elem_bytes: usize = if (want_dtype == .i8) 1 else 4;
        const elems = try checkedMul(want_rows, want_cols);
        const expected_bytes = try checkedMul(elems, elem_bytes);
        if (ref.data_bytes != expected_bytes) return Error.ValidationFailed;
        if ((ref.data_off & 3) != 0) return Error.ValidationFailed;
        const end = try checkedAdd(ref.data_off, ref.data_bytes);
        if (ref.data_off < self.weights_off or end > self.weights_off + self.weights_bytes) {
            return Error.ValidationFailed;
        }
        if (!std.math.isFinite(ref.scale) or ref.scale <= 0.0) return Error.ValidationFailed;
        if (want_dtype == .f32) {
            var i: usize = 0;
            while (i < elems) : (i += 1) {
                if (!std.math.isFinite(self.f32At(ref, i))) return Error.ValidationFailed;
            }
        }
    }
};

/// Compute exact tensor slot count for `(num_enc_layers, num_dec_layers)`.
pub fn expectedTensorCount(num_enc_layers: u8, num_dec_layers: u8) usize {
    return global_tensor_count +
        @as(usize, num_enc_layers) * enc_layer_tensor_count +
        @as(usize, num_dec_layers) * dec_layer_tensor_count;
}

/// Serialize a deterministic CRC-32-protected `TMNT` container into `out`.
pub fn writeContainer(
    opts: WriteOpts,
    inputs: []const TensorInput,
    out: []u8,
) Error!usize {
    const exp_count = expectedTensorCount(opts.num_enc_layers, opts.num_dec_layers);
    if (inputs.len != exp_count) return Error.InvalidArg;

    const dir_bytes = exp_count * tensor_desc_bytes;
    const weights_off = alignUp4(header_bytes + dir_bytes);
    if (out.len < weights_off) return Error.InvalidSize;

    var cursor: usize = weights_off;
    var offs: [512]u32 = undefined;
    if (exp_count > offs.len) return Error.InvalidSize;

    for (inputs, 0..) |inp, idx| {
        const slot_off = header_bytes + idx * tensor_desc_bytes;
        if (inp.dtype == .none) {
            writeDesc(out, slot_off, 0, 0, 0, 0, 1.0, 0, 0);
            offs[idx] = 0;
            continue;
        }
        if (inp.alias_slot) |src_idx| {
            if (src_idx >= idx) return Error.InvalidArg;
            const src_off = offs[src_idx];
            const byte_len: u32 = @intCast(inputs[src_idx].i8_data.len);
            offs[idx] = src_off;
            writeDesc(out, slot_off, src_off, byte_len, inp.rows, inp.cols, inp.scale, @backingInt(inp.dtype), inp.zero_point);
            continue;
        }

        cursor = alignUp4(cursor);
        const start_off = cursor;
        if (inp.dtype == .i8) {
            if (cursor + inp.i8_data.len > out.len) return Error.InvalidSize;
            for (inp.i8_data, 0..) |v, k| {
                out[cursor + k] = @bitCast(v);
            }
            cursor += inp.i8_data.len;
            offs[idx] = @intCast(start_off);
            writeDesc(out, slot_off, @intCast(start_off), @intCast(inp.i8_data.len), inp.rows, inp.cols, inp.scale, 1, inp.zero_point);
        } else {
            const byte_len = inp.f32_data.len * 4;
            if (cursor + byte_len > out.len) return Error.InvalidSize;
            for (inp.f32_data, 0..) |v, k| {
                writeF32(out, cursor + k * 4, v);
            }
            cursor += byte_len;
            offs[idx] = @intCast(start_off);
            writeDesc(out, slot_off, @intCast(start_off), @intCast(byte_len), inp.rows, inp.cols, inp.scale, 2, 0);
        }
    }

    const total_bytes = alignUp4(cursor);
    if (out.len < total_bytes) return Error.InvalidSize;
    @memset(out[cursor..total_bytes], 0);
    const weights_bytes = total_bytes - weights_off;
    const dir_end = header_bytes + dir_bytes;
    if (weights_off > dir_end) {
        @memset(out[dir_end..weights_off], 0);
    }

    @memset(out[0..header_bytes], 0);
    writeU32(out, 0, magic);
    writeU16(out, 4, version);
    writeU16(out, 6, opts.model_flags);
    writeU32(out, 8, @intCast(total_bytes));
    out[20] = opts.nmt_model;
    out[21] = opts.num_heads;
    out[22] = opts.num_enc_layers;
    out[23] = opts.num_dec_layers;
    writeU16(out, 24, opts.d_model);
    writeU16(out, 26, opts.d_ff);
    writeU16(out, 28, opts.max_seq_len);
    writeU16(out, 30, 0);
    writeU32(out, 32, opts.vocab_size);
    writeU32(out, 36, opts.unk_id);
    writeU32(out, 40, opts.bos_id);
    writeU32(out, 44, opts.eos_id);
    writeU32(out, 48, opts.pad_id);
    writeF32(out, 52, opts.ln_eps);
    writeU32(out, 72, @intCast(exp_count));
    writeU32(out, 76, @intCast(weights_off));
    writeU32(out, 80, @intCast(weights_bytes));

    const payload_crc = crc32.crc32(out[weights_off..total_bytes]);
    writeU32(out, 16, payload_crc);
    const meta_crc = crc32.crc32WithZeroWindow(out[0..dir_end], 12, 4);
    writeU32(out, 12, meta_crc);
    return total_bytes;
}

fn writeDesc(
    out: []u8,
    off: usize,
    data_off: u32,
    data_bytes: u32,
    rows: u32,
    cols: u32,
    scale: f32,
    dtype: u8,
    zp: i8,
) void {
    writeU32(out, off + 0, data_off);
    writeU32(out, off + 4, data_bytes);
    writeU32(out, off + 8, rows);
    writeU32(out, off + 12, cols);
    writeF32(out, off + 16, scale);
    out[off + 20] = dtype;
    out[off + 21] = @bitCast(zp);
    out[off + 22] = 0;
    out[off + 23] = 0;
}

fn validateSpan(off: u32, len: u32, min_off: usize, max_off: usize) Error!void {
    if (off == 0 and len == 0) return;
    if (off < min_off) return Error.ValidationFailed;
    const end = try checkedAdd(@as(usize, off), len);
    if (end > max_off) return Error.InvalidSize;
}

fn alignUp4(v: usize) usize {
    return (v + 3) & ~@as(usize, 3);
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

fn readF32(buf: []const u8, off: usize) f32 {
    return @bitCast(readU32(buf, off));
}

fn writeU16(buf: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, buf[off..][0..2], v, .little);
}

fn writeU32(buf: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, buf[off..][0..4], v, .little);
}

fn writeF32(buf: []u8, off: usize, v: f32) void {
    writeU32(buf, off, @bitCast(v));
}
