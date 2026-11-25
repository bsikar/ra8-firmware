//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zero-model per-title language selection from MDL metadata hints and
//! Unicode script range analysis over UTF-8 OCR text samples.

const std = @import("std");
const err_mod = @import("err");
const sidecar = @import("sidecar");

pub const Error = err_mod.TranslateError;
pub const Language = sidecar.Language;

/// Reason explaining how the selector picked `language`.
pub const SelectionReason = enum(u8) {
    unknown = 0,
    script_confirmed = 1,
    deferred_to_hint = 2,
    script_override = 3,
};

/// OCR recognizer profile selected for a source language.
pub const OcrModel = enum(u8) {
    none = 0,
    korean = 1,
    japanese = 2,
    chinese = 3,
};

/// NMT translation model profile selected for a source language.
pub const NmtModel = enum(u8) {
    none = 0,
    ko_en = 1,
    ja_en = 2,
    zh_en = 3,
};

/// Codepoint counts by script bucket (`ra8_translate_script_counts_t`).
pub const ScriptCounts = extern struct {
    hangul_count: u32 = 0,
    kana_count: u32 = 0,
    han_count: u32 = 0,
    other_count: u32 = 0,
};

/// Complete selection result (`ra8_translate_lang_selection_t`).
pub const Selection = extern struct {
    language: u8 = 0,
    reason: u8 = 0,
    ocr_model: u8 = 0,
    nmt_model: u8 = 0,
    reserved: [4]u8 = .{ 0, 0, 0, 0 },
    counts: ScriptCounts = .{},
};

/// Parse a per-title MDL/BCP-47 metadata string (e.g. `"ja"`, `"ko-KR"`, `"zh-Hans"`) into `Language`.
pub fn parseLanguageHint(raw_hint: []const u8) Language {
    const trimmed = trimAsciiWhitespace(raw_hint);
    if (trimmed.len == 0) return .unknown;

    if (matchesTagPrefix(trimmed, "ko") or matchesExactIgnoreCase(trimmed, "kor") or matchesExactIgnoreCase(trimmed, "korean")) {
        return .korean;
    }
    if (matchesTagPrefix(trimmed, "ja") or matchesTagPrefix(trimmed, "jp") or matchesExactIgnoreCase(trimmed, "jpn") or matchesExactIgnoreCase(trimmed, "japanese")) {
        return .japanese;
    }
    if (matchesTagPrefix(trimmed, "zh") or matchesTagPrefix(trimmed, "zho") or matchesExactIgnoreCase(trimmed, "chi") or matchesExactIgnoreCase(trimmed, "cmn") or matchesExactIgnoreCase(trimmed, "yue") or matchesExactIgnoreCase(trimmed, "chinese")) {
        return .chinese;
    }
    return .unknown;
}

/// Count Hangul, Kana, Han (CJK Ideographs), and remaining codepoints in valid UTF-8.
pub fn countScripts(sample_utf8: []const u8) Error!ScriptCounts {
    const view = std.unicode.Utf8View.init(sample_utf8) catch return Error.InvalidArg;
    var iter = view.iterator();
    var counts = ScriptCounts{};
    while (iter.nextCodepoint()) |cp| {
        if (isHangul(cp)) {
            counts.hangul_count +|= 1;
        } else if (isKana(cp)) {
            counts.kana_count +|= 1;
        } else if (isHan(cp)) {
            counts.han_count +|= 1;
        } else {
            counts.other_count +|= 1;
        }
    }
    return counts;
}

/// Select the source language and OCR/NMT pipeline profile from pre-aggregated script counts.
pub fn selectFromCounts(hint: Language, counts: ScriptCounts) Selection {
    const chosen = decideLanguage(hint, counts);
    const ocr = ocrModelFor(chosen.lang);
    const nmt = nmtModelFor(chosen.lang);

    return .{
        .language = @backingInt(chosen.lang),
        .reason = @backingInt(chosen.reason),
        .ocr_model = @backingInt(ocr),
        .nmt_model = @backingInt(nmt),
        .reserved = .{ 0, 0, 0, 0 },
        .counts = counts,
    };
}

/// Select the source language and OCR/NMT pipeline profile from a hint and UTF-8 OCR sample.
pub fn selectLanguage(hint: Language, sample_utf8: []const u8) Error!Selection {
    const counts = try countScripts(sample_utf8);
    return selectFromCounts(hint, counts);
}

/// Convenience wrapper parsing an MDL metadata hint string and selecting models.
pub fn selectFromHintTag(hint_tag: []const u8, sample_utf8: []const u8) Error!Selection {
    return selectLanguage(parseLanguageHint(hint_tag), sample_utf8);
}

/// Map a resolved language to its OCR recognizer model identifier.
pub fn ocrModelFor(lang: Language) OcrModel {
    return switch (lang) {
        .unknown => .none,
        .korean => .korean,
        .japanese => .japanese,
        .chinese => .chinese,
    };
}

/// Map a resolved language to its NMT translator model identifier.
pub fn nmtModelFor(lang: Language) NmtModel {
    return switch (lang) {
        .unknown => .none,
        .korean => .ko_en,
        .japanese => .ja_en,
        .chinese => .zh_en,
    };
}

const Decision = struct {
    lang: Language,
    reason: SelectionReason,
};

fn decideLanguage(hint: Language, counts: ScriptCounts) Decision {
    if (counts.hangul_count > 0 and counts.kana_count == 0) {
        return .{
            .lang = .korean,
            .reason = if (hint == .korean) .script_confirmed else .script_override,
        };
    }
    if (counts.kana_count > 0 and counts.hangul_count == 0) {
        return .{
            .lang = .japanese,
            .reason = if (hint == .japanese) .script_confirmed else .script_override,
        };
    }
    if (counts.hangul_count > 0 and counts.kana_count > 0) {
        if (counts.hangul_count > counts.kana_count) {
            return .{
                .lang = .korean,
                .reason = if (hint == .korean) .script_confirmed else .script_override,
            };
        }
        if (counts.kana_count > counts.hangul_count) {
            return .{
                .lang = .japanese,
                .reason = if (hint == .japanese) .script_confirmed else .script_override,
            };
        }
        if (hint == .korean or hint == .japanese) {
            return .{ .lang = hint, .reason = .deferred_to_hint };
        }
        return .{ .lang = .unknown, .reason = .unknown };
    }

    // Kanji-only / Han-only or punctuation-only text defers to the per-title hint.
    if (hint != .unknown) {
        return .{ .lang = hint, .reason = .deferred_to_hint };
    }
    return .{ .lang = .unknown, .reason = .unknown };
}

/// True when `cp` belongs to Unicode Hangul Syllables or Hangul Jamo blocks.
pub fn isHangul(cp: u21) bool {
    return (cp >= 0x1100 and cp <= 0x11FF) or
        (cp >= 0x3130 and cp <= 0x318F) or
        (cp >= 0xA960 and cp <= 0xA97F) or
        (cp >= 0xAC00 and cp <= 0xD7AF) or
        (cp >= 0xD7B0 and cp <= 0xD7FF);
}

/// True when `cp` belongs to Hiragana, Katakana, Halfwidth Katakana letters, or Kana extensions.
pub fn isKana(cp: u21) bool {
    return (cp >= 0x3040 and cp <= 0x309F) or
        (cp >= 0x30A0 and cp <= 0x30FF) or
        (cp >= 0x31F0 and cp <= 0x31FF) or
        (cp >= 0xFF66 and cp <= 0xFF9F) or
        (cp >= 0x1B000 and cp <= 0x1B16F);
}

/// True when `cp` belongs to CJK Unified or Compatibility Ideograph blocks.
pub fn isHan(cp: u21) bool {
    return (cp >= 0x3400 and cp <= 0x4DBF) or
        (cp >= 0x4E00 and cp <= 0x9FFF) or
        (cp >= 0xF900 and cp <= 0xFAFF) or
        (cp >= 0x20000 and cp <= 0x2A6DF) or
        (cp >= 0x2A700 and cp <= 0x2CEAF) or
        (cp >= 0x2CEB0 and cp <= 0x2EBEF) or
        (cp >= 0x2F800 and cp <= 0x2FA1F) or
        (cp >= 0x30000 and cp <= 0x323AF);
}

fn trimAsciiWhitespace(input: []const u8) []const u8 {
    var start: usize = 0;
    var end: usize = input.len;
    while (start < end and isAsciiSpace(input[start])) : (start += 1) {}
    while (end > start and isAsciiSpace(input[end - 1])) : (end -= 1) {}
    return input[start..end];
}

fn isAsciiSpace(b: u8) bool {
    return b == ' ' or b == '\t' or b == '\r' or b == '\n';
}

fn matchesExactIgnoreCase(value: []const u8, literal: []const u8) bool {
    if (value.len != literal.len) return false;
    for (value, literal) |a, b| {
        if (toLowerAscii(a) != b) return false;
    }
    return true;
}

fn matchesTagPrefix(value: []const u8, prefix: []const u8) bool {
    if (value.len < prefix.len) return false;
    for (value[0..prefix.len], prefix) |a, b| {
        if (toLowerAscii(a) != b) return false;
    }
    if (value.len == prefix.len) return true;
    const sep = value[prefix.len];
    return sep == '-' or sep == '_';
}

fn toLowerAscii(b: u8) u8 {
    if (b >= 'A' and b <= 'Z') return b + ('a' - 'A');
    return b;
}
