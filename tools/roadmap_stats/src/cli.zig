//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! argv membrane and exit contract of the `roadmap_stats` host tool (#858),
//! the half that touches argv, the environment and the file system. The
//! parser, the renderer and the marker substitution live in
//! `internal/root.zig` and never see any of those.
//!
//! The predecessor derived its default from `__file__.resolve().parents[2]`.
//! A compiled tool has no `__file__`, so the roadmap path resolves
//! `--roadmap` > (`--repo-root` > `RA8_REPO_ROOT` > cwd) + `docs/ROADMAP.md`,
//! exactly as the migrated tools in this epic do.
//!
//! Exit contract, inherited verbatim except for the one divergence recorded
//! on `RewriteError.EndBeforeBegin`:
//!
//!   0  the summary was already current (`unchanged (...)`), or it was
//!      rewritten (`rewrote summary (...)`), or `-h`.
//!   1  `--check` and the file would change, or the roadmap cannot be read
//!      as UTF-8 text (where CPython raised and exited 1 with a traceback).
//!   2  the roadmap path does not exist, or the text carries no usable
//!      BEGIN/END SUMMARY region, or argv is a usage error.
//!
//! Everything the tool prints goes to stderr; stdout stays empty in every
//! mode but `--help`.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const tool = implementation.tool;
pub const usage_line = "usage: " ++ tool ++ " [-h] [--check] [--roadmap ROADMAP] [--repo-root REPO_ROOT]";

/// `REPO_ROOT / "docs" / "ROADMAP.md"`, the predecessor's default target.
pub const default_roadmap_components = [_][]const u8{ "docs", "ROADMAP.md" };

/// A roadmap larger than this is not a document, it is an accident.
pub const max_file_bytes: usize = 16 * 1024 * 1024;

/// The parsed command line.
pub const Options = struct {
    check: bool = false,
    roadmap: ?[]const u8 = null,
    repo_root: ?[]const u8 = null,
};

/// argparse's own diagnostics, as the shapes this tool can produce.
pub const UsageKind = enum {
    unrecognized,
    ambiguous,
    expected_one_argument,
    ignored_explicit_argument,
};

/// One usage error plus the argument that caused it.
pub const UsageError = struct { kind: UsageKind, arg: []const u8 };

/// What argv asked for.
pub const Action = union(enum) {
    help,
    audit: Options,
    usage_error: UsageError,
};

/// The long options argparse knows, in definition order. Abbreviation is
/// resolved over exactly this set plus nothing else, so `--che` and `--ro`
/// resolve while `--r` is AMBIGUOUS between `--roadmap` and `--repo-root`.
pub const long_options = [_][]const u8{ "help", "check", "roadmap", "repo-root" };

const Resolved = enum { help, check, roadmap, repo_root, unknown, ambiguous };

/// argparse's `_get_option_tuples` prefix matching: an exact spelling wins
/// outright, otherwise a unique prefix resolves and a shared prefix is an
/// error rather than a silent pick.
pub fn resolveLong(name: []const u8) Resolved {
    if (name.len == 0) return .unknown;
    var matches: usize = 0;
    var last: usize = 0;
    for (long_options, 0..) |candidate, index| {
        if (std.mem.eql(u8, candidate, name)) return byIndex(index);
        if (std.mem.startsWith(u8, candidate, name)) {
            matches += 1;
            last = index;
        }
    }
    if (matches == 0) return .unknown;
    if (matches > 1) return .ambiguous;
    return byIndex(last);
}

fn byIndex(index: usize) Resolved {
    return switch (index) {
        0 => .help,
        1 => .check,
        2 => .roadmap,
        else => .repo_root,
    };
}

fn usage(kind: UsageKind, arg: []const u8) Action {
    return .{ .usage_error = .{ .kind = kind, .arg = arg } };
}

/// Whether argparse would read `arg` as a value rather than as an option.
fn looksLikeValue(arg: []const u8) bool {
    return arg.len < 2 or arg[0] != '-';
}

/// Parse the option list (argv WITHOUT the program name).
pub fn parseArgs(argv: []const []const u8) Action {
    var options = Options{};
    var positional_only = false;
    var index: usize = 0;

    while (index < argv.len) : (index += 1) {
        const arg = argv[index];

        // A positional argument is always an error: the predecessor declared
        // none, so argparse reported `unrecognized arguments`.
        if (positional_only or looksLikeValue(arg)) return usage(.unrecognized, arg);

        if (std.mem.eql(u8, arg, "--")) {
            positional_only = true;
            continue;
        }

        if (!std.mem.startsWith(u8, arg, "--")) {
            if (std.mem.eql(u8, arg, "-h")) return .help;
            return usage(.unrecognized, arg);
        }

        const body = arg[2..];
        const equals = std.mem.indexOfScalar(u8, body, '=');
        const name = if (equals) |at| body[0..at] else body;
        const inline_value: ?[]const u8 = if (equals) |at| body[at + 1 ..] else null;

        switch (resolveLong(name)) {
            .unknown => return usage(.unrecognized, arg),
            .ambiguous => return usage(.ambiguous, arg),
            .help => {
                if (inline_value != null) return usage(.ignored_explicit_argument, arg);
                return .help;
            },
            .check => {
                if (inline_value != null) return usage(.ignored_explicit_argument, arg);
                options.check = true;
            },
            .roadmap, .repo_root => |which| {
                var value: []const u8 = undefined;
                if (inline_value) |given| {
                    value = given;
                } else {
                    if (index + 1 >= argv.len or !looksLikeValue(argv[index + 1])) {
                        return usage(.expected_one_argument, arg);
                    }
                    index += 1;
                    value = argv[index];
                }
                if (which == .roadmap) options.roadmap = value else options.repo_root = value;
            },
        }
    }

    return .{ .audit = options };
}

fn printHelp(out: anytype) !void {
    try out.print("{s}\n\n", .{usage_line});
    try out.print("Refresh the summary of the closed historical HAL completion record.\n\n", .{});
    try out.print("options:\n", .{});
    try out.print("  -h, --help             show this help message and exit\n", .{});
    try out.print("  --check                check mode: exit 1 if the file would change\n", .{});
    try out.print("  --roadmap ROADMAP      path to ROADMAP.md (default: <repo root>/docs/ROADMAP.md)\n", .{});
    try out.print("  --repo-root REPO_ROOT  repository root (default: $RA8_REPO_ROOT, else the cwd)\n", .{});
}

fn printUsageError(err: anytype, failure: UsageError) !void {
    try err.print("{s}\n", .{usage_line});
    switch (failure.kind) {
        .unrecognized => try err.print(
            "{s}: error: unrecognized arguments: {s}\n",
            .{ tool, failure.arg },
        ),
        .ambiguous => try err.print(
            "{s}: error: ambiguous option: {s} could match --roadmap, --repo-root\n",
            .{ tool, failure.arg },
        ),
        .expected_one_argument => try err.print(
            "{s}: error: argument {s}: expected one argument\n",
            .{ tool, failure.arg },
        ),
        .ignored_explicit_argument => try err.print(
            "{s}: error: argument {s}: ignored explicit argument\n",
            .{ tool, failure.arg },
        ),
    }
}

/// `--roadmap` > (`--repo-root` > `RA8_REPO_ROOT` > cwd) + `docs/ROADMAP.md`.
pub fn resolveRoadmapPath(
    allocator: std.mem.Allocator,
    options: Options,
    repo_root_env: ?[]const u8,
) ![]u8 {
    if (options.roadmap) |explicit| return allocator.dupe(u8, explicit);
    const root = options.repo_root orelse repo_root_env orelse ".";
    return std.fs.path.join(allocator, &.{
        root,
        default_roadmap_components[0],
        default_roadmap_components[1],
    });
}

/// The whole job: read, parse, render, substitute, then either report the
/// summary current, refuse it as stale, or write it back.
pub fn audit(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    options: Options,
    repo_root_env: ?[]const u8,
    err: anytype,
) !u8 {
    const path = try resolveRoadmapPath(allocator, options, repo_root_env);
    defer allocator.free(path);

    const text = dir.readFileAlloc(allocator, path, max_file_bytes) catch |failure| switch (failure) {
        // `path.exists()` was False: the predecessor's own exit 2.
        error.FileNotFound, error.NotDir => {
            try err.print("{s}: not found: {s}\n", .{ tool, path });
            return 2;
        },
        // The path exists but is not readable text (a directory, a
        // permission denial). CPython raised here and exited 1 with a
        // traceback; one diagnostic line is the same status, read by a human.
        else => {
            try err.print("{s}: cannot read {s}: {s}\n", .{ tool, path, @errorName(failure) });
            return 1;
        },
    };
    defer allocator.free(text);

    if (!std.unicode.utf8ValidateSlice(text)) {
        try err.print("{s}: {s} is not valid UTF-8\n", .{ tool, path });
        return 1;
    }

    const stats = try implementation.parseRoadmap(allocator, text);
    const summary = try implementation.renderSummary(allocator, stats);
    defer allocator.free(summary);
    const census = try implementation.renderCensus(allocator, stats);
    defer allocator.free(census);

    const updated = implementation.rewrite(allocator, text, summary) catch |failure| switch (failure) {
        implementation.RewriteError.MissingMarkers => {
            try err.print(
                "{s}: ROADMAP.md is missing the BEGIN/END SUMMARY markers\n",
                .{tool},
            );
            return 2;
        },
        implementation.RewriteError.EndBeforeBegin => {
            try err.print(
                "{s}: ROADMAP.md has END SUMMARY before BEGIN SUMMARY; refusing to rewrite\n",
                .{tool},
            );
            return 2;
        },
        else => return failure,
    };
    defer allocator.free(updated);

    if (std.mem.eql(u8, updated, text)) {
        try err.print("{s}: unchanged {s}\n", .{ tool, census });
        return 0;
    }

    if (options.check) {
        try err.print(
            "{s}: ROADMAP.md summary is stale (run `just docs::record_stats` to refresh)\n",
            .{tool},
        );
        return 1;
    }

    dir.writeFile(.{ .sub_path = path, .data = updated }) catch |failure| {
        try err.print("{s}: cannot write {s}: {s}\n", .{ tool, path, @errorName(failure) });
        return 1;
    };
    try err.print("{s}: rewrote summary {s}\n", .{ tool, census });
    return 0;
}

/// Process-shaped entry point: argv as handed over (program name first),
/// the two streams, and the status to exit with.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    repo_root_env: ?[]const u8,
    out: anytype,
    err: anytype,
) !u8 {
    const tail = if (argv.len > 0) argv[1..] else argv;
    switch (parseArgs(tail)) {
        .help => {
            try printHelp(out);
            return 0;
        },
        .usage_error => |failure| {
            try printUsageError(err, failure);
            return 2;
        },
        .audit => |options| return audit(allocator, dir, options, repo_root_env, err),
    }
}
