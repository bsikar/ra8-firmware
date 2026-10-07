//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane and the exit-status contract of the GNU-attribute gate
//! (RA8FW-335). Everything here is parameterised on a directory handle, a
//! repo root and both output streams, so the whole contract is exercised by
//! `tests/cli_test.zig` against a temporary tree with no process spawning.
//!
//! EXIT CONTRACT, inherited unchanged from the Python predecessor:
//!   0  clean, or an argv list every element of which filtered out of scope,
//!      or a selftest whose cases all held
//!   1  at least one reportable `__attribute__`, or a failing selftest case
//!   2  ANY argument beginning with `-` other than a lone `--selftest`, or a
//!      whole-tree sweep below `implementation.file_floor`
//!
//! Findings, the summary and the clean line print on STDOUT (the predecessor
//! used bare `print()`); usage, the collapse line and the selftest failure
//! count print on STDERR.
//!
//! An argv file list is a deliberately NARROWED scope, not a collapsed one,
//! so the floor is enforced on the sweep only. An unreadable or undecodable
//! source is SKIPPED rather than reported, which is where the predecessor's
//! `except (OSError, UnicodeDecodeError): return findings` landed; that skip
//! is why the read below is UNCAPPED, since a size ceiling would turn a large
//! source into a silent pass.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const Finding = implementation.Finding;

pub const Outcome = struct {
    status: u8,
    /// Files that survived the scope filter and were actually read.
    scanned: usize = 0,
    findings: usize = 0,
};

/// One source, read and scanned. A read or decode failure answers "no
/// findings", exactly as inherited.
///
/// The read carries NO size ceiling, deliberately. A limited read does not
/// truncate at its limit, it fails with `error.StreamTooLong`, and that failure
/// lands in the same `catch` as a missing file: a source above any ceiling
/// would be reported as carrying no attribute without ever being read, while
/// still counting towards the floor. The predecessor's `read_text()` had no
/// ceiling either, so a ceiling here is a fail-OPEN divergence, not a guard.
fn scanOne(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    path: []const u8,
    stdout: *std.Io.Writer,
) !usize {
    const raw = dir.readFileAlloc(io, path, allocator, .unlimited) catch return 0;
    defer allocator.free(raw);
    if (!std.unicode.utf8ValidateSlice(raw)) return 0;
    const text = try implementation.normalizeTerminators(allocator, raw);
    defer allocator.free(text);

    var findings = try implementation.scanText(allocator, text);
    defer findings.deinit(allocator);
    for (findings.items) |finding| {
        try implementation.renderFinding(stdout, path, finding.line, finding.snippet);
    }
    return findings.items.len;
}

/// `discover()`: `Path(root).rglob("*<ext>")` for every root and extension.
///
/// Reproduced faithfully, quirks included: dot-prefixed directories and files
/// are NOT hidden from `pathlib` globbing, and a DIRECTORY whose name ends in
/// a scanned extension is yielded as a candidate (it then fails to read and
/// is skipped, while still counting towards the floor). Symlinked directories
/// are not descended into, matching `recurse_symlinks=False`.
pub fn discover(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    repo_root: []const u8,
) !std.ArrayList([]const u8) {
    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit(allocator);
    }
    for (implementation.roots) |root| {
        var opened = dir.openDir(io, root, .{ .iterate = true }) catch continue;
        defer opened.close(io);
        var walker = try opened.walk(allocator);
        defer walker.deinit();
        while (walker.next(io) catch null) |entry| {
            if (!implementation.hasScannedExt(entry.path)) continue;
            const joined = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ root, entry.path });
            var keep = true;
            if (try implementation.isBuildOutputPath(allocator, joined, repo_root)) keep = false;
            if (keep and implementation.isExemptPath(joined)) keep = false;
            if (keep) {
                try out.append(allocator, joined);
            } else {
                allocator.free(joined);
            }
        }
    }
    return out;
}

fn runSelftest(allocator: std.mem.Allocator, stdout: *std.Io.Writer, stderr: *std.Io.Writer) !u8 {
    const cases = try implementation.selftestCases(allocator);
    var failed: usize = 0;
    for (cases) |case| {
        if (!case.passed) failed += 1;
        try stdout.print("  [{s}] {s}\n", .{ if (case.passed) "ok" else "FAIL", case.label });
    }
    if (failed != 0) {
        try stderr.print("check_no_gnu_attribute --selftest: {d} failure(s)\n", .{failed});
        return 1;
    }
    try stdout.writeAll("check_no_gnu_attribute --selftest: all cases pass (both directions).\n");
    return 0;
}

/// The whole gate, minus the process.
pub fn run(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir: std.Io.Dir,
    repo_root: []const u8,
    args: []const []const u8,
    stdout: *std.Io.Writer,
    stderr: *std.Io.Writer,
) !Outcome {
    if (args.len == 1 and std.mem.eql(u8, args[0], "--selftest")) {
        return .{ .status = try runSelftest(allocator, stdout, stderr) };
    }
    for (args) |arg| {
        if (std.mem.startsWith(u8, arg, "-")) {
            try implementation.renderUsage(stderr);
            return .{ .status = 2 };
        }
    }

    const whole_tree = args.len == 0;
    var owned: std.ArrayList([]const u8) = .empty;
    defer {
        for (owned.items) |item| allocator.free(item);
        owned.deinit(allocator);
    }
    if (whole_tree) {
        var discovered = try discover(allocator, io, dir, repo_root);
        defer discovered.deinit(allocator);
        try owned.appendSlice(allocator, discovered.items);
    } else {
        for (args) |arg| try owned.append(allocator, try allocator.dupe(u8, arg));
    }

    if (whole_tree and owned.items.len < implementation.file_floor) {
        try implementation.renderCollapsed(stderr, owned.items.len);
        return .{ .status = 2 };
    }

    // `sorted(set(files))`.
    std.mem.sort([]const u8, owned.items, {}, implementation.pythonLessThan);

    var total: usize = 0;
    var scanned: usize = 0;
    var previous: ?[]const u8 = null;
    for (owned.items) |path| {
        if (previous) |seen| {
            if (std.mem.eql(u8, seen, path)) continue;
        }
        previous = path;
        if (!implementation.hasScannedExt(path)) continue;
        if (try implementation.isBuildOutputPath(allocator, path, repo_root)) continue;
        if (implementation.isExemptPath(path)) continue;
        scanned += 1;
        total += try scanOne(allocator, io, dir, path, stdout);
    }

    if (total != 0) {
        try implementation.renderSummary(stdout, total);
        return .{ .status = 1, .scanned = scanned, .findings = total };
    }
    try implementation.renderClean(stdout);
    return .{ .status = 0, .scanned = scanned };
}
