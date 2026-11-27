//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host CLI running `TMNT`/`TVOC`/`TSLT` translation via `libs/ra8_translate`.

const std = @import("std");
const json_out = @import("json_out.zig");
const runner = @import("runner.zig");

const max_tmnt_bytes: usize = 128 * 1024 * 1024;
const max_tvoc_bytes: usize = 16 * 1024 * 1024;
const max_json_bytes: usize = 4 * 1024 * 1024;

pub const usage_line =
    "usage: marian_zig_runner --tvoc PATH [--tmnt PATH | --tok-only] " ++
    "[--input-json PATH | --text UTF8] [--max-tgt N] [--top-k N] " ++
    "[--verify-payload] [--batch]";

const InputSpec = struct {
    sentences: []const []const u8,
    max_tgt_len: u16 = 24,
    top_k: usize = 5,
    verify_payload: bool = false,
    run_batch: bool = false,
};

const Options = struct {
    help: bool = false,
    tok_only: bool = false,
    tmnt_path: ?[]const u8 = null,
    tvoc_path: ?[]const u8 = null,
    input_json_path: ?[]const u8 = null,
    text: ?[]const u8 = null,
    max_tgt_len: ?u16 = null,
    top_k: ?usize = null,
    verify_payload: bool = false,
    run_batch: bool = false,
};

fn parseFlagValue(argv: []const []const u8, idx: *usize) ![]const u8 {
    if (idx.* + 1 >= argv.len) return error.InvalidArguments;
    idx.* += 1;
    return argv[idx.*];
}

fn parseArgs(argv: []const []const u8) !Options {
    var opts = Options{};
    var i: usize = 0;
    while (i < argv.len) : (i += 1) {
        const arg = argv[i];
        if (std.mem.eql(u8, arg, "-h") or std.mem.eql(u8, arg, "--help")) {
            opts.help = true;
            return opts;
        } else if (std.mem.eql(u8, arg, "--tok-only")) {
            opts.tok_only = true;
        } else if (std.mem.eql(u8, arg, "--tmnt")) {
            opts.tmnt_path = try parseFlagValue(argv, &i);
        } else if (std.mem.eql(u8, arg, "--tvoc")) {
            opts.tvoc_path = try parseFlagValue(argv, &i);
        } else if (std.mem.eql(u8, arg, "--input-json")) {
            opts.input_json_path = try parseFlagValue(argv, &i);
        } else if (std.mem.eql(u8, arg, "--text")) {
            opts.text = try parseFlagValue(argv, &i);
        } else if (std.mem.eql(u8, arg, "--max-tgt")) {
            opts.max_tgt_len = try std.fmt.parseInt(u16, try parseFlagValue(argv, &i), 10);
        } else if (std.mem.eql(u8, arg, "--top-k")) {
            opts.top_k = try std.fmt.parseInt(usize, try parseFlagValue(argv, &i), 10);
        } else if (std.mem.eql(u8, arg, "--verify-payload")) {
            opts.verify_payload = true;
        } else if (std.mem.eql(u8, arg, "--batch")) {
            opts.run_batch = true;
        } else {
            return error.InvalidArguments;
        }
    }
    return opts;
}

const LoadedInputs = struct {
    spec: InputSpec,
    parsed_json: ?std.json.Parsed(InputSpec) = null,
    raw_json: ?[]u8 = null,
    single_buf: [1][]const u8 = undefined,

    fn deinit(self: *LoadedInputs, allocator: std.mem.Allocator) void {
        if (self.parsed_json) |pj| pj.deinit();
        if (self.raw_json) |rj| allocator.free(rj);
    }
};

fn loadInputs(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    opts: Options,
    out: *LoadedInputs,
) !void {
    if (opts.input_json_path) |jp| {
        const jb = try dir.readFileAlloc(io, jp, allocator, .limited(max_json_bytes));
        out.raw_json = jb;
        out.parsed_json = try std.json.parseFromSlice(InputSpec, allocator, jb, .{
            .ignore_unknown_fields = false,
        });
        out.spec = out.parsed_json.?.value;
        if (opts.max_tgt_len) |m| out.spec.max_tgt_len = m;
        if (opts.top_k) |k| out.spec.top_k = k;
        out.spec.verify_payload = out.spec.verify_payload or opts.verify_payload;
        out.spec.run_batch = out.spec.run_batch or opts.run_batch;
    } else if (opts.text) |txt| {
        out.single_buf[0] = txt;
        out.spec = .{
            .sentences = &out.single_buf,
            .max_tgt_len = opts.max_tgt_len orelse 24,
            .top_k = opts.top_k orelse 5,
            .verify_payload = opts.verify_payload,
            .run_batch = opts.run_batch,
        };
    } else {
        return error.MissingInputText;
    }
}

fn executeRun(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    opts: Options,
    out: *std.Io.Writer,
) !u8 {
    const tvoc_path = opts.tvoc_path orelse return error.MissingTvocPath;
    const tvoc_bytes = try dir.readFileAlloc(io, tvoc_path, allocator, .limited(max_tvoc_bytes));
    defer allocator.free(tvoc_bytes);

    var inputs = LoadedInputs{ .spec = .{ .sentences = &.{} } };
    try loadInputs(allocator, io, dir, opts, &inputs);
    defer inputs.deinit(allocator);

    if (opts.tok_only) {
        var report = try runner.evaluateTokenizerOnly(allocator, tvoc_bytes, inputs.spec.sentences);
        defer report.deinit();
        try json_out.writeReport(out, report);
        return 0;
    }

    const tmnt_path = opts.tmnt_path orelse return error.MissingTmntPath;
    const tmnt_bytes = try dir.readFileAlloc(io, tmnt_path, allocator, .limited(max_tmnt_bytes));
    defer allocator.free(tmnt_bytes);

    var report = try runner.evaluateSentences(
        allocator,
        tmnt_bytes,
        tvoc_bytes,
        inputs.spec.sentences,
        inputs.spec.max_tgt_len,
        inputs.spec.top_k,
        inputs.spec.verify_payload,
        inputs.spec.run_batch,
    );
    defer report.deinit();
    try json_out.writeReport(out, report);
    return 0;
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*dst, s| dst.* = s;

    var stdout_buf: [8192]u8 = undefined;
    var stderr_buf: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buf);
    var stderr = std.Io.File.stderr().writer(io, &stderr_buf);

    const opts = parseArgs(if (argv.len > 1) argv[1..] else &.{}) catch |e| {
        try stderr.interface.print("marian_zig_runner: {s}\n{s}\n", .{ @errorName(e), usage_line });
        try stderr.interface.flush();
        return 2;
    };
    if (opts.help) {
        try stdout.interface.print("{s}\n", .{usage_line});
        try stdout.interface.flush();
        return 0;
    }

    const status = executeRun(allocator, io, std.Io.Dir.cwd(), opts, &stdout.interface) catch |e| {
        try stderr.interface.print("marian_zig_runner failed: {s}\n", .{@errorName(e)});
        try stderr.interface.flush();
        return 1;
    };
    try stdout.interface.flush();
    try stderr.interface.flush();
    return status;
}
