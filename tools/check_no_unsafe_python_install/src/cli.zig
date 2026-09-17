//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the PEP 668 override gate (#858).
//!
//! Exit 0 clean, 1 findings (or a failing detector selftest, or an unreadable
//! source), 2 any argument other than a lone `--selftest`, a census that
//! could not be enumerated, or a census that collapsed. `run` is
//! parameterised on a directory handle, the repository root, the census, the
//! scope policy and both streams, so every status above is provable in a test
//! with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_no_unsafe_python_install";

/// Ceiling on a single scanned file, far above anything in this tree.
const max_file_bytes = 64 * 1024 * 1024;

/// Ceiling on the census output, far above `git ls-files` on this tree.
const max_census_bytes = 16 * 1024 * 1024;

/// Where the scanned file set comes from.
pub const Census = union(enum) {
    /// An enumeration supplied by the caller, as the tests do.
    provided: []const []const u8,
    /// `git ls-files --cached --others --exclude-standard -z` at the root,
    /// so a brand new untracked file is in scope the moment it is written.
    git,
};

/// Scope policy: the census floor and the source that must survive it.
pub const Policy = struct {
    /// Smallest census the gate will trust.
    floor: usize,
    /// Path the census must contain, the gate's own implementation.
    self_source: []const u8,

    /// The live policy, as the gate runs it in CI.
    pub const default = Policy{
        .floor = implementation.min_scoped_files,
        .self_source = implementation.self_source,
    };
};

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    census: Census,
    policy: Policy,
    out: anytype,
    err: anytype,
) !u8 {
    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) return selftest(allocator, out, err);
    if (argv.len != 0) {
        try err.print("usage: {s} [--selftest]\n", .{tool});
        return 2;
    }

    const rels = switch (census) {
        .provided => |given| given,
        .git => gitCensus(allocator, repo_root) catch |failure| {
            try err.print("cannot enumerate first-party files: {s}\n", .{@errorName(failure)});
            return 2;
        },
    };

    var tree = Tree{ .dir = dir, .root = repo_root };
    const scoped = try implementation.selectScoped(allocator, rels, tree.resolver(), policy.self_source);
    if (implementation.scopeCollapsed(
        scoped.len,
        policy.floor,
        implementation.containsPath(scoped, policy.self_source),
    )) {
        try err.print(
            "scope collapsed to {d} files; expected at least {d} including {s}\n",
            .{ scoped.len, policy.floor, policy.self_source },
        );
        return 2;
    }

    var findings = std.ArrayList([]const u8).init(allocator);
    defer findings.deinit();
    for (scoped) |rel| {
        const raw = tree.read(allocator, rel) catch {
            try err.print("{s}: cannot read {s}\n", .{ tool, rel });
            return 1;
        };
        defer allocator.free(raw);
        // A file that is not valid UTF-8 is a binary asset, not guidance:
        // the gate skipped it rather than reporting it, and still does.
        if (!std.unicode.utf8ValidateSlice(raw)) continue;
        const unified = try implementation.normalizeTerminators(allocator, raw);
        defer allocator.free(unified);
        const hits = try implementation.scanText(allocator, unified);
        defer allocator.free(hits);
        for (hits) |line| try findings.append(try implementation.renderFinding(allocator, rel, line));
    }

    if (findings.items.len != 0) {
        try err.print("unsafe system-Python package override found:\n", .{});
        for (findings.items) |finding| try err.print("  {s}\n", .{finding});
        try err.print("Create a venv and wire its interpreter/PATH explicitly.\n", .{});
        return 1;
    }
    try out.print("{s}: clean ({d} first-party files)\n", .{ tool, scoped.len });
    return 0;
}

/// Prove the detector fires on an override and stays quiet on isolated
/// installation guidance, in both directions.
fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
    const failures = try implementation.selftestFailures(allocator);
    defer allocator.free(failures);
    if (failures.len != 0) {
        for (failures) |label| try err.print("{s} --selftest: FAIL: {s}\n", .{ tool, label });
        return 1;
    }
    try out.print("{s} --selftest: PASS ({d} cases)\n", .{ tool, implementation.selftest_cases.len });
    return 0;
}

/// The repository as the gate sees it: one directory handle plus the root the
/// census paths are relative to.
const Tree = struct {
    dir: std.fs.Dir,
    root: []const u8,

    fn resolver(self: *const Tree) implementation.Resolver {
        return .{ .context = self, .is_file_fn = isFileThunk };
    }

    fn isFileThunk(context: *const anyopaque, rel: []const u8) bool {
        const self: *const Tree = @ptrCast(@alignCast(context));
        return self.isFile(rel);
    }

    fn isFile(self: *const Tree, rel: []const u8) bool {
        var buffer: [std.fs.max_path_bytes]u8 = undefined;
        const joined = std.fmt.bufPrint(&buffer, "{s}/{s}", .{ self.root, rel }) catch return false;
        const stat = if (std.fs.path.isAbsolute(joined))
            std.fs.cwd().statFile(joined) catch return false
        else
            self.dir.statFile(joined) catch return false;
        return stat.kind == .file;
    }

    fn read(self: *const Tree, allocator: std.mem.Allocator, rel: []const u8) ![]u8 {
        const joined = try std.fs.path.join(allocator, &.{ self.root, rel });
        defer allocator.free(joined);
        const file = if (std.fs.path.isAbsolute(joined))
            try std.fs.openFileAbsolute(joined, .{})
        else
            try self.dir.openFile(joined, .{});
        defer file.close();
        return file.readToEndAlloc(allocator, max_file_bytes);
    }
};

/// Every first-party path Git knows about, tracked or newly written.
///
/// The census is taken AT the repository root, never at the working
/// directory: the gate is invoked from wherever the launcher was called, and
/// a census of the wrong tree is the collapsed scope this gate rejects.
fn gitCensus(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &.{ "git", "ls-files", "--cached", "--others", "--exclude-standard", "-z" },
        .cwd = repo_root,
        .max_output_bytes = max_census_bytes,
    });
    defer allocator.free(result.stderr);
    errdefer allocator.free(result.stdout);
    switch (result.term) {
        .Exited => |code| if (code != 0) return error.CensusFailed,
        else => return error.CensusFailed,
    }
    // The census was decoded strictly, so invalid bytes are an enumeration
    // failure rather than a silently shortened file set.
    if (!std.unicode.utf8ValidateSlice(result.stdout)) return error.CensusNotUtf8;

    var rels = std.ArrayList([]const u8).init(allocator);
    errdefer rels.deinit();
    var parts = std.mem.splitScalar(u8, result.stdout, 0);
    while (parts.next()) |part| try rels.append(part);
    return rels.toOwnedSlice();
}
