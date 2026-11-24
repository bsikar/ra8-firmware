//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding SentencePiece encoder and decoder (Unigram Viterbi and BPE merge
//! codecs) operating over a validated `TVOC` vocabulary container. Zero dynamic
//! memory allocation; all DP/merge state resides inside caller scratch buffers.

const std = @import("std");
const vocab = @import("vocab");
const err_mod = @import("err");

pub const Error = err_mod.TranslateError;

pub const magic = vocab.magic;
pub const version = vocab.version;
pub const header_bytes = vocab.header_bytes;
pub const none_id = vocab.none_id;

pub const flag_add_dummy_prefix = vocab.flag_add_dummy_prefix;
pub const flag_remove_extra_ws = vocab.flag_remove_extra_ws;
pub const flag_escape_ws = vocab.flag_escape_ws;
pub const flag_byte_fallback = vocab.flag_byte_fallback;
pub const flag_norm_fullwidth = vocab.flag_norm_fullwidth;
pub const flag_valid_mask = vocab.flag_valid_mask;
pub const default_flags = vocab.default_flags;

pub const enc_add_bos: u8 = 0x01;
pub const enc_add_eos: u8 = 0x02;
pub const enc_valid_mask: u8 = 0x03;

pub const ModelType = vocab.ModelType;
pub const PieceKind = vocab.PieceKind;
pub const VocabHeader = vocab.VocabHeader;
pub const PieceMeta = vocab.PieceMeta;
pub const PieceInput = vocab.PieceInput;
pub const VocabConfig = vocab.VocabConfig;
pub const requiredVocabBytes = vocab.requiredVocabBytes;
pub const writeVocab = vocab.writeVocab;

const k_metaspace = "\xe2\x96\x81"; // U+2581 LOWER ONE EIGHTH BLOCK.
const k_unk_surface = " \xe2\x81\x87 "; // Canonical SentencePiece unk_surface.
const k_utf8_replacement = "\xef\xbf\xbd"; // U+FFFD replacement character.
const k_unk_penalty: f32 = 10.0;
const k_no_prev: u32 = 0x7FFF_FFFF;
const k_unk_tag_bit: u32 = 0x8000_0000;

/// Zero-copy SentencePiece tokenizer handle backed by a validated `TVOC` buffer.
pub const TvocView = struct {
    container: vocab.VocabContainer,
    header: VocabHeader,

    /// Validate a serialized `TVOC` vocabulary container.
    pub fn open(bytes: []const u8) Error!TvocView {
        const container = try vocab.VocabContainer.open(bytes);
        return .{ .container = container, .header = container.header };
    }

    /// Borrow piece metadata and UTF-8 bytes for `token_id`.
    pub fn pieceAt(self: TvocView, token_id: u32) Error!PieceMeta {
        return self.container.pieceAt(token_id);
    }

    /// Encode UTF-8 `text` into token IDs using only `work_buf`, leaving `out_ids`
    /// completely untouched if any check or buffer capacity fails.
    pub fn encode(
        self: TvocView,
        text: []const u8,
        enc_flags: u8,
        work_buf: []u8,
        out_ids: []u32,
    ) Error!usize {
        if ((enc_flags & ~enc_valid_mask) != 0) return error.InvalidArg;
        if ((enc_flags & enc_add_bos) != 0 and self.header.bos_id == none_id) {
            return error.NotSupported;
        }
        if ((enc_flags & enc_add_eos) != 0 and self.header.eos_id == none_id) {
            return error.NotSupported;
        }

        const norm_len = try runNormalizer(text, self.header.flags, null);
        const max_out_ids = try vocab.addChecked(norm_len, 2);

        var cursor: usize = 0;
        const staged_ids = try allocAlignedSlice(u32, work_buf, &cursor, max_out_ids);
        if (norm_len > work_buf.len - cursor) return error.InvalidSize;
        const norm = work_buf[cursor .. cursor + norm_len];
        cursor += norm_len;
        const written = runNormalizer(text, self.header.flags, norm) catch unreachable;
        std.debug.assert(written == norm.len);

        const dp_buf = work_buf[cursor..];
        var out_count: usize = 0;
        if ((enc_flags & enc_add_bos) != 0) {
            staged_ids[out_count] = self.header.bos_id;
            out_count += 1;
        }

        if (norm.len > 0) {
            var last_was_unk = false;
            try self.encodeNormalized(norm, dp_buf, staged_ids, &out_count, &last_was_unk);
        }

        if ((enc_flags & enc_add_eos) != 0) {
            staged_ids[out_count] = self.header.eos_id;
            out_count += 1;
        }

        if (out_ids.len < out_count) return error.InvalidSize;
        if (out_count > 0) @memcpy(out_ids[0..out_count], staged_ids[0..out_count]);
        return out_count;
    }

    /// Decode token IDs into UTF-8 text transactionally without dynamic allocation.
    pub fn decode(self: TvocView, ids: []const u32, out_utf8: []u8) Error!usize {
        for (ids) |id| {
            if (id >= self.header.vocab_size) return error.OutOfRange;
        }
        const total = self.renderDecoded(ids, null);
        if (out_utf8.len < total) return error.InvalidSize;
        return self.renderDecoded(ids, out_utf8[0..total]);
    }

    fn encodeNormalized(
        self: TvocView,
        norm: []const u8,
        dp_buf: []u8,
        staged_ids: []u32,
        out_count: *usize,
        last_was_unk: *bool,
    ) Error!void {
        if (!self.container.has_user_defined) {
            return self.encodeSegment(norm, dp_buf, staged_ids, out_count, last_was_unk);
        }

        var pos: usize = 0;
        var seg_start: usize = 0;
        while (pos < norm.len) {
            if (self.container.findLongestPiece(norm[pos..], .user_defined)) |ud| {
                if (pos > seg_start) {
                    try self.encodeSegment(norm[seg_start..pos], dp_buf, staged_ids, out_count, last_was_unk);
                }
                staged_ids[out_count.*] = ud.id;
                out_count.* += 1;
                last_was_unk.* = false;
                pos += ud.len;
                seg_start = pos;
            } else {
                const cp_len = std.unicode.utf8ByteSequenceLength(norm[pos]) catch return error.InvalidArg;
                pos += cp_len;
            }
        }
        if (pos > seg_start) {
            try self.encodeSegment(norm[seg_start..pos], dp_buf, staged_ids, out_count, last_was_unk);
        }
    }

    fn encodeSegment(
        self: TvocView,
        seg: []const u8,
        dp_buf: []u8,
        staged_ids: []u32,
        out_count: *usize,
        last_was_unk: *bool,
    ) Error!void {
        return switch (self.header.model_type) {
            .unigram => self.unigramSegment(seg, dp_buf, staged_ids, out_count, last_was_unk),
            .bpe => self.bpeSegment(seg, dp_buf, staged_ids, out_count, last_was_unk),
        };
    }

    fn unigramSegment(
        self: TvocView,
        seg: []const u8,
        dp_buf: []u8,
        staged_ids: []u32,
        out_count: *usize,
        last_was_unk: *bool,
    ) Error!void {
        var dp_cursor: usize = 0;
        const nodes = try allocAlignedSlice(UnigramNode, dp_buf, &dp_cursor, seg.len + 1);
        const trace_ends = try allocAlignedSlice(u32, dp_buf, &dp_cursor, seg.len);

        for (nodes) |*n| {
            n.* = .{ .score = 0.0, .tok_id = 0, .prev_packed = k_no_prev };
        }
        nodes[0].prev_packed = 0;

        const unk_score = self.header.min_normal_score - k_unk_penalty;
        var pos: usize = 0;
        while (pos < seg.len) {
            const first_len = std.unicode.utf8ByteSequenceLength(seg[pos]) catch return error.InvalidArg;
            if ((nodes[pos].prev_packed & k_no_prev) == k_no_prev) {
                pos += first_len;
                continue;
            }

            var has_single_node = false;
            var lo: usize = 0;
            var hi: usize = self.header.vocab_size;
            var depth: usize = 0;
            var scan_end: usize = pos;

            while (scan_end < seg.len and lo < hi) {
                const cp_len = std.unicode.utf8ByteSequenceLength(seg[scan_end]) catch return error.InvalidArg;
                if (depth + cp_len > vocab.max_piece_bytes) break;

                var b: usize = 0;
                while (b < cp_len and lo < hi) : (b += 1) {
                    lo = self.container.skipExactLength(lo, hi, depth);
                    if (lo >= hi) break;
                    const narrowed = self.container.narrowPrefixByte(lo, hi, depth, seg[pos + depth]);
                    lo = narrowed.lo;
                    hi = narrowed.hi;
                    depth += 1;
                }
                if (b < cp_len or lo >= hi) break;
                scan_end += cp_len;

                const exact_end = self.container.skipExactLength(lo, hi, depth);
                if (self.container.pickKindScore(lo, exact_end, .normal)) |hit| {
                    if (depth == first_len) has_single_node = true;
                    const cand = nodes[pos].score + hit.score;
                    if ((nodes[scan_end].prev_packed & k_no_prev) == k_no_prev or cand > nodes[scan_end].score) {
                        nodes[scan_end].score = cand;
                        nodes[scan_end].tok_id = hit.id;
                        nodes[scan_end].prev_packed = @intCast(pos);
                    }
                }
            }

            if (!has_single_node) {
                const next_pos = pos + first_len;
                const cand = nodes[pos].score + unk_score;
                if ((nodes[next_pos].prev_packed & k_no_prev) == k_no_prev or cand > nodes[next_pos].score) {
                    nodes[next_pos].score = cand;
                    nodes[next_pos].tok_id = self.header.unk_id;
                    nodes[next_pos].prev_packed = @as(u32, @intCast(pos)) | k_unk_tag_bit;
                }
            }
            pos += first_len;
        }

        var trace_len: usize = 0;
        var cur: usize = seg.len;
        while (cur > 0) {
            const prev_pos: usize = @intCast(nodes[cur].prev_packed & k_no_prev);
            if (prev_pos >= cur) return error.ValidationFailed;
            trace_ends[trace_len] = @intCast(cur);
            trace_len += 1;
            cur = prev_pos;
        }

        var idx = trace_len;
        while (idx > 0) {
            idx -= 1;
            const end_pos: usize = @intCast(trace_ends[idx]);
            const packed_prev = nodes[end_pos].prev_packed;
            const start_pos: usize = @intCast(packed_prev & k_no_prev);
            if ((packed_prev & k_unk_tag_bit) != 0) {
                self.emitUnknownSpan(seg[start_pos..end_pos], staged_ids, out_count, last_was_unk);
            } else {
                staged_ids[out_count.*] = nodes[end_pos].tok_id;
                out_count.* += 1;
                last_was_unk.* = false;
            }
        }
    }

    fn bpeSegment(
        self: TvocView,
        seg: []const u8,
        dp_buf: []u8,
        staged_ids: []u32,
        out_count: *usize,
        last_was_unk: *bool,
    ) Error!void {
        var cp_count: usize = 0;
        var scan: usize = 0;
        while (scan < seg.len) {
            scan += std.unicode.utf8ByteSequenceLength(seg[scan]) catch return error.InvalidArg;
            cp_count += 1;
        }
        if (cp_count == 0) return;

        var dp_cursor: usize = 0;
        const nodes = try allocAlignedSlice(BpeNode, dp_buf, &dp_cursor, cp_count);

        scan = 0;
        var idx: usize = 0;
        while (scan < seg.len) : (idx += 1) {
            const cp_len = std.unicode.utf8ByteSequenceLength(seg[scan]) catch return error.InvalidArg;
            nodes[idx] = .{
                .prev = if (idx == 0) -1 else @as(i32, @intCast(idx - 1)),
                .next = if (idx + 1 == cp_count) -1 else @as(i32, @intCast(idx + 1)),
                .start = @intCast(scan),
                .len = @intCast(cp_len),
                .pair_score = 0.0,
                .merge_id = none_id,
            };
            scan += cp_len;
        }

        for (0..cp_count) |i| self.refreshBpePair(seg, nodes, i);

        while (true) {
            var best_idx: ?usize = null;
            var best_score: f32 = 0.0;
            var walk: i32 = 0;
            while (walk >= 0) {
                const u: usize = @intCast(walk);
                if (nodes[u].merge_id != none_id and (best_idx == null or nodes[u].pair_score > best_score)) {
                    best_idx = u;
                    best_score = nodes[u].pair_score;
                }
                walk = nodes[u].next;
            }

            const u = best_idx orelse break;
            const v: usize = @intCast(nodes[u].next);
            const w = nodes[v].next;
            nodes[u].len += nodes[v].len;
            nodes[u].next = w;
            if (w >= 0) nodes[@intCast(w)].prev = @intCast(u);

            self.refreshBpePair(seg, nodes, u);
            if (nodes[u].prev >= 0) self.refreshBpePair(seg, nodes, @intCast(nodes[u].prev));
        }

        var walk: i32 = 0;
        while (walk >= 0) {
            const u: usize = @intCast(walk);
            const sym = seg[nodes[u].start..][0..nodes[u].len];
            if (self.container.findExactPiece(sym, .normal)) |hit| {
                staged_ids[out_count.*] = hit.id;
                out_count.* += 1;
                last_was_unk.* = false;
            } else {
                self.emitUnknownSpan(sym, staged_ids, out_count, last_was_unk);
            }
            walk = nodes[u].next;
        }
    }

    fn refreshBpePair(self: TvocView, seg: []const u8, nodes: []BpeNode, idx: usize) void {
        const next_i = nodes[idx].next;
        if (next_i < 0) {
            nodes[idx].merge_id = none_id;
            return;
        }
        const nxt: usize = @intCast(next_i);
        const total_len: usize = @as(usize, nodes[idx].len) + @as(usize, nodes[nxt].len);
        if (total_len > vocab.max_piece_bytes) {
            nodes[idx].merge_id = none_id;
            return;
        }
        const merged = seg[nodes[idx].start..][0..total_len];
        if (self.container.findExactPiece(merged, .normal)) |hit| {
            nodes[idx].merge_id = hit.id;
            nodes[idx].pair_score = hit.score;
        } else {
            nodes[idx].merge_id = none_id;
        }
    }

    fn emitUnknownSpan(
        self: TvocView,
        span: []const u8,
        staged_ids: []u32,
        out_count: *usize,
        last_was_unk: *bool,
    ) void {
        if ((self.header.flags & flag_byte_fallback) != 0) {
            for (span) |byte| {
                staged_ids[out_count.*] = self.container.findByteToken(byte) orelse self.header.unk_id;
                out_count.* += 1;
            }
            last_was_unk.* = false;
            return;
        }
        if (!last_was_unk.*) {
            staged_ids[out_count.*] = self.header.unk_id;
            out_count.* += 1;
            last_was_unk.* = true;
        }
    }

    fn renderDecoded(self: TvocView, ids: []const u32, out_opt: ?[]u8) usize {
        const strip_dummy = (self.header.flags & flag_add_dummy_prefix) != 0 and
            (self.header.flags & flag_escape_ws) != 0;
        const escape_ws = (self.header.flags & flag_escape_ws) != 0;

        var out_len: usize = 0;
        var saw_content = false;
        var i: usize = 0;
        while (i < ids.len) {
            const meta = self.container.pieceAt(ids[i]) catch unreachable;
            switch (meta.kind) {
                .control, .unused => i += 1,
                .unknown => {
                    appendSlice(out_opt, &out_len, k_unk_surface);
                    saw_content = true;
                    i += 1;
                },
                .normal, .user_defined => {
                    var slice = meta.text;
                    if (!saw_content and strip_dummy and std.mem.startsWith(u8, slice, k_metaspace)) {
                        slice = slice[k_metaspace.len..];
                    }
                    saw_content = true;
                    appendUnescaped(out_opt, &out_len, slice, escape_ws);
                    i += 1;
                },
                .byte => {
                    var j = i;
                    while (j < ids.len) : (j += 1) {
                        const nxt = self.container.pieceAt(ids[j]) catch unreachable;
                        if (nxt.kind != .byte) break;
                    }
                    self.renderByteRun(ids[i..j], out_opt, &out_len);
                    saw_content = true;
                    i = j;
                },
            }
        }
        return out_len;
    }

    fn renderByteRun(self: TvocView, byte_ids: []const u32, out_opt: ?[]u8, out_len: *usize) void {
        var idx: usize = 0;
        while (idx < byte_ids.len) {
            var win: [4]u8 = undefined;
            const avail = @min(@as(usize, 4), byte_ids.len - idx);
            for (0..avail) |k| {
                const m = self.container.pieceAt(byte_ids[idx + k]) catch unreachable;
                win[k] = vocab.parseByteToken(m.text).?;
            }
            if (std.unicode.utf8ByteSequenceLength(win[0])) |seq_len| {
                if (seq_len <= avail) {
                    if (std.unicode.utf8Decode(win[0..seq_len])) |_| {
                        appendSlice(out_opt, out_len, win[0..seq_len]);
                        idx += seq_len;
                        continue;
                    } else |_| {}
                }
            } else |_| {}
            appendSlice(out_opt, out_len, k_utf8_replacement);
            idx += 1;
        }
    }
};

/// Conservative upper bound on caller workspace bytes required by `TvocView.encode()`.
pub fn minWorkBufBytes(text_len: usize, model_type: ModelType) Error!usize {
    const t3 = try vocab.mulChecked(text_len, 3);
    const max_norm = try vocab.addChecked(t3, 3);
    const max_ids = try vocab.addChecked(max_norm, 2);
    const staged_bytes = try vocab.addChecked(try vocab.mulChecked(max_ids, 4), 4);
    const dp_bytes: usize = switch (model_type) {
        .unigram => blk: {
            const n_nodes = try vocab.addChecked(max_norm, 1);
            const node_bytes = try vocab.mulChecked(n_nodes, @sizeOf(UnigramNode));
            const trace_bytes = try vocab.mulChecked(max_norm, @sizeOf(u32));
            break :blk try vocab.addChecked(try vocab.addChecked(node_bytes, trace_bytes), 16);
        },
        .bpe => blk: {
            const node_bytes = try vocab.mulChecked(max_norm, @sizeOf(BpeNode));
            break :blk try vocab.addChecked(node_bytes, 16);
        },
    };
    return try vocab.addChecked(try vocab.addChecked(staged_bytes, max_norm), dp_bytes);
}

const UnigramNode = struct {
    score: f32,
    tok_id: u32,
    prev_packed: u32,
};

const BpeNode = struct {
    prev: i32,
    next: i32,
    start: u32,
    len: u32,
    pair_score: f32,
    merge_id: u32,
};

fn runNormalizer(text: []const u8, flags: u8, out_opt: ?[]u8) Error!usize {
    const remove_extra = (flags & flag_remove_extra_ws) != 0;
    const escape_ws = (flags & flag_escape_ws) != 0;
    const add_dummy = (flags & flag_add_dummy_prefix) != 0;
    const norm_fw = (flags & flag_norm_fullwidth) != 0;
    const ws_rep: []const u8 = if (escape_ws) k_metaspace else " ";

    var out_len: usize = 0;
    var has_content = false;
    var pending_ws = false;
    var pos: usize = 0;

    while (pos < text.len) {
        const cp_len = std.unicode.utf8ByteSequenceLength(text[pos]) catch return error.InvalidArg;
        if (pos + cp_len > text.len) return error.InvalidArg;
        const cp = std.unicode.utf8Decode(text[pos .. pos + cp_len]) catch return error.InvalidArg;
        const mapped = mapCodepoint(cp, norm_fw);

        if (isWhitespaceCp(mapped)) {
            if (remove_extra) {
                if (has_content) pending_ws = true;
            } else {
                if (!has_content and add_dummy) appendSlice(out_opt, &out_len, ws_rep);
                appendSlice(out_opt, &out_len, ws_rep);
                has_content = true;
            }
        } else {
            if (!has_content) {
                if (add_dummy) appendSlice(out_opt, &out_len, ws_rep);
                has_content = true;
            } else if (pending_ws) {
                appendSlice(out_opt, &out_len, ws_rep);
                pending_ws = false;
            }

            if (mapped == cp) {
                appendSlice(out_opt, &out_len, text[pos .. pos + cp_len]);
            } else {
                var enc_buf: [4]u8 = undefined;
                const enc_len = std.unicode.utf8Encode(mapped, &enc_buf) catch unreachable;
                appendSlice(out_opt, &out_len, enc_buf[0..enc_len]);
            }
        }
        pos += cp_len;
    }

    return out_len;
}

fn mapCodepoint(cp: u21, norm_fw: bool) u21 {
    if (!norm_fw) return cp;
    if (cp >= 0xFF01 and cp <= 0xFF5E) return cp - 0xFF01 + 0x21;
    if (cp == 0x3000 or cp == 0x00A0) return ' ';
    return cp;
}

fn isWhitespaceCp(cp: u21) bool {
    return switch (cp) {
        ' ', '\t', '\n', '\r', 0x0B, 0x0C, 0x00A0, 0x3000 => true,
        else => false,
    };
}

fn appendUnescaped(out_opt: ?[]u8, out_len: *usize, slice: []const u8, escape_ws: bool) void {
    if (!escape_ws) {
        appendSlice(out_opt, out_len, slice);
        return;
    }
    var i: usize = 0;
    while (i < slice.len) {
        if (i + k_metaspace.len <= slice.len and
            std.mem.eql(u8, slice[i..][0..k_metaspace.len], k_metaspace))
        {
            if (out_opt) |out| out[out_len.*] = ' ';
            out_len.* += 1;
            i += k_metaspace.len;
        } else {
            if (out_opt) |out| out[out_len.*] = slice[i];
            out_len.* += 1;
            i += 1;
        }
    }
}

fn appendSlice(out_opt: ?[]u8, out_len: *usize, src: []const u8) void {
    if (out_opt) |out| @memcpy(out[out_len.*..][0..src.len], src);
    out_len.* += src.len;
}

fn allocAlignedSlice(comptime T: type, buf: []u8, cursor: *usize, count: usize) Error![]T {
    const base_addr = @intFromPtr(buf.ptr) + cursor.*;
    const aligned_addr = std.mem.alignForward(usize, base_addr, @alignOf(T));
    const pad = aligned_addr - base_addr;
    const byte_len = try vocab.mulChecked(count, @sizeOf(T));
    const end = try vocab.addChecked(try vocab.addChecked(cursor.*, pad), byte_len);
    if (end > buf.len) return error.InvalidSize;
    const start = cursor.* + pad;
    cursor.* = end;
    if (count == 0) return &.{};
    const typed: [*]T = @ptrCast(@alignCast(buf.ptr + start));
    return typed[0..count];
}
