//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the session-reference gate (#858).
//!
//! Exit 0 when the tree is clean or the selftest held; 1 when a reference was
//! found or an assertion failed; 2 when the derived scope collapsed below the
//! floor, or the census could not be enumerated or fell below its own floor.
//! Both floors are kept, because they fail for different reasons: a census
//! that collapsed never saw the tree, and a scope that collapsed saw it and
//! filtered it away.
//!
//! `run` is parameterised on a directory handle, the repository root, the
//! census and both streams, so every status above is provable in a test with
//! no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = implementation.tool;

/// Ceiling on the census output, far above `git ls-files` on this tree.
const max_census_bytes = 16 * 1024 * 1024;

/// Ceiling on one scanned file. The predecessor's `read_text` had none, but a
/// refusal here is not a fail-open: an unreadable file was already skipped,
/// and nothing in the derived scope is anywhere near this size.
const max_file_bytes = 16 * 1024 * 1024;

/// Where the whole-tree file set comes from.
pub const Census = union(enum) {
    /// An enumeration supplied by the caller, as the tests do.
    provided: []const []const u8,
    /// `git ls-files --cached --others --exclude-standard -z` at the root,
    /// which is `lint_targets._tracked` verbatim.
    git,
};

/// The two floors a collapsed enumeration has to trip.
pub const Policy = struct {
    /// Smallest derived scope the gate will trust, `FILE_FLOOR`.
    file_floor: usize,
    /// Smallest census the derived scope will trust, `TRACKED_FLOOR`.
    tracked_floor: usize,

    /// The live policy, as the gate runs it in CI.
    pub const default = Policy{
        .file_floor = implementation.file_floor,
        .tracked_floor = implementation.tracked_floor,
    };
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
    // One scan, one lifetime: the census, the file texts and the findings all
    // live until the verdict is printed and die together with it.
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const allocator = arena.allocator();

    // `--selftest` anywhere in argv wins over the scan, exactly as the
    // membrane this replaces matched `"--selftest" in sys.argv[1:]`. No other
    // argument means anything to this gate, and an unknown one never did.
    for (argv) |arg| {
        if (std.mem.eql(u8, arg, "--selftest")) {
            return selftest(allocator, dir, repo_root, census, policy, out, err);
        }
    }

    const tree = Tree{ .dir = dir, .root = repo_root };
    const scope = resolveScope(allocator, tree, census, policy, err) catch |failure| {
        return switch (failure) {
            error.CensusCollapsed, error.CensusUnavailable => 2,
            else => failure,
        };
    };

    if (scope.len < policy.file_floor) {
        try err.print(
            "{s}: FATAL -- only {d} file(s) in scope, floor is {d}. A collapsed " ++
                "scope reports a clean tree because it scanned nothing.\n",
            .{ tool, scope.len, policy.file_floor },
        );
        return 2;
    }

    var findings = std.ArrayList(implementation.Finding).init(allocator);
    for (scope) |rel| {
        if (implementation.isSelfExempt(rel)) continue;
        const text = tree.readText(allocator, rel) orelse continue;
        try findings.appendSlice(try implementation.scanText(allocator, rel, text));
    }

    if (findings.items.len == 0) {
        try out.print("no-wave-refs: 0 violations -- gate clean.\n", .{});
        return 0;
    }

    try out.print("no-wave-refs: {d} violations found.\n", .{findings.items.len});
    const shown = @min(findings.items.len, implementation.max_findings_shown);
    for (findings.items[0..shown]) |finding| {
        const snippet = try implementation.renderSnippet(allocator, finding.snippet);
        try out.print("  {s}:{d} {s}\n", .{ finding.path, finding.line, snippet });
    }
    if (findings.items.len > implementation.max_findings_shown) {
        try out.print(
            "  ... {d} more (truncated)\n",
            .{findings.items.len - implementation.max_findings_shown},
        );
    }
    try out.print("\n", .{});
    try out.print("Per-line opt-out: append \"WAVE-OK: <reason>\" on the offending line.\n", .{});
    try out.print("Auto-fix helper: scripts/fix/fix_wave_references.py --apply\n", .{});
    return 1;
}

/// The derived scope, or the census failure that stopped it being taken.
fn resolveScope(
    allocator: std.mem.Allocator,
    tree: Tree,
    census: Census,
    policy: Policy,
    err: anytype,
) ![][]const u8 {
    const rels = switch (census) {
        .provided => |given| given,
        .git => tree.gitCensus(allocator) catch |failure| {
            try err.print(
                "{s}: FATAL -- `git ls-files` failed ({s})\n",
                .{ tool, @errorName(failure) },
            );
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
    return implementation.derivedScope(allocator, rels);
}

/// The five detector cases the predecessor carried, in both directions.
pub const detector_cases = [_]struct {
    text: []const u8,
    must_fire: bool,
    label: []const u8,
}{
    .{ .text = "fixed in Wave 70", .must_fire = true, .label = "MUST FIRE: \"Wave 70\"" },
    .{ .text = "see wave-43b for context", .must_fire = true, .label = "MUST FIRE: \"wave-43b\"" },
    .{ .text = "the sine wave is smooth", .must_fire = false, .label = "MUST NOT FIRE: \"sine wave\"" },
    .{
        .text = "k_ra8_pdg_wave_saw selects the waveform",
        .must_fire = false,
        .label = "MUST NOT FIRE: wave_saw / waveform",
    },
    .{
        .text = "wave_table[0] holds the sample",
        .must_fire = false,
        .label = "MUST NOT FIRE: wave_table identifier",
    },
};

/// Prove the detector fires and stays quiet, then prove the derived scope is
/// real: it clears the floor and it reaches the roots a hardcoded root list
/// had dropped (#549). A clean run over a scope that never sees those roots
/// proves nothing.
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

    for (detector_cases) |item| {
        try expect(implementation.firesWave(item.text) == item.must_fire, item.label, &failures, out);
    }

    const rels = switch (census) {
        .provided => |given| given,
        .git => tree.gitCensus(allocator) catch |failure| {
            try err.print(
                "{s}: FATAL -- `git ls-files` failed ({s})\n",
                .{ tool, @errorName(failure) },
            );
            return 1;
        },
    };
    const scope = try implementation.derivedScope(allocator, rels);

    try expect(
        scope.len >= policy.file_floor,
        try std.fmt.allocPrint(
            allocator,
            "derived scope sees {d} file(s) (floor {d})",
            .{ scope.len, policy.file_floor },
        ),
        &failures,
        out,
    );
    for ([_][]const u8{ "infra", "just" }) |root_name| {
        try expect(
            implementation.scopeReaches(scope, root_name),
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
/// rather than returning early, so one failure does not hide the rest:
/// `selftest_assert.expect`, whose output convention CI logs are read against.
fn expect(
    condition: bool,
    label: []const u8,
    failures: *std.ArrayList([]const u8),
    out: anytype,
) !void {
    try out.print("  [{s}] {s}\n", .{ if (condition) "ok" else "FAIL", label });
    if (!condition) try failures.append(label);
}

/// The repository as the gate sees it: one directory handle plus the root the
/// census paths are relative to.
const Tree = struct {
    dir: std.fs.Dir,
    root: []const u8,

    /// One file's text, or null when it is unreadable or not UTF-8. Both were
    /// caught and skipped by the predecessor's `except (OSError,
    /// UnicodeDecodeError)`, and a file this gate cannot decode is not one it
    /// can honestly report on.
    fn readText(self: Tree, allocator: std.mem.Allocator, rel: []const u8) ?[]const u8 {
        const path = std.fs.path.join(allocator, &.{ self.root, rel }) catch return null;
        const file = if (std.fs.path.isAbsolute(path))
            std.fs.openFileAbsolute(path, .{}) catch return null
        else
            self.dir.openFile(path, .{}) catch return null;
        defer file.close();
        const text = file.readToEndAlloc(allocator, max_file_bytes) catch return null;
        if (!std.unicode.utf8ValidateSlice(text)) return null;
        return text;
    }

    fn stat(self: Tree, path: []const u8) ?std.fs.File.Stat {
        if (std.fs.path.isAbsolute(path)) {
            return std.fs.cwd().statFile(path) catch return null;
        }
        return self.dir.statFile(path) catch return null;
    }

    /// `lint_targets._tracked`: every path Git knows about, tracked or newly
    /// written, that exists as a file right now. `--cached` also prints paths
    /// deleted in the working tree; those are in the index but they are not
    /// scan targets, and handing one to the reader makes an ordinary deletion
    /// look like a finding-free file.
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
            const path = try std.fs.path.join(allocator, &.{ self.root, rel });
            const info = self.stat(path) orelse continue;
            if (info.kind != .file) continue;
            try rels.append(rel);
        }
        return rels.toOwnedSlice();
    }
};
