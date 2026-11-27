//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit and CLI contract tests for `tools/translate_eval`.

const std = @import("std");
const eval = @import("eval");
const tool_options = @import("tool_options");

const corpus = eval.corpus;
const metrics = eval.metrics;
const pipeline = eval.pipeline;

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;
const expectApproxEqAbs = std.testing.expectApproxEqAbs;

fn runTool(argv: []const []const u8) !std.process.RunResult {
    return std.process.run(std.testing.allocator, std.testing.io, .{
        .argv = argv,
        .stderr_limit = .limited(1024 * 1024),
        .stdout_limit = .limited(1024 * 1024),
    });
}

fn freeResult(result: std.process.RunResult) void {
    std.testing.allocator.free(result.stdout);
    std.testing.allocator.free(result.stderr);
}

test "boxIoU handles identical, disjoint, and partial intersections" {
    const a = metrics.Box{ .x = 10, .y = 20, .w = 100, .h = 50 };
    const b = metrics.Box{ .x = 60, .y = 20, .w = 100, .h = 50 };
    const c_box = metrics.Box{ .x = 500, .y = 500, .w = 20, .h = 20 };

    try expectApproxEqAbs(1.0, metrics.boxIoU(a, a), 1e-9);
    try expectApproxEqAbs(0.0, metrics.boxIoU(a, c_box), 1e-9);
    // overlap 50x50 = 2500, union = 5000 + 5000 - 2500 = 7500 -> 1/3
    try expectApproxEqAbs(1.0 / 3.0, metrics.boxIoU(a, b), 1e-9);
}

test "scoreDetection enforces 1-to-1 IoU matching with precision and recall" {
    const refs = [_]metrics.Box{
        .{ .x = 0, .y = 0, .w = 100, .h = 100 },
        .{ .x = 200, .y = 200, .w = 100, .h = 100 },
    };
    const preds = [_]metrics.Box{
        .{ .x = 0, .y = 0, .w = 100, .h = 100 },
        .{ .x = 10, .y = 10, .w = 90, .h = 90 },
        .{ .x = 500, .y = 500, .w = 40, .h = 40 },
    };

    const res = try metrics.scoreDetection(std.testing.allocator, &preds, &refs, 0.5);
    try expectEqual(@as(u32, 1), res.tp);
    try expectEqual(@as(u32, 2), res.fp);
    try expectEqual(@as(u32, 1), res.fn_count);
    try expectApproxEqAbs(1.0 / 3.0, res.precision(), 1e-9);
    try expectApproxEqAbs(0.5, res.recall(), 1e-9);
}

test "computeCer operates on UTF-8 scalar codepoints across ja/zh/ko" {
    try expectApproxEqAbs(0.0, try metrics.computeCer(std.testing.allocator, "Wait", "Wait"), 1e-9);

    // Substitute 1 Japanese character out of 5 ("\u4eca\u65e5\u306f\u96e8" vs "...\u96ea").
    const ja_ref = "\xe4\xbb\x8a\xe6\x97\xa5\xe3\x81\xaf\xe9\x9b\xa8";
    const ja_hyp = "\xe4\xbb\x8a\xe6\x97\xa5\xe3\x81\xaf\xe9\x9b\xaa";
    try expectApproxEqAbs(0.25, try metrics.computeCer(std.testing.allocator, ja_hyp, ja_ref), 1e-9);

    // Substitute 1 Korean syllable out of 4 ("\uc624\ub298\ube44\uc640" vs "\uc624\ub298\ub208\uc640").
    const ko_ref = "\xec\x98\xa4\xeb\x8a\x98\xeb\xb9\x84\xec\x99\x80";
    const ko_hyp = "\xec\x98\xa4\xeb\x8a\x98\xeb\x88\x88\xec\x99\x80";
    try expectApproxEqAbs(0.25, try metrics.computeCer(std.testing.allocator, ko_hyp, ko_ref), 1e-9);
}

test "computeChrF scores identical 100.0, disjoint 0.0, and partial similarity" {
    const identical = try metrics.computeChrF(std.testing.allocator, "Today it is raining.", "Today it is raining.");
    try expectApproxEqAbs(100.0, identical, 1e-6);

    const disjoint = try metrics.computeChrF(std.testing.allocator, "zzzz", "aaaa");
    try expectApproxEqAbs(0.0, disjoint, 1e-6);

    const partial = try metrics.computeChrF(std.testing.allocator, "We are still here.", "We are still in time.");
    try expect(partial > 40.0 and partial < 95.0);
}

test "embedded evaluation corpus parses cleanly with all 3 languages and MIT license" {
    const parsed = try corpus.parseCorpus(std.testing.allocator, corpus.default_corpus_json);
    defer parsed.deinit();

    try expectEqual(@as(u32, 1), parsed.value.schema);
    try expect(std.mem.eql(u8, parsed.value.license, "MIT"));
    try expectEqual(@as(usize, 3), parsed.value.languages.len);
    try expectEqual(@as(usize, 12), parsed.value.pages.len);
}

test "default evaluation pipeline scores float and host_i8 across ja, zh, ko" {
    const parsed = try corpus.parseCorpus(std.testing.allocator, corpus.default_corpus_json);
    defer parsed.deinit();

    var report = try pipeline.evaluate(std.testing.allocator, parsed.value, null);
    defer report.deinit();

    try expect(!report.has_emulator);
    try expectEqual(@as(usize, 8), report.rows.len);

    var found_ja_float = false;
    var found_overall_i8 = false;
    for (report.rows) |row| {
        if (std.mem.eql(u8, row.lang, "ja") and row.mode == .float_ref) {
            found_ja_float = true;
            try expect(row.furigana_dropped >= 2);
            try expectApproxEqAbs(1.0, row.det_precision, 1e-6);
            try expectApproxEqAbs(1.0, row.det_recall, 1e-6);
            try expectApproxEqAbs(0.0, row.ocr_cer, 1e-6);
            try expectApproxEqAbs(100.0, row.nmt_chrf, 1e-4);
        }
        if (std.mem.eql(u8, row.lang, "overall") and row.mode == .host_i8) {
            found_overall_i8 = true;
            try expect(row.weight_bytes > 0);
            try expect(row.det_recall < 1.0);
            try expect(row.ocr_cer > 0.0 and row.ocr_cer < 0.25);
            try expect(row.nmt_chrf > 80.0 and row.nmt_chrf < 100.0);
        }
    }
    try expect(found_ja_float);
    try expect(found_overall_i8);
}

test "emulator prediction scoring computes exact metrics on external trace" {
    const parsed = try corpus.parseCorpus(std.testing.allocator, corpus.default_corpus_json);
    defer parsed.deinit();

    // Build an external emulator trace JSON matching ground truth on the first page of each language
    // and empty predictions on the rest, via a one-page custom corpus slice.
    const one_lang_json =
        \\{
        \\  "schema": 1,
        \\  "license": "MIT",
        \\  "provenance": "unit test",
        \\  "languages": ["ja"],
        \\  "pages": [
        \\    {
        \\      "page_id": "p0",
        \\      "lang": "ja",
        \\      "layout": "manga_rtl",
        \\      "page_width": 640,
        \\      "page_height": 960,
        \\      "det_map_width": 64,
        \\      "det_map_height": 96,
        \\      "det_regions": [{ "x": 10, "y": 10, "w": 10, "h": 10, "prob": 0.9, "is_furigana": false }],
        \\      "bubbles": [{
        \\        "x": 100, "y": 100, "w": 100, "h": 100, "orientation": "horizontal",
        \\        "source_text": "ab", "english_ref": "Wait here.",
        \\        "ocr_margin": 0.3, "ocr_distractor_sub": "", "nmt_quant_margin": 0.3, "nmt_alt_token": ""
        \\      }]
        \\    }
        \\  ]
        \\}
    ;
    const emu_json =
        \\{
        \\  "schema": 1,
        \\  "pages": [{
        \\    "page_id": "p0",
        \\    "boxes": [{ "x": 100, "y": 100, "w": 100, "h": 100 }],
        \\    "bubbles": [{ "source_text": "ac", "english_text": "Wait here." }]
        \\  }]
        \\}
    ;
    const c_parsed = try corpus.parseCorpus(std.testing.allocator, one_lang_json);
    defer c_parsed.deinit();
    const e_parsed = try corpus.parseEmuTrace(std.testing.allocator, emu_json);
    defer e_parsed.deinit();

    var report = try pipeline.evaluate(std.testing.allocator, c_parsed.value, e_parsed.value);
    defer report.deinit();

    try expect(report.has_emulator);
    try expectEqual(@as(usize, 6), report.rows.len);
    for (report.rows) |r| {
        if (r.mode == .emulator and std.mem.eql(u8, r.lang, "ja")) {
            try expectApproxEqAbs(1.0, r.det_precision, 1e-6);
            try expectApproxEqAbs(0.5, r.ocr_cer, 1e-6);
            try expectApproxEqAbs(100.0, r.nmt_chrf, 1e-4);
        }
    }
}

test "parseArgs resolves help, json, corpus, emu-results, and errors" {
    try expectEqual(eval.Action.help, eval.parseArgs(&.{"--help"}));
    const act = eval.parseArgs(&.{ "--corpus", "c.json", "--emu-results", "e.json", "--json" });
    switch (act) {
        .run => |o| {
            try expect(o.json_out);
            try expect(std.mem.eql(u8, o.corpus_path.?, "c.json"));
            try expect(std.mem.eql(u8, o.emu_results_path.?, "e.json"));
        },
        else => return error.UnexpectedParseResult,
    }
    switch (eval.parseArgs(&.{"--bogus"})) {
        .usage_error => {},
        else => return error.ExpectedUsageError,
    }
}

test "CLI default execution exits 0 and prints per-language table" {
    const result = try runTool(&.{tool_options.tool_exe});
    defer freeResult(result);

    try expectEqual(@as(u8, 0), result.term.exited);
    try expectEqual(@as(usize, 0), result.stderr.len);
    try expect(std.mem.indexOf(u8, result.stdout, "# Translation Evaluation Harness") != null);
    try expect(std.mem.indexOf(u8, result.stdout, "| ja | float |") != null);
    try expect(std.mem.indexOf(u8, result.stdout, "| zh | host_i8 |") != null);
    try expect(std.mem.indexOf(u8, result.stdout, "| ko | host_i8 |") != null);
    try expect(std.mem.indexOf(u8, result.stdout, "| overall | host_i8 |") != null);
}

test "CLI --json emits valid structured report matching all languages" {
    const result = try runTool(&.{ tool_options.tool_exe, "--json" });
    defer freeResult(result);

    try expectEqual(@as(u8, 0), result.term.exited);
    const JsonReport = struct {
        schema: u32,
        has_emulator: bool,
        rows: []const struct {
            lang: []const u8,
            mode: []const u8,
            pages: u32,
            bubbles: u32,
            det_precision: f64,
            det_recall: f64,
            det_f1: f64,
            ocr_cer: f64,
            nmt_chrf: f64,
            furigana_dropped: u32,
            weight_bytes: u64,
        },
    };
    const parsed = try std.json.parseFromSlice(JsonReport, std.testing.allocator, result.stdout, .{});
    defer parsed.deinit();

    try expectEqual(@as(u32, 1), parsed.value.schema);
    try expect(!parsed.value.has_emulator);
    try expectEqual(@as(usize, 8), parsed.value.rows.len);
}

test "CLI --help prints usage and exits 0" {
    const result = try runTool(&.{ tool_options.tool_exe, "--help" });
    defer freeResult(result);

    try expectEqual(@as(u8, 0), result.term.exited);
    try expect(std.mem.indexOf(u8, result.stdout, "usage: translate_eval") != null);
}

test "CLI rejects invalid flag with exit code 2" {
    const result = try runTool(&.{ tool_options.tool_exe, "--unknown-flag" });
    defer freeResult(result);

    try expectEqual(@as(u8, 2), result.term.exited);
    try expect(result.stderr.len > 0);
}

test "CLI --emu-results scores external emulator trace from disk" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const corpus_str =
        \\{
        \\  "schema": 1, "license": "MIT", "provenance": "cli-test", "languages": ["ko"],
        \\  "pages": [{
        \\    "page_id": "k0", "lang": "ko", "layout": "webtoon_ttb",
        \\    "page_width": 640, "page_height": 960, "det_map_width": 64, "det_map_height": 96,
        \\    "det_regions": [{ "x": 10, "y": 10, "w": 10, "h": 10, "prob": 0.9, "is_furigana": false }],
        \\    "bubbles": [{
        \\      "x": 100, "y": 100, "w": 100, "h": 100, "orientation": "horizontal",
        \\      "source_text": "hi", "english_ref": "Hello!", "ocr_margin": 0.3,
        \\      "ocr_distractor_sub": "", "nmt_quant_margin": 0.3, "nmt_alt_token": ""
        \\    }]
        \\  }]
        \\}
    ;
    const trace_str =
        \\{
        \\  "schema": 1,
        \\  "pages": [{
        \\    "page_id": "k0",
        \\    "boxes": [{ "x": 100, "y": 100, "w": 100, "h": 100 }],
        \\    "bubbles": [{ "source_text": "hi", "english_text": "Hello!" }]
        \\  }]
        \\}
    ;
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "c.json", .data = corpus_str });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "e.json", .data = trace_str });

    const c_path = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache", "tmp", &tmp.sub_path, "c.json" });
    defer std.testing.allocator.free(c_path);
    const e_path = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache", "tmp", &tmp.sub_path, "e.json" });
    defer std.testing.allocator.free(e_path);

    const result = try runTool(&.{ tool_options.tool_exe, "--corpus", c_path, "--emu-results", e_path });
    defer freeResult(result);

    try expectEqual(@as(u8, 0), result.term.exited);
    try expect(std.mem.indexOf(u8, result.stdout, "| ko | emulator |") != null);
    try expect(std.mem.indexOf(u8, result.stdout, "| overall | emulator |") != null);
}

test "tpak builds valid aligned CRC-stamped bundle for default corpus" {
    const parsed = try corpus.parseCorpus(std.testing.allocator, corpus.default_corpus_json);
    defer parsed.deinit();

    const pack = try eval.tpak.buildTpak(std.testing.allocator, parsed.value);
    defer std.testing.allocator.free(pack);

    try expectEqual(eval.tpak.magic, std.mem.readInt(u32, pack[0..4], .little));
    try expectEqual(eval.tpak.version, std.mem.readInt(u32, pack[4..8], .little));
    try expectEqual(@as(u32, 12), std.mem.readInt(u32, pack[8..12], .little));
    try expectEqual(@as(u32, @intCast(pack.len)), std.mem.readInt(u32, pack[12..16], .little));
    const stored_crc = std.mem.readInt(u32, pack[16..20], .little);
    var copy = try std.testing.allocator.dupe(u8, pack);
    defer std.testing.allocator.free(copy);
    @memset(copy[16..20], 0);
    try expectEqual(eval.fixtures.crc32Ieee(copy), stored_crc);
}

test "extractTraceFromLog validates PASS banner, strips console prefixes, and rejects faulted runs" {
    const good_log =
        "console> translate_eval_e2e: boot\n" ++
        "console> EMU_TRACE_BEGIN\n" ++
        "console> {\"schema\":1,\"pages\":[\n" ++
        "console> ]}\n" ++
        "console> EMU_TRACE_END\n" ++
        "console> translate_eval_e2e: PASS ckpt=1\n";
    const json_slice = try eval.tpak.extractTraceFromLog(std.testing.allocator, good_log);
    defer std.testing.allocator.free(json_slice);
    try std.testing.expectEqualStrings("{\"schema\":1,\"pages\":[]}", json_slice);

    const fault_log = "translate_eval_e2e: PASS\nzig core: BKPT halted\n";
    try std.testing.expectError(
        error.EmulatorReportedFailure,
        eval.tpak.extractTraceFromLog(std.testing.allocator, fault_log),
    );

    const no_pass = "EMU_TRACE_BEGIN\n{}\nEMU_TRACE_END\n";
    try std.testing.expectError(
        error.EmulatorMissingPassBanner,
        eval.tpak.extractTraceFromLog(std.testing.allocator, no_pass),
    );
}

test "CLI --emit-tpak and --emu-log end-to-end" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const tpak_path = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache", "tmp", &tmp.sub_path, "out.tpak" });
    defer std.testing.allocator.free(tpak_path);
    const corpus_path = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache", "tmp", &tmp.sub_path, "c.json" });
    defer std.testing.allocator.free(corpus_path);
    const log_path = try std.fs.path.join(std.testing.allocator, &.{ ".zig-cache", "tmp", &tmp.sub_path, "emu.log" });
    defer std.testing.allocator.free(log_path);

    const corpus_str =
        \\{
        \\  "schema": 1, "license": "MIT", "provenance": "cli-test", "languages": ["ko"],
        \\  "pages": [{
        \\    "page_id": "k0", "lang": "ko", "layout": "webtoon_ttb",
        \\    "page_width": 640, "page_height": 960, "det_map_width": 64, "det_map_height": 96,
        \\    "det_regions": [{ "x": 10, "y": 10, "w": 10, "h": 10, "prob": 0.9, "is_furigana": false }],
        \\    "bubbles": [{
        \\      "x": 100, "y": 100, "w": 100, "h": 100, "orientation": "horizontal",
        \\      "source_text": "hi", "english_ref": "Hello!", "ocr_margin": 0.3,
        \\      "ocr_distractor_sub": "", "nmt_quant_margin": 0.3, "nmt_alt_token": ""
        \\    }]
        \\  }]
        \\}
    ;
    const log_str =
        "translate_eval_e2e: job ok\r\n" ++
        "EMU_TRACE_BEGIN\r\n" ++
        "{\"schema\":1,\"pages\":[{\"page_id\":\"k0\",\"boxes\":[{\"x\":100,\"y\":100,\"w\":100,\"h\":100}],\"bubbles\":[{\"source_text\":\"hi\",\"english_text\":\"Hello!\"}],\"furigana_dropped\":1,\"weight_bytes\":1234}]}\r\n" ++
        "EMU_TRACE_END\r\n" ++
        "translate_eval_e2e: PASS ckpt_match=1\r\n";

    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "c.json", .data = corpus_str });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "emu.log", .data = log_str });

    const emit_res = try runTool(&.{ tool_options.tool_exe, "--emit-tpak", tpak_path });
    defer freeResult(emit_res);
    try expectEqual(@as(u8, 0), emit_res.term.exited);

    const eval_res = try runTool(&.{ tool_options.tool_exe, "--corpus", corpus_path, "--emu-log", log_path });
    defer freeResult(eval_res);
    try expectEqual(@as(u8, 0), eval_res.term.exited);
    try expect(std.mem.indexOf(u8, eval_res.stdout, "| ko | emulator |") != null);
    try expect(std.mem.indexOf(u8, eval_res.stdout, "1234 |") != null);

    const bad_combo = try runTool(&.{ tool_options.tool_exe, "--emu-bin", "only_bin" });
    defer freeResult(bad_combo);
    try expectEqual(@as(u8, 2), bad_combo.term.exited);
}
