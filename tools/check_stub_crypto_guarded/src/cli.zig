//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the stub-crypto gate (#858).
//!
//! Exit 0 when every listed stub translation unit guards its insecure
//! placeholder body fail-closed, 1 on any finding (a missing guard, an
//! `#else` that is not fail-closed, an insecure body that is absent from or
//! escapes the guard, a stub TU that is not there, or a source that cannot be
//! read or decoded), and 2 for any argument other than a lone `--selftest`.
//!
//! A stub TU that has gone missing staying a finding is inherited and
//! deliberate: the one thing this gate must never do is report a clean sweep
//! of a file it never read.
//!
//! `run` is parameterised on a directory handle, the repository root, the
//! governed TU list and both streams, so every status above is provable in a
//! test with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_stub_crypto_guarded";

/// One stub translation unit and its insecure signature, re-exported so a
/// caller can inject its own list without reaching past the membrane.
pub const Stub = implementation.Stub;

/// The governed TU list as the gate runs it in CI.
pub const default_stubs: []const implementation.Stub = &implementation.stub_tus;

/// Resolve one repository-relative path against the root.
///
/// A root of `.` resolves to the path itself, so a gate run from the
/// repository root reports repository-relative paths and the launcher's
/// absolute root reports absolute ones.
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

/// Read one stub TU whole.
///
/// There is deliberately NO size ceiling. The predecessor called `read_text()`
/// with none, and a ceiling here does not truncate: `readToEndAlloc` fails
/// with `error.FileTooBig`, the caller turns that into "cannot read stub TU"
/// and returns 1 without a finding, so a TU past the ceiling loses its
/// findings and takes every TU listed after it down with it. A gate that
/// cannot read a file must say what it failed to prove, never report less
/// than the predecessor did.
fn readSource(allocator: std.mem.Allocator, dir: std.fs.Dir, path: []const u8) ![]const u8 {
    var file = try dir.openFile(path, .{});
    defer file.close();
    return file.readToEndAlloc(allocator, std.math.maxInt(usize));
}

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    caller_allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    stubs: []const implementation.Stub,
    out: anytype,
    err: anytype,
) !u8 {
    // One arena per run: the read sources, the split lines and the rendered
    // findings all live exactly as long as the run does.
    var arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) return selftest(allocator, out, err);
    if (argv.len != 0) {
        try err.print("usage: {s} [--selftest]\n", .{tool});
        return 2;
    }

    var problems = std.ArrayList([]const u8).init(allocator);
    for (stubs) |stub| {
        const path = try resolve(allocator, repo_root, stub.rel);
        if (!isRegularFile(dir, path)) {
            try problems.append(try implementation.renderMissingFile(allocator, stub.rel));
            continue;
        }
        const raw = readSource(allocator, dir, path) catch {
            try err.print("{s}: cannot read stub TU: {s}\n", .{ tool, path });
            return 1;
        };
        if (!std.unicode.utf8ValidateSlice(raw)) {
            try err.print("{s}: cannot decode stub TU as UTF-8: {s}\n", .{ tool, path });
            return 1;
        }
        for (try implementation.checkText(allocator, stub.rel, stub.token, raw)) |problem| {
            try problems.append(problem);
        }
    }

    if (problems.items.len != 0) {
        try out.print("{s}: insecure placeholder crypto not guarded fail-closed:\n", .{tool});
        for (problems.items) |problem| try out.print("  {s}\n", .{problem});
        try out.print("Fix each at the root -- wrap the insecure body in\n", .{});
        try out.print("  {s}\n", .{implementation.guard_spelling});
        try out.print("and make the #else fail closed (return k_ra8_err_* / #error), or replace\n", .{});
        try out.print("the placeholder with a real crypto backend.\n", .{});
        return 1;
    }

    try out.print(
        "{s}: PASS -- {d} stub crypto TU(s) guarded fail-closed.\n",
        .{ tool, stubs.len },
    );
    return 0;
}

/// Prove both detector directions, printing one line per case.
pub fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
    const cases = try implementation.selftestCases(allocator);
    var failures: usize = 0;
    for (cases) |case| {
        if (!case.passed) failures += 1;
        try out.print("  [{s}] {s}\n", .{ if (case.passed) "ok" else "FAIL", case.label });
    }
    if (failures != 0) {
        try err.print("{s} --selftest: {d} failure(s)\n", .{ tool, failures });
        return 1;
    }
    try out.print("{s} --selftest: all cases pass (both directions).\n", .{tool});
    return 0;
}
