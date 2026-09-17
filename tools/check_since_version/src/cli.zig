//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Command-line membrane for `check_since_version` (#858), replacing
//! `scripts/checks/check-since-version.py`, which this change deletes.
//!
//! The exit-status contract is inherited, not invented, and every status the
//! Python could produce is reproduced here:
//!
//!   0  no issues, or a selftest in which every assertion held
//!   1  a presence or value problem (the report goes to STDERR, as the
//!      Python's `print(..., file=sys.stderr)` did), a failing selftest, or a
//!      missing / non-semver `VERSION` file (the Python raised
//!      `SystemExit(msg)`, which CPython prints to stderr and exits 1 with)
//!   2  no arguments at all (the usage line), or a collapsed enumeration:
//!      `lint_targets.first_party_paths` called `sys.exit(2)` below its
//!      tracked floor, and `--all` inherits that
//!
//! `run` takes the directory paths resolve against, the value of
//! `RA8_REPO_ROOT` and both streams, so the whole contract is provable in a
//! temporary directory without spawning a process or reading the real
//! environment. `main` supplies the real cwd, environment and streams.

const std = @import("std");
const gate = @import("internal/root.zig");

pub const usage = "usage: check_since_version FILE [FILE ...] | --all";

pub const exit_ok: u8 = 0;
pub const exit_problems: u8 = 1;
pub const exit_usage: u8 = 2;

/// Largest file read, far above anything first-party in this tree. The
/// Python read whole files; a file past this is reported as unreadable
/// rather than silently half-checked.
pub const max_file_bytes: usize = 16 * 1024 * 1024;

/// Run the gate, returning the process exit status.
///
/// A compiled tool cannot use the Python's `__file__.parents[2]` trick to
/// find the repository, so the root resolves `--repo-root` > `RA8_REPO_ROOT`
/// > `dir`. The launcher runs from the repository root, so the last of those
/// is the ordinary path.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    repo_root_env: ?[]const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var arguments = std.ArrayList([]const u8).init(allocator);
    defer arguments.deinit();

    var repo_root_flag: ?[]const u8 = null;
    var selftest_requested = false;

    var index: usize = 1;
    while (index < argv.len) : (index += 1) {
        const argument = argv[index];
        if (std.mem.eql(u8, argument, "--selftest")) {
            // `"--selftest" in sys.argv[1:]`: position never mattered.
            selftest_requested = true;
            continue;
        }
        if (std.mem.eql(u8, argument, "--repo-root")) {
            if (index + 1 >= argv.len) {
                try stderr.print("{s}\n", .{usage});
                return exit_usage;
            }
            index += 1;
            repo_root_flag = argv[index];
            continue;
        }
        if (std.mem.startsWith(u8, argument, "--repo-root=")) {
            repo_root_flag = argument["--repo-root=".len..];
            continue;
        }
        try arguments.append(argument);
    }

    const root_path = repo_root_flag orelse repo_root_env orelse ".";
    var root = dir.openDir(root_path, .{ .iterate = true }) catch {
        try stderr.print(
            "{s}: cannot open repository root '{s}'\n",
            .{ gate.tool_name, root_path },
        );
        return exit_usage;
    };
    defer root.close();

    if (selftest_requested) return selftest(allocator, root, root_path, stdout, stderr);

    const project_version = switch (try readProjectVersion(allocator, root, root_path)) {
        .ok => |value| value,
        .failed => |message| {
            try stderr.print("{s}\n", .{message});
            return exit_problems;
        },
    };

    var paths = std.ArrayList([]const u8).init(allocator);
    defer paths.deinit();

    if (arguments.items.len > 0 and std.mem.eql(u8, arguments.items[0], "--all")) {
        switch (try firstPartyPaths(allocator, root, root_path)) {
            .ok => |relatives| {
                for (relatives) |relative| {
                    try paths.append(try std.fs.path.join(
                        allocator,
                        &[_][]const u8{ root_path, relative },
                    ));
                }
            },
            .failed => |message| {
                try stderr.print("{s}\n", .{message});
                return exit_usage;
            },
        }
    } else if (arguments.items.len > 0) {
        for (arguments.items) |argument| {
            // `pathlib.Path(p).resolve()`: absolute, symlinks followed. A
            // path that cannot be resolved cannot be a file either, and the
            // Python skipped it at `is_file()`.
            const resolved = dir.realpathAlloc(allocator, argument) catch continue;
            try paths.append(resolved);
        }
    } else {
        try stderr.print("{s}\n", .{usage});
        return exit_usage;
    }

    var problems = std.ArrayList([]const u8).init(allocator);
    defer problems.deinit();

    for (paths.items) |path| {
        const text = readFileIfRegular(allocator, dir, path) orelse continue;
        if (gate.isUnderLibInc(path)) {
            try gate.presenceProblems(allocator, path, text, &problems);
        }
        if (gate.hasSourceSuffix(path)) {
            try gate.valueProblems(allocator, path, text, project_version, &problems);
        }
    }

    if (problems.items.len > 0) {
        try gate.writeProblems(stderr, project_version, problems.items);
        return exit_problems;
    }
    return exit_ok;
}

/// A value, or the message the Python would have died with.
fn Outcome(comptime T: type) type {
    return union(enum) { ok: T, failed: []const u8 };
}

/// `read_project_version`, message for message.
fn readProjectVersion(
    allocator: std.mem.Allocator,
    root: std.fs.Dir,
    root_path: []const u8,
) !Outcome([]const u8) {
    const shown = try std.fs.path.join(allocator, &[_][]const u8{ root_path, "VERSION" });
    const stat = root.statFile("VERSION") catch {
        return .{ .failed = try std.fmt.allocPrint(
            allocator,
            "error: {s} missing -- create it with a single semver line",
            .{shown},
        ) };
    };
    if (stat.kind != .file) {
        return .{ .failed = try std.fmt.allocPrint(
            allocator,
            "error: {s} missing -- create it with a single semver line",
            .{shown},
        ) };
    }
    const raw = try root.readFileAlloc(allocator, "VERSION", max_file_bytes);
    const text = gate.pythonStrip(raw);
    if (!gate.isSemver(text)) {
        return .{ .failed = try std.fmt.allocPrint(
            allocator,
            "error: {s} content '{s}' is not semver MAJOR.MINOR.PATCH",
            .{ shown, text },
        ) };
    }
    return .{ .ok = text };
}

/// The derived first-party scope, from git itself.
///
/// `lint_targets._tracked()` shelled out to
/// `git ls-files -z --cached --others --exclude-standard`, so this does the
/// same rather than walking the tree: a hand-rolled walk would disagree with
/// git about ignored and untracked files, which is the drift the derived
/// scope exists to prevent.
fn firstPartyPaths(
    allocator: std.mem.Allocator,
    root: std.fs.Dir,
    root_path: []const u8,
) !Outcome([][]const u8) {
    const result = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &[_][]const u8{
            "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
        },
        .cwd = root_path,
        .max_output_bytes = 64 * 1024 * 1024,
    }) catch {
        return .{ .failed = try std.fmt.allocPrint(
            allocator,
            "{s}: FATAL -- `git ls-files` failed",
            .{gate.tool_name},
        ) };
    };
    const failed = switch (result.term) {
        .Exited => |code| code != 0,
        else => true,
    };
    if (failed) {
        return .{ .failed = try std.fmt.allocPrint(
            allocator,
            "{s}{s}: FATAL -- `git ls-files` failed",
            .{ result.stderr, gate.tool_name },
        ) };
    }

    var tracked: usize = 0;
    var kept = std.ArrayList([]const u8).init(allocator);
    errdefer kept.deinit();

    var entries = std.mem.splitScalar(u8, result.stdout, 0);
    while (entries.next()) |relative| {
        if (relative.len == 0) continue;
        // `(REPO_ROOT / rel).is_file()`: a path deleted in the working tree
        // is in the index but is not a lint target.
        const stat = root.statFile(relative) catch continue;
        if (stat.kind != .file) continue;
        tracked += 1;
        if (gate.inFirstPartyScope(relative)) try kept.append(relative);
    }

    if (tracked < gate.tracked_floor) {
        return .{ .failed = try std.fmt.allocPrint(
            allocator,
            "{s}: FATAL -- only {d} tracked path(s), floor is {d}. A collapsed " ++
                "enumeration reports a clean tree because it enumerated nothing.",
            .{ gate.tool_name, tracked, gate.tracked_floor },
        ) };
    }

    const owned = try kept.toOwnedSlice();
    std.mem.sort([]const u8, owned, {}, gate.lessThanByBytes);
    return .{ .ok = owned };
}

/// Read one file, or null when the Python's `is_file()` / `read_text()`
/// would have skipped it.
fn readFileIfRegular(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    path: []const u8,
) ?[]const u8 {
    const stat = dir.statFile(path) catch return null;
    if (stat.kind != .file) return null;
    return dir.readFileAlloc(allocator, path, max_file_bytes) catch null;
}

/// `selftest()`: both directions on both halves, plus the two scope
/// assertions #358 added.
///
/// The three text assertions no longer need a temporary directory, because
/// the checks they exercise are functions of text here. What they prove is
/// unchanged, and their labels are the Python's, so a CI log reads the same.
fn selftest(
    allocator: std.mem.Allocator,
    root: std.fs.Dir,
    root_path: []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    try stdout.print("{s} --selftest\n", .{gate.tool_name});

    var failures = std.ArrayList([]const u8).init(allocator);
    defer failures.deinit();

    const project_version = switch (try readProjectVersion(allocator, root, root_path)) {
        .ok => |value| value,
        .failed => |message| {
            try stderr.print("{s}\n", .{message});
            return exit_problems;
        },
    };

    var problems = std.ArrayList([]const u8).init(allocator);
    defer problems.deinit();

    try gate.valueProblems(allocator, "bad.c", "/** @since 9.9.9 */\n", project_version, &problems);
    try expect(stdout, problems.items.len > 0, "a wrong @since value fires", &failures);

    problems.clearRetainingCapacity();
    const good = try std.fmt.allocPrint(allocator, "/** @since {s} */\n", .{project_version});
    try gate.valueProblems(allocator, "good.c", good, project_version, &problems);
    try expect(
        stdout,
        problems.items.len == 0,
        "the correct @since value stays quiet",
        &failures,
    );

    problems.clearRetainingCapacity();
    try gate.presenceProblems(allocator, "decl.h", "ra8_err_t ra8_foo(void);\n", &problems);
    try expect(stdout, problems.items.len > 0, "a public decl missing @since fires", &failures);

    switch (try firstPartyPaths(allocator, root, root_path)) {
        .ok => |scope| {
            var has_tools = false;
            var has_soup = false;
            for (scope) |relative| {
                if (std.mem.startsWith(u8, relative, "tools/")) has_tools = true;
                if (std.mem.startsWith(u8, relative, "libs/third_party/") or
                    std.mem.startsWith(u8, relative, "apps/shared_libs/third_party/"))
                {
                    has_soup = true;
                }
            }
            try expect(
                stdout,
                has_tools,
                "tools/ is in scope (the scan-dir list omitted it before #358)",
                &failures,
            );
            try expect(stdout, !has_soup, "vendored SOUP stays out of scope", &failures);
        },
        .failed => |message| {
            try stderr.print("{s}\n", .{message});
            return exit_usage;
        },
    }

    return gate.writeSelftestVerdict(stdout, stderr, failures.items);
}

fn expect(
    stdout: anytype,
    held: bool,
    label: []const u8,
    failures: *std.ArrayList([]const u8),
) !void {
    try gate.writeExpectation(stdout, held, label);
    if (!held) try failures.append(label);
}
