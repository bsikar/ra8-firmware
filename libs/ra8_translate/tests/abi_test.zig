//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exported C ABI boundary and failure-precedence tests for `libs/ra8_translate`.

const std = @import("std");
const abi = @import("ra8_translate_abi");
const err_mod = @import("err");
const tok = @import("tokenizer");

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

test "C ABI tokenizer open, piece_info, work_bytes, encode, and decode round-trip cleanly" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown },
        .{ .text = "<s>", .score = 0.0, .kind = .control },
        .{ .text = "</s>", .score = 0.0, .kind = .control },
        .{ .text = "\xe2\x96\x81", .score = -2.0 },
        .{ .text = "\xe2\x96\x81\xeb\x82\x98\xeb\x8a\x94", .score = -1.0 },
        .{ .text = "\xe2\x96\x81go", .score = -0.8 },
        .{ .text = "!", .score = -0.4 },
    };
    var wire: [1024]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags | tok.flag_norm_fullwidth,
        .unk_id = 0,
        .bos_id = 1,
        .eos_id = 2,
    }, &pieces, &wire);

    var hdr: abi.VocabHeader = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_tok_open(&wire, tvoc_len, &hdr));
    try std.testing.expectEqual(@as(u32, 7), hdr.vocab_size);
    try std.testing.expectEqual(@as(u8, 0), hdr.model_type);
    try std.testing.expectEqual(@as(u32, 1), hdr.bos_id);
    try std.testing.expectEqual(@as(u32, 2), hdr.eos_id);

    var p_ptr: ?[*]const u8 = null;
    var p_len: u16 = 0;
    var p_kind: u8 = 0;
    var p_score: f32 = 0.0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_tok_piece_info(&wire, tvoc_len, 4, &p_ptr, &p_len, &p_kind, &p_score),
    );
    try std.testing.expectEqual(@as(u8, 0), p_kind);
    try std.testing.expectEqualStrings("\xe2\x96\x81\xeb\x82\x98\xeb\x8a\x94", p_ptr.?[0..p_len]);
    try std.testing.expectEqual(@as(f32, -1.0), p_score);

    const src = "\xeb\x82\x98\xeb\x8a\x94";
    var work_needed: usize = 0;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_tok_work_bytes(src.len, 0, &work_needed));

    var work: [512]u8 = undefined;
    var ids: [8]u32 = undefined;
    var id_count: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_tok_encode(&wire, tvoc_len, src.ptr, src.len, tok.enc_add_eos, &work, work_needed, &ids, ids.len, &id_count),
    );
    try std.testing.expectEqualSlices(u32, &[_]u32{ 4, 2 }, ids[0..id_count]);

    const dec_ids = [_]u32{ 1, 5, 6, 2 };
    var out_utf8: [32]u8 = undefined;
    var out_len: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_tok_decode(&wire, tvoc_len, &dec_ids, dec_ids.len, &out_utf8, out_utf8.len, &out_len),
    );
    try std.testing.expectEqualStrings("go!", out_utf8[0..out_len]);
}

test "C ABI tokenizer exports enforce null-first precedence and no-write on errors" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown },
        .{ .text = "ok", .score = -1.0 },
    };
    var wire: [512]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .bpe,
        .flags = tok.default_flags,
        .unk_id = 0,
    }, &pieces, &wire);

    var hdr: abi.VocabHeader = std.mem.zeroes(abi.VocabHeader);
    hdr.vocab_size = 777;
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_tok_open(&wire, tvoc_len, null));
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_tok_open(null, tvoc_len, &hdr));
    try std.testing.expectEqual(@as(u32, 777), hdr.vocab_size);

    var p_ptr: ?[*]const u8 = null;
    var p_len: u16 = 0;
    var p_kind: u8 = 0;
    var p_score: f32 = 0.0;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_tok_piece_info(&wire, tvoc_len, 0, null, &p_len, &p_kind, &p_score),
    );
    try std.testing.expectEqual(
        err_mod.k_ra8_err_range_check_failed,
        abi.ra8_translate_tok_piece_info(&wire, tvoc_len, 9, &p_ptr, &p_len, &p_kind, &p_score),
    );

    var work_needed: usize = 55;
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_tok_work_bytes(10, 0, null));
    try std.testing.expectEqual(err_mod.k_ra8_err_invalid_arg, abi.ra8_translate_tok_work_bytes(10, 99, &work_needed));
    try std.testing.expectEqual(@as(usize, 55), work_needed);

    var work: [256]u8 = undefined;
    var ids = [_]u32{ 99, 99 };
    var id_count: usize = 88;
    const text = "ok";
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_tok_encode(&wire, tvoc_len, text.ptr, text.len, 0, &work, work.len, null, 2, &id_count),
    );
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_tok_encode(&wire, tvoc_len, text.ptr, text.len, 0, &work, 2, &ids, ids.len, &id_count),
    );
    try std.testing.expectEqual(@as(u32, 99), ids[0]);
    try std.testing.expectEqual(@as(usize, 88), id_count);

    var out_utf8 = [_]u8{ 0xAA, 0xBB };
    var out_len: usize = 44;
    const bad_id = [_]u32{9};
    try std.testing.expectEqual(
        err_mod.k_ra8_err_range_check_failed,
        abi.ra8_translate_tok_decode(&wire, tvoc_len, &bad_id, 1, &out_utf8, out_utf8.len, &out_len),
    );
    try std.testing.expectEqual(@as(u8, 0xAA), out_utf8[0]);
    try std.testing.expectEqual(@as(usize, 44), out_len);
}

fn abiTestFill(_: ?*anyopaque, _: i32, _: i32, _: i32, _: i32, _: u32) callconv(.c) void {}
fn abiTestDraw(_: ?*anyopaque, _: i32, _: i32, _: [*:0]const u8, _: u32, _: u32) callconv(.c) void {}
fn abiTestSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    const s = std.mem.span(str);
    out_w.* = @intCast(s.len * 8);
    out_h.* = 16;
}

test "C ABI overlay exports enforce null-first guards and round-trip fit and render" {
    const paint = abi.OverlayPaint{
        .user = null,
        .fill_rect = abiTestFill,
        .draw_text = abiTestDraw,
        .text_size = abiTestSize,
    };
    const bubble = abi.OverlayBubbleSpec{ .x = 20, .y = 30, .w = 140, .h = 80, .shape = 2, .corner_radius = 8 };
    const style = abi.OverlayStyle{};

    var safe = abi.OverlayRect{ .x = 999, .y = 999, .w = 999, .h = 999 };
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_overlay_safe_rect(null, &style, &safe));
    try std.testing.expectEqual(@as(i32, 999), safe.x);
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_overlay_safe_rect(&bubble, &style, &safe));
    try std.testing.expect(safe.w > 0 and safe.h > 0);

    var breaks: [16]u16 = @splat(0);
    var fit: abi.OverlayFit = std.mem.zeroes(abi.OverlayFit);
    const eng = "Wait for me!";
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_overlay_fit_text(null, &bubble, &style, eng.ptr, eng.len, &breaks, breaks.len, &fit),
    );
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_overlay_fit_text(&paint, &bubble, &style, eng.ptr, eng.len, &breaks, breaks.len, &fit),
    );
    try std.testing.expectEqual(@as(u16, 16), fit.font_size_px);
    try std.testing.expect(fit.line_count >= 1);

    var dmg = abi.OverlayRect{ .x = 0, .y = 0, .w = 0, .h = 0 };
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_overlay_render_bubble(
            &paint,
            &bubble,
            &style,
            fit.font_size_px,
            eng.ptr,
            eng.len,
            &breaks,
            fit.line_count,
            &dmg,
        ),
    );
    try std.testing.expectEqual(@as(i32, 20), dmg.x);
    try std.testing.expectEqual(@as(i32, 140), dmg.w);

    var key = abi.SidecarKey{
        .chapter_hash = @splat(0x33),
        .detector_model_version = 1,
        .ocr_model_version = 1,
        .nmt_model_version = 1,
        .glossary_revision = 0,
        .source_lang = 2,
    };
    const pages = [_]abi.PageEntry{.{
        .page_index = 0,
        .bubble_count = 1,
        .first_bubble = 0,
        .page_width = 400,
        .page_height = 600,
    }};
    const pool = "okWait for me!";
    const recs = [_]abi.BubbleRecord{.{
        .x = 20,
        .y = 30,
        .w = 140,
        .h = 80,
        .shape = 2,
        .text_orientation = 1,
        .corner_radius = 8,
        .reading_order = 0,
        .ocr_confidence_bps = 9200,
        .font_size_px = fit.font_size_px,
        .line_count = fit.line_count,
        .first_line_break = 0,
        .source_text_off = 0,
        .source_text_len = 2,
        .english_text_off = 2,
        .english_text_len = @intCast(eng.len),
    }};
    var trn1: [512]u8 = undefined;
    var trn1_len: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_write(&key, &pages, 1, &recs, 1, &breaks, fit.line_count, pool.ptr, pool.len, &trn1, trn1.len, &trn1_len),
    );
    var page_dmg: [4]abi.OverlayRect = undefined;
    var dmg_cnt: usize = 77;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_overlay_render_page(null, &trn1, trn1_len, 0, &style, &page_dmg, page_dmg.len, &dmg_cnt),
    );
    try std.testing.expectEqual(@as(usize, 77), dmg_cnt);
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_overlay_render_page(&paint, &trn1, trn1_len, 0, &style, &page_dmg, page_dmg.len, &dmg_cnt),
    );
    try std.testing.expectEqual(@as(usize, 1), dmg_cnt);
    try std.testing.expectEqual(dmg, page_dmg[0]);
}
