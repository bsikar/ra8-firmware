//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for Unicode script range + per-title hint language selection.

const std = @import("std");
const lang = @import("lang");
const sidecar = @import("sidecar");

test "parseLanguageHint maps MDL language tags case-insensitively" {
    try std.testing.expectEqual(sidecar.Language.korean, lang.parseLanguageHint("ko"));
    try std.testing.expectEqual(sidecar.Language.korean, lang.parseLanguageHint("  KO-KR "));
    try std.testing.expectEqual(sidecar.Language.korean, lang.parseLanguageHint("kor"));
    try std.testing.expectEqual(sidecar.Language.japanese, lang.parseLanguageHint("ja-JP"));
    try std.testing.expectEqual(sidecar.Language.japanese, lang.parseLanguageHint("jpn"));
    try std.testing.expectEqual(sidecar.Language.chinese, lang.parseLanguageHint("zh-Hans"));
    try std.testing.expectEqual(sidecar.Language.chinese, lang.parseLanguageHint("zh_TW"));
    try std.testing.expectEqual(sidecar.Language.chinese, lang.parseLanguageHint("zho"));
    try std.testing.expectEqual(sidecar.Language.unknown, lang.parseLanguageHint("en"));
    try std.testing.expectEqual(sidecar.Language.unknown, lang.parseLanguageHint(""));
}

test "selectLanguage confirms Hangul as Korean and Kana as Japanese across hints" {
    // Pure Hangul ("annyeong" U+C548 U+B155) + CJK Hanja, even with unknown or mismatched hint.
    const hangul_sample = "\xec\x95\x88\xeb\x85\x95\xe9\x9f\x93";
    const ko_confirmed = try lang.selectLanguage(.korean, hangul_sample);
    try std.testing.expectEqual(@backingInt(sidecar.Language.korean), ko_confirmed.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.script_confirmed), ko_confirmed.reason);
    try std.testing.expectEqual(@backingInt(lang.OcrModel.korean), ko_confirmed.ocr_model);
    try std.testing.expectEqual(@backingInt(lang.NmtModel.ko_en), ko_confirmed.nmt_model);
    try std.testing.expectEqual(@as(u32, 2), ko_confirmed.counts.hangul_count);
    try std.testing.expectEqual(@as(u32, 1), ko_confirmed.counts.han_count);

    const ko_override = try lang.selectLanguage(.chinese, hangul_sample);
    try std.testing.expectEqual(@backingInt(sidecar.Language.korean), ko_override.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.script_override), ko_override.reason);

    // Hiragana + Katakana + Kanji ("\xe3\x81\x93\xe3\x82\x93" hiragana + "\xe3\x82\xab" katakana + "\xe6\xbc\xa2" kanji).
    const kana_sample = "\xe3\x81\x93\xe3\x82\x93\xe3\x82\xab\xe6\xbc\xa2";
    const ja_confirmed = try lang.selectLanguage(.japanese, kana_sample);
    try std.testing.expectEqual(@backingInt(sidecar.Language.japanese), ja_confirmed.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.script_confirmed), ja_confirmed.reason);
    try std.testing.expectEqual(@backingInt(lang.OcrModel.japanese), ja_confirmed.ocr_model);
    try std.testing.expectEqual(@backingInt(lang.NmtModel.ja_en), ja_confirmed.nmt_model);
    try std.testing.expectEqual(@as(u32, 3), ja_confirmed.counts.kana_count);
    try std.testing.expectEqual(@as(u32, 1), ja_confirmed.counts.han_count);

    // Halfwidth Katakana (\xef\xbd\xb6 = U+FF76) also disambiguates Japanese over unknown hint.
    const hw_kana_sample = "\xef\xbd\xb6\xe6\x9c\xac";
    const ja_override = try lang.selectLanguage(.unknown, hw_kana_sample);
    try std.testing.expectEqual(@backingInt(sidecar.Language.japanese), ja_override.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.script_override), ja_override.reason);
}

test "selectLanguage defers kanji-only text to per-title hint across all three languages" {
    // Pure Han / Kanji characters ("nihon" / "kanji" U+65E5 U+672C U+8A9E).
    const kanji_only = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";

    const zh_sel = try lang.selectFromHintTag("zh-CN", kanji_only);
    try std.testing.expectEqual(@backingInt(sidecar.Language.chinese), zh_sel.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.deferred_to_hint), zh_sel.reason);
    try std.testing.expectEqual(@backingInt(lang.OcrModel.chinese), zh_sel.ocr_model);
    try std.testing.expectEqual(@backingInt(lang.NmtModel.zh_en), zh_sel.nmt_model);
    try std.testing.expectEqual(@as(u32, 3), zh_sel.counts.han_count);

    const ja_sel = try lang.selectFromHintTag("ja", kanji_only);
    try std.testing.expectEqual(@backingInt(sidecar.Language.japanese), ja_sel.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.deferred_to_hint), ja_sel.reason);
    try std.testing.expectEqual(@backingInt(lang.OcrModel.japanese), ja_sel.ocr_model);
    try std.testing.expectEqual(@backingInt(lang.NmtModel.ja_en), ja_sel.nmt_model);

    const ko_sel = try lang.selectFromHintTag("ko", kanji_only);
    try std.testing.expectEqual(@backingInt(sidecar.Language.korean), ko_sel.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.deferred_to_hint), ko_sel.reason);

    const unk_sel = try lang.selectFromHintTag("", kanji_only);
    try std.testing.expectEqual(@backingInt(sidecar.Language.unknown), unk_sel.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.unknown), unk_sel.reason);
    try std.testing.expectEqual(@backingInt(lang.OcrModel.none), unk_sel.ocr_model);
    try std.testing.expectEqual(@backingInt(lang.NmtModel.none), unk_sel.nmt_model);
}

test "selectLanguage rejects malformed UTF-8 sequences and handles mixed Hangul/Kana dominance" {
    try std.testing.expectError(error.InvalidArg, lang.selectLanguage(.chinese, "\xff\xfe"));

    // Two Hangul characters + one Kana character -> Korean overrides Chinese hint.
    const mixed_ko = "\xec\x95\x88\xeb\x85\x95\xe3\x81\x82";
    const sel = try lang.selectLanguage(.chinese, mixed_ko);
    try std.testing.expectEqual(@backingInt(sidecar.Language.korean), sel.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.script_override), sel.reason);

    // Equal Hangul and Kana count (1 each) defers to hint if Korean/Japanese, else unknown.
    const tied = "\xec\x95\x88\xe3\x81\x82";
    const tied_ja = try lang.selectLanguage(.japanese, tied);
    try std.testing.expectEqual(@backingInt(sidecar.Language.japanese), tied_ja.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.deferred_to_hint), tied_ja.reason);

    const tied_zh = try lang.selectLanguage(.chinese, tied);
    try std.testing.expectEqual(@backingInt(sidecar.Language.unknown), tied_zh.language);
    try std.testing.expectEqual(@backingInt(lang.SelectionReason.unknown), tied_zh.reason);
}
