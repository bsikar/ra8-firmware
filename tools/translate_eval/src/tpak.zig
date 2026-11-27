//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Deterministic binary pack builder (`TPAK` v1) for target `ra8_emulator` runs
//! and extractor for `EMU_TRACE_BEGIN..EMU_TRACE_END` UART console captures.

const std = @import("std");
const c = @import("ra8_translate_c.zig");
const corpus = @import("corpus.zig");
const fixtures = @import("fixtures.zig");

pub const magic: u32 = 0x4B41_5054; // LE "TPAK".
pub const version: u32 = 1;
pub const header_bytes: usize = 20;
pub const page_entry_bytes: usize = 64;
pub const bubble_entry_bytes: usize = 44;
pub const page_id_cap: usize = 32;

const PageFixtures = struct {
    det: fixtures.DetMaps,
    ocr: []fixtures.OcrCase,
    nmt: []fixtures.NmtCase,
};

fn align4(n: usize) usize {
    return (n + 3) & ~@as(usize, 3);
}

fn writeU16(buf: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, buf[off..][0..2], v, .little);
}

fn writeU32(buf: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, buf[off..][0..4], v, .little);
}

fn langCode(lang: []const u8) u8 {
    if (std.mem.eql(u8, lang, "ko")) return 1;
    if (std.mem.eql(u8, lang, "ja")) return 2;
    return 3;
}

fn ocrModelCode(lang: []const u8) u8 {
    if (std.mem.eql(u8, lang, "ko")) return c.ocr_korean;
    if (std.mem.eql(u8, lang, "ja")) return c.ocr_japanese;
    return c.ocr_chinese;
}

fn freeBuiltPages(allocator: std.mem.Allocator, built: []PageFixtures) void {
    for (built) |*p| {
        p.det.deinit(allocator);
        for (p.ocr) |*o| o.deinit(allocator);
        for (p.nmt) |*n| n.deinit(allocator);
        allocator.free(p.ocr);
        allocator.free(p.nmt);
    }
    allocator.free(built);
}

/// Build an aligned 4-byte `TPAK` v1 binary bundle holding quantized detector
/// maps, `TCHR` charsets, `int8_t` CTC logits, `TVOC`, and `TMNT` containers for
/// every page and bubble in `eval_corpus`.
pub fn buildTpak(allocator: std.mem.Allocator, eval_corpus: corpus.Corpus) ![]u8 {
    if (eval_corpus.pages.len == 0) return error.EmptyCorpus;

    const built = try allocator.alloc(PageFixtures, eval_corpus.pages.len);
    var built_init: usize = 0;
    errdefer freeBuiltPages(allocator, built[0..built_init]);

    var total_bubbles: usize = 0;
    for (eval_corpus.pages, 0..) |page, pi| {
        if (page.page_id.len == 0 or page.page_id.len >= page_id_cap) return error.InvalidPageId;

        var det = try fixtures.buildDetMaps(allocator, page);
        errdefer det.deinit(allocator);

        const ocr_list = try allocator.alloc(fixtures.OcrCase, page.bubbles.len);
        var ocr_init: usize = 0;
        errdefer {
            for (ocr_list[0..ocr_init]) |*o| o.deinit(allocator);
            allocator.free(ocr_list);
        }

        const nmt_list = try allocator.alloc(fixtures.NmtCase, page.bubbles.len);
        var nmt_init: usize = 0;
        errdefer {
            for (nmt_list[0..nmt_init]) |*n| n.deinit(allocator);
            allocator.free(nmt_list);
        }

        for (page.bubbles, 0..) |b, bi| {
            ocr_list[bi] = try fixtures.buildOcrCase(allocator, page.lang, b);
            ocr_init += 1;
            nmt_list[bi] = try fixtures.buildNmtCase(allocator, page.lang, b);
            nmt_init += 1;
        }

        built[pi] = .{ .det = det, .ocr = ocr_list, .nmt = nmt_list };
        built_init += 1;
        total_bubbles += page.bubbles.len;
    }
    defer freeBuiltPages(allocator, built);

    const pages_off = header_bytes;
    const bubbles_off = pages_off + eval_corpus.pages.len * page_entry_bytes;
    var data_cursor = align4(bubbles_off + total_bubbles * bubble_entry_bytes);

    for (built) |p| {
        data_cursor = align4(data_cursor + p.det.i8_map.len);
        for (p.ocr, p.nmt) |ocr, nmt| {
            data_cursor = align4(data_cursor + ocr.tchr_buf.len);
            data_cursor = align4(data_cursor + ocr.logits_i8.len);
            data_cursor = align4(data_cursor + nmt.tvoc_buf.len);
            data_cursor = align4(data_cursor + nmt.tmnt_buf.len);
        }
    }

    const out = try allocator.alloc(u8, data_cursor);
    errdefer allocator.free(out);
    @memset(out, 0);

    writeU32(out, 0, magic);
    writeU32(out, 4, version);
    writeU32(out, 8, @intCast(eval_corpus.pages.len));
    writeU32(out, 12, @intCast(out.len));

    var cursor = align4(bubbles_off + total_bubbles * bubble_entry_bytes);
    var bubble_idx: usize = 0;

    for (eval_corpus.pages, built, 0..) |page, p, pi| {
        const poff = pages_off + pi * page_entry_bytes;
        const det_off = cursor;
        const det_src: []const u8 = @ptrCast(p.det.i8_map);
        @memcpy(out[det_off..][0..det_src.len], det_src);
        cursor = align4(cursor + det_src.len);

        writeU32(out, poff + 0, @intCast(page.page_id.len));
        @memcpy(out[poff + 4 ..][0..page.page_id.len], page.page_id);
        writeU16(out, poff + 36, page.page_width);
        writeU16(out, poff + 38, page.page_height);
        writeU16(out, poff + 40, page.det_map_width);
        writeU16(out, poff + 42, page.det_map_height);
        out[poff + 44] = langCode(page.lang);
        out[poff + 45] = if (std.mem.eql(u8, page.layout, "manga_rtl"))
            c.det_order_manga_rtl
        else
            c.det_order_webtoon_ttb;
        out[poff + 46] = @bitCast(p.det.text_zp);
        writeU16(out, poff + 48, @intCast(page.bubbles.len));
        writeU32(out, poff + 52, @intCast(det_off));
        writeU32(out, poff + 56, @bitCast(p.det.text_scale));
        writeU32(out, poff + 60, @intCast(bubble_idx));

        for (p.ocr, p.nmt) |ocr, nmt| {
            const boff = bubbles_off + bubble_idx * bubble_entry_bytes;

            const tchr_off = cursor;
            @memcpy(out[tchr_off..][0..ocr.tchr_buf.len], ocr.tchr_buf);
            cursor = align4(cursor + ocr.tchr_buf.len);

            const logits_off = cursor;
            const lsrc: []const u8 = @ptrCast(ocr.logits_i8);
            @memcpy(out[logits_off..][0..lsrc.len], lsrc);
            cursor = align4(cursor + lsrc.len);

            const tvoc_off = cursor;
            @memcpy(out[tvoc_off..][0..nmt.tvoc_buf.len], nmt.tvoc_buf);
            cursor = align4(cursor + nmt.tvoc_buf.len);

            const tmnt_off = cursor;
            @memcpy(out[tmnt_off..][0..nmt.tmnt_buf.len], nmt.tmnt_buf);
            cursor = align4(cursor + nmt.tmnt_buf.len);

            writeU32(out, boff + 0, @intCast(tchr_off));
            writeU32(out, boff + 4, @intCast(ocr.tchr_buf.len));
            writeU32(out, boff + 8, @intCast(logits_off));
            writeU32(out, boff + 12, @intCast(lsrc.len));
            writeU32(out, boff + 16, ocr.time_steps);
            writeU32(out, boff + 20, @bitCast(ocr.quant_scale));
            writeU32(out, boff + 24, @intCast(tvoc_off));
            writeU32(out, boff + 28, @intCast(nmt.tvoc_buf.len));
            writeU32(out, boff + 32, @intCast(tmnt_off));
            writeU32(out, boff + 36, @intCast(nmt.tmnt_buf.len));
            out[boff + 40] = @bitCast(ocr.zero_point);
            out[boff + 41] = ocrModelCode(page.lang);

            bubble_idx += 1;
        }
    }

    writeU32(out, 16, fixtures.crc32Ieee(out));
    return out;
}

/// Verify target PASS status and reassemble the JSON between `EMU_TRACE_BEGIN`
/// and `EMU_TRACE_END` from a raw or `console> `-prefixed UART capture.
pub fn extractTraceFromLog(allocator: std.mem.Allocator, log_bytes: []const u8) ![]u8 {
    if (std.mem.indexOf(u8, log_bytes, "translate_eval_e2e: FAIL") != null or
        std.mem.indexOf(u8, log_bytes, "BKPT halted") != null or
        std.mem.indexOf(u8, log_bytes, "bus fault") != null)
    {
        return error.EmulatorReportedFailure;
    }
    if (std.mem.indexOf(u8, log_bytes, "translate_eval_e2e: PASS") == null) {
        return error.EmulatorMissingPassBanner;
    }

    const begin_marker = "EMU_TRACE_BEGIN";
    const end_marker = "EMU_TRACE_END";
    const bpos = std.mem.indexOf(u8, log_bytes, begin_marker) orelse return error.MissingTraceBegin;
    var start = bpos + begin_marker.len;
    while (start < log_bytes.len and (log_bytes[start] == '\r' or log_bytes[start] == '\n')) : (start += 1) {}
    const epos = std.mem.indexOfPos(u8, log_bytes, start, end_marker) orelse return error.MissingTraceEnd;

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var lines = std.mem.splitScalar(u8, log_bytes[start..epos], '\n');
    while (lines.next()) |raw_line| {
        var line = std.mem.trim(u8, raw_line, " \t\r");
        if (std.mem.startsWith(u8, line, "console>")) {
            line = std.mem.trim(u8, line["console>".len..], " \t\r");
        }
        if (line.len == 0) continue;
        try out.appendSlice(allocator, line);
    }

    const assembled = try out.toOwnedSlice(allocator);
    errdefer allocator.free(assembled);
    if (assembled.len == 0 or assembled[0] != '{') return error.MalformedExtractedTrace;
    return assembled;
}
