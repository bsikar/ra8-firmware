//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exported C ABI boundary and failure-precedence tests for `libs/ra8_translate`.

const std = @import("std");
const abi = @import("ra8_translate_abi");
const err_mod = @import("err");

test "C ABI exports enforce null-first precedence and leave outputs untouched on error" {
    var sentinel_hdr: abi.SidecarHeader = std.mem.zeroes(abi.SidecarHeader);
    sentinel_hdr.total_bytes = 0xDEAD_BEEF;

    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_sidecar_open(null, 0, null));
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_sidecar_open(null, 10, &sentinel_hdr));
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), sentinel_hdr.total_bytes);

    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_sidecar_open(null, 0, &sentinel_hdr),
    );
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), sentinel_hdr.total_bytes);

    var matches = true;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_sidecar_key_matches(null, null, &matches),
    );
    try std.testing.expect(matches);
}

test "C ABI flat-table write and read exports round-trip cleanly" {
    var key = abi.SidecarKey{
        .chapter_hash = @splat(0x42),
        .detector_model_version = 10,
        .ocr_model_version = 20,
        .nmt_model_version = 30,
        .glossary_revision = 5,
        .source_lang = 1,
        .reserved = .{ 0, 0, 0 },
    };
    const pages = [_]abi.PageEntry{
        .{ .page_index = 0, .bubble_count = 1, .first_bubble = 0, .page_width = 600, .page_height = 800 },
    };
    const breaks = [_]u16{ 2, 5 };
    const pool = "okhello";
    const bubbles = [_]abi.BubbleRecord{
        .{
            .x = 10,
            .y = 20,
            .w = 100,
            .h = 40,
            .shape = 2,
            .text_orientation = 0,
            .corner_radius = 6,
            .reading_order = 0,
            .ocr_confidence_bps = 9100,
            .font_size_px = 16,
            .line_count = 2,
            .first_line_break = 0,
            .source_text_off = 0,
            .source_text_len = 2,
            .english_text_off = 2,
            .english_text_len = 5,
        },
    };

    var needed: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_required_bytes(1, 1, 2, pool.len, &needed),
    );
    try std.testing.expectEqual(@as(usize, 96 + 12 + 44 + 4 + 7), needed);

    var buf: [256]u8 = undefined;
    var written: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_write(
            &key,
            &pages,
            1,
            &bubbles,
            1,
            &breaks,
            2,
            pool.ptr,
            pool.len,
            &buf,
            buf.len,
            &written,
        ),
    );
    try std.testing.expectEqual(needed, written);

    var hdr: abi.SidecarHeader = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_open(&buf, written, &hdr));

    var matched = false;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_key_matches(&hdr, &key, &matched));
    try std.testing.expect(matched);

    var p0: abi.PageEntry = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_page_at(&buf, written, 0, &p0));
    try std.testing.expectEqual(@as(u16, 600), p0.page_width);

    var b0: abi.BubbleRecord = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_bubble_at(&buf, written, 0, &b0));
    try std.testing.expectEqual(@as(u16, 9100), b0.ocr_confidence_bps);

    var src_ptr: ?[*]const u8 = null;
    var src_len: u32 = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_source_text(&buf, written, 0, &src_ptr, &src_len),
    );
    try std.testing.expectEqualStrings("ok", src_ptr.?[0..src_len]);

    var eng_ptr: ?[*]const u8 = null;
    var eng_len: u32 = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_english_text(&buf, written, 0, &eng_ptr, &eng_len),
    );
    try std.testing.expectEqualStrings("hello", eng_ptr.?[0..eng_len]);

    var out_breaks = [_]u16{ 0xFFFF, 0xFFFF, 0xFFFF };
    var out_cnt: u16 = 99;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_sidecar_line_breaks(&buf, written, 0, &out_breaks, 1, &out_cnt),
    );
    try std.testing.expectEqual(@as(u16, 0xFFFF), out_breaks[0]);
    try std.testing.expectEqual(@as(u16, 99), out_cnt);

    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_line_breaks(&buf, written, 0, &out_breaks, 3, &out_cnt),
    );
    try std.testing.expectEqual(@as(u16, 2), out_cnt);
    try std.testing.expectEqualSlices(u16, &breaks, out_breaks[0..out_cnt]);
}

test "C ABI language hint parsing and script selection validate arguments" {
    var parsed: u8 = 0xAA;
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_lang_from_hint(null, 2, &parsed));
    try std.testing.expectEqual(@as(u8, 0xAA), parsed);
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_lang_from_hint("ja-JP", 5, &parsed));
    try std.testing.expectEqual(@as(u8, 2), parsed);

    var sel: abi.LangSelection = .{ .language = 99 };
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_arg,
        abi.ra8_translate_lang_select(9, null, 0, &sel),
    );
    try std.testing.expectEqual(@as(u8, 99), sel.language);

    const kanji = "\xe6\xbc\xa2\xe5\xad\x97";
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_lang_select(3, kanji.ptr, kanji.len, &sel),
    );
    try std.testing.expectEqual(@as(u8, 3), sel.language);
    try std.testing.expectEqual(@as(u32, 2), sel.counts.han_count);
}

test "C ABI glossary init, upsert, lookup, mask, and restore preserve error invariants" {
    var tgl_a: [256]u8 = undefined;
    var len_a: usize = 0;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_glossary_init(2, 1, &tgl_a, tgl_a.len, &len_a));

    const tanaka = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab";
    var tgl_b: [256]u8 = undefined;
    var len_b: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_glossary_upsert(&tgl_a, len_a, tanaka.ptr, tanaka.len, "Tanaka", 6, &tgl_b, tgl_b.len, &len_b),
    );

    var eng_ptr: ?[*]const u8 = null;
    var eng_len: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_not_found,
        abi.ra8_translate_glossary_lookup(&tgl_b, len_b, "none", 4, &eng_ptr, &eng_len),
    );
    try std.testing.expect(eng_ptr == null);

    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_glossary_lookup(&tgl_b, len_b, tanaka.ptr, tanaka.len, &eng_ptr, &eng_len),
    );
    try std.testing.expectEqualStrings("Tanaka", eng_ptr.?[0..eng_len]);

    const bub = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93";
    var masked: [64]u8 = undefined;
    var masked_len: usize = 0;
    var plan: [32]u8 = undefined;
    var plan_len: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_glossary_mask(&tgl_b, len_b, bub.ptr, bub.len, &masked, masked.len, &masked_len, &plan, plan.len, &plan_len),
    );
    try std.testing.expectEqualStrings("__G00__", masked[0..masked_len]);

    var restored: [64]u8 = undefined;
    var restored_len: usize = 0;
    const model_out = "Hey __G00__!";
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_glossary_restore(&tgl_b, len_b, &plan, plan_len, model_out.ptr, model_out.len, &restored, restored.len, &restored_len),
    );
    try std.testing.expectEqualStrings("Hey Tanaka-san!", restored[0..restored_len]);
}

test "C ABI OCR preprocessing validates nulls, bounds, rotation, and quantization" {
    var has_mve: bool = true;
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_ocr_has_helium(null));
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_ocr_has_helium(&has_mve));

    var req_bytes: usize = 999;
    try std.testing.expectEqual(err_mod.k_ra8_err_invalid_arg, abi.ra8_translate_ocr_required_bytes(10, 10, 9, &req_bytes));
    try std.testing.expectEqual(@as(usize, 999), req_bytes);
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_ocr_required_bytes(3, 2, 1, &req_bytes));
    try std.testing.expectEqual(@as(usize, 6), req_bytes);

    const src = [_]u8{ 10, 11, 20, 21, 30, 31 };
    const spec: abi.OcrImageSpec = .{ .stride_bytes = 2, .width = 2, .height = 3, .format = 1 };
    const crop: abi.OcrRect = .{ .x = 0, .y = 0, .width = 2, .height = 3 };
    const cfg: abi.OcrPreprocessCfg = .{ .dst_width = 3, .dst_height = 2, .dst_format = 1, .rotation = 1, .coord_mode = 1 };
    var out_u8: [6]u8 = @splat(0xAA);
    var out_written: usize = 77;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_ocr_resize_u8(&src, src.len, &spec, null, &cfg, &out_u8, out_u8.len, &out_written),
    );
    try std.testing.expectEqual(@as(usize, 77), out_written);
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_ocr_resize_u8(&src, src.len, &spec, &crop, &cfg, &out_u8, out_u8.len, &out_written),
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 11, 21, 31, 10, 20, 30 }, &out_u8);

    const q: abi.OcrQuantParams = .{ .mean = .{ 0.0, 0.0, 0.0 }, .inv_std = .{ 1.0, 1.0, 1.0 }, .input_scale = 1.0, .quant_scale = 2.0, .zero_point = -10 };
    var out_i8: [6]i8 = @splat(99);
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_ocr_normalize_i8(&out_u8, out_u8.len, 1, &q, &out_i8, out_i8.len, &out_written),
    );
    try std.testing.expectEqual(@as(i8, -5), out_i8[0]);

    var fused_i8: [6]i8 = @splat(99);
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_ocr_preprocess_i8(&src, src.len, &spec, &crop, &cfg, &q, &fused_i8, fused_i8.len, &out_written),
    );
    try std.testing.expectEqualSlices(i8, &out_i8, &fused_i8);
}
