//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding `TRN1` per-chapter translation sidecar reader and writer.
//! Performs zero heap allocations, validates all section bounds, CRC-32,
//! reading-order permutations, line breaks, and UTF-8 spans before publishing.

const std = @import("std");
const crc32 = @import("crc32");
const err_mod = @import("err");

pub const Error = err_mod.TranslateError;

pub const magic: u32 = 0x314E5254; // Little-endian "TRN1".
pub const version: u16 = 1;
pub const header_bytes: usize = 96;
pub const page_bytes: usize = 12;
pub const bubble_bytes: usize = 44;
pub const max_page_bubbles: u16 = 1024;
pub const max_confidence_bps: u16 = 10000;
pub const chapter_hash_bytes: usize = 32;
pub const bflag_glossary: u8 = 1 << 0;

/// Translation source script / language identifier (`ra8_translate_lang_t`).
pub const Language = enum(u8) {
    unknown = 0,
    korean = 1,
    japanese = 2,
    chinese = 3,

    /// Decode a raw ABI byte safely without panicking on unknown discriminants.
    pub fn fromRaw(raw: u8) ?Language {
        return switch (raw) {
            0 => .unknown,
            1 => .korean,
            2 => .japanese,
            3 => .chinese,
            else => null,
        };
    }
};

/// Speech or narration bubble contour shape (`ra8_translate_shape_t`).
pub const Shape = enum(u8) {
    rect = 0,
    ellipse = 1,
    rounded_rect = 2,
    cloud = 3,

    /// Decode a raw ABI byte safely without panicking on unknown discriminants.
    pub fn fromRaw(raw: u8) ?Shape {
        return switch (raw) {
            0 => .rect,
            1 => .ellipse,
            2 => .rounded_rect,
            3 => .cloud,
            else => null,
        };
    }
};

/// Source OCR text orientation (`ra8_translate_orientation_t`).
pub const TextOrientation = enum(u8) {
    horizontal = 0,
    vertical_rl = 1,
    vertical_lr = 2,

    /// Decode a raw ABI byte safely without panicking on unknown discriminants.
    pub fn fromRaw(raw: u8) ?TextOrientation {
        return switch (raw) {
            0 => .horizontal,
            1 => .vertical_rl,
            2 => .vertical_lr,
            else => null,
        };
    }
};

/// Cache invalidation key (`ra8_translate_sidecar_key_t`).
pub const SidecarKey = extern struct {
    chapter_hash: [chapter_hash_bytes]u8 = @splat(0),
    detector_model_version: u32 = 0,
    ocr_model_version: u32 = 0,
    nmt_model_version: u32 = 0,
    glossary_revision: u32 = 0,
    source_lang: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// Validated `TRN1` header view (`ra8_translate_sidecar_header_t`).
pub const SidecarHeader = extern struct {
    total_bytes: u32,
    file_crc32: u32,
    key: SidecarKey,
    page_count: u16,
    reserved: u16,
    bubble_count: u32,
    line_break_count: u32,
    text_pool_bytes: u32,
};

/// Per-page descriptor (`ra8_translate_sidecar_page_t`).
pub const PageEntry = extern struct {
    page_index: u16,
    bubble_count: u16,
    first_bubble: u32,
    page_width: u16,
    page_height: u16,
};

/// Per-bubble metadata record (`ra8_translate_sidecar_bubble_t`).
pub const BubbleRecord = extern struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    shape: u8,
    text_orientation: u8,
    corner_radius: u16,
    reading_order: u16,
    ocr_confidence_bps: u16,
    font_size_px: u16,
    line_count: u16,
    first_line_break: u32,
    source_text_off: u32,
    source_text_len: u32,
    english_text_off: u32,
    english_text_len: u32,
    bubble_flags: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// Rich Zig-native bubble descriptor used by `writeSidecarFromInputs`.
pub const BubbleInput = struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    shape: Shape,
    text_orientation: TextOrientation,
    corner_radius: u16 = 0,
    reading_order: u16,
    ocr_confidence_bps: u16,
    font_size_px: u16,
    bubble_flags: u8 = 0,
    line_breaks: []const u16,
    source_text: []const u8,
    english_text: []const u8,
};

/// Computed byte offsets of all four variable sections in a `TRN1` container.
pub const Sections = struct {
    page_table_off: usize,
    bubble_table_off: usize,
    line_break_table_off: usize,
    text_pool_off: usize,
    text_pool_bytes: usize,
    total_bytes: usize,
};

/// Validated zero-allocation view over an immutable serialized `TRN1` buffer.
pub const SidecarView = struct {
    bytes: []const u8,
    header: SidecarHeader,
    sections: Sections,

    /// Validate every header field, whole-file CRC-32, tables, and UTF-8 spans.
    pub fn open(bytes: []const u8) Error!SidecarView {
        if (bytes.len < header_bytes) return Error.InvalidSize;
        if (readU32Le(bytes[0..4]) != magic) return Error.InvalidArg;
        if (readU16Le(bytes[4..6]) != version) return Error.NotSupported;
        if (readU16Le(bytes[6..8]) != 0) return Error.NotSupported;

        const total_u32 = readU32Le(bytes[8..12]);
        if (@as(u64, total_u32) != @as(u64, bytes.len)) return Error.InvalidSize;

        const stored_crc = readU32Le(bytes[12..16]);
        if (crc32.crc32WithZeroWindow(bytes, 12, 4) != stored_crc) return Error.CrcMismatch;

        var key = SidecarKey{
            .chapter_hash = undefined,
            .detector_model_version = readU32Le(bytes[0x30..0x34]),
            .ocr_model_version = readU32Le(bytes[0x34..0x38]),
            .nmt_model_version = readU32Le(bytes[0x38..0x3C]),
            .glossary_revision = readU32Le(bytes[0x3C..0x40]),
            .source_lang = bytes[0x40],
            .reserved = .{ bytes[0x41], bytes[0x42], bytes[0x43] },
        };
        @memcpy(&key.chapter_hash, bytes[0x10..0x30]);
        try validateKey(&key);

        const page_count = readU16Le(bytes[0x44..0x46]);
        const reserved1 = readU16Le(bytes[0x46..0x48]);
        if (reserved1 != 0) return Error.InvalidArg;
        if (!allZero(bytes[0x54..0x60])) return Error.InvalidArg;

        const bubble_count = readU32Le(bytes[0x48..0x4C]);
        const line_break_count = readU32Le(bytes[0x4C..0x50]);
        const text_pool_bytes = readU32Le(bytes[0x50..0x54]);

        const sections = try computeSections(page_count, bubble_count, line_break_count, text_pool_bytes);
        if (sections.total_bytes != bytes.len) return Error.InvalidSize;

        try validateWireTables(bytes, page_count, bubble_count, line_break_count, sections);

        return .{
            .bytes = bytes,
            .header = .{
                .total_bytes = total_u32,
                .file_crc32 = stored_crc,
                .key = key,
                .page_count = page_count,
                .reserved = reserved1,
                .bubble_count = bubble_count,
                .line_break_count = line_break_count,
                .text_pool_bytes = text_pool_bytes,
            },
            .sections = sections,
        };
    }

    /// Return true iff chapter hash, model versions, glossary revision, and language match.
    pub fn matchesKey(self: SidecarView, expected: *const SidecarKey) Error!bool {
        return keysMatch(&self.header.key, expected);
    }

    /// Decode page record at `page_index`.
    pub fn pageAt(self: SidecarView, page_index: u16) Error!PageEntry {
        if (page_index >= self.header.page_count) return Error.OutOfRange;
        return readPageEntry(self.bytes, self.sections.page_table_off, page_index);
    }

    /// Decode bubble record at `bubble_index`.
    pub fn bubbleAt(self: SidecarView, bubble_index: u32) Error!BubbleRecord {
        if (bubble_index >= self.header.bubble_count) return Error.OutOfRange;
        return readBubbleRecord(self.bytes, self.sections.bubble_table_off, bubble_index);
    }

    /// Borrow UTF-8 source text slice directly inside `self.bytes`.
    pub fn sourceText(self: SidecarView, bubble_index: u32) Error![]const u8 {
        const bubble = try self.bubbleAt(bubble_index);
        const start = self.sections.text_pool_off + @as(usize, bubble.source_text_off);
        const length = @as(usize, bubble.source_text_len);
        return self.bytes[start .. start + length];
    }

    /// Borrow UTF-8 English text slice directly inside `self.bytes`.
    pub fn englishText(self: SidecarView, bubble_index: u32) Error![]const u8 {
        const bubble = try self.bubbleAt(bubble_index);
        const start = self.sections.text_pool_off + @as(usize, bubble.english_text_off);
        const length = @as(usize, bubble.english_text_len);
        return self.bytes[start .. start + length];
    }

    /// Copy fitted `u16` line-break end offsets into caller slice `out`.
    pub fn lineBreaks(self: SidecarView, bubble_index: u32, out: []u16) Error!u16 {
        const bubble = try self.bubbleAt(bubble_index);
        if (out.len < bubble.line_count) return Error.InvalidSize;
        var idx: usize = 0;
        while (idx < bubble.line_count) : (idx += 1) {
            const lb_index = @as(usize, bubble.first_line_break) + idx;
            out[idx] = readU16Le(self.bytes[self.sections.line_break_table_off + lb_index * 2 ..][0..2]);
        }
        return bubble.line_count;
    }
};

/// Compare two validated sidecar keys field-by-field.
pub fn keysMatch(actual: *const SidecarKey, expected: *const SidecarKey) Error!bool {
    try validateKey(actual);
    try validateKey(expected);
    return std.mem.eql(u8, &actual.chapter_hash, &expected.chapter_hash) and
        actual.detector_model_version == expected.detector_model_version and
        actual.ocr_model_version == expected.ocr_model_version and
        actual.nmt_model_version == expected.nmt_model_version and
        actual.glossary_revision == expected.glossary_revision and
        actual.source_lang == expected.source_lang;
}

/// Validate cache invalidation key constraints (non-zero models, valid enum, zero padding).
pub fn validateKey(key: *const SidecarKey) Error!void {
    if (key.detector_model_version == 0 or
        key.ocr_model_version == 0 or
        key.nmt_model_version == 0)
    {
        return Error.InvalidArg;
    }
    if (Language.fromRaw(key.source_lang) == null) return Error.InvalidArg;
    if (!allZero(&key.reserved)) return Error.InvalidArg;
}

/// Compute exact section layout with overflow protection.
pub fn computeSections(
    page_count: u16,
    bubble_count: u32,
    line_break_count: u32,
    text_pool_bytes: u32,
) Error!Sections {
    const pages_bytes: u64 = @as(u64, page_count) * page_bytes;
    const bubbles_bytes: u64 = @as(u64, bubble_count) * bubble_bytes;
    const breaks_bytes: u64 = @as(u64, line_break_count) * 2;

    const b_off: u64 = @as(u64, header_bytes) + pages_bytes;
    const lb_off: u64 = b_off + bubbles_bytes;
    const tp_off: u64 = lb_off + breaks_bytes;
    const total_u64: u64 = tp_off + @as(u64, text_pool_bytes);

    if (total_u64 > std.math.maxInt(u32) or total_u64 > std.math.maxInt(usize)) {
        return Error.InvalidSize;
    }
    return .{
        .page_table_off = header_bytes,
        .bubble_table_off = @intCast(b_off),
        .line_break_table_off = @intCast(lb_off),
        .text_pool_off = @intCast(tp_off),
        .text_pool_bytes = @intCast(text_pool_bytes),
        .total_bytes = @intCast(total_u64),
    };
}

/// Compute exact required output bytes for flat-table sidecar serialization.
pub fn requiredBytes(
    page_count: u16,
    bubble_count: u32,
    line_break_count: u32,
    text_pool_bytes: u32,
) Error!usize {
    const sections = try computeSections(page_count, bubble_count, line_break_count, text_pool_bytes);
    return sections.total_bytes;
}

/// Compute exact required output bytes for rich Zig `BubbleInput` descriptors.
pub fn requiredBytesForInputs(pages: []const PageEntry, bubbles: []const BubbleInput) Error!usize {
    if (pages.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (bubbles.len > std.math.maxInt(u32)) return Error.InvalidSize;
    var lb_count: u64 = 0;
    var pool_bytes: u64 = 0;
    for (bubbles) |b| {
        lb_count += b.line_breaks.len;
        pool_bytes += b.source_text.len;
        pool_bytes += b.english_text.len;
        if (lb_count > std.math.maxInt(u32) or pool_bytes > std.math.maxInt(u32)) {
            return Error.InvalidSize;
        }
    }
    return requiredBytes(
        @intCast(pages.len),
        @intCast(bubbles.len),
        @intCast(lb_count),
        @intCast(pool_bytes),
    );
}

/// Serialize a `TRN1` sidecar from rich Zig `BubbleInput` descriptors into `out_buf`.
pub fn writeSidecarFromInputs(
    key: *const SidecarKey,
    pages: []const PageEntry,
    bubbles: []const BubbleInput,
    out_buf: []u8,
) Error!usize {
    try validateKey(key);
    if (pages.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (bubbles.len > std.math.maxInt(u32)) return Error.InvalidSize;

    var total_breaks_u64: u64 = 0;
    var total_pool_u64: u64 = 0;
    for (bubbles) |b| {
        if (b.line_breaks.len > std.math.maxInt(u16)) return Error.InvalidSize;
        if (b.source_text.len > std.math.maxInt(u32)) return Error.InvalidSize;
        if (b.english_text.len > std.math.maxInt(u32)) return Error.InvalidSize;
        total_breaks_u64 += b.line_breaks.len;
        total_pool_u64 += b.source_text.len;
        total_pool_u64 += b.english_text.len;
        if (total_breaks_u64 > std.math.maxInt(u32) or total_pool_u64 > std.math.maxInt(u32)) {
            return Error.InvalidSize;
        }
    }

    const page_count: u16 = @intCast(pages.len);
    const bubble_count: u32 = @intCast(bubbles.len);
    const break_count: u32 = @intCast(total_breaks_u64);
    const pool_bytes: u32 = @intCast(total_pool_u64);
    const sections = try computeSections(page_count, bubble_count, break_count, pool_bytes);
    if (out_buf.len < sections.total_bytes) return Error.InvalidSize;

    try validateInputPageAndBubbleOrder(pages, bubbles);

    writeHeaderBytes(out_buf[0..header_bytes], key, page_count, bubble_count, break_count, pool_bytes, sections.total_bytes);

    var pi: usize = 0;
    while (pi < pages.len) : (pi += 1) {
        writePageEntry(out_buf[sections.page_table_off + pi * page_bytes ..][0..page_bytes], pages[pi]);
    }

    var next_lb: u32 = 0;
    var next_pool_off: u32 = 0;
    var bi: usize = 0;
    while (bi < bubbles.len) : (bi += 1) {
        const b = bubbles[bi];
        const src_off = next_pool_off;
        const src_len: u32 = @intCast(b.source_text.len);
        next_pool_off += src_len;
        const eng_off = next_pool_off;
        const eng_len: u32 = @intCast(b.english_text.len);
        next_pool_off += eng_len;

        if (src_len > 0) @memcpy(out_buf[sections.text_pool_off + src_off ..][0..src_len], b.source_text);
        if (eng_len > 0) @memcpy(out_buf[sections.text_pool_off + eng_off ..][0..eng_len], b.english_text);

        for (b.line_breaks, 0..) |brk, k| {
            const lb_off = sections.line_break_table_off + (@as(usize, next_lb) + k) * 2;
            writeU16Le(out_buf[lb_off..][0..2], brk);
        }

        const rec = BubbleRecord{
            .x = b.x,
            .y = b.y,
            .w = b.w,
            .h = b.h,
            .shape = @backingInt(b.shape),
            .text_orientation = @backingInt(b.text_orientation),
            .corner_radius = b.corner_radius,
            .reading_order = b.reading_order,
            .ocr_confidence_bps = b.ocr_confidence_bps,
            .font_size_px = b.font_size_px,
            .line_count = @intCast(b.line_breaks.len),
            .first_line_break = next_lb,
            .source_text_off = src_off,
            .source_text_len = src_len,
            .english_text_off = eng_off,
            .english_text_len = eng_len,
            .bubble_flags = b.bubble_flags,
            .reserved = .{ 0, 0, 0 },
        };
        writeBubbleRecord(out_buf[sections.bubble_table_off + bi * bubble_bytes ..][0..bubble_bytes], rec);
        next_lb += @intCast(b.line_breaks.len);
    }

    const crc = crc32.crc32WithZeroWindow(out_buf[0..sections.total_bytes], 12, 4);
    writeU32Le(out_buf[12..16], crc);
    return sections.total_bytes;
}

/// Serialize a `TRN1` sidecar from flat caller tables into `out_buf` with no partial writes on failure.
pub fn writeSidecar(
    key: *const SidecarKey,
    pages: []const PageEntry,
    bubbles: []const BubbleRecord,
    line_breaks: []const u16,
    text_pool: []const u8,
    out_buf: []u8,
) Error!usize {
    try validateKey(key);
    if (pages.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (bubbles.len > std.math.maxInt(u32)) return Error.InvalidSize;
    if (line_breaks.len > std.math.maxInt(u32)) return Error.InvalidSize;
    if (text_pool.len > std.math.maxInt(u32)) return Error.InvalidSize;

    const sections = try computeSections(
        @intCast(pages.len),
        @intCast(bubbles.len),
        @intCast(line_breaks.len),
        @intCast(text_pool.len),
    );
    if (out_buf.len < sections.total_bytes) return Error.InvalidSize;

    try validateFlatTables(pages, bubbles, line_breaks, text_pool);

    writeHeaderBytes(
        out_buf[0..header_bytes],
        key,
        @intCast(pages.len),
        @intCast(bubbles.len),
        @intCast(line_breaks.len),
        @intCast(text_pool.len),
        sections.total_bytes,
    );

    for (pages, 0..) |p, pi| {
        writePageEntry(out_buf[sections.page_table_off + pi * page_bytes ..][0..page_bytes], p);
    }
    for (bubbles, 0..) |b, bi| {
        writeBubbleRecord(out_buf[sections.bubble_table_off + bi * bubble_bytes ..][0..bubble_bytes], b);
    }
    for (line_breaks, 0..) |brk, li| {
        writeU16Le(out_buf[sections.line_break_table_off + li * 2 ..][0..2], brk);
    }
    if (text_pool.len > 0) {
        @memcpy(out_buf[sections.text_pool_off..][0..text_pool.len], text_pool);
    }

    const crc = crc32.crc32WithZeroWindow(out_buf[0..sections.total_bytes], 12, 4);
    writeU32Le(out_buf[12..16], crc);
    return sections.total_bytes;
}

fn validateInputPageAndBubbleOrder(pages: []const PageEntry, bubbles: []const BubbleInput) Error!void {
    var expected_bubble: u64 = 0;
    for (pages, 0..) |page, pi| {
        try validatePageHeader(page, pi);
        if (@as(u64, page.first_bubble) != expected_bubble) return Error.InvalidArg;
        if (expected_bubble + page.bubble_count > bubbles.len) return Error.InvalidArg;

        var seen: [16]u64 = @splat(0);
        var local: usize = 0;
        while (local < page.bubble_count) : (local += 1) {
            const b = bubbles[@as(usize, page.first_bubble) + local];
            if (b.line_breaks.len > std.math.maxInt(u16)) return Error.InvalidSize;
            try validateBoxAndFlags(
                b.x,
                b.y,
                b.w,
                b.h,
                page.page_width,
                page.page_height,
                @backingInt(b.shape),
                @backingInt(b.text_orientation),
                b.corner_radius,
                b.ocr_confidence_bps,
                b.font_size_px,
                b.bubble_flags,
                &.{ 0, 0, 0 },
                b.english_text.len,
            );
            try markReadingOrder(&seen, page.bubble_count, b.reading_order);
            try validateTextAndLineBreaks(b.source_text, b.english_text, b.line_breaks);
        }
        expected_bubble += page.bubble_count;
    }
    if (expected_bubble != bubbles.len) return Error.InvalidArg;
}

fn validateFlatTables(
    pages: []const PageEntry,
    bubbles: []const BubbleRecord,
    line_breaks: []const u16,
    text_pool: []const u8,
) Error!void {
    var expected_bubble: u64 = 0;
    var expected_lb: u64 = 0;
    for (pages, 0..) |page, pi| {
        try validatePageHeader(page, pi);
        if (@as(u64, page.first_bubble) != expected_bubble) return Error.InvalidArg;
        if (expected_bubble + page.bubble_count > bubbles.len) return Error.InvalidArg;

        var seen: [16]u64 = @splat(0);
        var local: usize = 0;
        while (local < page.bubble_count) : (local += 1) {
            const b = bubbles[@as(usize, page.first_bubble) + local];
            if (@as(u64, b.first_line_break) != expected_lb) return Error.InvalidArg;
            if (expected_lb + b.line_count > line_breaks.len) return Error.InvalidArg;
            try validateOneFlatBubble(page, b, line_breaks[@intCast(expected_lb)..][0..b.line_count], text_pool);
            try markReadingOrder(&seen, page.bubble_count, b.reading_order);
            expected_lb += b.line_count;
        }
        expected_bubble += page.bubble_count;
    }
    if (expected_bubble != bubbles.len or expected_lb != line_breaks.len) return Error.InvalidArg;
}

fn validateWireTables(
    bytes: []const u8,
    page_count: u16,
    bubble_count: u32,
    line_break_count: u32,
    sections: Sections,
) Error!void {
    const text_pool = bytes[sections.text_pool_off..][0..sections.text_pool_bytes];
    var expected_bubble: u64 = 0;
    var expected_lb: u64 = 0;
    var pi: u16 = 0;
    while (pi < page_count) : (pi += 1) {
        const page = readPageEntry(bytes, sections.page_table_off, pi);
        try validatePageHeader(page, pi);
        if (@as(u64, page.first_bubble) != expected_bubble) return Error.InvalidArg;
        if (expected_bubble + page.bubble_count > bubble_count) return Error.InvalidArg;

        var seen: [16]u64 = @splat(0);
        var local: u16 = 0;
        while (local < page.bubble_count) : (local += 1) {
            const b = readBubbleRecord(bytes, sections.bubble_table_off, page.first_bubble + local);
            if (@as(u64, b.first_line_break) != expected_lb) return Error.InvalidArg;
            if (expected_lb + b.line_count > line_break_count) return Error.InvalidArg;

            try validateBoxAndFlags(
                b.x,
                b.y,
                b.w,
                b.h,
                page.page_width,
                page.page_height,
                b.shape,
                b.text_orientation,
                b.corner_radius,
                b.ocr_confidence_bps,
                b.font_size_px,
                b.bubble_flags,
                &b.reserved,
                b.english_text_len,
            );
            try markReadingOrder(&seen, page.bubble_count, b.reading_order);

            const src = try sliceTextPool(text_pool, b.source_text_off, b.source_text_len);
            const eng = try sliceTextPool(text_pool, b.english_text_off, b.english_text_len);
            if (!std.unicode.utf8ValidateSlice(src) or !std.unicode.utf8ValidateSlice(eng)) {
                return Error.InvalidArg;
            }
            try validateWireLineBreaks(
                bytes[sections.line_break_table_off..][@as(usize, b.first_line_break) * 2 ..][0 .. @as(usize, b.line_count) * 2],
                b.line_count,
                eng,
            );
            expected_lb += b.line_count;
        }
        expected_bubble += page.bubble_count;
    }
    if (expected_bubble != bubble_count or expected_lb != line_break_count) return Error.InvalidArg;
}

fn validateOneFlatBubble(
    page: PageEntry,
    b: BubbleRecord,
    breaks: []const u16,
    text_pool: []const u8,
) Error!void {
    try validateBoxAndFlags(
        b.x,
        b.y,
        b.w,
        b.h,
        page.page_width,
        page.page_height,
        b.shape,
        b.text_orientation,
        b.corner_radius,
        b.ocr_confidence_bps,
        b.font_size_px,
        b.bubble_flags,
        &b.reserved,
        b.english_text_len,
    );
    const src = try sliceTextPool(text_pool, b.source_text_off, b.source_text_len);
    const eng = try sliceTextPool(text_pool, b.english_text_off, b.english_text_len);
    try validateTextAndLineBreaks(src, eng, breaks);
}

fn validatePageHeader(page: PageEntry, expected_index: usize) Error!void {
    if (@as(usize, page.page_index) != expected_index) return Error.InvalidArg;
    if (page.bubble_count > max_page_bubbles) return Error.InvalidArg;
    if (page.page_width == 0 or page.page_height == 0) return Error.InvalidArg;
}

fn validateBoxAndFlags(
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    page_w: u16,
    page_h: u16,
    shape_raw: u8,
    orient_raw: u8,
    corner_radius: u16,
    confidence_bps: u16,
    font_size_px: u16,
    bubble_flags: u8,
    reserved: *const [3]u8,
    english_text_len: usize,
) Error!void {
    if (w == 0 or h == 0) return Error.InvalidArg;
    if (@as(u32, x) + w > page_w or @as(u32, y) + h > page_h) return Error.InvalidArg;
    const shape = Shape.fromRaw(shape_raw) orelse return Error.InvalidArg;
    if (shape != .rounded_rect and corner_radius != 0) return Error.InvalidArg;
    if (TextOrientation.fromRaw(orient_raw) == null) return Error.InvalidArg;
    if (confidence_bps > max_confidence_bps) return Error.InvalidArg;
    if (english_text_len > 0 and font_size_px == 0) return Error.InvalidArg;
    if ((bubble_flags & ~bflag_glossary) != 0) return Error.InvalidArg;
    if (!allZero(reserved)) return Error.InvalidArg;
}

fn markReadingOrder(seen: *[16]u64, page_bubbles: u16, reading_order: u16) Error!void {
    if (reading_order >= page_bubbles) return Error.InvalidArg;
    const word: usize = reading_order >> 6;
    const mask: u64 = @as(u64, 1) << @as(u6, @truncate(reading_order));
    if ((seen[word] & mask) != 0) return Error.InvalidArg;
    seen[word] |= mask;
}

fn sliceTextPool(text_pool: []const u8, off: u32, length: u32) Error![]const u8 {
    const end_u64 = @as(u64, off) + @as(u64, length);
    if (end_u64 > text_pool.len) return Error.InvalidSize;
    return text_pool[@intCast(off)..@intCast(end_u64)];
}

fn validateTextAndLineBreaks(src: []const u8, eng: []const u8, breaks: []const u16) Error!void {
    if (!std.unicode.utf8ValidateSlice(src) or !std.unicode.utf8ValidateSlice(eng)) {
        return Error.InvalidArg;
    }
    if (eng.len == 0) {
        if (breaks.len != 0) return Error.InvalidArg;
        return;
    }
    if (eng.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (breaks.len == 0) return Error.InvalidArg;
    var prev: u16 = 0;
    for (breaks) |cur| {
        if (cur <= prev or cur > eng.len) return Error.InvalidArg;
        if (cur < eng.len and (eng[cur] & 0xC0) == 0x80) return Error.InvalidArg;
        prev = cur;
    }
    if (prev != eng.len) return Error.InvalidArg;
}

fn validateWireLineBreaks(raw_breaks: []const u8, count: u16, eng: []const u8) Error!void {
    if (eng.len == 0) {
        if (count != 0) return Error.InvalidArg;
        return;
    }
    if (eng.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (count == 0) return Error.InvalidArg;
    var prev: u16 = 0;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const cur = readU16Le(raw_breaks[i * 2 ..][0..2]);
        if (cur <= prev or cur > eng.len) return Error.InvalidArg;
        if (cur < eng.len and (eng[cur] & 0xC0) == 0x80) return Error.InvalidArg;
        prev = cur;
    }
    if (prev != eng.len) return Error.InvalidArg;
}

fn writeHeaderBytes(
    hdr: *[header_bytes]u8,
    key: *const SidecarKey,
    page_count: u16,
    bubble_count: u32,
    line_break_count: u32,
    text_pool_bytes: u32,
    total_bytes: usize,
) void {
    @memset(hdr, 0);
    writeU32Le(hdr[0..4], magic);
    writeU16Le(hdr[4..6], version);
    writeU16Le(hdr[6..8], 0);
    writeU32Le(hdr[8..12], @intCast(total_bytes));
    @memcpy(hdr[0x10..0x30], &key.chapter_hash);
    writeU32Le(hdr[0x30..0x34], key.detector_model_version);
    writeU32Le(hdr[0x34..0x38], key.ocr_model_version);
    writeU32Le(hdr[0x38..0x3C], key.nmt_model_version);
    writeU32Le(hdr[0x3C..0x40], key.glossary_revision);
    hdr[0x40] = key.source_lang;
    writeU16Le(hdr[0x44..0x46], page_count);
    writeU16Le(hdr[0x46..0x48], 0);
    writeU32Le(hdr[0x48..0x4C], bubble_count);
    writeU32Le(hdr[0x4C..0x50], line_break_count);
    writeU32Le(hdr[0x50..0x54], text_pool_bytes);
}

fn readPageEntry(bytes: []const u8, table_off: usize, index: u16) PageEntry {
    const p = bytes[table_off + @as(usize, index) * page_bytes ..][0..page_bytes];
    return .{
        .page_index = readU16Le(p[0..2]),
        .bubble_count = readU16Le(p[2..4]),
        .first_bubble = readU32Le(p[4..8]),
        .page_width = readU16Le(p[8..10]),
        .page_height = readU16Le(p[10..12]),
    };
}

fn writePageEntry(dst: *[page_bytes]u8, p: PageEntry) void {
    writeU16Le(dst[0..2], p.page_index);
    writeU16Le(dst[2..4], p.bubble_count);
    writeU32Le(dst[4..8], p.first_bubble);
    writeU16Le(dst[8..10], p.page_width);
    writeU16Le(dst[10..12], p.page_height);
}

fn readBubbleRecord(bytes: []const u8, table_off: usize, index: u32) BubbleRecord {
    const r = bytes[table_off + @as(usize, index) * bubble_bytes ..][0..bubble_bytes];
    return .{
        .x = readU16Le(r[0..2]),
        .y = readU16Le(r[2..4]),
        .w = readU16Le(r[4..6]),
        .h = readU16Le(r[6..8]),
        .shape = r[8],
        .text_orientation = r[9],
        .corner_radius = readU16Le(r[10..12]),
        .reading_order = readU16Le(r[12..14]),
        .ocr_confidence_bps = readU16Le(r[14..16]),
        .font_size_px = readU16Le(r[16..18]),
        .line_count = readU16Le(r[18..20]),
        .first_line_break = readU32Le(r[20..24]),
        .source_text_off = readU32Le(r[24..28]),
        .source_text_len = readU32Le(r[28..32]),
        .english_text_off = readU32Le(r[32..36]),
        .english_text_len = readU32Le(r[36..40]),
        .bubble_flags = r[40],
        .reserved = .{ r[41], r[42], r[43] },
    };
}

fn writeBubbleRecord(dst: *[bubble_bytes]u8, b: BubbleRecord) void {
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
    dst[41] = b.reserved[0];
    dst[42] = b.reserved[1];
    dst[43] = b.reserved[2];
}

fn allZero(slice: []const u8) bool {
    for (slice) |byte| {
        if (byte != 0) return false;
    }
    return true;
}

fn readU16Le(bytes: *const [2]u8) u16 {
    return std.mem.readInt(u16, bytes, .little);
}

fn readU32Le(bytes: *const [4]u8) u32 {
    return std.mem.readInt(u32, bytes, .little);
}

fn writeU16Le(dst: *[2]u8, value: u16) void {
    std.mem.writeInt(u16, dst, value, .little);
}

fn writeU32Le(dst: *[4]u8, value: u32) void {
    std.mem.writeInt(u32, dst, value, .little);
}
