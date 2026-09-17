//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane and exit contract for `check_tz_boundary_discard` (#1250).
//! Everything that touches argv, the directory tree or a stream lives here;
//! the rules themselves are in `internal/root.zig`. Parameterised on a
//! directory handle, the working-directory text and both streams so the
//! contract is testable without a process.
//!
//! Exit contract, inherited verbatim from
//! `scripts/checks/check_tz_boundary_discard.py`:
//!
//!   0  clean, or an explicit path list with nothing to report, or a
//!      `--selftest` that held in both directions.
//!   1  one or more discards, or a failing selftest case.
//!   2  usage (any argument starting with `-` that is not a lone
//!      `--selftest`), and a whole-tree sweep below `file_floor`.
//!
//! No other status exists: the predecessor had none, and the launcher's own
//! exit 2 for an absent zig is the launcher's, not this tool's.

const std = @import("std");
pub const impl = @import("internal/root.zig");

/// Ceiling on one source file. The predecessor had none (`read_text` would
/// have read whatever was there), but a first-party C/C++ file above this is
/// not source, and an unbounded read in a gate is a denial-of-service.
pub const max_file_bytes: usize = 16 * 1024 * 1024;

pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    args: []const []const u8,
    out: anytype,
    error_out: anytype,
) !u8 {
    if (args.len == 1 and std.mem.eql(u8, args[0], "--selftest")) {
        const outcome = try impl.runSelftest(allocator);
        return impl.renderSelftest(out, error_out, outcome);
    }
    for (args) |arg| {
        if (std.mem.startsWith(u8, arg, "-")) {
            try impl.renderUsage(error_out);
            return 2;
        }
    }

    const whole_tree = args.len == 0;
    var files: [][]const u8 = undefined;
    if (whole_tree) {
        files = try discover(allocator, dir, repo_root);
    } else {
        files = try allocator.alloc([]const u8, args.len);
        for (args, 0..) |arg, index| files[index] = arg;
    }
    defer {
        // Only the swept paths are owned here; argv slices belong to the caller.
        if (whole_tree) for (files) |path| allocator.free(path);
        allocator.free(files);
    }

    if (whole_tree and files.len < impl.file_floor) {
        try impl.renderFloor(error_out, files.len);
        return 2;
    }

    std.mem.sort([]const u8, files, {}, impl.pythonLessThan);

    var total: usize = 0;
    var previous: ?[]const u8 = null;
    for (files) |path| {
        if (previous) |seen| {
            if (std.mem.eql(u8, seen, path)) continue; // `set(files)`
        }
        previous = path;
        if (!impl.hasScannedExt(path) or
            impl.isBuildOutputPath(path, repo_root) or
            impl.isExempt(path)) continue;

        const text = dir.readFileAlloc(allocator, path, max_file_bytes) catch continue;
        defer allocator.free(text);
        if (!std.unicode.utf8ValidateSlice(text)) continue; // UnicodeDecodeError

        const findings = try impl.scanText(allocator, text, impl.isCFile(path));
        defer allocator.free(findings);
        for (findings) |finding| {
            try impl.renderFinding(out, path, finding);
            total += 1;
        }
    }

    if (total != 0) {
        try impl.renderSummary(out, total);
        return 1;
    }
    try impl.renderClean(out);
    return 0;
}

/// `discover()`: every boot-boundary source under the roots, as
/// `Path(root).rglob("*<ext>")` enumerated them (hidden files and hidden
/// directories included, symlinked directories not followed), each already
/// filtered for build output and the exempt trees.
pub fn discover(allocator: std.mem.Allocator, dir: std.fs.Dir, repo_root: []const u8) ![][]const u8 {
    var found = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (found.items) |item| allocator.free(item);
        found.deinit();
    }
    for (impl.roots) |root| {
        var tree = dir.openDir(root, .{ .iterate = true }) catch continue;
        defer tree.close();
        var walker = try tree.walk(allocator);
        defer walker.deinit();
        while (try walker.next()) |entry| {
            if (entry.kind != .file) continue;
            if (!impl.hasScannedExt(entry.basename)) continue;
            const path = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ root, entry.path });
            if (impl.isBuildOutputPath(path, repo_root) or impl.isExempt(path)) {
                allocator.free(path);
                continue;
            }
            try found.append(path);
        }
    }
    return found.toOwnedSlice();
}
