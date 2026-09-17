//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the redundant-cast gate (#858).
//!
//! Exit 0 clean, 1 findings (or an empty argv target set, or an unreadable
//! source), 2 unknown or incompatible arguments. `run` is parameterised on a
//! directory handle, the repository root and both streams, so every status
//! above is provable in a test with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_assert_casts";

/// Ceiling on a single scanned source, far above any file in this tree.
const max_source_bytes = 64 * 1024 * 1024;

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    out: anytype,
    err: anytype,
) !u8 {
    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) return selftest(allocator, out, err);

    var saw_all = false;
    var saw_unknown = false;
    for (argv) |arg| {
        if (std.mem.eql(u8, arg, "--all")) {
            saw_all = true;
            continue;
        }
        if (std.mem.startsWith(u8, arg, "-")) saw_unknown = true;
    }
    if (saw_unknown or (saw_all and argv.len != 1)) {
        try err.print("{s}: unknown or incompatible arguments\n", .{tool});
        return 2;
    }

    const paths = if (saw_all)
        try collectTestSources(allocator, dir, repo_root)
    else
        argv;

    // An empty target set exits 1 with usage, whichever way it emptied: an
    // argv list that filtered to nothing and a --all sweep that found no
    // tests tree are both "nothing was scanned", and a gate that scanned
    // nothing must never report a clean tree.
    if (paths.len == 0) {
        try err.print("usage: {s} <file> [...] or {s} --all\n", .{ tool, tool });
        return 1;
    }

    var rows = std.ArrayList([]const u8).init(allocator);
    defer rows.deinit();
    for (paths) |raw| {
        const shown = try implementation.normalizePath(allocator, raw);
        const bytes = readFileRelative(allocator, dir, raw) catch {
            try err.print("{s}: cannot read {s}\n", .{ tool, shown });
            return 1;
        };
        const unified = try implementation.normalizeTerminators(allocator, bytes);
        const decoded = try implementation.decodeAsciiReplace(allocator, unified);
        for (try implementation.scanText(allocator, decoded)) |finding| {
            try rows.append(try implementation.renderFinding(allocator, shown, finding));
        }
    }

    for (rows.items) |row| try out.print("{s}\n", .{row});
    if (rows.items.len != 0) {
        try err.print(
            "\n{d} redundant cast(s) in TEST_ASSERT_EQ.\nRun scripts/fix/strip_assert_casts.py to fix automatically.\n",
            .{rows.items.len},
        );
        return 1;
    }
    return 0;
}

/// Prove the detector fires on leading casts and stays quiet on clean code.
fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
    const fires = "TEST_ASSERT_EQ((int)value, (uint32_t)expected);\n";
    const quiet = "TEST_ASSERT_EQ(value, expected);\nTEST_ASSERT_EQ(load((int)value), expected);\n";
    const fired = try implementation.scanText(allocator, fires);
    defer allocator.free(fired);
    const stayed = try implementation.scanText(allocator, quiet);
    defer allocator.free(stayed);

    const cases = [_]struct { ok: bool, label: []const u8 }{
        .{ .ok = fired.len == 2, .label = "leading casts on both arguments fire" },
        .{ .ok = stayed.len == 0, .label = "clean and nested casts stay quiet" },
    };
    var failures: usize = 0;
    for (cases) |case| {
        if (!case.ok) failures += 1;
        const mark: []const u8 = if (case.ok) "ok" else "FAIL";
        try out.print("  [{s}] {s}\n", .{ mark, case.label });
    }
    if (failures != 0) {
        try err.print("{s} --selftest: {d} failure(s)\n", .{ tool, failures });
        return 1;
    }
    try out.print("{s} --selftest: all cases pass (both directions).\n", .{tool});
    return 0;
}

/// Every `*.c` under `<repo_root>/tests`, sorted, absolute-or-relative as the
/// root was given. A missing tests tree yields no paths rather than an error.
fn collectTestSources(allocator: std.mem.Allocator, dir: std.fs.Dir, repo_root: []const u8) ![][]const u8 {
    const root = try std.fs.path.join(allocator, &.{ repo_root, "tests" });
    var tests_dir = openDirRelative(dir, root) catch return allocator.alloc([]const u8, 0);
    defer tests_dir.close();

    var walker = try tests_dir.walk(allocator);
    defer walker.deinit();
    var found = std.ArrayList([]const u8).init(allocator);
    errdefer found.deinit();
    while (try walker.next()) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.basename, ".c")) continue;
        try found.append(try std.fs.path.join(allocator, &.{ root, entry.path }));
    }
    const items = try found.toOwnedSlice();
    std.mem.sort([]const u8, items, {}, lessThanPath);
    return items;
}

/// Order two paths the way `sorted()` over `pathlib.Path` objects did.
///
/// `Path.__lt__` compares the COMPONENT TUPLE, not the joined string, so a
/// separator never takes part in the comparison. Comparing joined bytes
/// instead reorders siblings whenever one directory name is a prefix of
/// another and the next character sorts below `/` (`a-b/x.c` vs `a/b.c`),
/// which would shuffle the finding rows this gate has always printed.
fn lessThanPath(_: void, left: []const u8, right: []const u8) bool {
    var left_parts = std.mem.splitScalar(u8, left, '/');
    var right_parts = std.mem.splitScalar(u8, right, '/');
    while (true) {
        const left_part = left_parts.next();
        const right_part = right_parts.next();
        if (left_part == null) return right_part != null;
        if (right_part == null) return false;
        switch (std.mem.order(u8, left_part.?, right_part.?)) {
            .lt => return true,
            .gt => return false,
            .eq => {},
        }
    }
}

fn openDirRelative(dir: std.fs.Dir, path: []const u8) !std.fs.Dir {
    if (std.fs.path.isAbsolute(path)) return std.fs.openDirAbsolute(path, .{ .iterate = true });
    return dir.openDir(path, .{ .iterate = true });
}

fn readFileRelative(allocator: std.mem.Allocator, dir: std.fs.Dir, path: []const u8) ![]u8 {
    const file = if (std.fs.path.isAbsolute(path))
        try std.fs.openFileAbsolute(path, .{})
    else
        try dir.openFile(path, .{});
    defer file.close();
    return file.readToEndAlloc(allocator, max_source_bytes);
}
