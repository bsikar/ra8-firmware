//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Serialized per-page NV checkpoint (`TRJC`) encoder and strict decoder for
//! resumable background chapter translation. Performs zero dynamic memory
//! allocation and validates magic, version, CRC-32, bounds, UTF-8, and `TGL1`.

const std = @import("std");
const crc32 = @import("crc32");
const err_mod = @import("err");
const glossary = @import("glossary");
const sidecar = @import("sidecar");

pub const Error = err_mod.TranslateError;

pub const ckpt_magic: u32 = 0x434A5254; // Little-endian "TRJC".
pub const ckpt_version: u16 = 1;
pub const ckpt_header_bytes: usize = 32;
pub const chapter_spec_bytes: usize = 68;

pub const max_queue: usize = 16;
pub const max_pages: usize = 64;
pub const max_bubbles: usize = 256;
pub const max_page_bubbles: usize = 64;
pub const max_line_breaks: usize = 2048;
pub const max_text_pool: usize = 16384;
pub const max_batch: usize = 16;
pub const default_batch: usize = 8;
pub const default_batt_pct: u8 = 25;
pub const max_glossary_bytes: usize = 3072;

/// Source trigger that enqueued a chapter job (`ra8_translate_job_trigger_t`).
pub const Trigger = enum(u8) {
    mdl_download = 1,
    user_request = 2,

    pub fn fromRaw(raw: u8) ?Trigger {
        return switch (raw) {
            1 => .mdl_download,
            2 => .user_request,
            else => null,
        };
    }
};

/// Scheduler operational state (`ra8_translate_job_state_t`).
pub const State = enum(u8) {
    idle = 0,
    running = 1,
    yielded = 2,
    paused_battery = 3,
    completed = 4,
    failed = 5,
};

/// Active chapter pipeline stage (`ra8_translate_job_stage_t`).
pub const Stage = enum(u8) {
    none = 0,
    detect_ocr = 1,
    lang_glossary = 2,
    translate_fit = 3,
    commit_sidecar = 4,
    done = 5,

    pub fn fromRaw(raw: u8) ?Stage {
        return switch (raw) {
            0 => .none,
            1 => .detect_ocr,
            2 => .lang_glossary,
            3 => .translate_fit,
            4 => .commit_sidecar,
            5 => .done,
            else => null,
        };
    }
};

/// Pause reason reported when scheduler yields or enters low-battery wait.
pub const PauseReason = enum(u8) {
    none = 0,
    reader_active = 1,
    display_busy = 2,
    low_battery = 3,
};

/// Per-chapter status exposed to reader UI (`ra8_translate_job_chapter_state_t`).
pub const ChapterState = enum(u8) {
    none = 0,
    queued = 1,
    translating = 2,
    paused = 3,
};

/// C ABI power and foreground contention sample (`ra8_translate_job_env_t`).
pub const EnvState = extern struct {
    battery_pct: u8 = 100,
    charging: u8 = 0,
    reader_active: u8 = 0,
    display_busy: u8 = 0,
};

/// C ABI OCR page-detector output item (`ra8_translate_job_ocr_bubble_t`).
pub const OcrBubble = extern struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    shape: u8,
    text_orientation: u8,
    corner_radius: u16,
    reading_order: u16,
    ocr_confidence_bps: u16,
    source_off: u32,
    source_len: u32,
};

/// C ABI byte span within flat batch translation buffers (`ra8_translate_job_span_t`).
pub const Span = extern struct {
    off: u32 = 0,
    len: u32 = 0,
};

/// C ABI observable scheduler snapshot (`ra8_translate_job_status_t`).
pub const Status = extern struct {
    active_title_id: u32 = 0,
    active_chapter_order: u32 = 0,
    checkpoint_seq: u32 = 0,
    queued_count: u16 = 0,
    page_count: u16 = 0,
    pages_ocr_done: u16 = 0,
    pages_translated_done: u16 = 0,
    last_error: u16 = 0,
    state: u8 = @backingInt(State.idle),
    stage: u8 = @backingInt(Stage.none),
    pause_reason: u8 = @backingInt(PauseReason.none),
    resolved_lang: u8 = @backingInt(sidecar.Language.unknown),
    has_active: u8 = 0,
    reserved0: [5]u8 = .{ 0, 0, 0, 0, 0 },
};

/// Validate detected OCR bubbles for one page before appending to active state.
pub fn validateOcrBubbles(items: []const OcrBubble, text_buf: []const u8, pw: u16, ph: u16) Error!u32 {
    var seen_mask: u64 = 0;
    var extra_pool: u32 = 0;
    for (items) |b| {
        if (b.w == 0 or b.h == 0) return Error.ValidationFailed;
        if (@as(u32, b.x) + b.w > pw or @as(u32, b.y) + b.h > ph) return Error.ValidationFailed;
        if (sidecar.Shape.fromRaw(b.shape) == null) return Error.InvalidArg;
        if (sidecar.TextOrientation.fromRaw(b.text_orientation) == null) return Error.InvalidArg;
        if (b.ocr_confidence_bps > sidecar.max_confidence_bps) return Error.ValidationFailed;
        if (b.reading_order >= items.len) return Error.ValidationFailed;
        const bit: u64 = @as(u64, 1) << @intCast(b.reading_order);
        if ((seen_mask & bit) != 0) return Error.ValidationFailed;
        seen_mask |= bit;

        const end_u64: u64 = @as(u64, b.source_off) + b.source_len;
        if (end_u64 > text_buf.len) return Error.ValidationFailed;
        if (!std.unicode.utf8ValidateSlice(text_buf[b.source_off..][0..b.source_len])) return Error.ValidationFailed;
        extra_pool += b.source_len;
    }
    return extra_pool;
}

/// C ABI chapter descriptor (`ra8_translate_job_chapter_t`).
pub const ChapterSpec = extern struct {
    sidecar_key: sidecar.SidecarKey = .{
        .chapter_hash = @splat(0),
        .detector_model_version = 0,
        .ocr_model_version = 0,
        .nmt_model_version = 0,
        .glossary_revision = 0,
        .source_lang = 0,
        .reserved = .{ 0, 0, 0 },
    },
    title_id: u32 = 0,
    chapter_order: u32 = 0,
    page_count: u16 = 0,
    lang_hint: u8 = 0,
    unread: u8 = 0,
    trigger: u8 = 0,
    reserved0: [3]u8 = .{ 0, 0, 0 },
};

/// Immutable view over active workspace state to persist into `TRJC`.
pub const SerializeInput = struct {
    chapter: *const ChapterSpec,
    stage: Stage,
    resolved_lang: sidecar.Language,
    checkpoint_seq: u32,
    pages_ocr_done: u16,
    pages_translated_done: u16,
    pages: []const sidecar.PageEntry,
    bubbles: []const sidecar.BubbleRecord,
    line_breaks: []const u16,
    text_pool: []const u8,
    tgl1_bytes: []const u8,
};

/// Mutable target buffers filled atomically when restoring a `TRJC` checkpoint.
pub const RestoreTarget = struct {
    chapter: *ChapterSpec,
    stage: *Stage,
    resolved_lang: *sidecar.Language,
    checkpoint_seq: *u32,
    pages_ocr_done: *u16,
    pages_translated_done: *u16,
    bubble_count: *u16,
    line_break_count: *u32,
    text_pool_bytes: *u32,
    tgl1_len: *u32,
    pages: []sidecar.PageEntry,
    bubbles: []sidecar.BubbleRecord,
    line_breaks: []u16,
    text_pool: []u8,
    tgl1_bytes: []u8,
};

/// Validate a chapter descriptor for enqueueing or checkpoint loading.
pub fn validateChapterSpec(spec: *const ChapterSpec) Error!void {
    if (spec.page_count == 0 or spec.page_count > max_pages) return Error.InvalidSize;
    if (sidecar.Language.fromRaw(spec.lang_hint) == null) return Error.InvalidArg;
    if (spec.unread > 1) return Error.InvalidArg;
    if (Trigger.fromRaw(spec.trigger) == null) return Error.InvalidArg;
    if (!allZero(&spec.reserved0)) return Error.InvalidArg;
    try validateSidecarKey(&spec.sidecar_key);
}

fn validateSidecarKey(key: *const sidecar.SidecarKey) Error!void {
    if (key.detector_model_version == 0 or key.ocr_model_version == 0 or key.nmt_model_version == 0) {
        return Error.InvalidArg;
    }
    if (key.glossary_revision == 0) return Error.InvalidArg;
    if (sidecar.Language.fromRaw(key.source_lang) == null) return Error.InvalidArg;
    if (!allZero(&key.reserved)) return Error.InvalidArg;
}

/// Encode `input` into CRC-32-protected `TRJC` wire bytes inside `out_buf`.
pub fn serializeCheckpoint(input: SerializeInput, out_buf: []u8) Error!usize {
    if (input.pages.len > max_pages or input.bubbles.len > max_bubbles) return Error.InvalidSize;
    if (input.line_breaks.len > max_line_breaks or input.text_pool.len > max_text_pool) return Error.InvalidSize;
    if (input.tgl1_bytes.len > max_glossary_bytes) return Error.InvalidSize;
    if (input.pages.len != input.pages_ocr_done) return Error.InvalidArg;

    const total = ckpt_header_bytes + chapter_spec_bytes + 8 +
        input.pages.len * sidecar.page_bytes +
        input.bubbles.len * sidecar.bubble_bytes +
        input.line_breaks.len * 2 +
        input.text_pool.len +
        input.tgl1_bytes.len;
    if (out_buf.len < total) return Error.InvalidSize;

    const hdr = out_buf[0..ckpt_header_bytes];
    writeU32Le(hdr[0..4], ckpt_magic);
    writeU16Le(hdr[4..6], ckpt_version);
    writeU16Le(hdr[6..8], 0);
    writeU32Le(hdr[8..12], @intCast(total));
    writeU32Le(hdr[12..16], 0);
    hdr[16] = @backingInt(input.stage);
    hdr[17] = @backingInt(input.resolved_lang);
    writeU16Le(hdr[18..20], input.pages_ocr_done);
    writeU16Le(hdr[20..22], input.pages_translated_done);
    writeU16Le(hdr[22..24], @intCast(input.bubbles.len));
    writeU32Le(hdr[24..28], @intCast(input.line_breaks.len));
    writeU32Le(hdr[28..32], @intCast(input.text_pool.len));

    var cursor: usize = ckpt_header_bytes;
    encodeChapterSpec(out_buf[cursor..][0..chapter_spec_bytes], input.chapter);
    cursor += chapter_spec_bytes;
    writeU32Le(out_buf[cursor..][0..4], input.checkpoint_seq);
    cursor += 4;
    writeU32Le(out_buf[cursor..][0..4], @intCast(input.tgl1_bytes.len));
    cursor += 4;

    for (input.pages) |p| {
        encodePageEntry(out_buf[cursor..][0..sidecar.page_bytes], p);
        cursor += sidecar.page_bytes;
    }
    for (input.bubbles) |b| {
        encodeBubbleRecord(out_buf[cursor..][0..sidecar.bubble_bytes], b);
        cursor += sidecar.bubble_bytes;
    }
    for (input.line_breaks) |lb| {
        writeU16Le(out_buf[cursor..][0..2], lb);
        cursor += 2;
    }
    if (input.text_pool.len > 0) {
        @memcpy(out_buf[cursor..][0..input.text_pool.len], input.text_pool);
        cursor += input.text_pool.len;
    }
    if (input.tgl1_bytes.len > 0) {
        @memcpy(out_buf[cursor..][0..input.tgl1_bytes.len], input.tgl1_bytes);
        cursor += input.tgl1_bytes.len;
    }

    const crc = crc32.crc32WithZeroWindow(out_buf[0..total], 12, 4);
    writeU32Le(out_buf[12..16], crc);
    return total;
}

/// Validate a `TRJC` checkpoint blob strictly before populating `dst` atomically.
pub fn restoreCheckpoint(bytes: []const u8, dst: RestoreTarget) Error!void {
    const min_bytes = ckpt_header_bytes + chapter_spec_bytes + 8;
    if (bytes.len < min_bytes) return Error.InvalidSize;
    if (readU32Le(bytes[0..4]) != ckpt_magic) return Error.InvalidArg;
    if (readU16Le(bytes[4..6]) != ckpt_version or readU16Le(bytes[6..8]) != 0) return Error.NotSupported;

    const total = readU32Le(bytes[8..12]);
    if (@as(u64, total) != @as(u64, bytes.len)) return Error.InvalidSize;
    if (crc32.crc32WithZeroWindow(bytes, 12, 4) != readU32Le(bytes[12..16])) return Error.CrcMismatch;

    const stage = Stage.fromRaw(bytes[16]) orelse return Error.InvalidArg;
    if (stage == .none or stage == .done) return Error.InvalidArg;
    const resolved_lang = sidecar.Language.fromRaw(bytes[17]) orelse return Error.InvalidArg;
    const pages_ocr_done = readU16Le(bytes[18..20]);
    const pages_tr_done = readU16Le(bytes[20..22]);
    const bubble_count = readU16Le(bytes[22..24]);
    const break_count = readU32Le(bytes[24..28]);
    const pool_bytes = readU32Le(bytes[28..32]);

    const chapter = decodeChapterSpec(bytes[ckpt_header_bytes..][0..chapter_spec_bytes]);
    try validateChapterSpec(&chapter);

    var cursor: usize = ckpt_header_bytes + chapter_spec_bytes;
    const seq = readU32Le(bytes[cursor..][0..4]);
    cursor += 4;
    const tgl_len = readU32Le(bytes[cursor..][0..4]);
    cursor += 4;

    if (seq == 0) return Error.InvalidArg;
    if (pages_ocr_done > chapter.page_count or pages_tr_done > pages_ocr_done) return Error.ValidationFailed;
    if (bubble_count > max_bubbles or break_count > max_line_breaks or pool_bytes > max_text_pool) {
        return Error.InvalidSize;
    }
    if (tgl_len > max_glossary_bytes) return Error.InvalidSize;

    const need_u64: u64 = @as(u64, cursor) +
        @as(u64, pages_ocr_done) * sidecar.page_bytes +
        @as(u64, bubble_count) * sidecar.bubble_bytes +
        @as(u64, break_count) * 2 +
        @as(u64, pool_bytes) +
        @as(u64, tgl_len);
    if (need_u64 != @as(u64, total)) return Error.InvalidSize;

    const pages_off = cursor;
    cursor += @as(usize, pages_ocr_done) * sidecar.page_bytes;
    const bubbles_off = cursor;
    cursor += @as(usize, bubble_count) * sidecar.bubble_bytes;
    const breaks_off = cursor;
    cursor += @as(usize, break_count) * 2;
    const pool_off = cursor;
    cursor += @as(usize, pool_bytes);
    const tgl_off = cursor;

    const pool_slice = bytes[pool_off..][0..@intCast(pool_bytes)];
    const tgl_slice = bytes[tgl_off..][0..@intCast(tgl_len)];
    if (tgl_len > 0) {
        const view = try glossary.TglView.open(tgl_slice);
        if (resolved_lang != .unknown and view.source_lang != .unknown and view.source_lang != resolved_lang) {
            return Error.ValidationFailed;
        }
    }

    try validateWireTables(
        bytes[pages_off..][0 .. @as(usize, pages_ocr_done) * sidecar.page_bytes],
        bytes[bubbles_off..][0 .. @as(usize, bubble_count) * sidecar.bubble_bytes],
        bytes[breaks_off..][0 .. @as(usize, break_count) * 2],
        pool_slice,
        pages_ocr_done,
        pages_tr_done,
        bubble_count,
        break_count,
    );

    if (dst.pages.len < pages_ocr_done or dst.bubbles.len < bubble_count or
        dst.line_breaks.len < break_count or dst.text_pool.len < pool_bytes or
        dst.tgl1_bytes.len < tgl_len)
    {
        return Error.InvalidSize;
    }

    var pi: usize = 0;
    while (pi < pages_ocr_done) : (pi += 1) {
        dst.pages[pi] = decodePageEntry(bytes[pages_off + pi * sidecar.page_bytes ..][0..sidecar.page_bytes]);
    }
    var bi: usize = 0;
    while (bi < bubble_count) : (bi += 1) {
        dst.bubbles[bi] = decodeBubbleRecord(bytes[bubbles_off + bi * sidecar.bubble_bytes ..][0..sidecar.bubble_bytes]);
    }
    var li: usize = 0;
    while (li < break_count) : (li += 1) {
        dst.line_breaks[li] = readU16Le(bytes[breaks_off + li * 2 ..][0..2]);
    }
    if (pool_bytes > 0) @memcpy(dst.text_pool[0..@intCast(pool_bytes)], pool_slice);
    if (tgl_len > 0) @memcpy(dst.tgl1_bytes[0..@intCast(tgl_len)], tgl_slice);

    dst.chapter.* = chapter;
    dst.stage.* = stage;
    dst.resolved_lang.* = resolved_lang;
    dst.checkpoint_seq.* = seq;
    dst.pages_ocr_done.* = pages_ocr_done;
    dst.pages_translated_done.* = pages_tr_done;
    dst.bubble_count.* = bubble_count;
    dst.line_break_count.* = break_count;
    dst.text_pool_bytes.* = pool_bytes;
    dst.tgl1_len.* = tgl_len;
}

fn validateWireTables(
    pages_wire: []const u8,
    bubbles_wire: []const u8,
    breaks_wire: []const u8,
    pool: []const u8,
    page_count: u16,
    pages_tr_done: u16,
    bubble_count: u16,
    break_count: u32,
) Error!void {
    var expected_first: u32 = 0;
    var expected_lb: u32 = 0;
    var pi: u16 = 0;
    while (pi < page_count) : (pi += 1) {
        const p = decodePageEntry(pages_wire[@as(usize, pi) * sidecar.page_bytes ..][0..sidecar.page_bytes]);
        if (p.page_index != pi or p.page_width == 0 or p.page_height == 0) return Error.ValidationFailed;
        if (p.bubble_count > max_page_bubbles) return Error.InvalidSize;
        if (p.first_bubble != expected_first) return Error.ValidationFailed;
        const page_end: u64 = @as(u64, p.first_bubble) + @as(u64, p.bubble_count);
        if (page_end > bubble_count) return Error.ValidationFailed;

        const is_translated = pi < pages_tr_done;
        var k: u16 = 0;
        while (k < p.bubble_count) : (k += 1) {
            const bi: usize = @intCast(p.first_bubble + k);
            const b = decodeBubbleRecord(bubbles_wire[bi * sidecar.bubble_bytes ..][0..sidecar.bubble_bytes]);
            if (b.w == 0 or b.h == 0) return Error.ValidationFailed;
            if (@as(u32, b.x) + b.w > p.page_width or @as(u32, b.y) + b.h > p.page_height) return Error.ValidationFailed;
            if (sidecar.Shape.fromRaw(b.shape) == null) return Error.ValidationFailed;
            if (sidecar.TextOrientation.fromRaw(b.text_orientation) == null) return Error.ValidationFailed;
            if (b.ocr_confidence_bps > sidecar.max_confidence_bps) return Error.ValidationFailed;
            if (!allZero(&b.reserved)) return Error.ValidationFailed;
            try validatePoolUtf8(pool, b.source_text_off, b.source_text_len);

            if (is_translated) {
                if (b.first_line_break != expected_lb) return Error.ValidationFailed;
                if (@as(u64, b.first_line_break) + b.line_count > break_count) return Error.ValidationFailed;
                try validatePoolUtf8(pool, b.english_text_off, b.english_text_len);
                if (b.english_text_len > 0) {
                    if (b.font_size_px == 0 or b.line_count == 0) return Error.ValidationFailed;
                    try validateBreakSlice(breaks_wire, b.first_line_break, b.line_count, b.english_text_len);
                } else if (b.line_count != 0) {
                    return Error.ValidationFailed;
                }
                expected_lb += b.line_count;
            } else if (b.line_count != 0 or b.english_text_len != 0 or b.font_size_px != 0) {
                return Error.ValidationFailed;
            }
        }
        expected_first = @intCast(page_end);
    }
    if (expected_first != bubble_count or expected_lb != break_count) return Error.ValidationFailed;
}

fn validateBreakSlice(breaks_wire: []const u8, first: u32, count: u16, eng_len: u32) Error!void {
    var prev: u16 = 0;
    var i: u16 = 0;
    while (i < count) : (i += 1) {
        const off = (@as(usize, first) + i) * 2;
        const brk = readU16Le(breaks_wire[off..][0..2]);
        if (brk <= prev or brk > eng_len) return Error.ValidationFailed;
        prev = brk;
    }
    if (prev != eng_len) return Error.ValidationFailed;
}

fn validatePoolUtf8(pool: []const u8, off: u32, len: u32) Error!void {
    const end: u64 = @as(u64, off) + @as(u64, len);
    if (end > pool.len) return Error.ValidationFailed;
    if (!std.unicode.utf8ValidateSlice(pool[@intCast(off)..][0..@intCast(len)])) return Error.ValidationFailed;
}

fn encodeChapterSpec(dst: *[chapter_spec_bytes]u8, s: *const ChapterSpec) void {
    @memcpy(dst[0..32], &s.sidecar_key.chapter_hash);
    writeU32Le(dst[32..36], s.sidecar_key.detector_model_version);
    writeU32Le(dst[36..40], s.sidecar_key.ocr_model_version);
    writeU32Le(dst[40..44], s.sidecar_key.nmt_model_version);
    writeU32Le(dst[44..48], s.sidecar_key.glossary_revision);
    dst[48] = s.sidecar_key.source_lang;
    dst[49..52].* = s.sidecar_key.reserved;
    writeU32Le(dst[52..56], s.title_id);
    writeU32Le(dst[56..60], s.chapter_order);
    writeU16Le(dst[60..62], s.page_count);
    dst[62] = s.lang_hint;
    dst[63] = s.unread;
    dst[64] = s.trigger;
    dst[65..68].* = s.reserved0;
}

fn decodeChapterSpec(src: *const [chapter_spec_bytes]u8) ChapterSpec {
    return .{
        .sidecar_key = .{
            .chapter_hash = src[0..32].*,
            .detector_model_version = readU32Le(src[32..36]),
            .ocr_model_version = readU32Le(src[36..40]),
            .nmt_model_version = readU32Le(src[40..44]),
            .glossary_revision = readU32Le(src[44..48]),
            .source_lang = src[48],
            .reserved = src[49..52].*,
        },
        .title_id = readU32Le(src[52..56]),
        .chapter_order = readU32Le(src[56..60]),
        .page_count = readU16Le(src[60..62]),
        .lang_hint = src[62],
        .unread = src[63],
        .trigger = src[64],
        .reserved0 = src[65..68].*,
    };
}

fn encodePageEntry(dst: *[sidecar.page_bytes]u8, p: sidecar.PageEntry) void {
    writeU16Le(dst[0..2], p.page_index);
    writeU16Le(dst[2..4], p.page_width);
    writeU16Le(dst[4..6], p.page_height);
    writeU16Le(dst[6..8], p.bubble_count);
    writeU32Le(dst[8..12], p.first_bubble);
}

fn decodePageEntry(src: *const [sidecar.page_bytes]u8) sidecar.PageEntry {
    return .{
        .page_index = readU16Le(src[0..2]),
        .page_width = readU16Le(src[2..4]),
        .page_height = readU16Le(src[4..6]),
        .bubble_count = readU16Le(src[6..8]),
        .first_bubble = readU32Le(src[8..12]),
    };
}

fn encodeBubbleRecord(dst: *[sidecar.bubble_bytes]u8, b: sidecar.BubbleRecord) void {
    writeU16Le(dst[0..2], b.x);
    writeU16Le(dst[2..4], b.y);
    writeU16Le(dst[4..6], b.w);
    writeU16Le(dst[6..8], b.h);
    dst[8] = b.shape;
    dst[9] = b.text_orientation;
    writeU16Le(dst[10..12], b.corner_radius);
    writeU16Le(dst[12..14], b.reading_order);
    writeU16Le(dst[14..16], b.ocr_confidence_bps);
    writeU16Le(dst[16..18], b.font_size_px);
    writeU16Le(dst[18..20], b.line_count);
    writeU32Le(dst[20..24], b.first_line_break);
    writeU32Le(dst[24..28], b.source_text_off);
    writeU32Le(dst[28..32], b.source_text_len);
    writeU32Le(dst[32..36], b.english_text_off);
    writeU32Le(dst[36..40], b.english_text_len);
    dst[40] = b.bubble_flags;
    dst[41..44].* = b.reserved;
}

fn decodeBubbleRecord(src: *const [sidecar.bubble_bytes]u8) sidecar.BubbleRecord {
    return .{
        .x = readU16Le(src[0..2]),
        .y = readU16Le(src[2..4]),
        .w = readU16Le(src[4..6]),
        .h = readU16Le(src[6..8]),
        .shape = src[8],
        .text_orientation = src[9],
        .corner_radius = readU16Le(src[10..12]),
        .reading_order = readU16Le(src[12..14]),
        .ocr_confidence_bps = readU16Le(src[14..16]),
        .font_size_px = readU16Le(src[16..18]),
        .line_count = readU16Le(src[18..20]),
        .first_line_break = readU32Le(src[20..24]),
        .source_text_off = readU32Le(src[24..28]),
        .source_text_len = readU32Le(src[28..32]),
        .english_text_off = readU32Le(src[32..36]),
        .english_text_len = readU32Le(src[36..40]),
        .bubble_flags = src[40],
        .reserved = src[41..44].*,
    };
}

pub fn allZero(bytes: []const u8) bool {
    for (bytes) |b| {
        if (b != 0) return false;
    }
    return true;
}

fn writeU16Le(dst: *[2]u8, v: u16) void {
    std.mem.writeInt(u16, dst, v, .little);
}

fn writeU32Le(dst: *[4]u8, v: u32) void {
    std.mem.writeInt(u32, dst, v, .little);
}

fn readU16Le(src: *const [2]u8) u16 {
    return std.mem.readInt(u16, src, .little);
}

fn readU32Le(src: *const [4]u8) u32 {
    return std.mem.readInt(u32, src, .little);
}
