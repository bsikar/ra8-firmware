//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the trailing-newline gate (#858).
//!
//! Exit 0 when every scanned file ends in a newline, or when an argv-supplied
//! file list filtered to nothing; 1 when a file is missing its trailing
//! newline, or the selftest failed; 2 when the whole-tree sweep collapsed
//! below the floor, or the census could not be enumerated or fell below the
//! derived-scope floor. An empty argv list and an empty SWEEP are treated
//! differently on purpose: the pre-commit hook legitimately hands over a
//! staged list that filters to nothing, while nothing about this tree can
//! legitimately reduce the sweep to a handful of files.
//!
//! `run` is parameterised on a directory handle, the repository root, the
//! census, the scope policy and both streams, so every status above is
//! provable in a test with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_final_newline";

/// Ceiling on a single scanned file, far above anything in this tree.
const max_file_bytes = 64 * 1024 * 1024;

/// Ceiling on the census output, far above `git ls-files` on this tree.
const max_census_bytes = 16 * 1024 * 1024;

/// Where the whole-tree file set comes from.
pub const Census = union(enum) {
    /// An enumeration supplied by the caller, as the tests do.
    provided: []const []const u8,
    /// `git ls-files --cached --others --exclude-standard -z` at the root,
    /// so a newly written file is in scope the moment it lands.
    git,
};

/// Scope policy: the two floors a collapsed enumeration has to trip.
pub const Policy = struct {
    /// Smallest whole-tree sweep the gate will trust.
    file_floor: usize,
    /// Smallest census the derived scope will trust.
    tracked_floor: usize,

    /// The live policy, as the gate runs it in CI.
    pub const default = Policy{
        .file_floor = implementation.file_floor,
        .tracked_floor = implementation.tracked_floor,
    };
};

/// One scan target: the path to read, and the path to print about it.
const Target = struct {
    absolute: []const u8,
    display: []const u8,
};

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    gpa: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    census: Census,
    policy: Policy,
    out: anytype,
    err: anytype,
) !u8 {
    // One scan, one lifetime: the census, the joined paths and the findings
    // all live until the verdict is printed and die together with it.
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const allocator = arena.allocator();

    // `--selftest` anywhere in argv wins over the scan, exactly as the
    // membrane this replaces matched `"--selftest" in argv[1:]`.
    for (argv) |arg| {
        if (std.mem.eql(u8, arg, "--selftest")) {
            return selftest(allocator, dir, repo_root, census, policy, out, err);
        }
    }

    const tree = Tree{ .dir = dir, .root = repo_root };
    const targets = enumerateTargets(allocator, tree, argv, census, policy, err) catch |failure| {
        return switch (failure) {
            error.CensusCollapsed, error.CensusUnavailable => 2,
            else => failure,
        };
    };

    if (argv.len == 0 and targets.len < policy.file_floor) {
        try err.print(
            "{s}: FATAL -- only {d} file(s) in scope, floor is {d}. A collapsed " ++
                "sweep reports a clean tree because it scanned nothing.\n",
            .{ tool, targets.len, policy.file_floor },
        );
        return 2;
    }
    if (targets.len == 0) {
        try err.print("{s}: no files to scan\n", .{tool});
        return 0;
    }

    var missing = std.ArrayList([]const u8).init(allocator);
    for (targets) |target| {
        if (try tree.endsInNewline(allocator, target.absolute)) continue;
        try missing.append(target.display);
    }

    if (missing.items.len == 0) {
        try out.print("{s}: {d} file(s) scanned, all end in a newline.\n", .{ tool, targets.len });
        return 0;
    }

    implementation.sortPaths(missing.items);
    try err.print("{s}: {d} file(s) missing a trailing newline:\n\n", .{ tool, missing.items.len });
    for (missing.items) |path| try err.print("  {s}\n", .{path});
    try err.print("\nAdd a single newline at end of file.\n", .{});
    return 1;
}

/// The scan set: an argv list when one was given, the derived sweep otherwise.
fn enumerateTargets(
    allocator: std.mem.Allocator,
    tree: Tree,
    argv: []const []const u8,
    census: Census,
    policy: Policy,
    err: anytype,
) ![]const Target {
    var targets = std.ArrayList(Target).init(allocator);
    errdefer targets.deinit();

    if (argv.len != 0) {
        for (argv) |raw| {
            const absolute = if (std.fs.path.isAbsolute(raw))
                try allocator.dupe(u8, raw)
            else
                try std.fs.path.join(allocator, &.{ tree.root, raw });
            if (tree.isDir(absolute)) {
                try collectTree(allocator, tree, absolute, &targets);
                continue;
            }
            // A path that is not a source file is dropped silently, and a
            // path that does not exist is kept when its name says source:
            // the read fails, an unreadable file is not this gate's problem,
            // and it never reaches the missing list.
            if (implementation.isSource(absolute)) try appendTarget(allocator, tree, absolute, &targets);
        }
        return targets.toOwnedSlice();
    }

    const rels = switch (census) {
        .provided => |given| given,
        .git => tree.gitCensus(allocator) catch |failure| {
            try err.print("{s}: FATAL -- `git ls-files` failed ({s})\n", .{ tool, @errorName(failure) });
            return error.CensusUnavailable;
        },
    };
    if (rels.len < policy.tracked_floor) {
        try err.print(
            "{s}: FATAL -- only {d} tracked path(s), floor is {d}. A collapsed " ++
                "enumeration reports a clean tree because it enumerated nothing.\n",
            .{ tool, rels.len, policy.tracked_floor },
        );
        return error.CensusCollapsed;
    }

    const scope = try implementation.derivedScope(allocator, rels);
    for (scope) |rel| {
        const absolute = try std.fs.path.join(allocator, &.{ tree.root, rel });
        try appendTarget(allocator, tree, absolute, &targets);
    }
    return targets.toOwnedSlice();
}

/// Keep one path unless the gate's own subtraction drops it.
fn appendTarget(
    allocator: std.mem.Allocator,
    tree: Tree,
    absolute: []const u8,
    targets: *std.ArrayList(Target),
) !void {
    if (try implementation.isBuildOutputPath(allocator, absolute, tree.root)) return;
    if (implementation.hasExcludedFragment(absolute)) return;
    try targets.append(.{
        .absolute = absolute,
        .display = implementation.displayPath(absolute, tree.root),
    });
}

/// Every source file beneath an argv-supplied directory, at any depth.
fn collectTree(
    allocator: std.mem.Allocator,
    tree: Tree,
    root: []const u8,
    targets: *std.ArrayList(Target),
) !void {
    var opened = tree.openIterable(root) catch return;
    defer opened.close();
    var walker = try opened.walk(allocator);
    defer walker.deinit();
    while (try walker.next()) |entry| {
        if (entry.kind != .file) continue;
        const absolute = try std.fs.path.join(allocator, &.{ root, entry.path });
        if (!implementation.isSource(absolute)) continue;
        try appendTarget(allocator, tree, absolute, targets);
    }
}

/// Prove the detector fires on a missing newline and stays quiet on a good
/// file and an empty one, then prove the derived scope is real: it clears the
/// floor and it reaches the roots a hardcoded root list had dropped (#549).
fn selftest(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    census: Census,
    policy: Policy,
    out: anytype,
    err: anytype,
) !u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    const tree = Tree{ .dir = dir, .root = repo_root };

    var scratch = try Scratch.open(allocator);
    defer scratch.close();
    try scratch.write("good.py", "x = 1\n");
    try scratch.write("bad.py", "x = 1");
    try scratch.write("empty.py", "");
    try expect(
        try scratch.endsInNewline(allocator, "good.py"),
        "MUST NOT FIRE: a newline-terminated file",
        &failures,
        out,
    );
    try expect(
        try scratch.endsInNewline(allocator, "empty.py"),
        "MUST NOT FIRE: an empty file",
        &failures,
        out,
    );
    try expect(
        !(try scratch.endsInNewline(allocator, "bad.py")),
        "MUST FIRE: a file with no trailing newline",
        &failures,
        out,
    );

    const rels = switch (census) {
        .provided => |given| given,
        .git => tree.gitCensus(allocator) catch |failure| {
            try err.print("{s}: FATAL -- `git ls-files` failed ({s})\n", .{ tool, @errorName(failure) });
            return 1;
        },
    };
    const scope = try implementation.derivedScope(allocator, rels);
    var kept = std.ArrayList([]const u8).init(allocator);
    for (scope) |rel| {
        const absolute = try std.fs.path.join(allocator, &.{ tree.root, rel });
        if (try implementation.isBuildOutputPath(allocator, absolute, tree.root)) continue;
        if (implementation.hasExcludedFragment(absolute)) continue;
        try kept.append(rel);
    }

    try expect(
        kept.items.len >= policy.file_floor,
        try std.fmt.allocPrint(
            allocator,
            "derived scope sees {d} file(s) (floor {d})",
            .{ kept.items.len, policy.file_floor },
        ),
        &failures,
        out,
    );
    for ([_][]const u8{ "just", "infra" }) |root_name| {
        try expect(
            implementation.scopeReaches(kept.items, root_name),
            try std.fmt.allocPrint(
                allocator,
                "the derived scope reaches {s}/ (previously omitted)",
                .{root_name},
            ),
            &failures,
            out,
        );
    }

    if (failures.items.len != 0) {
        try err.print("\nSELFTEST FAILED: {d} assertion(s)\n", .{failures.items.len});
        for (failures.items) |label| try err.print("  {s}\n", .{label});
        return 1;
    }
    try out.print("selftest: all assertions held (both directions).\n", .{});
    return 0;
}

/// Record one selftest assertion and print its pass/fail line. Accumulates
/// rather than returning early, so one failure does not hide the rest.
fn expect(condition: bool, label: []const u8, failures: *std.ArrayList([]const u8), out: anytype) !void {
    try out.print("  [{s}] {s}\n", .{ if (condition) "ok" else "FAIL", label });
    if (!condition) try failures.append(label);
}

/// A throwaway directory the selftest writes its three probe files into.
const Scratch = struct {
    path: []const u8,
    dir: std.fs.Dir,
    allocator: std.mem.Allocator,

    fn open(allocator: std.mem.Allocator) !Scratch {
        const base = std.posix.getenv("TMPDIR") orelse "/tmp";
        const path = try std.fmt.allocPrint(allocator, "{s}/{s}-selftest-{x}", .{
            std.mem.trimRight(u8, base, "/"),
            tool,
            std.crypto.random.int(u64),
        });
        const dir = try std.fs.cwd().makeOpenPath(path, .{});
        return .{ .path = path, .dir = dir, .allocator = allocator };
    }

    fn write(self: *Scratch, name: []const u8, body: []const u8) !void {
        try self.dir.writeFile(.{ .sub_path = name, .data = body });
    }

    fn endsInNewline(self: *Scratch, allocator: std.mem.Allocator, name: []const u8) !bool {
        const file = self.dir.openFile(name, .{}) catch return true;
        defer file.close();
        const data = try file.readToEndAlloc(allocator, max_file_bytes);
        defer allocator.free(data);
        return implementation.endsInNewline(data);
    }

    fn close(self: *Scratch) void {
        self.dir.close();
        std.fs.cwd().deleteTree(self.path) catch {};
        self.allocator.free(self.path);
    }
};

/// The repository as the gate sees it: one directory handle plus the root the
/// census paths are relative to.
const Tree = struct {
    dir: std.fs.Dir,
    root: []const u8,

    fn isDir(self: Tree, absolute: []const u8) bool {
        const info = self.stat(absolute) orelse return false;
        return info.kind == .directory;
    }

    fn stat(self: Tree, absolute: []const u8) ?std.fs.File.Stat {
        if (std.fs.path.isAbsolute(absolute)) {
            return std.fs.cwd().statFile(absolute) catch return null;
        }
        return self.dir.statFile(absolute) catch return null;
    }

    fn openIterable(self: Tree, absolute: []const u8) !std.fs.Dir {
        if (std.fs.path.isAbsolute(absolute)) {
            return std.fs.openDirAbsolute(absolute, .{ .iterate = true });
        }
        return self.dir.openDir(absolute, .{ .iterate = true });
    }

    /// An unreadable file answers true: not this gate's problem, exactly as
    /// the `OSError` branch it replaces.
    fn endsInNewline(self: Tree, allocator: std.mem.Allocator, absolute: []const u8) !bool {
        const file = if (std.fs.path.isAbsolute(absolute))
            std.fs.openFileAbsolute(absolute, .{}) catch return true
        else
            self.dir.openFile(absolute, .{}) catch return true;
        defer file.close();
        const data = file.readToEndAlloc(allocator, max_file_bytes) catch return true;
        defer allocator.free(data);
        return implementation.endsInNewline(data);
    }

    /// Every path Git knows about, tracked or newly written, that exists as a
    /// file right now. `--cached` also prints paths deleted in the working
    /// tree; those are in the index but they are not scan targets, and
    /// handing one to the reader makes an ordinary deletion fail.
    fn gitCensus(self: Tree, allocator: std.mem.Allocator) ![][]const u8 {
        const result = try std.process.Child.run(.{
            .allocator = allocator,
            .argv = &.{ "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard" },
            .cwd = self.root,
            .max_output_bytes = max_census_bytes,
        });
        defer allocator.free(result.stderr);
        errdefer allocator.free(result.stdout);
        switch (result.term) {
            .Exited => |code| if (code != 0) return error.CensusFailed,
            else => return error.CensusFailed,
        }
        if (!std.unicode.utf8ValidateSlice(result.stdout)) return error.CensusNotUtf8;

        var rels = std.ArrayList([]const u8).init(allocator);
        errdefer rels.deinit();
        var parts = std.mem.splitScalar(u8, result.stdout, 0);
        while (parts.next()) |rel| {
            if (rel.len == 0) continue;
            const absolute = try std.fs.path.join(allocator, &.{ self.root, rel });
            const info = self.stat(absolute) orelse continue;
            if (info.kind != .file) continue;
            try rels.append(rel);
        }
        return rels.toOwnedSlice();
    }
};
