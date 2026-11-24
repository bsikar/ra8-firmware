//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `TVOC` binary SentencePiece vocabulary container reader, CRC-32 validator,
//! lexicographical prefix/exact lookup primitives, and in-place writer.

const std = @import("std");
const crc32 = @import("crc32");
const err_mod = @import("err");

pub const Error = err_mod.TranslateError;

pub const magic: u32 = 0x434F5654; // Little-endian "TVOC".
pub const version: u16 = 1;
pub const header_bytes: usize = 56;
pub const piece_record_bytes: usize = 12;
pub const sorted_index_entry_bytes: usize = 4;
pub const max_vocab_size: u32 = 262_144;
pub const max_piece_bytes: usize = 256;
pub const none_id: u32 = 0xFFFF_FFFF;

pub const flag_add_dummy_prefix: u8 = 0x01;
pub const flag_remove_extra_ws: u8 = 0x02;
pub const flag_escape_ws: u8 = 0x04;
pub const flag_byte_fallback: u8 = 0x08;
pub const flag_norm_fullwidth: u8 = 0x10;
pub const flag_valid_mask: u8 = 0x1F;
pub const default_flags: u8 =
    flag_add_dummy_prefix | flag_remove_extra_ws | flag_escape_ws;

const k_default_min_score: f32 = -10.0;

/// SentencePiece subword model family stored in a `TVOC` container.
pub const ModelType = enum(u8) {
    unigram = 0,
    bpe = 1,

    pub fn fromRaw(raw: u8) ?ModelType {
        return switch (raw) {
            0 => .unigram,
            1 => .bpe,
            else => null,
        };
    }
};

/// SentencePiece vocabulary entry classification matching `ModelProto`.
pub const PieceKind = enum(u8) {
    normal = 0,
    unknown = 1,
    control = 2,
    user_defined = 3,
    byte = 4,
    unused = 5,

    pub fn fromRaw(raw: u8) ?PieceKind {
        return switch (raw) {
            0 => .normal,
            1 => .unknown,
            2 => .control,
            3 => .user_defined,
            4 => .byte,
            5 => .unused,
            else => null,
        };
    }
};

/// Validated metadata extracted from a `TVOC` header.
pub const VocabHeader = struct {
    total_bytes: u32,
    container_crc32: u32,
    vocab_size: u32,
    unk_id: u32,
    bos_id: u32,
    eos_id: u32,
    pad_id: u32,
    min_normal_score: f32,
    model_type: ModelType,
    flags: u8,
};

/// Validated metadata for a single vocabulary entry.
pub const PieceMeta = struct {
    text: []const u8,
    score: f32,
    kind: PieceKind,
};

/// Input piece descriptor used by `writeVocab()` to serialize `TVOC` containers.
pub const PieceInput = struct {
    text: []const u8,
    score: f32,
    kind: PieceKind = .normal,
};

/// Configuration fields for serializing a `TVOC` container via `writeVocab()`.
pub const VocabConfig = struct {
    model_type: ModelType,
    flags: u8 = default_flags,
    unk_id: u32 = 0,
    bos_id: u32 = none_id,
    eos_id: u32 = none_id,
    pad_id: u32 = none_id,
};

pub const PieceHit = struct { id: u32, score: f32 };
pub const PieceSpan = struct { id: u32, len: usize };
pub const ByteRange = struct { lo: usize, hi: usize };
pub const SortedEntry = struct { id: u32, kind: PieceKind, score: f32, text: []const u8 };

/// Validated read-only handle over a serialized `TVOC` container.
pub const VocabContainer = struct {
    bytes: []const u8,
    header: VocabHeader,
    piece_table_off: usize,
    sorted_index_off: usize,
    pool_off: usize,
    has_user_defined: bool,

    /// Validate `TVOC` header, CRC-32, record bounds, UTF-8 pieces, and sorted index.
    pub fn open(bytes: []const u8) Error!VocabContainer {
        if (bytes.len < header_bytes) return error.InvalidSize;
        if (readU32(bytes, 0) != magic) return error.ValidationFailed;
        if (readU16(bytes, 4) != version) return error.NotSupported;

        const model_type = ModelType.fromRaw(bytes[6]) orelse return error.NotSupported;
        const flags = bytes[7];
        if ((flags & ~flag_valid_mask) != 0) return error.NotSupported;
        if (readU32(bytes, 52) != 0) return error.ValidationFailed;

        const total_bytes = readU32(bytes, 8);
        if (total_bytes != bytes.len) return error.InvalidSize;

        const expected_crc = readU32(bytes, 12);
        if (crc32.crc32WithZeroWindow(bytes, 12, 4) != expected_crc) {
            return error.CrcMismatch;
        }

        const vocab_size = readU32(bytes, 16);
        if (vocab_size == 0 or vocab_size > max_vocab_size) return error.ValidationFailed;

        const unk_id = readU32(bytes, 20);
        const bos_id = readU32(bytes, 24);
        const eos_id = readU32(bytes, 28);
        const pad_id = readU32(bytes, 32);
        if (unk_id >= vocab_size) return error.ValidationFailed;
        if (bos_id != none_id and bos_id >= vocab_size) return error.ValidationFailed;
        if (eos_id != none_id and eos_id >= vocab_size) return error.ValidationFailed;
        if (pad_id != none_id and pad_id >= vocab_size) return error.ValidationFailed;

        const min_normal_score = readF32(bytes, 36);
        if (!std.math.isFinite(min_normal_score)) return error.ValidationFailed;

        const piece_table_off = readU32(bytes, 40);
        const sorted_index_off = readU32(bytes, 44);
        const pool_off = readU32(bytes, 48);
        const piece_bytes_total = @as(u64, vocab_size) * piece_record_bytes;
        const index_bytes_total = @as(u64, vocab_size) * sorted_index_entry_bytes;
        const exp_sorted = @as(u64, header_bytes) + piece_bytes_total;
        const exp_pool = exp_sorted + index_bytes_total;
        if (piece_table_off != header_bytes or
            sorted_index_off != exp_sorted or
            pool_off != exp_pool or
            pool_off > total_bytes)
        {
            return error.ValidationFailed;
        }

        const pool = bytes[pool_off..total_bytes];
        const has_ud = try validateRecords(bytes, piece_table_off, vocab_size, unk_id, pool);
        try validateSortedIndex(bytes, piece_table_off, sorted_index_off, vocab_size, pool);

        return .{
            .bytes = bytes,
            .header = .{
                .total_bytes = total_bytes,
                .container_crc32 = expected_crc,
                .vocab_size = vocab_size,
                .unk_id = unk_id,
                .bos_id = bos_id,
                .eos_id = eos_id,
                .pad_id = pad_id,
                .min_normal_score = min_normal_score,
                .model_type = model_type,
                .flags = flags,
            },
            .piece_table_off = piece_table_off,
            .sorted_index_off = sorted_index_off,
            .pool_off = pool_off,
            .has_user_defined = has_ud,
        };
    }

    pub fn pieceAt(self: VocabContainer, token_id: u32) Error!PieceMeta {
        if (token_id >= self.header.vocab_size) return error.OutOfRange;
        const rec_off = self.piece_table_off + @as(usize, token_id) * piece_record_bytes;
        const text_off = readU32(self.bytes, rec_off);
        const text_len = readU16(self.bytes, rec_off + 4);
        const kind = PieceKind.fromRaw(self.bytes[rec_off + 6]) orelse return error.NotSupported;
        return .{
            .text = self.bytes[self.pool_off + text_off ..][0..text_len],
            .score = readF32(self.bytes, rec_off + 8),
            .kind = kind,
        };
    }

    pub fn sortedTextAt(self: VocabContainer, sorted_pos: usize) SortedEntry {
        const id = readU32(self.bytes, self.sorted_index_off + sorted_pos * 4);
        const rec_off = self.piece_table_off + @as(usize, id) * piece_record_bytes;
        const text_off = readU32(self.bytes, rec_off);
        const text_len = readU16(self.bytes, rec_off + 4);
        return .{
            .id = id,
            .kind = PieceKind.fromRaw(self.bytes[rec_off + 6]).?,
            .score = readF32(self.bytes, rec_off + 8),
            .text = self.bytes[self.pool_off + text_off ..][0..text_len],
        };
    }

    pub fn skipExactLength(self: VocabContainer, lo_in: usize, hi: usize, target_len: usize) usize {
        var lo = lo_in;
        var r = hi;
        while (lo < r) {
            const mid = lo + (r - lo) / 2;
            if (self.sortedTextAt(mid).text.len <= target_len) {
                lo = mid + 1;
            } else {
                r = mid;
            }
        }
        return lo;
    }

    pub fn narrowPrefixByte(self: VocabContainer, lo_in: usize, hi_in: usize, depth: usize, ch: u8) ByteRange {
        var l = lo_in;
        var r = hi_in;
        while (l < r) {
            const mid = l + (r - l) / 2;
            if (self.sortedTextAt(mid).text[depth] < ch) l = mid + 1 else r = mid;
        }
        const new_lo = l;
        r = hi_in;
        while (l < r) {
            const mid = l + (r - l) / 2;
            if (self.sortedTextAt(mid).text[depth] <= ch) l = mid + 1 else r = mid;
        }
        return .{ .lo = new_lo, .hi = l };
    }

    pub fn pickKindScore(self: VocabContainer, lo: usize, hi: usize, want: PieceKind) ?PieceHit {
        var best_id: ?u32 = null;
        var best_score: f32 = 0.0;
        var k = lo;
        while (k < hi) : (k += 1) {
            const entry = self.sortedTextAt(k);
            if (entry.kind == want and (best_id == null or entry.score > best_score)) {
                best_id = entry.id;
                best_score = entry.score;
            }
        }
        if (best_id) |id| return .{ .id = id, .score = best_score };
        return null;
    }

    pub fn findExactPiece(self: VocabContainer, target: []const u8, want: PieceKind) ?PieceHit {
        var l: usize = 0;
        var r: usize = self.header.vocab_size;
        while (l < r) {
            const mid = l + (r - l) / 2;
            if (std.mem.order(u8, self.sortedTextAt(mid).text, target) == .lt) {
                l = mid + 1;
            } else {
                r = mid;
            }
        }
        var k = l;
        while (k < self.header.vocab_size) : (k += 1) {
            const entry = self.sortedTextAt(k);
            if (!std.mem.eql(u8, entry.text, target)) break;
            if (entry.kind == want) return .{ .id = entry.id, .score = entry.score };
        }
        return null;
    }

    pub fn findLongestPiece(self: VocabContainer, slice: []const u8, want: PieceKind) ?PieceSpan {
        var lo: usize = 0;
        var hi: usize = self.header.vocab_size;
        var depth: usize = 0;
        var scan_end: usize = 0;
        var best: ?PieceSpan = null;

        while (scan_end < slice.len and lo < hi) {
            const cp_len = std.unicode.utf8ByteSequenceLength(slice[scan_end]) catch return null;
            if (depth + cp_len > max_piece_bytes) break;

            var b: usize = 0;
            while (b < cp_len and lo < hi) : (b += 1) {
                lo = self.skipExactLength(lo, hi, depth);
                if (lo >= hi) break;
                const narrowed = self.narrowPrefixByte(lo, hi, depth, slice[depth]);
                lo = narrowed.lo;
                hi = narrowed.hi;
                depth += 1;
            }
            if (b < cp_len or lo >= hi) break;
            scan_end += cp_len;

            const exact_end = self.skipExactLength(lo, hi, depth);
            if (self.pickKindScore(lo, exact_end, want)) |hit| {
                best = .{ .id = hit.id, .len = depth };
            }
        }
        return best;
    }

    pub fn findByteToken(self: VocabContainer, byte: u8) ?u32 {
        const hex = "0123456789ABCDEF";
        const buf = [6]u8{ '<', '0', 'x', hex[byte >> 4], hex[byte & 0x0F], '>' };
        if (self.findExactPiece(&buf, .byte)) |hit| return hit.id;
        return null;
    }
};

/// Compute exact byte size required to serialize a `TVOC` container via `writeVocab()`.
pub fn requiredVocabBytes(pieces: []const PieceInput) Error!usize {
    if (pieces.len == 0 or pieces.len > max_vocab_size) return error.InvalidArg;
    var pool_bytes: usize = 0;
    for (pieces) |p| {
        if (p.text.len > max_piece_bytes) return error.InvalidArg;
        pool_bytes = try addChecked(pool_bytes, p.text.len);
    }
    const piece_bytes = try mulChecked(pieces.len, piece_record_bytes);
    const index_bytes = try mulChecked(pieces.len, sorted_index_entry_bytes);
    return try addChecked(try addChecked(header_bytes, try addChecked(piece_bytes, index_bytes)), pool_bytes);
}

/// Serialize a `TVOC` binary vocabulary container into `out_buf` with CRC-32 stamp.
pub fn writeVocab(cfg: VocabConfig, pieces: []const PieceInput, out_buf: []u8) Error!usize {
    if ((cfg.flags & ~flag_valid_mask) != 0) return error.InvalidArg;
    const total_bytes = try requiredVocabBytes(pieces);
    const vocab_size: u32 = @intCast(pieces.len);
    if (cfg.unk_id >= vocab_size or pieces[cfg.unk_id].kind != .unknown) return error.InvalidArg;
    if (cfg.bos_id != none_id and cfg.bos_id >= vocab_size) return error.InvalidArg;
    if (cfg.eos_id != none_id and cfg.eos_id >= vocab_size) return error.InvalidArg;
    if (cfg.pad_id != none_id and cfg.pad_id >= vocab_size) return error.InvalidArg;

    var min_normal: f32 = k_default_min_score;
    var saw_normal = false;
    for (pieces) |p| {
        if (!std.math.isFinite(p.score)) return error.InvalidArg;
        switch (p.kind) {
            .unused => {},
            .byte => if (parseByteToken(p.text) == null) return error.InvalidArg,
            .normal, .unknown, .control, .user_defined => {
                if (p.text.len == 0 or !std.unicode.utf8ValidateSlice(p.text)) {
                    return error.InvalidArg;
                }
                if (p.kind == .normal and (!saw_normal or p.score < min_normal)) {
                    min_normal = p.score;
                    saw_normal = true;
                }
            },
        }
    }

    if (out_buf.len < total_bytes) return error.InvalidSize;

    const piece_table_off = header_bytes;
    const sorted_index_off = piece_table_off + pieces.len * piece_record_bytes;
    const pool_off = sorted_index_off + pieces.len * sorted_index_entry_bytes;

    var pool_cursor: usize = 0;
    for (pieces, 0..) |p, id| {
        const rec_off = piece_table_off + id * piece_record_bytes;
        writeU32(out_buf, rec_off, @intCast(pool_cursor));
        writeU16(out_buf, rec_off + 4, @intCast(p.text.len));
        out_buf[rec_off + 6] = @backingInt(p.kind);
        out_buf[rec_off + 7] = 0;
        writeU32(out_buf, rec_off + 8, @bitCast(p.score));
        if (p.text.len > 0) {
            @memcpy(out_buf[pool_off + pool_cursor ..][0..p.text.len], p.text);
            pool_cursor += p.text.len;
        }
        writeU32(out_buf, sorted_index_off + id * 4, @intCast(id));
    }

    sortIndexInPlace(out_buf, pieces.len, piece_table_off, sorted_index_off, pool_off);

    writeU32(out_buf, 0, magic);
    writeU16(out_buf, 4, version);
    out_buf[6] = @backingInt(cfg.model_type);
    out_buf[7] = cfg.flags;
    writeU32(out_buf, 8, @intCast(total_bytes));
    writeU32(out_buf, 12, 0);
    writeU32(out_buf, 16, vocab_size);
    writeU32(out_buf, 20, cfg.unk_id);
    writeU32(out_buf, 24, cfg.bos_id);
    writeU32(out_buf, 28, cfg.eos_id);
    writeU32(out_buf, 32, cfg.pad_id);
    writeU32(out_buf, 36, @bitCast(min_normal));
    writeU32(out_buf, 40, @intCast(piece_table_off));
    writeU32(out_buf, 44, @intCast(sorted_index_off));
    writeU32(out_buf, 48, @intCast(pool_off));
    writeU32(out_buf, 52, 0);

    writeU32(out_buf, 12, crc32.crc32(out_buf[0..total_bytes]));
    return total_bytes;
}

pub fn parseByteToken(text: []const u8) ?u8 {
    if (text.len != 6) return null;
    if (text[0] != '<' or text[1] != '0' or text[2] != 'x' or text[5] != '>') return null;
    const hi = parseHexUpper(text[3]) orelse return null;
    const lo = parseHexUpper(text[4]) orelse return null;
    return (@as(u8, hi) << 4) | @as(u8, lo);
}

pub fn addChecked(a: usize, b: usize) Error!usize {
    return std.math.add(usize, a, b) catch error.InvalidSize;
}

pub fn mulChecked(a: usize, b: usize) Error!usize {
    return std.math.mul(usize, a, b) catch error.InvalidSize;
}

fn validateRecords(
    bytes: []const u8,
    piece_table_off: usize,
    vocab_size: u32,
    unk_id: u32,
    pool: []const u8,
) Error!bool {
    var has_user_defined = false;
    var id: u32 = 0;
    while (id < vocab_size) : (id += 1) {
        const rec_off = piece_table_off + @as(usize, id) * piece_record_bytes;
        const text_off = readU32(bytes, rec_off);
        const text_len = readU16(bytes, rec_off + 4);
        const kind = PieceKind.fromRaw(bytes[rec_off + 6]) orelse return error.NotSupported;
        if (bytes[rec_off + 7] != 0) return error.ValidationFailed;
        if (!std.math.isFinite(readF32(bytes, rec_off + 8))) return error.ValidationFailed;
        if (text_len > max_piece_bytes) return error.ValidationFailed;
        if (@as(u64, text_off) + text_len > pool.len) return error.ValidationFailed;

        const text = pool[text_off..][0..text_len];
        switch (kind) {
            .unused => {},
            .byte => if (parseByteToken(text) == null) return error.ValidationFailed,
            .normal, .unknown, .control, .user_defined => {
                if (text_len == 0 or !std.unicode.utf8ValidateSlice(text)) {
                    return error.ValidationFailed;
                }
                if (kind == .user_defined) has_user_defined = true;
            },
        }
        if (id == unk_id and kind != .unknown) return error.ValidationFailed;
    }
    return has_user_defined;
}

fn validateSortedIndex(
    bytes: []const u8,
    piece_table_off: usize,
    sorted_index_off: usize,
    vocab_size: u32,
    pool: []const u8,
) Error!void {
    // Strict total ordering on (piece_bytes, id) proves `sorted_indices` is
    // a valid bijection over [0, vocab_size) without extra bitset memory.
    var prev_id: u32 = 0;
    var prev_text: []const u8 = &.{};
    var k: u32 = 0;
    while (k < vocab_size) : (k += 1) {
        const cur_id = readU32(bytes, sorted_index_off + @as(usize, k) * 4);
        if (cur_id >= vocab_size) return error.ValidationFailed;
        const rec_off = piece_table_off + @as(usize, cur_id) * piece_record_bytes;
        const cur_text = pool[readU32(bytes, rec_off)..][0..readU16(bytes, rec_off + 4)];
        if (k > 0) {
            const ord = std.mem.order(u8, prev_text, cur_text);
            if (ord == .gt or (ord == .eq and prev_id >= cur_id)) {
                return error.ValidationFailed;
            }
        }
        prev_id = cur_id;
        prev_text = cur_text;
    }
}

fn parseHexUpper(ch: u8) ?u4 {
    return switch (ch) {
        '0'...'9' => @intCast(ch - '0'),
        'A'...'F' => @intCast(ch - 'A' + 10),
        else => null,
    };
}

fn sortIndexInPlace(
    buf: []u8,
    count: usize,
    piece_table_off: usize,
    sorted_index_off: usize,
    pool_off: usize,
) void {
    if (count < 2) return;
    // Iterative heapsort keeps sorting stack depth O(1) on embedded targets.
    var start = count / 2;
    while (start > 0) {
        start -= 1;
        siftDown(buf, start, count, piece_table_off, sorted_index_off, pool_off);
    }
    var end = count;
    while (end > 1) {
        end -= 1;
        const a = readU32(buf, sorted_index_off);
        const b = readU32(buf, sorted_index_off + end * 4);
        writeU32(buf, sorted_index_off, b);
        writeU32(buf, sorted_index_off + end * 4, a);
        siftDown(buf, 0, end, piece_table_off, sorted_index_off, pool_off);
    }
}

fn siftDown(
    buf: []u8,
    root_in: usize,
    end: usize,
    piece_table_off: usize,
    sorted_index_off: usize,
    pool_off: usize,
) void {
    var root = root_in;
    while (true) {
        const left = root * 2 + 1;
        if (left >= end) break;
        var swap_idx = left;
        const right = left + 1;
        if (right < end and indexLessThan(buf, piece_table_off, sorted_index_off, pool_off, left, right)) {
            swap_idx = right;
        }
        if (!indexLessThan(buf, piece_table_off, sorted_index_off, pool_off, root, swap_idx)) break;
        const r_val = readU32(buf, sorted_index_off + root * 4);
        const s_val = readU32(buf, sorted_index_off + swap_idx * 4);
        writeU32(buf, sorted_index_off + root * 4, s_val);
        writeU32(buf, sorted_index_off + swap_idx * 4, r_val);
        root = swap_idx;
    }
}

fn indexLessThan(
    buf: []const u8,
    piece_table_off: usize,
    sorted_index_off: usize,
    pool_off: usize,
    pos_a: usize,
    pos_b: usize,
) bool {
    const id_a = readU32(buf, sorted_index_off + pos_a * 4);
    const id_b = readU32(buf, sorted_index_off + pos_b * 4);
    const rec_a = piece_table_off + @as(usize, id_a) * piece_record_bytes;
    const rec_b = piece_table_off + @as(usize, id_b) * piece_record_bytes;
    const txt_a = buf[pool_off + readU32(buf, rec_a) ..][0..readU16(buf, rec_a + 4)];
    const txt_b = buf[pool_off + readU32(buf, rec_b) ..][0..readU16(buf, rec_b + 4)];
    const ord = std.mem.order(u8, txt_a, txt_b);
    return ord == .lt or (ord == .eq and id_a < id_b);
}

fn readU16(bytes: []const u8, off: usize) u16 {
    return std.mem.readInt(u16, bytes[off..][0..2], .little);
}

fn readU32(bytes: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, bytes[off..][0..4], .little);
}

fn readF32(bytes: []const u8, off: usize) f32 {
    return @bitCast(readU32(bytes, off));
}

fn writeU16(bytes: []u8, off: usize, val: u16) void {
    std.mem.writeInt(u16, bytes[off..][0..2], val, .little);
}

fn writeU32(bytes: []u8, off: usize, val: u32) void {
    std.mem.writeInt(u32, bytes[off..][0..4], val, .little);
}
