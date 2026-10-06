//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! JSON compilation-database emitter for the root build graph.
//!
//! Extracted from build.zig unchanged in behaviour. The root build file is at
//! the 1000-line ceiling scripts/checks/check_file_size.py holds every Zig
//! source to, and this is the largest self-contained piece of it: the record
//! for one compile command, how that record becomes an argument vector, how two
//! records are told apart, and how the set is rendered and installed.
//!
//! What stays in build.zig is the part that cannot move: which translation
//! units this graph actually compiles, at which flag bars, which is the
//! per-slice knowledge the file exists to hold.

const std = @import("std");
const src_tree = @import("src_tree.zig");
const pkg_path = @import("pkg_path.zig");

/// One compile command: the TU, the driver that compiles it, the flags it is
/// really given, its include path in order, and where its object goes.
pub const Entry = struct {
    file: []const u8,
    driver: []const u8,
    flags: []const []const u8,
    include_dirs: []const []const u8,
    /// `-isystem` directories, which come after every `-I` on the command line
    /// and suppress the vendor headers' own diagnostics. Empty for every TU
    /// whose whole include path is first-party.
    system_include_dirs: []const []const u8 = &.{},
    object: []const u8,
};

/// The full argument vector for one entry, in compiler order: driver, flags,
/// include path, then the TU and its output. Absolute paths, as CMake writes
/// them, so a consumer that ignores the `directory` field still resolves.
pub fn arguments(b: *std.Build, entry: Entry) []const []const u8 {
    var argv: std.ArrayList([]const u8) = .empty;
    argv.append(b.allocator, entry.driver) catch @panic("OOM");
    for (entry.flags) |flag| argv.append(b.allocator, flag) catch @panic("OOM");
    for (entry.include_dirs) |include_dir| {
        argv.append(b.allocator, b.fmt("-I{s}", .{pkg_path.absolute(b, include_dir)})) catch @panic("OOM");
    }
    for (entry.system_include_dirs) |include_dir| {
        argv.append(b.allocator, "-isystem") catch @panic("OOM");
        argv.append(b.allocator, pkg_path.absolute(b, include_dir)) catch @panic("OOM");
    }
    argv.append(b.allocator, "-c") catch @panic("OOM");
    argv.append(b.allocator, pkg_path.absolute(b, entry.file)) catch @panic("OOM");
    argv.append(b.allocator, "-o") catch @panic("OOM");
    argv.append(b.allocator, entry.object) catch @panic("OOM");
    return argv.items;
}

/// Everything in an entry except its object path, joined. Two entries with the
/// same signature are the same compile command written twice: `ra8_scb.c` is
/// compiled into each of the three host suite modules identically, and one
/// command is what CMake's database would carry for it too. Two entries that
/// differ are a real difference and both stay -- which is how the vendored
/// slice's asymmetry survives into the database, `ra8_scb.c` appearing once at
/// the host bar and again at the stricter -Wconversion bar the SOUP drivers
/// take.
pub fn signature(b: *std.Build, entry: Entry) []const u8 {
    var out: std.ArrayList(u8) = .empty;
    out.appendSlice(b.allocator, entry.driver) catch @panic("OOM");
    out.appendSlice(b.allocator, "\x00") catch @panic("OOM");
    out.appendSlice(b.allocator, entry.file) catch @panic("OOM");
    for (entry.flags) |flag| {
        out.appendSlice(b.allocator, "\x00") catch @panic("OOM");
        out.appendSlice(b.allocator, flag) catch @panic("OOM");
    }
    for (entry.include_dirs) |include_dir| {
        out.appendSlice(b.allocator, "\x00") catch @panic("OOM");
        out.appendSlice(b.allocator, include_dir) catch @panic("OOM");
    }
    for (entry.system_include_dirs) |include_dir| {
        out.appendSlice(b.allocator, "\x00") catch @panic("OOM");
        out.appendSlice(b.allocator, include_dir) catch @panic("OOM");
    }
    return out.items;
}

pub fn appendJsonString(gpa: std.mem.Allocator, out: *std.ArrayList(u8), value: []const u8) void {
    out.append(gpa, '"') catch @panic("OOM");
    for (value) |byte| switch (byte) {
        '"' => out.appendSlice(gpa, "\\\"") catch @panic("OOM"),
        '\\' => out.appendSlice(gpa, "\\\\") catch @panic("OOM"),
        '\n' => out.appendSlice(gpa, "\\n") catch @panic("OOM"),
        '\t' => out.appendSlice(gpa, "\\t") catch @panic("OOM"),
        else => out.append(gpa, byte) catch @panic("OOM"),
    };
    out.append(gpa, '"') catch @panic("OOM");
}

/// Drop the entries that are the same compile command written twice, keeping
/// the first of each. Order is otherwise preserved: the database reads in the
/// order the graph compiles.
pub fn deduplicate(b: *std.Build, candidates: []const Entry) []const Entry {
    var entries: std.ArrayList(Entry) = .empty;
    var seen = std.StringHashMap(void).init(b.allocator);
    for (candidates) |entry| {
        const key = signature(b, entry);
        if (seen.contains(key)) continue;
        seen.put(key, {}) catch @panic("OOM");
        entries.append(b.allocator, entry) catch @panic("OOM");
    }
    return entries.items;
}

/// Render the deduplicated set as a standard JSON compilation database.
pub fn render(b: *std.Build, entries: []const Entry) []const u8 {
    const directory = src_tree.absolute(b, ".");

    var json: std.ArrayList(u8) = .empty;
    json.appendSlice(b.allocator, "[\n") catch @panic("OOM");
    for (entries, 0..) |entry, index| {
        json.appendSlice(b.allocator, "  {\n    \"directory\": ") catch @panic("OOM");
        appendJsonString(b.allocator, &json, directory);
        json.appendSlice(b.allocator, ",\n    \"file\": ") catch @panic("OOM");
        appendJsonString(b.allocator, &json, pkg_path.absolute(b, entry.file));
        json.appendSlice(b.allocator, ",\n    \"output\": ") catch @panic("OOM");
        appendJsonString(b.allocator, &json, entry.object);
        json.appendSlice(b.allocator, ",\n    \"arguments\": [") catch @panic("OOM");
        for (arguments(b, entry), 0..) |argument, argument_index| {
            if (argument_index != 0) json.appendSlice(b.allocator, ", ") catch @panic("OOM");
            appendJsonString(b.allocator, &json, argument);
        }
        json.appendSlice(b.allocator, "]\n  }") catch @panic("OOM");
        if (index + 1 != entries.len) json.append(b.allocator, ',') catch @panic("OOM");
        json.append(b.allocator, '\n') catch @panic("OOM");
    }
    json.appendSlice(b.allocator, "]\n") catch @panic("OOM");
    return json.items;
}

/// Wire the database into `step` and hand back how many commands it carries,
/// so `zig build parity` can print the count without rebuilding the list.
pub fn add(b: *std.Build, step: *std.Build.Step, candidates: []const Entry) usize {
    const entries = deduplicate(b, candidates);

    const written = b.addWriteFiles();
    const database = written.add("compile_commands.json", render(b, entries));
    const install = b.addInstallFileWithDir(
        database,
        .{ .custom = "analysis" },
        "compile_commands.json",
    );
    step.dependOn(&install.step);

    const report = b.addSystemCommand(&.{
        "printf",
        "compile-db: %s compile commands -> zig-out/analysis/compile_commands.json\n",
        b.fmt("{d}", .{entries.len}),
    });
    report.step.dependOn(&install.step);
    step.dependOn(&report.step);

    return entries.len;
}
