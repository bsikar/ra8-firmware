//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the pointer-only comment gate
//! (#858).
//!
//! Exit 0 clean, 1 findings or a failing detector selftest, 2 any argument
//! other than a lone `--selftest`, a census that could not be enumerated, a
//! source that could not be read or decoded, or a scope that collapsed.
//! `run` is parameterised on a directory handle, the repository root, the
//! census, the scope policy and both streams, so every status above is
//! provable in a test with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_pointer_boilerplate";

/// Ceiling on a single scanned source, far above anything in this tree.
const max_file_bytes = 64 * 1024 * 1024;

/// Ceiling on the census output, far above `git ls-files` on this tree.
const max_census_bytes = 16 * 1024 * 1024;

/// Where the scanned file set comes from.
pub const Census = union(enum) {
    /// An enumeration supplied by the caller, as the tests do.
    provided: []const []const u8,
    /// `git ls-files --cached --others --exclude-standard -z` at the root, so
    /// a brand new untracked source is in scope the moment it is written.
    git,
};

/// Scope policy: the floor the scoped set must clear.
pub const Policy = struct {
    floor: usize,

    /// The live policy, as the gate runs it in CI.
    pub const default = Policy{ .floor = implementation.min_scoped_files };
};

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    caller_allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    census: Census,
    policy: Policy,
    out: anytype,
    err: anytype,
) !u8 {
    // One arena per run: the scope, the census and the rendered findings all
    // live exactly as long as the run does, so nothing outlives this call and
    // no path through the status contract has to remember to free.
    var arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) return selftest(allocator, out, err);
    if (argv.len != 0) {
        try err.print("usage: {s} [--selftest]\n", .{tool});
        return 2;
    }

    const rels = switch (census) {
        .provided => |given| given,
        .git => gitCensus(allocator, repo_root) catch |failure| {
            try err.print("{s}: cannot scan source tree: {s}\n", .{ tool, @errorName(failure) });
            return 2;
        },
    };

    var tree = Tree{ .dir = dir, .root = repo_root };
    const scoped = try implementation.selectScoped(allocator, rels, tree.resolver());

    // Read before the floor is judged, exactly as the gate always has: a
    // source it cannot read or decode is a broken sweep, never a quiet pass.
    var findings = std.ArrayList([]const u8).init(allocator);
    defer findings.deinit();
    for (scoped) |rel| {
        const raw = tree.read(allocator, rel) catch {
            try err.print("{s}: cannot scan source tree: cannot read {s}\n", .{ tool, rel });
            return 2;
        };
        defer allocator.free(raw);
        // The gate decoded UTF-8 strictly, so an undecodable source stops the
        // sweep rather than being skipped as a binary asset.
        if (!std.unicode.utf8ValidateSlice(raw)) {
            try err.print("{s}: cannot scan source tree: {s} is not valid UTF-8\n", .{ tool, rel });
            return 2;
        }
        const hits = try implementation.scanText(allocator, raw);
        defer allocator.free(hits);
        for (hits) |line| try findings.append(try implementation.renderFinding(allocator, rel, line));
    }

    if (implementation.scopeCollapsed(scoped.len, policy.floor)) {
        try err.print(
            "{s}: scope collapsed to {d} file(s); expected at least {d}\n",
            .{ tool, scoped.len, policy.floor },
        );
        return 2;
    }

    if (findings.items.len != 0) {
        try err.print("Generated pointer-only definition comment(s):\n", .{});
        for (findings.items) |finding| try err.print("  {s}\n", .{finding});
        try err.print("Delete the comment; the declaration owns the contract.\n", .{});
        return 1;
    }
    try out.print("{s}: clean ({d} app/example source files)\n", .{ tool, scoped.len });
    return 0;
}

/// Prove the detector fires on the generated sentence and stays quiet on
/// legacy wording, an annotated note and a string literal.
fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
    const failures = try implementation.selftestFailures(allocator);
    defer allocator.free(failures);
    if (failures.len != 0) {
        for (failures) |label| try err.print("{s} --selftest: FAIL: {s}\n", .{ tool, label });
        return 1;
    }
    try out.print(
        "{s} --selftest: PASS ({d} both-direction cases)\n",
        .{ tool, implementation.selftest_cases.len },
    );
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

/// Every path Git knows about at the root, tracked or newly written.
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
