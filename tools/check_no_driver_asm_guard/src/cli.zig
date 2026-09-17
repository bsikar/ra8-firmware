//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the HAL driver asm-guard gate
//! (#858).
//!
//! Exit 0 when every HAL peripheral driver routes its CPU primitives through
//! the shared seam, 1 on a violation, a failing detector selftest, a driver
//! directory that is not there, or a driver that cannot be read or decoded,
//! and 2 for any argument other than a lone `--selftest`.
//!
//! A missing driver directory exiting 1 rather than 0 is inherited and
//! deliberate: this gate has a single scan root, so the directory vanishing
//! means the tree moved under it, and the one thing it must never do is
//! report a clean sweep of somewhere that does not exist.
//!
//! `run` is parameterised on a directory handle, the repository root, the
//! scanned path and both streams, so every status above is provable in a test
//! with no process and no real repository.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_no_driver_asm_guard";

/// The scan root, relative to the repository root.
pub const Paths = struct {
    /// The HAL peripheral drivers, the only scope this gate governs.
    driver_dir: []const u8,

    /// The live path, as the gate runs it in CI.
    pub const default = Paths{ .driver_dir = "libs/ra8_hal/src" };
};

/// Resolve one repository-relative path against the root.
///
/// A root of `.` resolves to the path itself, so a gate run from the
/// repository root reports repository-relative paths and the launcher's
/// absolute root reports absolute ones.
fn resolve(allocator: std.mem.Allocator, repo_root: []const u8, rel: []const u8) ![]const u8 {
    if (repo_root.len == 0 or std.mem.eql(u8, repo_root, ".")) return rel;
    return std.fs.path.join(allocator, &.{ repo_root, rel });
}

/// Compare two driver names for the sorted sweep order.
fn beforeByName(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.lessThan(u8, left, right);
}

/// Every `*.c` entry directly inside the driver directory, sorted by name.
///
/// Entries are selected by name alone, which is what the predecessor's
/// `glob("*.c")` did: a dot-prefixed name is NOT hidden, and a DIRECTORY
/// whose name ends in `.c` is listed and then fails to read, which is a
/// status 1 rather than a silent skip.
fn listDrivers(allocator: std.mem.Allocator, dir: std.fs.Dir) ![][]const u8 {
    var names = std.ArrayList([]const u8).init(allocator);
    errdefer names.deinit();
    var walker = dir.iterate();
    while (try walker.next()) |entry| {
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        try names.append(try allocator.dupe(u8, entry.name));
    }
    const owned = try names.toOwnedSlice();
    std.mem.sort([]const u8, owned, {}, beforeByName);
    return owned;
}

/// Read one driver, rejecting an undecodable one the way strict UTF-8 did.
///
/// There is deliberately NO size ceiling. The predecessor called `read_text()`
/// with none, and a ceiling here does not truncate: `readToEndAlloc` fails
/// with `error.FileTooBig`, the caller turns any read error into "cannot read
/// driver", and the scan stops there. A driver past the ceiling would lose its
/// findings and take every driver sorted after it down with it, which is the
/// one thing a gate must never do quietly.
fn readSource(allocator: std.mem.Allocator, dir: std.fs.Dir, path: []const u8) ![]const u8 {
    var file = try dir.openFile(path, .{});
    defer file.close();
    const raw = try file.readToEndAlloc(allocator, std.math.maxInt(usize));
    if (!std.unicode.utf8ValidateSlice(raw)) return error.InvalidUtf8;
    return implementation.normalizeTerminators(allocator, raw);
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
    // One arena per run: the listing, the stripped lines and the rendered
    // report all live exactly as long as the run does.
    var arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    if (argv.len == 1 and std.mem.eql(u8, argv[0], "--selftest")) return selftest(allocator, out);
    if (argv.len != 0) {
        try err.print("usage: {s} [--selftest]\n", .{tool});
        return 2;
    }

    const dir_path = try resolve(allocator, repo_root, paths.driver_dir);
    var drivers_dir = dir.openDir(dir_path, .{ .iterate = true }) catch {
        try out.print("{s}: driver dir not found: {s}\n", .{ tool, dir_path });
        return 1;
    };
    defer drivers_dir.close();

    const names = try listDrivers(allocator, drivers_dir);

    var problems = std.ArrayList([]const u8).init(allocator);
    for (names) |name| {
        const read_path = try std.fs.path.join(allocator, &.{ dir_path, name });
        const rel = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ paths.driver_dir, name });
        const text = readSource(allocator, drivers_dir, name) catch {
            try err.print("{s}: cannot read driver: {s}\n", .{ tool, read_path });
            return 1;
        };
        for (try implementation.scanText(allocator, text)) |finding| {
            try problems.append(try implementation.renderFinding(allocator, rel, finding));
        }
    }

    if (problems.items.len != 0) {
        try out.print("{s}: a HAL driver guards bare asm on {s}:\n", .{ tool, implementation.off_target });
        for (problems.items) |problem| try out.print("  {s}\n", .{problem});
        try out.print("Fix at the root -- call the ra8_hw_* primitive from\n", .{});
        try out.print("  {s}\n", .{implementation.seam_header});
        try out.print("(add a new one there plus its host body in\n", .{});
        try out.print(" tests/mocks/src/ra8_host_asm_stub.c if it does not exist yet).\n", .{});
        return 1;
    }

    try out.print(
        "{s}: PASS -- {d} HAL driver TU(s) carry no {s}-guarded asm.\n",
        .{ tool, names.len, implementation.off_target },
    );
    return 0;
}

/// Prove both detector directions, printing one line per case.
pub fn selftest(allocator: std.mem.Allocator, out: anytype) !u8 {
    const cases = try implementation.selftestCases(allocator);
    var failures: usize = 0;
    for (cases) |case| {
        if (!case.passed) failures += 1;
        try out.print("  [{s}] {s}\n", .{ if (case.passed) "ok" else "FAIL", case.label });
    }
    if (failures != 0) {
        try out.print("{s} --selftest: {d} failure(s)\n", .{ tool, failures });
        return 1;
    }
    try out.print("{s} --selftest: all cases pass (both directions).\n", .{tool});
    return 0;
}
