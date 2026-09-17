//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the obsolete-standards gate
//! (#858).
//!
//! Two scan modes, and NEITHER is the default: `--all` sweeps the tracked
//! first-party tree and is what CI runs, `--staged` reads the git index and
//! is what the commit hook runs. A bare invocation is an error, because this
//! checker once defaulted to `--staged` and so scanned ZERO files on every CI
//! run while printing a clean verdict.
//!
//! Exit 0 when the selected set is clean or the selftest held; 1 for a
//! finding or a failing selftest; 2 for a usage error, a missing mode, or an
//! enumeration that collapsed below either floor.
//!
//! `run` is parameterised on a directory handle, the repository root, the
//! census, the staged list and both streams, so every status above is
//! provable in a test with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = implementation.tool;

/// The usage line, as argparse derived it from the parser's options.
pub const usage_line = "usage: check_obsolete_standards [-h] [--all | --staged] [--selftest]";

/// Ceiling on one file read. A ceiling is safe here only because a read that
/// hits it FAILS rather than truncating, and a failed read is skipped exactly
/// as the predecessor's `OSError` branch skipped it.
const max_file_bytes = 16 * 1024 * 1024;

/// Ceiling on a census or index listing, far above this tree.
const max_census_bytes = 16 * 1024 * 1024;

/// Where the whole-tree file set comes from.
pub const Census = union(enum) {
    /// An enumeration supplied by the caller, as the tests do.
    provided: []const []const u8,
    /// `git ls-files --cached --others --exclude-standard -z` at the root.
    git,
};

/// Where the `--staged` file set comes from.
pub const Staged = union(enum) {
    provided: []const []const u8,
    /// `git diff --cached --name-only --diff-filter=ACMR`.
    git,
};

/// The two floors a collapsed enumeration has to trip.
pub const Policy = struct {
    /// Smallest `--all` sweep the gate will trust.
    tree_floor: usize,
    /// Smallest census the derived scope will trust.
    tracked_floor: usize,

    /// The live policy, as the gate runs it in CI.
    pub const default = Policy{
        .tree_floor = implementation.tree_floor,
        .tracked_floor = implementation.tracked_floor,
    };
};

/// What argv asked for. `usage_error` carries the detail argparse would have
/// printed after `error:`.
pub const Action = union(enum) {
    help,
    selftest,
    all,
    staged,
    none,
    usage_error: []const u8,
};

const long_options = [_][]const u8{ "help", "all", "staged", "selftest" };

/// `argparse.parse_args` for this parser, including prefix abbreviation.
///
/// `--se` resolves to `--selftest` and `--st` to `--staged`, while `--s` is
/// ambiguous between them and is an error, exactly as argparse resolves an
/// abbreviation against the set of long option strings. `--selftest` wins
/// over a mode because `main` tested `args.selftest` first.
pub fn parseArgs(argv: []const []const u8) Action {
    var want_help = false;
    var want_selftest = false;
    var want_all = false;
    var want_staged = false;
    var positional_only = false;

    for (argv) |arg| {
        if (positional_only) return .{ .usage_error = unrecognized(arg) };
        if (std.mem.eql(u8, arg, "--")) {
            positional_only = true;
            continue;
        }
        if (arg.len < 2 or arg[0] != '-') return .{ .usage_error = unrecognized(arg) };

        if (std.mem.startsWith(u8, arg, "--")) {
            const body = arg[2..];
            if (std.mem.indexOfScalar(u8, body, '=') != null) {
                return .{ .usage_error = "an option of this parser takes no value" };
            }
            var matched: ?[]const u8 = null;
            var matches: usize = 0;
            for (long_options) |name| {
                if (!std.mem.startsWith(u8, name, body)) continue;
                if (std.mem.eql(u8, name, body)) {
                    matched = name;
                    matches = 1;
                    break;
                }
                matched = name;
                matches += 1;
            }
            if (matches == 0) return .{ .usage_error = unrecognized(arg) };
            if (matches > 1) return .{ .usage_error = "ambiguous option" };
            const name = matched.?;
            if (std.mem.eql(u8, name, "help")) want_help = true;
            if (std.mem.eql(u8, name, "selftest")) want_selftest = true;
            if (std.mem.eql(u8, name, "all")) want_all = true;
            if (std.mem.eql(u8, name, "staged")) want_staged = true;
            continue;
        }

        // Short options: only -h exists, and no cluster is meaningful.
        if (std.mem.eql(u8, arg, "-h")) {
            want_help = true;
            continue;
        }
        return .{ .usage_error = unrecognized(arg) };
    }

    if (want_help) return .help;
    if (want_all and want_staged) {
        return .{ .usage_error = "argument --staged: not allowed with argument --all" };
    }
    if (want_selftest) return .selftest;
    if (want_all) return .all;
    if (want_staged) return .staged;
    return .none;
}

fn unrecognized(_: []const u8) []const u8 {
    return "unrecognized arguments";
}

/// Print the parser's help, as `-h` did.
fn printHelp(out: anytype) !void {
    try out.print("{s}\n\n", .{usage_line});
    try out.print("ban references to superseded safety standards\n\n", .{});
    try out.print("options:\n", .{});
    try out.print("  -h, --help  show this help message and exit\n", .{});
    try out.print("  --all       scan every tracked first-party file\n", .{});
    try out.print("  --staged    scan the git index (commit hook)\n", .{});
    try out.print("  --selftest  prove the detector both ways\n", .{});
}

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    gpa: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    census: Census,
    staged: Staged,
    policy: Policy,
    out: anytype,
    err: anytype,
) !u8 {
    // One scan, one lifetime: the census, the joined paths and the findings
    // all live until the verdict is printed and die together with it.
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const allocator = arena.allocator();

    const tree = Tree{ .dir = dir, .root = repo_root };

    switch (parseArgs(argv)) {
        .help => {
            try printHelp(out);
            return 0;
        },
        .usage_error => |detail| {
            try err.print("{s}\n{s}: error: {s}\n", .{ usage_line, tool, detail });
            return 2;
        },
        .selftest => return selftest(allocator, tree, census, policy, out, err),
        .none => {
            try implementation.renderNoMode(err);
            return 2;
        },
        .all => {
            const scope = treeScope(allocator, tree, census, policy, err) catch |failure| {
                return switch (failure) {
                    error.CensusCollapsed, error.CensusUnavailable => 2,
                    else => failure,
                };
            };
            if (scope.len < policy.tree_floor) {
                try implementation.renderFloorBreach(scope.len, policy.tree_floor, err);
                return 2;
            }
            return report(allocator, tree, scope, out);
        },
        .staged => {
            const names = switch (staged) {
                .provided => |given| given,
                .git => tree.stagedFiles(allocator) catch |failure| {
                    try err.print(
                        "{s}: FATAL -- `git diff --cached` failed ({s})\n",
                        .{ tool, @errorName(failure) },
                    );
                    return 2;
                },
            };
            return report(allocator, tree, names, out);
        },
    }
}

/// Scan `names`, print the verdict, and answer the status.
///
/// The count in the clean line is the SELECTED set, before the per-file
/// subtractions, exactly as the predecessor reported `len(names)`. That is
/// the honest number for a commit hook: it says how many paths were offered,
/// not how many survived the filter.
fn report(
    allocator: std.mem.Allocator,
    tree: Tree,
    names: []const []const u8,
    out: anytype,
) !u8 {
    var findings = std.ArrayList(implementation.Finding).init(allocator);

    for (names) |name| {
        if (!implementation.isScannable(name)) continue;
        const text = try tree.readFile(allocator, name) orelse continue;
        if (!std.unicode.utf8ValidateSlice(text)) continue; // UnicodeDecodeError
        const hits = try implementation.scanText(allocator, name, text);
        try findings.appendSlice(hits);
    }

    if (findings.items.len == 0) {
        try implementation.renderClean(names.len, out);
        return 0;
    }
    try implementation.renderFindings(findings.items, out);
    return 1;
}

/// The `--all` file list: the derived scope over the census, with the
/// census's own floor enforced first.
fn treeScope(
    allocator: std.mem.Allocator,
    tree: Tree,
    census: Census,
    policy: Policy,
    err: anytype,
) ![][]const u8 {
    const rels = switch (census) {
        .provided => |given| given,
        .git => tree.gitCensus(allocator) catch |failure| {
            try err.print("{s}: FATAL -- `git ls-files` failed ({s})\n", .{ tool, @errorName(failure) });
            return error.CensusUnavailable;
        },
    };
    if (rels.len < policy.tracked_floor) {
        try implementation.renderCensusCollapsed(rels.len, policy.tracked_floor, err);
        return error.CensusCollapsed;
    }
    return implementation.derivedScope(allocator, rels);
}

/// The selftest's detector fixtures, in the predecessor's order: label, text,
/// and whether the detector must fire on it.
pub const Case = struct { label: []const u8, text: []const u8, must_fire: bool };

/// Four cases, both directions. The two spellings must fire; the CURRENT
/// standard and an unrelated line naming no standard must stay quiet.
pub const cases = [_]Case{
    .{ .label = "a DO-178B citation", .text = "/* Written to DO-178B Level B. */", .must_fire = true },
    .{ .label = "the hyphen-less DO178B spelling", .text = "# targets DO178B objectives", .must_fire = true },
    .{ .label = "the current DO-178C citation", .text = "/* Written to DO-178C Level B. */", .must_fire = false },
    .{ .label = "an unrelated line naming no standard", .text = "int x = 178;", .must_fire = false },
};

/// True when every detector case agrees with its expectation. The tests use
/// this to assert the selftest cannot pass vacuously.
pub fn caseFailures() usize {
    var failures: usize = 0;
    for (cases) |case| {
        if (implementation.lineCitesObsolete(case.text) != case.must_fire) failures += 1;
    }
    return failures;
}

/// Prove the detector fires on both spellings and stays quiet otherwise, then
/// prove the tree-wide enumeration is real.
///
/// That last assertion is the point: the scan was silently reduced to zero
/// files for its whole life in CI, and a detector nobody has watched fire
/// over a real file list is indistinguishable from one that has stopped
/// looking.
fn selftest(
    allocator: std.mem.Allocator,
    tree: Tree,
    census: Census,
    policy: Policy,
    out: anytype,
    err: anytype,
) !u8 {
    var failures = std.ArrayList([]const u8).init(allocator);

    for (cases) |case| {
        const fired = implementation.lineCitesObsolete(case.text);
        const expectation = if (case.must_fire) "must fire" else "must stay quiet";
        const label = try std.fmt.allocPrint(allocator, "{s} ({s})", .{ case.label, expectation });
        try expect(fired == case.must_fire, label, &failures, out);
    }

    const scope = treeScope(allocator, tree, census, policy, err) catch |failure| {
        switch (failure) {
            error.CensusCollapsed, error.CensusUnavailable => return 1,
            else => return failure,
        }
    };
    try expect(
        scope.len >= policy.tree_floor,
        try std.fmt.allocPrint(
            allocator,
            "tree-wide enumeration sees {d} file(s) (floor {d})",
            .{ scope.len, policy.tree_floor },
        ),
        &failures,
        out,
    );

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

/// The repository as the gate sees it: one directory handle plus the root the
/// census paths are relative to.
const Tree = struct {
    dir: std.fs.Dir,
    root: []const u8,

    fn joined(self: Tree, allocator: std.mem.Allocator, rel: []const u8) ![]const u8 {
        if (std.fs.path.isAbsolute(rel)) return allocator.dupe(u8, rel);
        return std.fs.path.join(allocator, &.{ self.root, rel });
    }

    fn stat(self: Tree, absolute: []const u8) ?std.fs.File.Stat {
        if (std.fs.path.isAbsolute(absolute)) {
            return std.fs.cwd().statFile(absolute) catch return null;
        }
        return self.dir.statFile(absolute) catch return null;
    }

    /// `Path.read_text(encoding="utf-8")` behind `Path.is_file()`: null for
    /// anything that is not a readable regular file, which the predecessor
    /// skipped through either `is_file()` or its `OSError` branch.
    fn readFile(self: Tree, allocator: std.mem.Allocator, rel: []const u8) !?[]const u8 {
        const absolute = try self.joined(allocator, rel);
        const info = self.stat(absolute) orelse return null;
        if (info.kind != .file) return null;
        const file = if (std.fs.path.isAbsolute(absolute))
            std.fs.openFileAbsolute(absolute, .{}) catch return null
        else
            self.dir.openFile(absolute, .{}) catch return null;
        defer file.close();
        return file.readToEndAlloc(allocator, max_file_bytes) catch null;
    }

    /// Every path Git knows about, tracked or newly written, that exists as a
    /// file right now. `--cached` also prints paths deleted in the working
    /// tree; those are in the index but they are not scan targets.
    fn gitCensus(self: Tree, allocator: std.mem.Allocator) ![][]const u8 {
        const result = try std.process.Child.run(.{
            .allocator = allocator,
            .argv = &.{ "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard" },
            .cwd = self.root,
            .max_output_bytes = max_census_bytes,
        });
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
            const absolute = try self.joined(allocator, rel);
            const info = self.stat(absolute) orelse continue;
            if (info.kind != .file) continue;
            try rels.append(rel);
        }
        return rels.toOwnedSlice();
    }

    /// `git diff --cached --name-only --diff-filter=ACMR`, the index as the
    /// commit hook sees it. Deletions are filtered out by `ACMR` itself.
    fn stagedFiles(self: Tree, allocator: std.mem.Allocator) ![][]const u8 {
        const result = try std.process.Child.run(.{
            .allocator = allocator,
            .argv = &.{ "git", "diff", "--cached", "--name-only", "--diff-filter=ACMR" },
            .cwd = self.root,
            .max_output_bytes = max_census_bytes,
        });
        switch (result.term) {
            .Exited => |code| if (code != 0) return error.StagedListFailed,
            else => return error.StagedListFailed,
        }

        var names = std.ArrayList([]const u8).init(allocator);
        errdefer names.deinit();
        var lines = std.mem.splitScalar(u8, result.stdout, '\n');
        while (lines.next()) |line| {
            const name = std.mem.trimRight(u8, line, "\r");
            if (name.len == 0) continue;
            try names.append(name);
        }
        return names.toOwnedSlice();
    }
};
