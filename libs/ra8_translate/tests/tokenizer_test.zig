//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit and reference-vector tests for `TVOC` serialization, SentencePiece
//! Unigram Viterbi and BPE codecs, multilingual (ko/ja/zh/en) normalization,
//! byte fallback, and adversarial container validation.

const std = @import("std");
const tok = @import("tokenizer");

const k_ms = "\xe2\x96\x81";

test "unigram Viterbi beats greedy longest-match, normalizes fullwidth ASCII, and handles BOS/EOS" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown }, // 0
        .{ .text = "<s>", .score = 0.0, .kind = .control }, // 1
        .{ .text = "</s>", .score = 0.0, .kind = .control }, // 2
        .{ .text = "<pad>", .score = 0.0, .kind = .control }, // 3
        .{ .text = k_ms, .score = -4.0 }, // 4
        .{ .text = k_ms ++ "ab", .score = -5.0 }, // 5
        .{ .text = "c", .score = -5.0 }, // 6 (5+6 => -10.0)
        .{ .text = k_ms ++ "a", .score = -1.0 }, // 7
        .{ .text = "bc", .score = -1.0 }, // 8 (7+8 => -2.0 wins!)
        .{ .text = k_ms ++ "hello", .score = -0.5 }, // 9
        .{ .text = k_ms ++ "world", .score = -0.8 }, // 10
        .{ .text = "!", .score = -0.4 }, // 11
    };

    var wire: [2048]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags | tok.flag_norm_fullwidth,
        .unk_id = 0,
        .bos_id = 1,
        .eos_id = 2,
        .pad_id = 3,
    }, &pieces, &wire);
    try std.testing.expectEqualStrings("TVOC", wire[0..4]);

    const view = try tok.TvocView.open(wire[0..tvoc_len]);
    try std.testing.expectEqual(@as(u32, 12), view.header.vocab_size);
    try std.testing.expectEqual(tok.ModelType.unigram, view.header.model_type);

    var work: [4096]u8 = undefined;
    var ids: [32]u32 = undefined;

    // Viterbi picks [_a, bc] (-2.0) rather than greedy longest-match [_ab, c] (-10.0).
    const v_n = try view.encode("  abc  ", 0, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 7, 8 }, ids[0..v_n]);

    // U+3000 ideographic space + tab + newline + fullwidth '!' (U+FF01).
    const fw_in = "  \xe3\x80\x80 hello\t\nworld\xef\xbc\x81 ";
    const needed = try tok.minWorkBufBytes(fw_in.len, .unigram);
    const n = try view.encode(fw_in, tok.enc_add_bos | tok.enc_add_eos, work[0..needed], &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 1, 9, 10, 11, 2 }, ids[0..n]);

    var decoded: [64]u8 = undefined;
    const dec_ids = [_]u32{ 3, 1, 9, 10, 11, 2, 3 };
    const dec_len = try view.decode(&dec_ids, &decoded);
    try std.testing.expectEqualStrings("hello world!", decoded[0..dec_len]);

    // Empty and whitespace-only strings produce zero tokens without BOS/EOS.
    try std.testing.expectEqual(@as(usize, 0), try view.encode("", 0, &work, &ids));
    try std.testing.expectEqual(@as(usize, 0), try view.encode("   \t\n  ", 0, &work, &ids));
    try std.testing.expectEqual(@as(usize, 2), try view.encode("", tok.enc_add_bos | tok.enc_add_eos, &work, &ids));
    try std.testing.expectEqual(@as(usize, 0), try view.decode(&[_]u32{}, &decoded));
}

test "unigram encodes Korean, Japanese, Chinese with glossary user_defined spans and decodes English" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown }, // 0
        .{ .text = "<s>", .score = 0.0, .kind = .control }, // 1
        .{ .text = "</s>", .score = 0.0, .kind = .control }, // 2
        .{ .text = "__G00__", .score = 0.0, .kind = .user_defined }, // 3
        .{ .text = "__", .score = -0.1 }, // 4 (must NOT split user_defined __G00__)
        .{ .text = "G00", .score = -0.1 }, // 5
        .{ .text = k_ms, .score = -2.5 }, // 6
        // Korean pieces:
        .{ .text = k_ms ++ "\xeb\x82\x98\xeb\x8a\x94", .score = -1.2 }, // 7: _naneun
        .{ .text = k_ms ++ "\xed\x95\x99\xea\xb5\x90", .score = -1.4 }, // 8: _hakgyo
        .{ .text = "\xec\x97\x90", .score = -0.9 }, // 9: -e
        .{ .text = k_ms ++ "\xea\xb0\x84\xeb\x8b\xa4", .score = -1.1 }, // 10: _ganda
        .{ .text = "\xeb\x8b\x98", .score = -1.0 }, // 11: -nim
        .{ .text = "\xec\x9d\x80", .score = -0.8 }, // 12: -eun
        // Japanese pieces:
        .{ .text = k_ms ++ "\xe7\xa7\x81", .score = -1.1 }, // 13: _watashi
        .{ .text = "\xe3\x81\xaf", .score = -0.6 }, // 14: -wa
        .{ .text = "\xe5\xad\xa6\xe6\xa0\xa1", .score = -1.3 }, // 15: gakkou
        .{ .text = "\xe3\x81\xab", .score = -0.7 }, // 16: -ni
        .{ .text = "\xe8\xa1\x8c\xe3\x81\x8f", .score = -1.0 }, // 17: iku
        .{ .text = "\xe3\x81\x95\xe3\x82\x93", .score = -0.9 }, // 18: -san
        .{ .text = "\xe3\x80\x82", .score = -0.5 }, // 19: U+3002 period
        // Chinese pieces:
        .{ .text = k_ms ++ "\xe4\xbb\x96", .score = -1.0 }, // 20: _ta
        .{ .text = "\xe5\x9c\xa8", .score = -0.8 }, // 21: zai
        .{ .text = "\xe7\x9c\x8b", .score = -1.1 }, // 22: kan
        .{ .text = "\xe6\xbc\xab\xe7\x94\xbb", .score = -1.2 }, // 23: manhua
        .{ .text = ".", .score = -0.4 }, // 24
        .{ .text = "!", .score = -0.4 }, // 25
        // English output targets:
        .{ .text = k_ms ++ "I", .score = -1.0 }, // 26
        .{ .text = k_ms ++ "go", .score = -1.1 }, // 27
        .{ .text = k_ms ++ "to", .score = -0.9 }, // 28
        .{ .text = k_ms ++ "school", .score = -1.5 }, // 29
        .{ .text = k_ms ++ "He", .score = -1.0 }, // 30
        .{ .text = k_ms ++ "reads", .score = -1.2 }, // 31
        .{ .text = k_ms ++ "manga", .score = -1.3 }, // 32
        .{ .text = k_ms ++ "See", .score = -1.0 }, // 33
    };

    var wire: [4096]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags | tok.flag_norm_fullwidth,
        .unk_id = 0,
        .bos_id = 1,
        .eos_id = 2,
    }, &pieces, &wire);
    const view = try tok.TvocView.open(wire[0..tvoc_len]);

    var work: [4096]u8 = undefined;
    var ids: [32]u32 = undefined;

    // Korean: "naneun hakgyoe ganda." and "__G00__nim-eun hakgyoe ganda".
    const ko_src = "\xeb\x82\x98\xeb\x8a\x94 \xed\x95\x99\xea\xb5\x90\xec\x97\x90 \xea\xb0\x84\xeb\x8b\xa4.";
    const ko_n = try view.encode(ko_src, tok.enc_add_eos, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 7, 8, 9, 10, 24, 2 }, ids[0..ko_n]);

    const ko_mask_src = "__G00__\xeb\x8b\x98\xec\x9d\x80 \xed\x95\x99\xea\xb5\x90\xec\x97\x90 \xea\xb0\x84\xeb\x8b\xa4";
    const ko_mask_n = try view.encode(ko_mask_src, 0, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 6, 3, 11, 12, 8, 9, 10 }, ids[0..ko_mask_n]);

    // Japanese: "watashi wa gakkou ni iku." and "__G00__san wa gakkou ni iku!".
    const ja_src = "\xe7\xa7\x81\xe3\x81\xaf\xe5\xad\xa6\xe6\xa0\xa1\xe3\x81\xab\xe8\xa1\x8c\xe3\x81\x8f\xe3\x80\x82";
    const ja_n = try view.encode(ja_src, tok.enc_add_eos, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 13, 14, 15, 16, 17, 19, 2 }, ids[0..ja_n]);

    const ja_mask_src = "__G00__\xe3\x81\x95\xe3\x82\x93\xe3\x81\xaf\xe5\xad\xa6\xe6\xa0\xa1\xe3\x81\xab\xe8\xa1\x8c\xe3\x81\x8f\xef\xbc\x81";
    const ja_mask_n = try view.encode(ja_mask_src, 0, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 6, 3, 18, 14, 15, 16, 17, 25 }, ids[0..ja_mask_n]);

    // Chinese: "ta zai kan manhua.".
    const zh_src = "\xe4\xbb\x96\xe5\x9c\xa8\xe7\x9c\x8b\xe6\xbc\xab\xe7\x94\xbb\xe3\x80\x82";
    const zh_n = try view.encode(zh_src, tok.enc_add_eos, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 20, 21, 22, 23, 19, 2 }, ids[0..zh_n]);

    // English decode back to canonical strings (including preserved "__G00__").
    var out: [128]u8 = undefined;
    const len1 = try view.decode(&[_]u32{ 1, 26, 27, 28, 29, 24, 2 }, &out);
    try std.testing.expectEqualStrings("I go to school.", out[0..len1]);

    const len2 = try view.decode(&[_]u32{ 1, 30, 31, 32, 25, 2 }, &out);
    try std.testing.expectEqualStrings("He reads manga!", out[0..len2]);

    const len3 = try view.decode(&[_]u32{ 1, 33, 6, 3, 25, 2 }, &out);
    try std.testing.expectEqualStrings("See __G00__!", out[0..len3]);
}

test "BPE honors merge ranks, leftmost tie-breaking, user_defined spans, and English decode" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown }, // 0
        .{ .text = "<s>", .score = 0.0, .kind = .control }, // 1
        .{ .text = "</s>", .score = 0.0, .kind = .control }, // 2
        .{ .text = "__G01__", .score = 0.0, .kind = .user_defined }, // 3
        .{ .text = k_ms, .score = -10.0 }, // 4
        .{ .text = "p", .score = -20.0 }, // 5
        .{ .text = "l", .score = -21.0 }, // 6
        .{ .text = "a", .score = -22.0 }, // 7
        .{ .text = "y", .score = -23.0 }, // 8
        .{ .text = "i", .score = -24.0 }, // 9
        .{ .text = "n", .score = -25.0 }, // 10
        .{ .text = "g", .score = -26.0 }, // 11
        .{ .text = "b", .score = -27.0 }, // 12
        .{ .text = "ab", .score = -1.0 }, // 13: equal priority to "ba" (-1.0)
        .{ .text = "ba", .score = -1.0 }, // 14: tie -> leftmost "ab" wins in "abab"
        .{ .text = "in", .score = -1.0 }, // 15
        .{ .text = "ing", .score = -2.0 }, // 16
        .{ .text = "pl", .score = -3.0 }, // 17
        .{ .text = "pla", .score = -4.0 }, // 18
        .{ .text = "play", .score = -5.0 }, // 19
        .{ .text = k_ms ++ "play", .score = -6.0 }, // 20
        // Korean Hangul two-syllable merge: "hangeul" (han + geul).
        .{ .text = "\xed\x95\x9c", .score = -30.0 }, // 21: han
        .{ .text = "\xea\xb8\x80", .score = -31.0 }, // 22: geul
        .{ .text = k_ms ++ "\xed\x95\x9c", .score = -7.0 }, // 23: _han
        .{ .text = k_ms ++ "\xed\x95\x9c\xea\xb8\x80", .score = -8.0 }, // 24: _hangeul
    };

    var wire: [4096]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .bpe,
        .flags = tok.default_flags,
        .unk_id = 0,
        .bos_id = 1,
        .eos_id = 2,
    }, &pieces, &wire);
    const view = try tok.TvocView.open(wire[0..tvoc_len]);

    var work: [4096]u8 = undefined;
    var ids: [32]u32 = undefined;
    var out: [64]u8 = undefined;

    // "playing" -> [_play, ing] -> "playing".
    const needed = try tok.minWorkBufBytes(16, .bpe);
    const n1 = try view.encode("playing", 0, work[0..needed], &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 20, 16 }, ids[0..n1]);
    try std.testing.expectEqualStrings("playing", out[0..try view.decode(ids[0..n1], &out)]);

    // "abab" -> left-to-right tie-break on score -1.0 picks (0..2)="ab", then (2..4)="ab".
    const n2 = try view.encode("abab", 0, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 4, 13, 13 }, ids[0..n2]);
    try std.testing.expectEqualStrings("abab", out[0..try view.decode(ids[0..n2], &out)]);

    // Korean Hangul BPE merge + user-defined placeholder: "hangeul __G01__".
    const n3 = try view.encode("\xed\x95\x9c\xea\xb8\x80 __G01__", tok.enc_add_bos | tok.enc_add_eos, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 1, 24, 4, 3, 2 }, ids[0..n3]);
    try std.testing.expectEqualStrings("\xed\x95\x9c\xea\xb8\x80 __G01__", out[0..try view.decode(ids[0..n3], &out)]);
}

test "unknown collapsing vs byte fallback UTF-8 reassembly and replacement" {
    // Byte-fallback disabled: adjacent unknown codepoints merge into single unk_id.
    const no_bf_pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown }, // 0
        .{ .text = k_ms, .score = -1.0 }, // 1
        .{ .text = "A", .score = -2.0 }, // 2
    };
    var wire_no_bf: [1024]u8 = undefined;
    const len_no_bf = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags,
        .unk_id = 0,
    }, &no_bf_pieces, &wire_no_bf);
    const v_no_bf = try tok.TvocView.open(wire_no_bf[0..len_no_bf]);

    var work: [2048]u8 = undefined;
    var ids: [32]u32 = undefined;
    var out: [64]u8 = undefined;

    const u_n = try v_no_bf.encode("A\xf0\x9f\x98\x80?!", 0, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 1, 2, 0 }, ids[0..u_n]);
    const u_dec = try v_no_bf.decode(ids[0..u_n], &out);
    try std.testing.expectEqualStrings("A \xe2\x81\x87 ", out[0..u_dec]);

    // Byte-fallback enabled: multi-byte kanji U+6F22 (E6 BC A2) decomposes and reassembles cleanly.
    const bf_pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown }, // 0
        .{ .text = k_ms, .score = -1.0 }, // 1
        .{ .text = "<0xE6>", .score = 0.0, .kind = .byte }, // 2
        .{ .text = "<0xBC>", .score = 0.0, .kind = .byte }, // 3
        .{ .text = "<0xA2>", .score = 0.0, .kind = .byte }, // 4
        .{ .text = "<0xFF>", .score = 0.0, .kind = .byte }, // 5 (invalid lone byte)
        .{ .text = "<0x41>", .score = 0.0, .kind = .byte }, // 6 ('A')
    };
    var wire_bf: [2048]u8 = undefined;
    const len_bf = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags | tok.flag_byte_fallback,
        .unk_id = 0,
    }, &bf_pieces, &wire_bf);
    const v_bf = try tok.TvocView.open(wire_bf[0..len_bf]);

    const bf_n = try v_bf.encode("\xe6\xbc\xa2", 0, &work, &ids);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 1, 2, 3, 4 }, ids[0..bf_n]);
    try std.testing.expectEqualStrings("\xe6\xbc\xa2", out[0..try v_bf.decode(ids[0..bf_n], &out)]);

    // Lone invalid byte token <0xFF> yields U+FFFD followed cleanly by 'A'.
    const bad_dec = try v_bf.decode(&[_]u32{ 1, 5, 6 }, &out);
    try std.testing.expectEqualStrings("\xef\xbf\xbdA", out[0..bad_dec]);
}

test "encode and decode preserve transactional no-write guarantees on errors" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown }, // 0
        .{ .text = k_ms ++ "ok", .score = -1.0 }, // 1
    };
    var wire: [1024]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags,
        .unk_id = 0,
    }, &pieces, &wire);
    const view = try tok.TvocView.open(wire[0..tvoc_len]);

    var work: [512]u8 = undefined;
    var out_ids = [_]u32{ 0xAAAA_AAAA, 0xBBBB_BBBB };
    try std.testing.expectError(error.InvalidSize, view.encode("ok", 0, work[0..4], &out_ids));
    try std.testing.expectEqual(@as(u32, 0xAAAA_AAAA), out_ids[0]);

    var zero_ids = [_]u32{};
    try std.testing.expectError(error.InvalidSize, view.encode("ok", 0, &work, &zero_ids));

    try std.testing.expectError(error.InvalidArg, view.encode("\xff", 0, &work, &out_ids));
    try std.testing.expectEqual(@as(u32, 0xAAAA_AAAA), out_ids[0]);

    try std.testing.expectError(error.NotSupported, view.encode("ok", tok.enc_add_bos, &work, &out_ids));
    try std.testing.expectError(error.NotSupported, view.encode("ok", tok.enc_add_eos, &work, &out_ids));

    var out_utf8 = [_]u8{ 0x5A, 0x5A };
    try std.testing.expectError(error.OutOfRange, view.decode(&[_]u32{99}, &out_utf8));
    try std.testing.expectEqual(@as(u8, 0x5A), out_utf8[0]);

    try std.testing.expectError(error.InvalidSize, view.decode(&[_]u32{1}, out_utf8[0..1]));
    try std.testing.expectEqual(@as(u8, 0x5A), out_utf8[0]);
}

test "TVOC reader rejects corrupt magic, CRC mismatches, bad byte tokens, and bad indices" {
    const pieces = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown },
        .{ .text = "a", .score = -1.0 },
        .{ .text = "<0x0A>", .score = 0.0, .kind = .byte },
    };
    var wire: [512]u8 = undefined;
    const tvoc_len = try tok.writeVocab(.{
        .model_type = .unigram,
        .flags = tok.default_flags,
        .unk_id = 0,
    }, &pieces, &wire);

    // Truncated and corrupted magic / CRC.
    try std.testing.expectError(error.InvalidSize, tok.TvocView.open(wire[0..32]));
    try std.testing.expectError(error.InvalidSize, tok.TvocView.open(wire[0 .. tvoc_len - 1]));

    var bad = wire;
    bad[0] ^= 0xFF;
    try std.testing.expectError(error.ValidationFailed, tok.TvocView.open(bad[0..tvoc_len]));

    bad = wire;
    bad[4] = 9; // bad version
    try std.testing.expectError(error.NotSupported, tok.TvocView.open(bad[0..tvoc_len]));

    bad = wire;
    bad[tvoc_len - 1] ^= 0x01; // flip last pool byte -> CRC mismatch
    try std.testing.expectError(error.CrcMismatch, tok.TvocView.open(bad[0..tvoc_len]));

    // Reject malformed byte token text and wrong unk_id kind at write time.
    const bad_byte = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .unknown },
        .{ .text = "<0xzz>", .score = 0.0, .kind = .byte },
    };
    try std.testing.expectError(error.InvalidArg, tok.writeVocab(.{ .model_type = .unigram, .unk_id = 0 }, &bad_byte, &wire));

    const bad_unk = [_]tok.PieceInput{
        .{ .text = "<unk>", .score = 0.0, .kind = .normal },
    };
    try std.testing.expectError(error.InvalidArg, tok.writeVocab(.{ .model_type = .unigram, .unk_id = 0 }, &bad_unk, &wire));
}
