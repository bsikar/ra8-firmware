//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the NSC veneer gate (#858).
//!
//! Exit 0 when every `RA8_NSC_VENEER` declared in the public header has a
//! definition in the `ra8_nsc` sources, 1 on a phantom veneer, a missing or
//! unreadable header, an unreadable source or a failing detector selftest,
//! and 2 for any argument other than a lone `--selftest`.
//!
//! `run` is parameterised on a directory handle, the repository root, the two
//! scanned paths and both streams, so every status above is provable in a
//! test with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_nsc_veneer_defs";

/// The two paths the gate reads, relative to the repository root.
pub const Paths = struct {
    /// The public header that advertises the non-secure entry points.
    header: []const u8,
    /// The directory whose `*.c` sources must define them.
    src_dir: []const u8,

    /// The live paths, as the gate runs them in CI.
    pub const default = Paths{
        .header = "libs/ra8_nsc/inc/ra8_nsc.h",
        .src_dir = "libs/ra8_nsc/src",
    };
};

/// One candidate source, read on demand and remembered.
const Source = struct {
    /// Path the source is read and reported at.
    path: []const u8,
    /// Contents once read; null until then.
    text: ?[]const u8 = null,
};

/// Resolve one repository-relative path against the root.
///
/// A root of `.` resolves to the path itself, so a gate run from the
/// repository root reports the same repository-relative paths the Python did,
/// and the launcher's absolute root reports absolute ones.
fn resolve(allocator: std.mem.Allocator, repo_root: []const u8, rel: []const u8) ![]const u8 {
    if (repo_root.len == 0 or std.mem.eql(u8, repo_root, ".")) return rel;
    return std.fs.path.join(allocator, &.{ repo_root, rel });
}

/// Run the gate. Returns the process exit status rather than calling exit.
pub fn run(
    caller_allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    paths: Paths,
    out: anytype,
    err: anytype,
) !u8 {
    // One arena per run: the declarations, the source texts and the rendered
    // report all live exactly as long as the run does.
    var arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) {
        return selftest(allocator, out, err);
    }
    if (argv.len != 0) {
        try err.print("usage: {s} [--selftest]\n", .{tool});
        return 2;
    }

    const header_path = try resolve(allocator, repo_root, paths.header);

    // A header that is not a regular file exits 1, never 0: with nothing to
    // parse there are zero declarations, and "zero declared, zero missing"
    // would pass while verifying nothing.
    const stat = dir.statFile(header_path) catch {
        try err.print("{s}: header not found: {s}\n", .{ tool, header_path });
        return 1;
    };
    if (stat.kind != .file) {
        try err.print("{s}: header not found: {s}\n", .{ tool, header_path });
        return 1;
    }

    const header_text = readText(allocator, dir, header_path) catch {
        try err.print("{s}: cannot read header: {s}\n", .{ tool, header_path });
        return 1;
    };

    const sources = try listSources(allocator, dir, repo_root, paths.src_dir);
    const veneers = try implementation.declaredVeneers(allocator, header_text);

    var missing = std.ArrayList([]const u8).init(allocator);
    for (veneers) |name| {
        var defined = false;
        // Sources are visited in sorted order and reading stops at the first
        // definition, which is the order the gate has always read them in.
        for (sources) |*source| {
            const text = source.text orelse blk: {
                const raw = readText(allocator, dir, source.path) catch {
                    try err.print("{s}: cannot read source: {s}\n", .{ tool, source.path });
                    return 1;
                };
                source.text = raw;
                break :blk raw;
            };
            if (implementation.definesVeneer(text, name)) {
                defined = true;
                break;
            }
        }
        if (!defined) try missing.append(name);
    }

    if (missing.items.len != 0) {
        try out.print("{s}: RA8_NSC_VENEER declared without a definition:\n", .{tool});
        for (missing.items) |name| {
            const line = try implementation.renderMissing(
                allocator,
                name,
                paths.header,
                paths.src_dir,
            );
            try out.print("{s}\n", .{line});
        }
        const fix_line = "Fix each at the root -- implement the veneer, or delete the declaration.";
        try out.print("{s}\n", .{fix_line});
        try out.print("A phantom NS->S entry point in the public header is a trust hazard.\n", .{});
        return 1;
    }

    try out.print(
        "{s}: PASS -- all {d} RA8_NSC_VENEER declaration(s) defined.\n",
        .{ tool, veneers.len },
    );
    return 0;
}

/// Read one file and reject an undecodable one.
///
/// The gate read text with universal newlines, so a CRLF source is translated
/// before scanning. Both terminators are whitespace to the matcher, so this
/// changes no verdict; it is done because the gate's meaning is "the text
/// Python saw", and a future line-numbered finding would depend on it.
fn readText(allocator: std.mem.Allocator, dir: std.fs.Dir, path: []const u8) ![]const u8 {
    // No constant ceiling: the Python called `read()` with no limit, so size
    // never decided whether a source was scanned. A ceiling turns the first
    // source past it into `cannot read` and hides every phantom veneer behind
    // it, so the allocation is bounded by the file itself.
    const raw = try dir.readFileAlloc(allocator, path, std.math.maxInt(usize));
    if (!std.unicode.utf8ValidateSlice(raw)) return error.InvalidUtf8;
    return normalizeTerminators(allocator, raw);
}

/// Collapse CRLF and a lone CR to LF, as text-mode reading did.
fn normalizeTerminators(allocator: std.mem.Allocator, raw: []const u8) ![]const u8 {
    if (std.mem.indexOfScalar(u8, raw, '\r') == null) return raw;
    var translated = try std.ArrayList(u8).initCapacity(allocator, raw.len);
    var index: usize = 0;
    while (index < raw.len) : (index += 1) {
        if (raw[index] == '\r') {
            translated.appendAssumeCapacity('\n');
            if (index + 1 < raw.len and raw[index + 1] == '\n') index += 1;
        } else {
            translated.appendAssumeCapacity(raw[index]);
        }
    }
    return translated.toOwnedSlice();
}

/// The `*.c` entries of the source directory, sorted by name.
///
/// A missing directory yields no sources rather than an error, matching the
/// glob the gate used: every declared veneer is then missing, which fails.
/// Dot-prefixed names stay in scope, because `pathlib.Path.glob` does not
/// hide them, and an entry that is not a readable file is left in the list so
/// that reading it fails loudly rather than being skipped.
fn listSources(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    src_dir: []const u8,
) ![]Source {
    var sources = std.ArrayList(Source).init(allocator);
    const dir_path = try resolve(allocator, repo_root, src_dir);
    var opened = dir.openDir(dir_path, .{ .iterate = true }) catch return sources.toOwnedSlice();
    defer opened.close();

    var names = std.ArrayList([]const u8).init(allocator);
    var walker = opened.iterate();
    while (try walker.next()) |entry| {
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        try names.append(try allocator.dupe(u8, entry.name));
    }
    std.mem.sort([]const u8, names.items, {}, lessThanByBytes);
    for (names.items) |name| {
        try sources.append(.{ .path = try std.fs.path.join(allocator, &.{ dir_path, name }) });
    }
    return sources.toOwnedSlice();
}

/// Byte order, which is the order `sorted()` put the globbed paths in.
fn lessThanByBytes(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.lessThan(u8, left, right);
}

/// Prove the detector fires on a phantom veneer and stays quiet on a defined
/// one, in both directions, before the gate is trusted on the real tree.
fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
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
