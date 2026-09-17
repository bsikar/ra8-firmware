//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the legacy-task-runner gate (#858).
//!
//! Exit 0 when no authored surface carries a command-shaped legacy task
//! invocation, 1 on a finding, a failing detector self-test, or a source that
//! cannot be read, and 2 for any argument other than a lone `--selftest`, a
//! census that cannot be enumerated, or a scope that has collapsed below its
//! floor.
//!
//! A collapsed scope staying an error is inherited and deliberate: a gate
//! that silently narrows to nothing reports a clean tree for exactly the
//! wrong reason.
//!
//! `run` is parameterised on a directory handle, the repository root, the
//! census and both streams, so every status above is provable in a test with
//! no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_no_legacy_make";

/// Ceiling on the census `git ls-files` may print.
const max_census_bytes = 64 * 1024 * 1024;

/// Where the scanned set comes from: a caller-supplied list in a test, or
/// the live tracked-and-untracked tree in CI.
pub const Census = union(enum) {
    provided: []const []const u8,
    git,
};

/// The floor the scope is judged against, and the source that must survive
/// selection. Injected so a test can prove the collapse without 650 files.
pub const Policy = struct {
    floor: usize = implementation.min_scoped_files,
    self_source: []const u8 = implementation.self_source,
};

/// Resolve one repository-relative path against the root.
fn resolve(allocator: std.mem.Allocator, repo_root: []const u8, rel: []const u8) ![]const u8 {
    if (repo_root.len == 0 or std.mem.eql(u8, repo_root, ".")) return rel;
    return std.fs.path.join(allocator, &.{ repo_root, rel });
}

/// Whether `path` is a regular file, as `pathlib.Path.is_file` answers it:
/// a missing path and a directory are both false, and symlinks are followed.
fn isRegularFile(dir: std.fs.Dir, path: []const u8) bool {
    const stat = dir.statFile(path) catch return false;
    return stat.kind == .file;
}

/// Enumerate the tracked and untracked tree the way the predecessor did.
fn gitCensus(allocator: std.mem.Allocator, repo_root: []const u8) ![]const []const u8 {
    const result = try std.process.Child.run(.{
        .allocator = allocator,
        .argv = &.{ "git", "ls-files", "--cached", "--others", "--exclude-standard", "-z" },
        .cwd = if (repo_root.len == 0) "." else repo_root,
        .max_output_bytes = max_census_bytes,
    });
    switch (result.term) {
        .Exited => |code| if (code != 0) return error.GitFailed,
        else => return error.GitFailed,
    }
    if (!std.unicode.utf8ValidateSlice(result.stdout)) return error.InvalidUtf8;
    var rels = std.ArrayList([]const u8).init(allocator);
    var parts = std.mem.splitScalar(u8, result.stdout, 0);
    while (parts.next()) |rel| {
        if (rel.len == 0) continue;
        try rels.append(rel);
    }
    return rels.toOwnedSlice();
}

fn lessThanBytes(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.lessThan(u8, left, right);
}

/// The authored surfaces the contract covers, sorted and de-duplicated.
fn scopedFiles(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    census: []const []const u8,
    policy: Policy,
) ![]const []const u8 {
    var seen = std.StringHashMap(void).init(allocator);
    var selected = std.ArrayList([]const u8).init(allocator);
    for (census) |rel| {
        if (rel.len == 0 or implementation.isExcluded(rel)) continue;
        const path = try resolve(allocator, repo_root, rel);
        if (!isRegularFile(dir, path)) continue;
        if (!implementation.isSelected(rel)) continue;
        if (seen.contains(rel)) continue;
        try seen.put(rel, {});
        try selected.append(rel);
    }
    // The gate may be validated before its own source is staged.
    if (!seen.contains(policy.self_source)) {
        const path = try resolve(allocator, repo_root, policy.self_source);
        if (isRegularFile(dir, path)) {
            try seen.put(policy.self_source, {});
            try selected.append(policy.self_source);
        }
    }
    std.mem.sort([]const u8, selected.items, {}, lessThanBytes);
    return selected.toOwnedSlice();
}

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
    var arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) {
        const failures = try implementation.selftestFailures(allocator);
        if (failures.len > 0) {
            for (failures) |label| {
                try err.print("{s} --selftest: FAIL: {s}\n", .{ tool, label });
            }
            return 1;
        }
        try out.print("{s} --selftest: PASS ({d} both-direction cases)\n", .{
            tool,
            implementation.selftest_cases.len,
        });
        return 0;
    }
    if (argv.len != 0) {
        try err.print("usage: {s} [--selftest]\n", .{tool});
        return 2;
    }

    const enumerated = switch (census) {
        .provided => |rels| rels,
        .git => gitCensus(allocator, repo_root) catch |failure| {
            try err.print("{s}: cannot enumerate tracked files: {s}\n", .{ tool, @errorName(failure) });
            return 2;
        },
    };
    const rels = try scopedFiles(allocator, dir, repo_root, enumerated, policy);

    var carries_self = false;
    for (rels) |rel| {
        if (std.mem.eql(u8, rel, policy.self_source)) carries_self = true;
    }
    if (rels.len < policy.floor or !carries_self) {
        try err.print(
            "{s}: scope collapsed to {d} file(s); expected at least {d} including {s}\n",
            .{ tool, rels.len, policy.floor, policy.self_source },
        );
        return 2;
    }

    var findings = std.ArrayList([]const u8).init(allocator);
    // One source is held at a time: the predecessor read each file, scanned
    // it and dropped it, and a gate that swept a large tree into memory
    // instead would fall over on exactly the trees it is there to police.
    var scratch_arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer scratch_arena.deinit();
    const scratch = scratch_arena.allocator();
    for (rels) |rel| {
        defer _ = scratch_arena.reset(.retain_capacity);
        const path = try resolve(scratch, repo_root, rel);
        // Read with no ceiling, as the predecessor's `read_text()` had none.
        // `readFileAlloc` does not truncate at a cap: it fails with
        // error.FileTooBig, and this caller turns any read failure into a
        // diagnostic and an immediate return, so a source above a ceiling
        // would lose its own findings and take every source sorted after it
        // down with it while still being counted as scanned.
        const file = dir.openFile(path, .{}) catch |failure| {
            try err.print("{s}: cannot read {s}: {s}\n", .{ tool, rel, @errorName(failure) });
            return 1;
        };
        defer file.close();
        const text = file.readToEndAlloc(scratch, std.math.maxInt(usize)) catch |failure| {
            try err.print("{s}: cannot read {s}: {s}\n", .{ tool, rel, @errorName(failure) });
            return 1;
        };
        // A source that does not decode is a binary asset, skipped exactly as
        // the predecessor skipped a UnicodeDecodeError.
        if (!std.unicode.utf8ValidateSlice(text)) continue;
        try implementation.scanText(allocator, scratch, rel, text, &findings);
    }

    if (findings.items.len > 0) {
        try err.print("{s}: legacy repository task references:\n", .{tool});
        for (findings.items) |finding| try err.print("  {s}\n", .{finding});
        try err.print("Use the authoritative namespaced Just recipe instead.\n", .{});
        return 1;
    }
    try out.print("{s}: clean ({d} authored files)\n", .{ tool, rels.len });
    return 0;
}
