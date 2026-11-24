//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for `TRN1` per-chapter translation sidecar writer and reader.

const std = @import("std");
const sidecar = @import("sidecar");

fn testKey(lang: sidecar.Language) sidecar.SidecarKey {
    var key = sidecar.SidecarKey{
        .chapter_hash = undefined,
        .detector_model_version = 1,
        .ocr_model_version = 2,
        .nmt_model_version = 3,
        .glossary_revision = 4,
        .source_lang = @backingInt(lang),
        .reserved = .{ 0, 0, 0 },
    };
    for (&key.chapter_hash, 0..) |*slot, i| {
        slot.* = @intCast(i + 1);
    }
    return key;
}

test "worked example round-trips through writeSidecarFromInputs and SidecarView" {
    const key = testKey(.korean);
    const pages = [_]sidecar.PageEntry{
        .{ .page_index = 0, .bubble_count = 1, .first_bubble = 0, .page_width = 800, .page_height = 1200 },
    };
    const breaks = [_]u16{ 3, 8 };
    const bubbles = [_]sidecar.BubbleInput{
        .{
            .x = 20,
            .y = 40,
            .w = 200,
            .h = 80,
            .shape = .rounded_rect,
            .text_orientation = .horizontal,
            .corner_radius = 12,
            .reading_order = 0,
            .ocr_confidence_bps = 9450,
            .font_size_px = 20,
            .bubble_flags = sidecar.bflag_glossary,
            .line_breaks = &breaks,
            .source_text = "hello_ko",
            .english_text = "Hi there",
        },
    };

    const need = try sidecar.requiredBytesForInputs(&pages, &bubbles);
    try std.testing.expectEqual(@as(usize, 172), need);

    var buf: [256]u8 = undefined;
    const written = try sidecar.writeSidecarFromInputs(&key, &pages, &bubbles, &buf);
    try std.testing.expectEqual(@as(usize, 172), written);
    try std.testing.expectEqualSlices(u8, "TRN1", buf[0..4]);

    const view = try sidecar.SidecarView.open(buf[0..written]);
    try std.testing.expectEqual(@as(u32, 172), view.header.total_bytes);
    try std.testing.expectEqual(@as(u16, 1), view.header.page_count);
    try std.testing.expectEqual(@as(u32, 1), view.header.bubble_count);
    try std.testing.expectEqual(@as(u32, 2), view.header.line_break_count);
    try std.testing.expectEqual(@as(u32, 16), view.header.text_pool_bytes);
    try std.testing.expect(try view.matchesKey(&key));

    const p0 = try view.pageAt(0);
    try std.testing.expectEqual(@as(u16, 800), p0.page_width);
    try std.testing.expectEqual(@as(u16, 1200), p0.page_height);

    const b0 = try view.bubbleAt(0);
    try std.testing.expectEqual(@as(u16, 9450), b0.ocr_confidence_bps);
    try std.testing.expectEqual(@as(u16, 20), b0.font_size_px);
    try std.testing.expectEqual(@as(u8, sidecar.bflag_glossary), b0.bubble_flags);
    try std.testing.expectEqualStrings("hello_ko", try view.sourceText(0));
    try std.testing.expectEqualStrings("Hi there", try view.englishText(0));

    var out_breaks: [4]u16 = undefined;
    const got_breaks = try view.lineBreaks(0, &out_breaks);
    try std.testing.expectEqual(@as(u16, 2), got_breaks);
    try std.testing.expectEqualSlices(u16, &breaks, out_breaks[0..got_breaks]);
}

test "multi-page non-linear reading order, empty pages, and shared pool round-trip" {
    const key = testKey(.japanese);
    const pages = [_]sidecar.PageEntry{
        .{ .page_index = 0, .bubble_count = 2, .first_bubble = 0, .page_width = 1000, .page_height = 1400 },
        .{ .page_index = 1, .bubble_count = 0, .first_bubble = 2, .page_width = 1000, .page_height = 1400 },
        .{ .page_index = 2, .bubble_count = 1, .first_bubble = 2, .page_width = 900, .page_height = 1300 },
    };
    const pool = "SFX!";
    const breaks = [_]u16{ 2, 4 };
    const bubbles = [_]sidecar.BubbleRecord{
        .{
            .x = 500,
            .y = 20,
            .w = 120,
            .h = 300,
            .shape = @backingInt(sidecar.Shape.cloud),
            .text_orientation = @backingInt(sidecar.TextOrientation.vertical_rl),
            .corner_radius = 0,
            .reading_order = 1,
            .ocr_confidence_bps = 10000,
            .font_size_px = 18,
            .line_count = 2,
            .first_line_break = 0,
            .source_text_off = 0,
            .source_text_len = 4,
            .english_text_off = 0,
            .english_text_len = 4,
        },
        .{
            .x = 40,
            .y = 40,
            .w = 100,
            .h = 100,
            .shape = @backingInt(sidecar.Shape.ellipse),
            .text_orientation = @backingInt(sidecar.TextOrientation.vertical_lr),
            .corner_radius = 0,
            .reading_order = 0,
            .ocr_confidence_bps = 0,
            .font_size_px = 0,
            .line_count = 0,
            .first_line_break = 2,
            .source_text_off = 0,
            .source_text_len = 0,
            .english_text_off = 0,
            .english_text_len = 0,
        },
        .{
            .x = 10,
            .y = 10,
            .w = 200,
            .h = 80,
            .shape = @backingInt(sidecar.Shape.rect),
            .text_orientation = @backingInt(sidecar.TextOrientation.horizontal),
            .corner_radius = 0,
            .reading_order = 0,
            .ocr_confidence_bps = 8125,
            .font_size_px = 16,
            .line_count = 1,
            .first_line_break = 2,
            .source_text_off = 0,
            .source_text_len = 4,
            .english_text_off = 0,
            .english_text_len = 4,
        },
    };
    const all_breaks = [_]u16{ breaks[0], breaks[1], 4 };

    var buf: [512]u8 = undefined;
    const written = try sidecar.writeSidecar(&key, &pages, &bubbles, &all_breaks, pool, &buf);
    const view = try sidecar.SidecarView.open(buf[0..written]);

    try std.testing.expectEqual(@as(u16, 3), view.header.page_count);
    try std.testing.expectEqual(@as(u32, 3), view.header.bubble_count);
    try std.testing.expectEqualStrings("", try view.sourceText(1));
    try std.testing.expectEqualStrings("", try view.englishText(1));
    try std.testing.expectEqualStrings("SFX!", try view.englishText(2));
    try std.testing.expectError(error.OutOfRange, view.pageAt(3));
    try std.testing.expectError(error.OutOfRange, view.bubbleAt(3));
}

test "cache invalidation key detects hash, model versions, glossary, and language drift" {
    const key = testKey(.chinese);
    var buf: [128]u8 = undefined;
    const written = try sidecar.writeSidecar(&key, &.{}, &.{}, &.{}, "", &buf);
    const view = try sidecar.SidecarView.open(buf[0..written]);

    var k_hash = key;
    k_hash.chapter_hash[0] ^= 0xFF;
    try std.testing.expect(!try view.matchesKey(&k_hash));

    var k_det = key;
    k_det.detector_model_version += 1;
    try std.testing.expect(!try view.matchesKey(&k_det));

    var k_ocr = key;
    k_ocr.ocr_model_version += 1;
    try std.testing.expect(!try view.matchesKey(&k_ocr));

    var k_nmt = key;
    k_nmt.nmt_model_version += 1;
    try std.testing.expect(!try view.matchesKey(&k_nmt));

    var k_gloss = key;
    k_gloss.glossary_revision += 1;
    try std.testing.expect(!try view.matchesKey(&k_gloss));

    var k_lang = key;
    k_lang.source_lang = @backingInt(sidecar.Language.korean);
    try std.testing.expect(!try view.matchesKey(&k_lang));
}

test "reader rejects corrupt headers, CRC mismatch, UTF-8 boundary split, and bad tables" {
    const key = testKey(.korean);
    const pages = [_]sidecar.PageEntry{
        .{ .page_index = 0, .bubble_count = 1, .first_bubble = 0, .page_width = 400, .page_height = 600 },
    };
    const breaks = [_]u16{ 3, 4 };
    const bubbles = [_]sidecar.BubbleInput{
        .{
            .x = 0,
            .y = 0,
            .w = 100,
            .h = 50,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 0,
            .ocr_confidence_bps = 9000,
            .font_size_px = 14,
            .line_breaks = &breaks,
            .source_text = "src",
            .english_text = "a\xc3\xa9b",
        },
    };

    var buf: [256]u8 = undefined;
    const written = try sidecar.writeSidecarFromInputs(&key, &pages, &bubbles, &buf);

    try std.testing.expectError(error.InvalidSize, sidecar.SidecarView.open(buf[0 .. written - 1]));

    var bad_magic = buf;
    bad_magic[0] = 'X';
    try std.testing.expectError(error.InvalidArg, sidecar.SidecarView.open(bad_magic[0..written]));

    var bad_ver = buf;
    bad_ver[4] = 2;
    try std.testing.expectError(error.NotSupported, sidecar.SidecarView.open(bad_ver[0..written]));

    var bad_crc = buf;
    bad_crc[written - 1] ^= 0x01;
    try std.testing.expectError(error.CrcMismatch, sidecar.SidecarView.open(bad_crc[0..written]));

    // Reject splitting a multi-byte UTF-8 sequence in line breaks.
    const split_breaks = [_]u16{ 2, 4 };
    const split_bubbles = [_]sidecar.BubbleInput{
        .{
            .x = 0,
            .y = 0,
            .w = 100,
            .h = 50,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 0,
            .ocr_confidence_bps = 9000,
            .font_size_px = 14,
            .line_breaks = &split_breaks,
            .source_text = "src",
            .english_text = "a\xc3\xa9b",
        },
    };
    try std.testing.expectError(
        error.InvalidArg,
        sidecar.writeSidecarFromInputs(&key, &pages, &split_bubbles, &buf),
    );
}

test "writer preserves output buffer on capacity failure and rejects duplicate reading order" {
    const key = testKey(.korean);
    var tiny = [_]u8{ 0xAA, 0xBB, 0xCC, 0xDD };
    try std.testing.expectError(
        error.InvalidSize,
        sidecar.writeSidecar(&key, &.{}, &.{}, &.{}, "", &tiny),
    );
    try std.testing.expectEqualSlices(u8, &.{ 0xAA, 0xBB, 0xCC, 0xDD }, &tiny);

    const pages = [_]sidecar.PageEntry{
        .{ .page_index = 0, .bubble_count = 2, .first_bubble = 0, .page_width = 400, .page_height = 600 },
    };
    const breaks = [_]u16{2};
    const dup_bubbles = [_]sidecar.BubbleInput{
        .{
            .x = 0,
            .y = 0,
            .w = 50,
            .h = 50,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 0,
            .ocr_confidence_bps = 8000,
            .font_size_px = 12,
            .line_breaks = &breaks,
            .source_text = "s0",
            .english_text = "hi",
        },
        .{
            .x = 60,
            .y = 0,
            .w = 50,
            .h = 50,
            .shape = .rect,
            .text_orientation = .horizontal,
            .reading_order = 0,
            .ocr_confidence_bps = 8000,
            .font_size_px = 12,
            .line_breaks = &breaks,
            .source_text = "s1",
            .english_text = "yo",
        },
    };
    var buf: [512]u8 = undefined;
    @memset(&buf, 0x5A);
    try std.testing.expectError(
        error.InvalidArg,
        sidecar.writeSidecarFromInputs(&key, &pages, &dup_bubbles, &buf),
    );
    try std.testing.expectEqual(@as(u8, 0x5A), buf[0]);
}

test "requiredBytes catches u32 size overflow safely" {
    try std.testing.expectError(
        error.InvalidSize,
        sidecar.requiredBytes(1, 0x4000_0000, 0, 0),
    );
    try std.testing.expectEqual(
        @as(usize, 96),
        try sidecar.requiredBytes(0, 0, 0, 0),
    );
}
