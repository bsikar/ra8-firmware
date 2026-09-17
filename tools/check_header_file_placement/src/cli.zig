//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane and exit contract for `check_header_file_placement`
//! (#858, #1219). Everything that touches the file system or argv lives here;
//! the rules themselves are in `src/internal/root.zig`. The entry point is
//! parameterised on the directory to resolve against, the repository root and
//! both output streams, so the CLI tests drive it over a temporary tree
//! without spawning a process.

const std = @import("std");
pub const implementation = @import("internal/root.zig");

/// Argparse's own statuses, plus the predecessor's own `2` for
/// `--selftest` handed paths.
pub const usage_status: u8 = 2;

/// The argparse-rendered usage line, byte-for-byte, so a misspelled option
/// still fails the way the Python did.
pub const usage_line = "usage: check_header_file_placement.py [-h] [--selftest] [paths ...]\n";

pub const help_text = usage_line ++
    \\
    \\Gate: a header under a ``src/`` directory shall be module-private.
    \\
    \\positional arguments:
    \\  paths
    \\
    \\options:
    \\  -h, --help  show this help message and exit
    \\  --selftest
    \\
;

/// What `_parse_args` resolved argv into, or the argparse error it raised.
pub const Parsed = union(enum) {
    ok: struct { selftest: bool, paths: []const []const u8 },
    help,
    unrecognized: []const u8,
};

fn isLongOptionPrefixOf(candidate: []const u8, full: []const u8) bool {
    // argparse allows any unambiguous abbreviation of a long option, and
    // `--selftest` is the only long option besides `--help`, so a `--s...`
    // prefix is never ambiguous.
    return candidate.len >= 3 and std.mem.startsWith(u8, full, candidate);
}

/// Reproduce `_parse_args`: one store_true flag, a `paths` remainder, option
/// abbreviation, and `--` ending option parsing.
pub fn parseArgs(allocator: std.mem.Allocator, argv: []const []const u8) !Parsed {
    var paths = std.ArrayList([]const u8).init(allocator);
    errdefer paths.deinit();
    var selftest = false;
    var options_done = false;

    for (argv) |argument| {
        if (options_done) {
            try paths.append(argument);
            continue;
        }
        if (std.mem.eql(u8, argument, "--")) {
            options_done = true;
            continue;
        }
        if (argument.len >= 2 and std.mem.startsWith(u8, argument, "--")) {
            if (isLongOptionPrefixOf(argument, "--selftest")) {
                selftest = true;
                continue;
            }
            if (isLongOptionPrefixOf(argument, "--help")) return .help;
            return .{ .unrecognized = argument };
        }
        // A bare "-" is a positional to argparse; anything else starting with
        // a single dash is an unrecognised short option.
        if (argument.len > 1 and argument[0] == '-') {
            if (std.mem.eql(u8, argument, "-h")) return .help;
            return .{ .unrecognized = argument };
        }
        try paths.append(argument);
    }
    return .{ .ok = .{ .selftest = selftest, .paths = try paths.toOwnedSlice() } };
}

fn joinPath(allocator: std.mem.Allocator, left: []const u8, right: []const u8) ![]u8 {
    if (left.len == 0) return allocator.dupe(u8, right);
    const trimmed = std.mem.trimRight(u8, left, "/");
    return std.fmt.allocPrint(allocator, "{s}/{s}", .{ trimmed, right });
}

/// True when a DIRECTORY is out of scope, so the walk can stop at it. Every
/// descendant of an excluded directory is excluded too: the build-output rule
/// reads directory components only, and the vendored fragments are substrings
/// of every path beneath them. Pruning here is what keeps the sweep off the
/// build trees and `.git`, which `rglob` walked in full before discarding
/// them.
fn directoryExcluded(
    allocator: std.mem.Allocator,
    dir_path: []const u8,
    repo_root: []const u8,
) !bool {
    // The trailing component makes `dir_path` a directory component of the
    // probe, which is the only position the build-output rule examines.
    const probe = try std.fmt.allocPrint(allocator, "{s}/x", .{dir_path});
    defer allocator.free(probe);
    return implementation.isExcluded(allocator, probe, repo_root);
}

/// Walk `dir_path` recursively and append every entry whose suffix is a header
/// suffix, DIRECTORIES INCLUDED. `Path.rglob("*.h")` matches a directory named
/// `x.h`, and the predecessor did not filter those out, so one under a `src/`
/// tree is scanned and can be reported.
fn collectRecursive(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    dir_path: []const u8,
    repo_root: []const u8,
    out: *std.ArrayList([]const u8),
) !void {
    var handle = dir.openDir(dir_path, .{ .iterate = true }) catch return;
    defer handle.close();
    var iterator = handle.iterate();
    while (try iterator.next()) |entry| {
        const child = try joinPath(allocator, dir_path, entry.name);
        if (implementation.isHeader(child)) try out.append(child);
        if (entry.kind == .directory) {
            if (try directoryExcluded(allocator, child, repo_root)) continue;
            try collectRecursive(allocator, dir, child, repo_root, out);
        }
    }
}

/// Reproduce `_enumerate_targets`. With an explicit list, a directory is
/// expanded and a header-suffixed path is taken as given whether or not it
/// exists; everything else is dropped. With no list, the scan roots are walked.
pub fn enumerateTargets(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    paths: []const []const u8,
) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer out.deinit();

    if (paths.len != 0) {
        for (paths) |raw| {
            const resolved = if (raw.len != 0 and raw[0] == '/')
                try allocator.dupe(u8, raw)
            else
                try joinPath(allocator, repo_root, raw);
            const is_dir = blk: {
                var probe = dir.openDir(resolved, .{}) catch break :blk false;
                probe.close();
                break :blk true;
            };
            if (is_dir) {
                try collectRecursive(allocator, dir, resolved, repo_root, &out);
            } else if (implementation.isHeader(resolved)) {
                try out.append(resolved);
            }
        }
    } else {
        for (implementation.scan_roots) |root| {
            const resolved = try joinPath(allocator, repo_root, root);
            try collectRecursive(allocator, dir, resolved, repo_root, &out);
        }
    }

    var kept = std.ArrayList([]const u8).init(allocator);
    errdefer kept.deinit();
    for (out.items) |path| {
        if (!try implementation.isExcluded(allocator, path, repo_root)) {
            try kept.append(path);
        }
    }
    return kept.toOwnedSlice();
}

/// Build the predecessor's temporary fixture tree, run both directions over
/// it, and report every assertion that did not hold.
pub fn runSelftest(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    scratch_root: []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    defer failures.deinit();

    const fixtures = [_][]const u8{
        "tests/module/src/widget_internal.h",
        "tests/module/src/widget.h",
        "tests/module/src/sub/inc/public.h",
        "libs/third_party/vendor/src/public.h",
        "apps/shared_libs/third_party/vendor/src/public.h",
        "libs/ra8_fonts/src/generated.h",
        "CMakeFiles/src/generated.h",
    };
    var absolute: [fixtures.len][]const u8 = undefined;
    for (fixtures, 0..) |relative, index| {
        const full = try joinPath(allocator, scratch_root, relative);
        absolute[index] = full;
        if (std.fs.path.dirname(full)) |parent| try dir.makePath(parent);
        var file = try dir.createFile(full, .{ .truncate = true });
        defer file.close();
        try file.writeAll("#pragma once\n");
    }
    const good = absolute[0];
    const bad = absolute[1];
    const nested_public = absolute[2];

    // The fixture root is handed in as an absolute path, exactly as the
    // predecessor handed `str(root)` to `_enumerate_targets`, so exclusion and
    // relative rendering still run against the real repository root.
    const roots = [_][]const u8{scratch_root};
    const targets = try enumerateTargets(allocator, dir, repo_root, &roots);
    const mixed = try implementation.auditTargets(allocator, targets, repo_root);
    const bad_relative = implementation.relativeTo(bad, repo_root);
    const mixed_ok = mixed.scanned == 2 and mixed.offenders.len == 1 and
        std.mem.eql(u8, mixed.offenders[0], bad_relative);
    if (!mixed_ok) {
        try failures.append(try std.fmt.allocPrint(
            allocator,
            "mixed fixture scanned={d}, offenders={d}; expected bad only",
            .{ mixed.scanned, mixed.offenders.len },
        ));
    }

    const quiet_targets = [_][]const u8{ good, nested_public };
    const quiet = try implementation.auditTargets(allocator, &quiet_targets, repo_root);
    if (quiet.scanned != 1 or quiet.offenders.len != 0) {
        try failures.append(
            "private _internal.h or nearest nested inc/ did not stay quiet",
        );
    }

    var leaked = false;
    for (absolute[3..]) |excluded| {
        for (targets) |target| {
            if (std.mem.eql(u8, target, excluded)) leaked = true;
        }
    }
    if (leaked) {
        try failures.append(
            "vendor, generated-font, or build exclusion leaked into the scan",
        );
    }

    if (!try implementation.isExcluded(
        allocator,
        "tests/module/build/src/generated.h",
        repo_root,
    )) {
        try failures.append("tests/ build-tree output is not excluded");
    }

    var has_tests_root = false;
    for (implementation.scan_roots) |root| {
        if (std.mem.eql(u8, root, "tests")) has_tests_root = true;
    }
    if (!has_tests_root) {
        try failures.append(
            "tests/ is absent from the repository-wide scan roots",
        );
    }

    if (implementation.censusOk(implementation.min_private_headers - 1, false)) {
        try failures.append(
            "collapsed whole-tree private-header census did not fail",
        );
    }
    if (!implementation.censusOk(0, true)) {
        try failures.append(
            "explicit-file mode incorrectly requires the whole-tree floor",
        );
    }

    if (failures.items.len != 0) {
        for (failures.items) |failure| try implementation.renderSelftestFailure(stderr, failure);
        return 1;
    }
    try implementation.renderSelftestPass(stdout);
    return 0;
}

/// The whole gate, minus the process shell. `scratch_root` is where the
/// selftest builds its fixture tree.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    scratch_root: []const u8,
    argv: []const []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    const parsed = try parseArgs(allocator, argv);
    switch (parsed) {
        .help => {
            try stdout.writeAll(help_text);
            return 0;
        },
        .unrecognized => |argument| {
            try stderr.writeAll(usage_line);
            try stderr.print(
                "check_header_file_placement.py: error: unrecognized arguments: {s}\n",
                .{argument},
            );
            return usage_status;
        },
        .ok => |args| {
            if (args.selftest) {
                if (args.paths.len != 0) {
                    try implementation.renderSelftestWithPaths(stderr);
                    return usage_status;
                }
                return runSelftest(allocator, dir, repo_root, scratch_root, stdout, stderr);
            }

            const targets = try enumerateTargets(allocator, dir, repo_root, args.paths);
            if (targets.len == 0 and args.paths.len != 0) {
                try implementation.renderNoHeaders(stderr);
                return 0;
            }

            const audit = try implementation.auditTargets(allocator, targets, repo_root);
            if (!implementation.censusOk(audit.scanned, args.paths.len != 0)) {
                try implementation.renderCollapsed(stderr, audit.scanned);
                return 1;
            }
            if (audit.offenders.len == 0) {
                try implementation.renderClean(stdout, audit.scanned);
                return 0;
            }
            try implementation.renderOffenders(stderr, audit.offenders);
            return 1;
        },
    }
}
