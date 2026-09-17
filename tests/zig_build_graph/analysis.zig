//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Verification for the analysis inputs the root build graph emits (#1157).
//!
//! The graph has written `zig-out/analysis/compile_commands.json` since #959,
//! and it now carries a command for every translation unit the graph compiles.
//! Nothing ever proved one of those commands compiles the file it describes.
//!
//! `scripts/builders/build_cross_compile_db.py` holds a DERIVED command to
//! exactly this bar, in its own words: a derived command that does not compile
//! the file is a hard error there, so the fallback can never quietly hand
//! clang-tidy a wrong command and let a bogus parse read as a clean lint. The
//! rows here are assembled by `compileDbEntries()` in build.zig, a code path
//! SEPARATE from the compile steps that build the images, so a slice that
//! moves a flag on the compile step and forgets the database row leaves the
//! analysis gates parsing something the compiler never accepted -- and every
//! step stays green, because no step ran the row.
//!
//! What this module adds is the step that runs them. One compile per DISTINCT
//! command rather than per row: a row differs from its neighbour only in which
//! TU it names, and 2700 re-compiles would duplicate `zig build arm` rather
//! than test the database.

const std = @import("std");
const compile_db = @import("compile_db.zig");

/// The install directory and file name of the database, named here because
/// they are a CONTRACT with a consumer outside this build graph:
/// scripts/checks/check_unused_includes.py probes `install_path` for its
/// compile commands, and its selftest asserts these two strings still say
/// what it probes for. Moving either one without moving that probe empties
/// the lint gate instead of failing it.
pub const install_dir = "analysis";
pub const install_name = "compile_commands.json";
pub const install_path = "zig-out/" ++ install_dir ++ "/" ++ install_name;

/// Everything in an entry except the TU it names and where its object goes:
/// the driver, the flags in order, the include path in order, and the system
/// include path in order. Two entries with the same class are the same compile
/// command pointed at two different files, so one of them tests both.
///
/// Deliberately NOT compile_db.signature, which includes the file and is there
/// to tell two ROWS apart. This tells two COMMANDS apart.
pub fn commandClass(allocator: std.mem.Allocator, entry: compile_db.Entry) []const u8 {
    var out = std.ArrayList(u8).init(allocator);
    out.appendSlice(entry.driver) catch @panic("OOM");
    for (entry.flags) |flag| {
        out.appendSlice("\x00") catch @panic("OOM");
        out.appendSlice(flag) catch @panic("OOM");
    }
    for (entry.include_dirs) |include_dir| {
        out.appendSlice("\x00-I") catch @panic("OOM");
        out.appendSlice(include_dir) catch @panic("OOM");
    }
    for (entry.system_include_dirs) |include_dir| {
        out.appendSlice("\x00-isystem") catch @panic("OOM");
        out.appendSlice(include_dir) catch @panic("OOM");
    }
    return out.items;
}

/// One entry per distinct command class, the first of each, order preserved.
pub fn representatives(
    allocator: std.mem.Allocator,
    entries: []const compile_db.Entry,
) []const compile_db.Entry {
    var chosen = std.ArrayList(compile_db.Entry).init(allocator);
    var seen = std.StringHashMap(void).init(allocator);
    for (entries) |entry| {
        const key = commandClass(allocator, entry);
        if (seen.contains(key)) continue;
        seen.put(key, {}) catch @panic("OOM");
        chosen.append(entry) catch @panic("OOM");
    }
    return chosen.items;
}

/// Whether this step can run a command itself.
///
/// The cross rows name the toolchain by resolved absolute path, which is what
/// makes them runnable from anywhere. A bare program name is a row whose
/// driver the graph did not resolve (the host suites name `clang`, which the
/// graph does not invoke itself), and running it would test whichever compiler
/// happens to be on PATH rather than the database. Those are reported by name
/// in the step's own output, never skipped silently.
pub fn isRunnableDriver(driver: []const u8) bool {
    return std.fs.path.isAbsolute(driver);
}

/// The database's own argument vector with its output redirected, and nothing
/// else touched. The object path is the ONE field no analysis consumer parses
/// with, and writing objects for a verification run is not this step's job.
fn verificationArgv(b: *std.Build, entry: compile_db.Entry) []const []const u8 {
    const argv = compile_db.arguments(b, entry);
    std.debug.assert(argv.len >= 2);
    std.debug.assert(std.mem.eql(u8, argv[argv.len - 2], "-o"));
    const copy = b.allocator.dupe([]const u8, argv) catch @panic("OOM");
    copy[copy.len - 1] = "/dev/null";
    return copy;
}

/// Run one compile per distinct command and require exit 0, then report how
/// many classes were covered. Returns the number verified.
///
/// The runs are CHAINED rather than left to fan out: twenty cross-compiles
/// starting at once is a memory spike on a build host that is also compiling
/// the apps, and this step is a gate, not a race.
pub fn add(b: *std.Build, step: *std.Build.Step, entries: []const compile_db.Entry) usize {
    const chosen = representatives(b.allocator, entries);

    var verified: usize = 0;
    var unrunnable = std.ArrayList([]const u8).init(b.allocator);
    var previous: ?*std.Build.Step = null;

    for (chosen) |entry| {
        if (!isRunnableDriver(entry.driver)) {
            unrunnable.append(entry.driver) catch @panic("OOM");
            continue;
        }
        const run = b.addSystemCommand(verificationArgv(b, entry));
        run.expectExitCode(0);
        if (previous) |earlier| run.step.dependOn(earlier);
        previous = &run.step;
        step.dependOn(&run.step);
        verified += 1;
    }

    const report = b.addSystemCommand(&.{
        "printf",
        "analysis: %s of %s distinct compile commands verified, %s unrunnable here%s\n",
        b.fmt("{d}", .{verified}),
        b.fmt("{d}", .{chosen.len}),
        b.fmt("{d}", .{unrunnable.items.len}),
        if (unrunnable.items.len == 0)
            ""
        else
            b.fmt(" ({s})", .{std.mem.join(b.allocator, ", ", unrunnable.items) catch @panic("OOM")}),
    });
    if (previous) |earlier| report.step.dependOn(earlier);
    step.dependOn(&report.step);

    return verified;
}
