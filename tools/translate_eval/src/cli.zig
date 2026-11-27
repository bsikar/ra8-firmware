//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CLI parsing, corpus/trace loading, and formatted reporting for `translate_eval`.

const std = @import("std");
pub const corpus = @import("corpus.zig");
pub const fixtures = @import("fixtures.zig");
pub const metrics = @import("metrics.zig");
pub const pipeline = @import("pipeline.zig");
pub const tpak = @import("tpak.zig");

pub const tool = "translate_eval";
pub const usage_line =
    "usage: translate_eval [-h|--help] [--corpus PATH] [--emu-results PATH | --emu-log PATH | --emu-bin BIN --emu-elf ELF] [--emit-tpak PATH] [--json]";

pub const Options = struct {
    corpus_path: ?[]const u8 = null,
    emu_results_path: ?[]const u8 = null,
    emu_log_path: ?[]const u8 = null,
    emu_bin_path: ?[]const u8 = null,
    emu_elf_path: ?[]const u8 = null,
    emit_tpak_path: ?[]const u8 = null,
    json_out: bool = false,
};

pub const Action = union(enum) {
    help,
    run: Options,
    usage_error: []const u8,
};

pub fn parseArgs(argv: []const []const u8) Action {
    var opts = Options{};
    var i: usize = 0;
    while (i < argv.len) : (i += 1) {
        const arg = argv[i];
        if (std.mem.eql(u8, arg, "-h") or std.mem.eql(u8, arg, "--help")) {
            return .help;
        } else if (std.mem.eql(u8, arg, "--json")) {
            opts.json_out = true;
        } else if (std.mem.eql(u8, arg, "--corpus")) {
            i += 1;
            if (i >= argv.len) return .{ .usage_error = "--corpus requires a path argument" };
            opts.corpus_path = argv[i];
        } else if (std.mem.eql(u8, arg, "--emu-results")) {
            i += 1;
            if (i >= argv.len) return .{ .usage_error = "--emu-results requires a path argument" };
            opts.emu_results_path = argv[i];
        } else if (std.mem.eql(u8, arg, "--emu-log")) {
            i += 1;
            if (i >= argv.len) return .{ .usage_error = "--emu-log requires a path argument" };
            opts.emu_log_path = argv[i];
        } else if (std.mem.eql(u8, arg, "--emu-bin")) {
            i += 1;
            if (i >= argv.len) return .{ .usage_error = "--emu-bin requires a path argument" };
            opts.emu_bin_path = argv[i];
        } else if (std.mem.eql(u8, arg, "--emu-elf")) {
            i += 1;
            if (i >= argv.len) return .{ .usage_error = "--emu-elf requires a path argument" };
            opts.emu_elf_path = argv[i];
        } else if (std.mem.eql(u8, arg, "--emit-tpak")) {
            i += 1;
            if (i >= argv.len) return .{ .usage_error = "--emit-tpak requires a path argument" };
            opts.emit_tpak_path = argv[i];
        } else {
            return .{ .usage_error = arg };
        }
    }
    if ((opts.emu_bin_path != null) != (opts.emu_elf_path != null)) {
        return .{ .usage_error = "--emu-bin and --emu-elf must be passed together" };
    }
    var emu_modes: u8 = 0;
    if (opts.emu_results_path != null) emu_modes += 1;
    if (opts.emu_log_path != null) emu_modes += 1;
    if (opts.emu_bin_path != null) emu_modes += 1;
    if (emu_modes > 1) {
        return .{ .usage_error = "pass at most one of --emu-results, --emu-log, or --emu-bin/--emu-elf" };
    }
    return .{ .run = opts };
}

fn findRow(rows: []const pipeline.EvalRow, lang: []const u8, mode: pipeline.Mode) ?pipeline.EvalRow {
    for (rows) |r| {
        if (std.mem.eql(u8, r.lang, lang) and r.mode == mode) return r;
    }
    return null;
}

fn writeTable(report: pipeline.EvalReport, corpus_info: corpus.Corpus, out: *std.Io.Writer) !void {
    try out.print(
        "# Translation Evaluation Harness (schema v{d}, license: {s})\n",
        .{ corpus_info.schema, corpus_info.license },
    );
    try out.print("Corpus: {d} pages across {d} languages\n", .{ corpus_info.pages.len, corpus_info.languages.len });
    if (!report.has_emulator) {
        try out.writeAll("Emulator: n/a (pass --emu-results <json> once target runner / model weights land)\n");
    }
    try out.writeAll("\n");
    try out.writeAll(
        "| Lang | Mode | Pages | Bubbles | Det P | Det R | Det F1 | OCR CER | NMT chrF | Furigana Dropped | Weight Bytes |\n",
    );
    try out.writeAll(
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n",
    );

    for (report.rows) |row| {
        try out.print(
            "| {s} | {s} | {d} | {d} | {d:.3} | {d:.3} | {d:.3} | {d:.4} | {d:.2} | {d} | {d} |\n",
            .{
                row.lang,
                row.mode.label(),
                row.pages,
                row.bubbles,
                row.det_precision,
                row.det_recall,
                row.det_f1,
                row.ocr_cer,
                row.nmt_chrf,
                row.furigana_dropped,
                row.weight_bytes,
            },
        );
    }

    try out.writeAll("\nQuantization Delta (host_i8 - float):\n");
    for (corpus_info.languages) |lang| {
        const f_row = findRow(report.rows, lang, .float_ref) orelse continue;
        const q_row = findRow(report.rows, lang, .host_i8) orelse continue;
        try printDeltaLine(out, lang, q_row.det_f1 - f_row.det_f1, q_row.ocr_cer - f_row.ocr_cer, q_row.nmt_chrf - f_row.nmt_chrf);
    }
    if (findRow(report.rows, "overall", .float_ref)) |fo| {
        if (findRow(report.rows, "overall", .host_i8)) |qo| {
            try printDeltaLine(out, "overall", qo.det_f1 - fo.det_f1, qo.ocr_cer - fo.ocr_cer, qo.nmt_chrf - fo.nmt_chrf);
        }
    }
}

fn signPrefix(v: f64) []const u8 {
    return if (v < 0.0) "-" else "+";
}

fn printDeltaLine(
    out: *std.Io.Writer,
    label: []const u8,
    df1: f64,
    dcer: f64,
    dchrf: f64,
) !void {
    try out.print(
        "  {s}: dDetF1={s}{d:.3} dCER={s}{d:.4} dchrF={s}{d:.2}\n",
        .{
            label,
            signPrefix(df1),
            @abs(df1),
            signPrefix(dcer),
            @abs(dcer),
            signPrefix(dchrf),
            @abs(dchrf),
        },
    );
}

fn writeJson(report: pipeline.EvalReport, out: *std.Io.Writer) !void {
    try out.print("{{\"schema\":1,\"has_emulator\":{},\"rows\":[", .{report.has_emulator});
    for (report.rows, 0..) |r, idx| {
        if (idx > 0) try out.writeAll(",");
        try out.print(
            "{{\"lang\":\"{s}\",\"mode\":\"{s}\",\"pages\":{d},\"bubbles\":{d}," ++
                "\"det_precision\":{d:.6},\"det_recall\":{d:.6},\"det_f1\":{d:.6}," ++
                "\"ocr_cer\":{d:.6},\"nmt_chrf\":{d:.4},\"furigana_dropped\":{d},\"weight_bytes\":{d}}}",
            .{
                r.lang,
                r.mode.label(),
                r.pages,
                r.bubbles,
                r.det_precision,
                r.det_recall,
                r.det_f1,
                r.ocr_cer,
                r.nmt_chrf,
                r.furigana_dropped,
                r.weight_bytes,
            },
        );
    }
    try out.writeAll("]}\n");
}

fn runEmulatorTarget(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    eval_corpus: corpus.Corpus,
    emu_bin: []const u8,
    emu_elf: []const u8,
    out_trace_bytes: *?[]u8,
    err: *std.Io.Writer,
) !?std.json.Parsed(corpus.EmuTrace) {
    const scratch_rel = ".zig-cache/tmp/translate_eval_sd";
    const tpak_rel = ".zig-cache/tmp/translate_eval_sd/TPAK.BIN";
    dir.createDirPath(io, scratch_rel) catch |e| {
        try err.print("{s}: cannot create temp SD dir '{s}': {s}\n", .{ tool, scratch_rel, @errorName(e) });
        return null;
    };
    defer dir.deleteTree(io, scratch_rel) catch {};

    const pack = try tpak.buildTpak(allocator, eval_corpus);
    defer allocator.free(pack);
    dir.writeFile(io, .{ .sub_path = tpak_rel, .data = pack }) catch |e| {
        try err.print("{s}: cannot write TPAK '{s}': {s}\n", .{ tool, tpak_rel, @errorName(e) });
        return null;
    };

    const run_res = std.process.run(allocator, io, .{
        .argv = &.{
            emu_bin,
            emu_elf,
            "--sd-dir",
            scratch_rel,
            "--console",
            "--until",
            "translate_eval_e2e: END",
            "--ms",
            "15000",
        },
        .stdout_limit = .limited(2 * 1024 * 1024),
        .stderr_limit = .limited(1024 * 1024),
    }) catch |e| {
        try err.print("{s}: failed running emulator '{s}': {s}\n", .{ tool, emu_bin, @errorName(e) });
        return null;
    };
    defer allocator.free(run_res.stdout);
    defer allocator.free(run_res.stderr);

    const trace_json = tpak.extractTraceFromLog(allocator, run_res.stdout) catch |e| {
        try err.print("{s}: invalid emulator output ({s})\n{s}\n", .{ tool, @errorName(e), run_res.stderr });
        return null;
    };
    errdefer allocator.free(trace_json);
    const parsed = corpus.parseEmuTrace(allocator, trace_json) catch |e| {
        try err.print("{s}: invalid extracted emulator trace JSON: {s}\n", .{ tool, @errorName(e) });
        return null;
    };
    out_trace_bytes.* = trace_json;
    return parsed;
}

pub fn run(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    argv: []const []const u8,
    out: *std.Io.Writer,
    err: *std.Io.Writer,
) !u8 {
    const tail = if (argv.len > 0) argv[1..] else argv;
    switch (parseArgs(tail)) {
        .help => {
            try out.print("{s}\n", .{usage_line});
            try out.writeAll(
                "Options:\n" ++
                    "  --corpus PATH        Evaluate custom JSON corpus instead of built-in corpus\n" ++
                    "  --emu-results PATH   Score emulator trace JSON alongside float and host_i8\n" ++
                    "  --emu-log PATH       Extract EMU_TRACE JSON from UART log and score emulator\n" ++
                    "  --emu-bin BIN        Path to ra8_emulator binary (requires --emu-elf)\n" ++
                    "  --emu-elf ELF        Path to ARM translate_eval_e2e.elf target binary\n" ++
                    "  --emit-tpak PATH     Write deterministic TPAK v1 corpus binary for target runs\n" ++
                    "  --json               Emit machine-readable JSON summary\n" ++
                    "  -h, --help           Show this help message\n",
            );
            return 0;
        },
        .usage_error => |msg| {
            try err.print("{s}: invalid argument: {s}\n{s}\n", .{ tool, msg, usage_line });
            return 2;
        },
        .run => |opts| {
            var loaded_corpus_bytes: ?[]u8 = null;
            defer if (loaded_corpus_bytes) |b| allocator.free(b);

            const corpus_bytes: []const u8 = if (opts.corpus_path) |p| blk: {
                const text = dir.readFileAlloc(io, p, allocator, .limited(corpus.max_json_bytes)) catch |e| {
                    try err.print("{s}: cannot read corpus '{s}': {s}\n", .{ tool, p, @errorName(e) });
                    return 2;
                };
                loaded_corpus_bytes = text;
                break :blk text;
            } else corpus.default_corpus_json;

            const parsed_corpus = corpus.parseCorpus(allocator, corpus_bytes) catch |e| {
                try err.print("{s}: invalid corpus JSON: {s}\n", .{ tool, @errorName(e) });
                return 1;
            };
            defer parsed_corpus.deinit();

            if (opts.emit_tpak_path) |tp| {
                const pack = try tpak.buildTpak(allocator, parsed_corpus.value);
                defer allocator.free(pack);
                dir.writeFile(io, .{ .sub_path = tp, .data = pack }) catch |e| {
                    try err.print("{s}: cannot write TPAK '{s}': {s}\n", .{ tool, tp, @errorName(e) });
                    return 2;
                };
            }

            var loaded_emu_bytes: ?[]u8 = null;
            defer if (loaded_emu_bytes) |b| allocator.free(b);
            var parsed_emu: ?std.json.Parsed(corpus.EmuTrace) = null;
            defer if (parsed_emu) |pe| pe.deinit();

            if (opts.emu_results_path) |ep| {
                const eb = dir.readFileAlloc(io, ep, allocator, .limited(corpus.max_json_bytes)) catch |e| {
                    try err.print("{s}: cannot read emu-results '{s}': {s}\n", .{ tool, ep, @errorName(e) });
                    return 2;
                };
                loaded_emu_bytes = eb;
                parsed_emu = corpus.parseEmuTrace(allocator, eb) catch |e| {
                    try err.print("{s}: invalid emu-results JSON: {s}\n", .{ tool, @errorName(e) });
                    return 1;
                };
            } else if (opts.emu_log_path) |lp| {
                const lb = dir.readFileAlloc(io, lp, allocator, .limited(corpus.max_json_bytes)) catch |e| {
                    try err.print("{s}: cannot read emu-log '{s}': {s}\n", .{ tool, lp, @errorName(e) });
                    return 2;
                };
                defer allocator.free(lb);
                const extracted = tpak.extractTraceFromLog(allocator, lb) catch |e| {
                    try err.print("{s}: invalid emu-log '{s}': {s}\n", .{ tool, lp, @errorName(e) });
                    return 1;
                };
                loaded_emu_bytes = extracted;
                parsed_emu = corpus.parseEmuTrace(allocator, extracted) catch |e| {
                    try err.print("{s}: invalid extracted trace JSON: {s}\n", .{ tool, @errorName(e) });
                    return 1;
                };
            } else if (opts.emu_bin_path) |bin_p| {
                parsed_emu = (try runEmulatorTarget(
                    allocator,
                    io,
                    dir,
                    parsed_corpus.value,
                    bin_p,
                    opts.emu_elf_path.?,
                    &loaded_emu_bytes,
                    err,
                )) orelse return 1;
            }

            var report = pipeline.evaluate(
                allocator,
                parsed_corpus.value,
                if (parsed_emu) |pe| pe.value else null,
            ) catch |e| {
                try err.print("{s}: evaluation failed: {s}\n", .{ tool, @errorName(e) });
                return 1;
            };
            defer report.deinit();

            if (opts.json_out) {
                try writeJson(report, out);
            } else {
                try writeTable(report, parsed_corpus.value, out);
            }
            return 0;
        },
    }
}
