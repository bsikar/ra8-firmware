//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for per-series glossary persistence, candidate discovery,
//! consistent honorific policy, and multi-page chapter name stabilization.

const std = @import("std");
const glossary = @import("glossary");

fn isolateMockTranslate(stem: []const u8) []const u8 {
    // Katakana "tanaka" -> "Tanaka", "ekusukaribaa" -> "Excalibur", Hangul "minjun" -> "Minjun".
    if (std.mem.eql(u8, stem, "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab")) return "Tanaka";
    if (std.mem.eql(u8, stem, "\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x82\xab\xe3\x83\xaa\xe3\x83\x90\xe3\x83\xbc")) return "Excalibur";
    if (std.mem.eql(u8, stem, "\xeb\xaf\xbc\xec\xa4\x80")) return "Minjun";
    return "Unknown";
}

fn mockDriftingNmt(masked_src: []const u8, page_index: usize) []const u8 {
    // Simulates an unguided NMT model whose rendering of "__G00__" survives unchanged
    // across pages while raw non-placeholder words would vary across pages.
    _ = page_index;
    if (std.mem.eql(u8, masked_src, "__G00__!")) return "Wait, __G00__!";
    if (std.mem.eql(u8, masked_src, "__G00__\xe3\x81\xaf__G01__\xe3\x82\x92")) return "__G00__ holds __G01__";
    if (std.mem.eql(u8, masked_src, "__G00__\xe3\x81\xa0")) return "That is __G00__!";
    return masked_src;
}

test "series glossary persists as TGL1 and reloads identically across chapters" {
    var table = glossary.GlossaryTable.init(.japanese, 1);
    try table.upsert("\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab", "Tanaka");
    try table.upsert("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x82\xab\xe3\x83\xaa\xe3\x83\x90\xe3\x83\xbc", "Excalibur");

    var wire: [1024]u8 = undefined;
    const saved_len = try table.save(&wire);
    try std.testing.expectEqualStrings("TGL1", wire[0..4]);

    var reloaded = glossary.GlossaryTable.init(.unknown, 0);
    try reloaded.load(wire[0..saved_len]);
    try std.testing.expectEqual(table.revision, reloaded.revision);
    try std.testing.expectEqual(table.source_lang, reloaded.source_lang);
    try std.testing.expectEqual(@as(u16, 2), reloaded.entry_count);
    try std.testing.expectEqualStrings("Tanaka", reloaded.lookup("\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab").?);
    try std.testing.expectEqualStrings("Excalibur", reloaded.lookup("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x82\xab\xe3\x83\xaa\xe3\x83\x90\xe3\x83\xbc").?);

    // Bit flip in serialized TGL1 must fail CRC-32 check.
    var corrupt = wire;
    corrupt[saved_len - 1] ^= 0x04;
    try std.testing.expectError(error.CrcMismatch, reloaded.load(corrupt[0..saved_len]));
}

test "multi-page Japanese golden chapter extracts candidates, translates once, and renders names consistently" {
    var series_table = glossary.GlossaryTable.init(.japanese, 1);
    // Page 0: "Tanaka-san!" and "Tanaka wa Excalibur wo"
    // Page 1: "Excalibur-sama"
    // Page 2: "Tanaka-kun da"
    const page0_b0 = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93!";
    const page0_b1 = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\xaf\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x82\xab\xe3\x83\xaa\xe3\x83\x90\xe3\x83\xbc\xe3\x82\x92";
    const page1_b0 = "\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x82\xab\xe3\x83\xaa\xe3\x83\x90\xe3\x83\xbc\xe3\x81\x95\xe3\x81\xbe";
    const page2_b0 = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x8f\xe3\x82\x93\xe3\x81\xa0";
    const chapter_bubbles = [_][]const u8{ page0_b0, page0_b1, page1_b0, page2_b0 };

    var candidates: [16]glossary.Candidate = undefined;
    const found = try glossary.extractCandidates(.japanese, &series_table, &chapter_bubbles, &candidates);
    try std.testing.expectEqual(@as(usize, 2), found);
    try std.testing.expectEqualStrings("\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab", candidates[0].stem_slice);
    try std.testing.expect(candidates[0].saw_honorific);
    try std.testing.expectEqualStrings("\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x82\xab\xe3\x83\xaa\xe3\x83\x90\xe3\x83\xbc", candidates[1].stem_slice);

    for (candidates[0..found]) |cand| {
        try series_table.upsert(cand.stem_slice, isolateMockTranslate(cand.stem_slice));
    }

    // Subsequent extraction skips terms already persisted in series_table.
    const found_again = try glossary.extractCandidates(.japanese, &series_table, &chapter_bubbles, &candidates);
    try std.testing.expectEqual(@as(usize, 0), found_again);

    const expected_renders = [_][]const u8{
        "Wait, Tanaka-san!",
        "Tanaka holds Excalibur",
        "__G00__sama-check",
        "That is Tanaka-kun!",
    };
    var mask_buf: [128]u8 = undefined;
    var rest_buf: [128]u8 = undefined;
    var plan: glossary.SubstitutionPlan = .{};

    // Page 0 bubble 0 ("Tanaka-san!") -> "Wait, Tanaka-san!"
    const m0 = try series_table.maskText(page0_b0, &mask_buf, &plan);
    try std.testing.expectEqualStrings("__G00__!", mask_buf[0..m0]);
    const r0 = try series_table.restoreText(&plan, mockDriftingNmt(mask_buf[0..m0], 0), &rest_buf);
    try std.testing.expectEqualStrings(expected_renders[0], rest_buf[0..r0]);

    // Page 0 bubble 1 -> "Tanaka holds Excalibur"
    const m1 = try series_table.maskText(page0_b1, &mask_buf, &plan);
    try std.testing.expectEqualStrings("__G00__\xe3\x81\xaf__G01__\xe3\x82\x92", mask_buf[0..m1]);
    const r1 = try series_table.restoreText(&plan, mockDriftingNmt(mask_buf[0..m1], 0), &rest_buf);
    try std.testing.expectEqualStrings(expected_renders[1], rest_buf[0..r1]);

    // Page 2 bubble 0 ("Tanaka-kun da") -> "That is Tanaka-kun!"
    const m3 = try series_table.maskText(page2_b0, &mask_buf, &plan);
    try std.testing.expectEqualStrings("__G00__\xe3\x81\xa0", mask_buf[0..m3]);
    const r3 = try series_table.restoreText(&plan, mockDriftingNmt(mask_buf[0..m3], 2), &rest_buf);
    try std.testing.expectEqualStrings(expected_renders[3], rest_buf[0..r3]);
}

test "Korean honorific policy keeps -nim/-ssi suffixes and avoids double suffixes" {
    var table = glossary.GlossaryTable.init(.korean, 1);
    const minjun = "\xeb\xaf\xbc\xec\xa4\x80";
    try table.upsert(minjun, isolateMockTranslate(minjun));

    const bub0 = "\xeb\xaf\xbc\xec\xa4\x80\xeb\x8b\x98";
    const bub1 = "\xeb\xaf\xbc\xec\xa4\x80\xec\x94\xa8";
    var cands: [8]glossary.Candidate = undefined;
    var empty_tbl = glossary.GlossaryTable.init(.korean, 1);
    const n = try glossary.extractCandidates(.korean, &empty_tbl, &.{ bub0, bub1 }, &cands);
    try std.testing.expectEqual(@as(usize, 1), n);
    try std.testing.expectEqualStrings(minjun, cands[0].stem_slice);

    var mask_buf: [64]u8 = undefined;
    var rest_buf: [64]u8 = undefined;
    var plan: glossary.SubstitutionPlan = .{};

    const ml0 = try table.maskText(bub0, &mask_buf, &plan);
    try std.testing.expectEqualStrings("__G00__", mask_buf[0..ml0]);
    const rl0 = try table.restoreText(&plan, "Hello __G00__", &rest_buf);
    try std.testing.expectEqualStrings("Hello Minjun-nim", rest_buf[0..rl0]);

    const ml1 = try table.maskText(bub1, &mask_buf, &plan);
    try std.testing.expectEqualStrings("__G00__", mask_buf[0..ml1]);
    const rl1 = try table.restoreText(&plan, "Call __G00__", &rest_buf);
    try std.testing.expectEqualStrings("Call Minjun-ssi", rest_buf[0..rl1]);

    // Entry already ending in -nim does not append a duplicate -nim suffix.
    var pre_suffixed = glossary.GlossaryTable.init(.korean, 1);
    try pre_suffixed.upsert(minjun, "Minjun-nim");
    const ml2 = try pre_suffixed.maskText(bub0, &mask_buf, &plan);
    try std.testing.expectEqualStrings("__G00__", mask_buf[0..ml2]);
    const rl2 = try pre_suffixed.restoreText(&plan, "Hi __G00__", &rest_buf);
    try std.testing.expectEqualStrings("Hi Minjun-nim", rest_buf[0..rl2]);
}

test "TglView maskText and restoreText preserve output buffers on capacity failure" {
    var wire: [256]u8 = undefined;
    const l0 = try glossary.initEmptyWire(.japanese, 1, &wire);
    var wire2: [256]u8 = undefined;
    const l1 = try glossary.upsertWire(wire[0..l0], "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab", "Tanaka", &wire2);
    const view = try glossary.TglView.open(wire2[0..l1]);

    var tiny_mask = [_]u8{ 0xAA, 0xBB };
    var plan: glossary.SubstitutionPlan = .{ .count = 99 };
    try std.testing.expectError(error.InvalidSize, view.maskText("\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab", &tiny_mask, &plan));
    try std.testing.expectEqualSlices(u8, &.{ 0xAA, 0xBB }, &tiny_mask);
    try std.testing.expectEqual(@as(usize, 99), plan.count);

    var mask_ok: [32]u8 = undefined;
    const ml = try view.maskText("\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93", &mask_ok, &plan);
    try std.testing.expectEqualStrings("__G00__", mask_ok[0..ml]);

    var tiny_rest = [_]u8{ 0x55, 0x66 };
    try std.testing.expectError(error.InvalidSize, view.restoreText(&plan, "__G00__", &tiny_rest));
    try std.testing.expectEqualSlices(u8, &.{ 0x55, 0x66 }, &tiny_rest);
}
