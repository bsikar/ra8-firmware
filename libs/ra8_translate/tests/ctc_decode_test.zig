//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit and C ABI tests for CTC per-script `TCHR` tables and greedy decoding.

const std = @import("std");
const abi = @import("ctc_decode_abi");
const ctc = @import("ctc_decode");
const err_mod = @import("err");

fn buildCharset(
    opts: ctc.CharsetWriteOpts,
    glyphs: []const []const u8,
    buf: []u8,
) !ctc.CharsetView {
    const n = try ctc.writeCharset(opts, glyphs, buf);
    return try ctc.CharsetView.open(buf[0..n]);
}

test "per-script Korean, Japanese, and Chinese TCHR tables round-trip and reject corruption" {
    const ko_glyphs = [_][]const u8{
        "",
        "\xec\x95\x88", // U+C548
        "\xeb\x85\x95", // U+B155
        "!",
    };
    const ja_glyphs = [_][]const u8{
        "",
        "\xe3\x81\x93", // U+3053
        "\xe7\xa7\x81", // U+79C1
        "\xe3\x80\x82", // U+3002
        "<unk>",
    };
    const zh_glyphs = [_][]const u8{
        "\xe4\xbd\xa0", // U+4F60
        "\xe5\xa5\xbd", // U+597D
        "", // blank at last index (C - 1)
    };

    var wire_ko: [256]u8 = undefined;
    const ko_len = try ctc.writeCharset(.{ .ocr_model = .korean, .blank_index = 0 }, &ko_glyphs, &wire_ko);
    const ko_view = try ctc.CharsetView.open(wire_ko[0..ko_len]);
    try std.testing.expectEqual(@as(u8, 1), ko_view.header.ocr_model);
    try std.testing.expectEqual(@as(u32, 4), ko_view.header.class_count);
    try std.testing.expectEqualStrings("\xeb\x85\x95", try ko_view.glyphAt(2));
    try std.testing.expectError(error.OutOfRange, ko_view.glyphAt(4));

    var wire_ja: [256]u8 = undefined;
    const ja_view = try buildCharset(.{
        .ocr_model = .japanese,
        .blank_index = 0,
        .unk_index = 4,
    }, &ja_glyphs, &wire_ja);
    try std.testing.expectEqual(@as(u8, 2), ja_view.header.ocr_model);
    try std.testing.expectEqualStrings("<unk>", try ja_view.glyphAt(4));

    var wire_zh: [256]u8 = undefined;
    const zh_view = try buildCharset(.{
        .ocr_model = .chinese,
        .blank_index = 2,
    }, &zh_glyphs, &wire_zh);
    try std.testing.expectEqual(@as(u8, 3), zh_view.header.ocr_model);
    try std.testing.expectEqual(@as(u32, 2), zh_view.header.blank_index);
    try std.testing.expectEqualStrings("\xe5\xa5\xbd", try zh_view.glyphAt(1));

    // Bit-flip breaks CRC-32; truncation fails size; invalid UTF-8 rejected.
    var copy_buf: [256]u8 = undefined;
    @memcpy(copy_buf[0..ko_len], wire_ko[0..ko_len]);
    copy_buf[ko_len - 1] ^= 0x01;
    try std.testing.expectError(error.CrcMismatch, ctc.CharsetView.open(copy_buf[0..ko_len]));
    try std.testing.expectError(error.InvalidSize, ctc.CharsetView.open(wire_ko[0 .. ko_len - 1]));

    const bad_utf8 = [_][]const u8{ "", "\xff\xfe" };
    try std.testing.expectError(
        error.InvalidArg,
        ctc.writeCharset(.{ .ocr_model = .korean, .blank_index = 0 }, &bad_utf8, &copy_buf),
    );
}

test "CTC greedy decode collapses repeats across blanks and distinguishes double characters" {
    // Classes: 0=blank, 1="a", 2="b", 3="c", 4=unk("?")
    const glyphs = [_][]const u8{ "", "a", "b", "c", "?" };
    var wire: [256]u8 = undefined;
    const view = try buildCharset(.{
        .ocr_model = .korean,
        .blank_index = 0,
        .unk_index = 4,
    }, &glyphs, &wire);

    // Path: [b, a, a, blank, a, c, c] -> "baac"
    // T=7, C=5
    const logits = [_]f32{
        -4, -4, 6, -4, -4, // b
        -4, 4, -4, -4, -4, // a (run1 frame 1, lower conf)
        -4, 8, -4, -4, -4, // a (run1 frame 2, high peak)
        6, -4, -4, -4, -4, // blank separates "a" and next "a"
        -4, 6, -4, -4, -4, // a
        -4, -4, -4, 5, -4, // c
        -4, -4, -4, 7, -4, // c
    };

    var out: [32]u8 = undefined;
    const res = try ctc.decodeF32(view, &logits, .{
        .time_steps = 7,
        .score_mode = @backingInt(ctc.ScoreMode.logits),
        .low_confidence_thresh_bps = 9000,
    }, &out);
    try std.testing.expectEqualStrings("baac", out[0..res.text_bytes]);
    try std.testing.expectEqual(@as(u32, 4), res.emitted_tokens);
    try std.testing.expectEqual(@as(u32, 1), res.blank_frames);
    try std.testing.expectEqual(@as(u32, 0), res.unk_count);
    try std.testing.expect(res.confidence_bps >= 9900);
    try std.testing.expectEqual(@as(u8, 0), res.low_confidence);
}

test "CTC confidence flags low-margin lines and supports int8 stride and skip_unk" {
    // Blank at index 3 (C - 1), unk at index 2.
    // Japanese: 0="\xe3\x81\xaf" (ha), 1="\xe3\x81\x84" (i), 2="?", 3=blank
    const glyphs = [_][]const u8{ "\xe3\x81\xaf", "\xe3\x81\x84", "?", "" };
    var wire: [256]u8 = undefined;
    const view = try buildCharset(.{
        .ocr_model = .japanese,
        .blank_index = 3,
        .unk_index = 2,
    }, &glyphs, &wire);

    // 4 frames, C=4, stride=6 (2 padding lanes at end of each row).
    // Quant scale = 0.25, zero_point = -20.
    // Frame 0: unk dominates strongly (+20 vs -20)
    // Frame 1: low margin ambiguous frame for glyph 0: [1, 0, 0, -1] -> small diff 0.25 -> ~31% prob
    // Frame 2: blank dominates
    // Frame 3: glyph 1 ambiguous: [-1, 1, 0, 0]
    const logits_i8 = [_]i8{
        -20, -20, 20,  -20, 99,  99,
        1,   0,   0,   -1,  -99, -99,
        -20, -20, -20, 20,  127, -128,
        -1,  1,   0,   0,   0,   0,
    };

    var out: [32]u8 = undefined;
    const res_keep = try ctc.decodeI8(view, &logits_i8, 0.25, -20, .{
        .time_steps = 4,
        .class_stride = 6,
        .score_mode = @backingInt(ctc.ScoreMode.logits),
        .decode_flags = 0,
        .low_confidence_thresh_bps = 7500,
        .expected_ocr_model = @backingInt(ctc.OcrModel.japanese),
    }, &out);
    try std.testing.expectEqualStrings("?\xe3\x81\xaf\xe3\x81\x84", out[0..res_keep.text_bytes]);
    try std.testing.expectEqual(@as(u32, 3), res_keep.emitted_tokens);
    try std.testing.expectEqual(@as(u32, 1), res_keep.blank_frames);
    try std.testing.expectEqual(@as(u32, 1), res_keep.unk_count);
    try std.testing.expect(res_keep.confidence_bps < 7500);
    try std.testing.expect(res_keep.min_token_confidence_bps < 4000);
    try std.testing.expectEqual(@as(u8, 1), res_keep.low_confidence);

    const res_skip = try ctc.decodeI8(view, &logits_i8, 0.25, -20, .{
        .time_steps = 4,
        .class_stride = 6,
        .score_mode = @backingInt(ctc.ScoreMode.logits),
        .decode_flags = ctc.flag_skip_unk,
        .low_confidence_thresh_bps = 7500,
    }, &out);
    try std.testing.expectEqualStrings("\xe3\x81\xaf\xe3\x81\x84", out[0..res_skip.text_bytes]);
    try std.testing.expectEqual(@as(u32, 1), res_skip.unk_count);
}

test "CTC probs mode, all-blank frame sequence, and expNegF32 match reference math" {
    // Check pure IEEE expNegF32 accuracy across [-80, 0] against std.math.exp.
    const samples = [_]f32{ 0.0, -0.05, -0.34, -0.5, -1.0, -3.75, -15.2, -50.0, -87.2 };
    for (samples) |x| {
        const got = ctc.expNegF32(x);
        const want = @exp(x);
        const rel_err = @abs(got - want) / @max(want, 1e-38);
        try std.testing.expect(rel_err < 2e-6);
    }
    try std.testing.expectEqual(@as(f32, 0.0), ctc.expNegF32(-120.0));

    const glyphs = [_][]const u8{ "", "X", "Y" };
    var wire: [128]u8 = undefined;
    const view = try buildCharset(.{ .ocr_model = .chinese, .blank_index = 0 }, &glyphs, &wire);

    // Pre-softmax quantized probabilities: scale = 1/100.0, zp = -100 -> q = 100*p - 100.
    // Frame 0: blank p=0.95 (q=-5), X p=0.03, Y p=0.02
    // Frame 1: X p=0.80 (q=-20), blank p=0.10, Y p=0.10
    const probs_i8 = [_]i8{
        -5,  -97, -98,
        -90, -20, -90,
    };
    var out: [16]u8 = undefined;
    const res = try ctc.decodeI8(view, &probs_i8, 0.01, -100, .{
        .time_steps = 2,
        .score_mode = @backingInt(ctc.ScoreMode.probs),
        .low_confidence_thresh_bps = 7500,
    }, &out);
    try std.testing.expectEqualStrings("X", out[0..res.text_bytes]);
    try std.testing.expectEqual(@as(u16, 8000), res.confidence_bps);
    try std.testing.expectEqual(@as(u16, 8000), res.min_token_confidence_bps);
    try std.testing.expectEqual(@as(u8, 0), res.low_confidence);

    // All-blank frames -> empty text, 0 confidence, low_confidence=1.
    const blank_f32 = [_]f32{ 0.99, 0.01, 0.0, 0.98, 0.01, 0.01 };
    const blank_res = try ctc.decodeF32(view, &blank_f32, .{
        .time_steps = 2,
        .score_mode = @backingInt(ctc.ScoreMode.probs),
        .low_confidence_thresh_bps = 7500,
    }, &out);
    try std.testing.expectEqual(@as(u32, 0), blank_res.text_bytes);
    try std.testing.expectEqual(@as(u32, 0), blank_res.emitted_tokens);
    try std.testing.expectEqual(@as(u32, 2), blank_res.blank_frames);
    try std.testing.expectEqual(@as(u16, 0), blank_res.confidence_bps);
    try std.testing.expectEqual(@as(u8, 1), blank_res.low_confidence);
}

test "C ABI CTC exports enforce null precedence, validation errors, and no-write on error" {
    const glyphs = [_][]const u8{ "", "\xe4\xb8\xad", "\xe6\x96\x87" };
    var wire: [128]u8 = undefined;
    const tchr_len = try ctc.writeCharset(.{ .ocr_model = .chinese, .blank_index = 0 }, &glyphs, &wire);

    var hdr: abi.CharsetHeader = std.mem.zeroes(abi.CharsetHeader);
    hdr.class_count = 999;
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_ctc_charset_open(&wire, tchr_len, null));
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_ctc_charset_open(null, tchr_len, &hdr));
    try std.testing.expectEqual(err_mod.k_ra8_err_invalid_size, abi.ra8_translate_ctc_charset_open(null, 0, &hdr));
    try std.testing.expectEqual(@as(u32, 999), hdr.class_count);

    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_ctc_charset_open(&wire, tchr_len, &hdr));
    try std.testing.expectEqual(@as(u32, 3), hdr.class_count);
    try std.testing.expectEqual(@as(u8, 3), hdr.ocr_model);

    var g_ptr: ?[*]const u8 = null;
    var g_len: u16 = 77;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_range_check_failed,
        abi.ra8_translate_ctc_charset_glyph(&wire, tchr_len, 3, &g_ptr, &g_len),
    );
    try std.testing.expectEqual(@as(u16, 77), g_len);
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_ctc_charset_glyph(&wire, tchr_len, 1, &g_ptr, &g_len));
    try std.testing.expectEqualStrings("\xe4\xb8\xad", g_ptr.?[0..g_len]);

    const cfg: abi.DecodeCfg = .{
        .time_steps = 2,
        .class_stride = 0,
        .score_mode = 0,
        .decode_flags = 0,
        .low_confidence_thresh_bps = 7500,
        .expected_ocr_model = 3,
    };
    const logits_i8 = [_]i8{ -10, 20, -10, -10, -10, 20 };
    var out_buf = [_]u8{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    var res: abi.DecodeResult = std.mem.zeroes(abi.DecodeResult);
    res.text_bytes = 555;

    // Null checks take precedence over everything.
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_ctc_decode_i8(&wire, tchr_len, &logits_i8, 6, 0.5, 0, &cfg, &out_buf, 6, null),
    );
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_ctc_decode_i8(&wire, tchr_len, &logits_i8, 6, 0.5, 0, null, &out_buf, 6, &res),
    );
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_ctc_decode_i8(&wire, tchr_len, null, 6, 0.5, 0, &cfg, &out_buf, 6, &res),
    );

    // Output buffer too small must leave BOTH out_buf and out_result untouched!
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_ctc_decode_i8(&wire, tchr_len, &logits_i8, 6, 0.5, 0, &cfg, &out_buf, 5, &res),
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, &out_buf);
    try std.testing.expectEqual(@as(u32, 555), res.text_bytes);

    // Script mismatch fails validation without touching out_buf.
    var bad_cfg = cfg;
    bad_cfg.expected_ocr_model = 1;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_validation_failed,
        abi.ra8_translate_ctc_decode_i8(&wire, tchr_len, &logits_i8, 6, 0.5, 0, &bad_cfg, &out_buf, 6, &res),
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, &out_buf);

    // Valid i8 decode succeeds and matches exact UTF-8 output.
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_ctc_decode_i8(&wire, tchr_len, &logits_i8, 6, 0.5, 0, &cfg, &out_buf, 6, &res),
    );
    try std.testing.expectEqualStrings("\xe4\xb8\xad\xe6\x96\x87", out_buf[0..res.text_bytes]);

    // NaN in float logits fails with invalid_arg without touching out_buf.
    const nan_logits = [_]f32{ -1, 10, -1, std.math.nan(f32), -1, 10 };
    @memset(&out_buf, 0x5A);
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_arg,
        abi.ra8_translate_ctc_decode_f32(&wire, tchr_len, &nan_logits, 6, &cfg, &out_buf, 6, &res),
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A }, &out_buf);
}
