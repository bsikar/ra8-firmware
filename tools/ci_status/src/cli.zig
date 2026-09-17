//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane and exit contract of the ci-monitor status reader
//! (#858, #1144).
//!
//! `run` is parameterised on the directory the state file is resolved
//! against and on both output streams, so every status below is provable in a
//! test without a process, a real repository or a real monitor.
//!
//! EXIT CONTRACT, inherited from the Python this replaced:
//!
//!   0  a handled mode printed its answer
//!   1  an unknown mode (`unknown mode: <mode>` on stderr), fewer than two
//!      arguments, or a state file that cannot be read or parsed
//!
//! There is no exit 2 anywhere in this tool, because the Python had none: it
//! raised for a bad path and for a malformed document, and `sys.exit(str)`
//! for a bad mode, all of which leave status 1. Tracebacks are replaced by
//! one stderr line each; the statuses are unchanged.
//!
//! Arguments past the third are IGNORED, exactly as `path, mode, *rest` with
//! `rest[0]` ignored the rest.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const Value = implementation.Value;

/// Largest state file read. The monitor writes one poll of workflow runs, so
/// this is orders of magnitude above the real document and exists only so a
/// corrupt path cannot ask for an unbounded allocation.
pub const max_state_bytes: usize = 16 * 1024 * 1024;

const usage = "ci_status: usage: ci_status <state-file> <mode> [arg]\n";

fn shapeMessage(err: implementation.ShapeError) []const u8 {
    return switch (err) {
        error.DocumentNotAnObject => "state file is not a JSON object",
        error.RunsNotAList => "\"runs\" is not a list",
        error.RunNotAnObject => "a \"runs\" entry is not an object",
        error.NameNotHashable => "a run's \"name\" is not a scalar",
    };
}

/// Dispatch one read mode against the state file named in `argv[0]`.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    out: anytype,
    err: anytype,
) !u8 {
    if (argv.len < 2) {
        try err.writeAll(usage);
        return 1;
    }
    const path = argv[0];
    const mode = argv[1];
    const arg: []const u8 = if (argv.len > 2) argv[2] else "";

    const text = dir.readFileAlloc(allocator, path, max_state_bytes) catch |read_err| {
        try err.print("ci_status: cannot read {s}: {s}\n", .{ path, @errorName(read_err) });
        return 1;
    };
    defer allocator.free(text);

    var parsed = std.json.parseFromSlice(Value, allocator, text, .{}) catch |parse_err| {
        try err.print("ci_status: cannot parse {s}: {s}\n", .{ path, @errorName(parse_err) });
        return 1;
    };
    defer parsed.deinit();
    const doc = parsed.value;

    // Python bound `runs = doc.get("runs") or []` before it dispatched, so a
    // top-level value that is not an object failed every mode, `field`
    // included, and did so before any output.
    if (implementation.objectOf(doc) == null) {
        try err.print("ci_status: {s}: {s}\n", .{ path, shapeMessage(error.DocumentNotAnObject) });
        return 1;
    }

    // `field` never touches the run list, so a malformed `runs` value cannot
    // fail it: Python only raised where it called `r.get`, and it called that
    // nowhere on this path.
    if (std.mem.eql(u8, mode, "field")) {
        const value = try implementation.fieldText(allocator, doc, arg);
        defer allocator.free(value);
        try out.print("{s}\n", .{value});
        return 0;
    }

    // Every mode below iterates the runs, which is exactly where the Python
    // raised on a malformed list or a non-object entry. An unknown mode is
    // still reported before any of that, because Python reached its `else`
    // branch without iterating either.
    const iterating = std.mem.eql(u8, mode, "count") or
        std.mem.eql(u8, mode, "verdict") or
        std.mem.eql(u8, mode, "skipped-count") or
        std.mem.eql(u8, mode, "cancelled-count") or
        std.mem.eql(u8, mode, "lines-sha") or
        std.mem.eql(u8, mode, "lines-head");
    if (!iterating) {
        try err.print("unknown mode: {s}\n", .{mode});
        return 1;
    }

    const runs = implementation.runsOf(doc) catch |shape_err| {
        try err.print("ci_status: {s}: {s}\n", .{ path, shapeMessage(shape_err) });
        return 1;
    };

    // `lines-head` printed each row as it reached it, so a bad entry halfway
    // down leaves the rows above it already on stdout. Everything else called
    // `matching()` first, which touched every entry before printing anything.
    if (std.mem.eql(u8, mode, "lines-head")) {
        const shown = @min(runs.len, implementation.head_rows);
        for (runs[0..shown]) |entry| {
            implementation.requireRunObjects(entry_slice(&entry)) catch |shape_err| {
                try err.print("ci_status: {s}: {s}\n", .{ path, shapeMessage(shape_err) });
                return 1;
            };
            const line = try implementation.renderRun(allocator, entry, true);
            defer allocator.free(line);
            try out.print("{s}\n", .{line});
        }
        return 0;
    }

    implementation.requireRunObjects(runs) catch |shape_err| {
        try err.print("ci_status: {s}: {s}\n", .{ path, shapeMessage(shape_err) });
        return 1;
    };

    if (std.mem.eql(u8, mode, "count")) {
        const got = try implementation.matching(allocator, runs, arg);
        defer allocator.free(got);
        try out.print("{d}\n", .{got.len});
        return 0;
    }
    if (std.mem.eql(u8, mode, "verdict")) {
        const answer = implementation.verdict(allocator, runs, arg) catch |verdict_err| switch (verdict_err) {
            error.NameNotHashable => {
                try err.print("ci_status: {s}: {s}\n", .{ path, shapeMessage(error.NameNotHashable) });
                return 1;
            },
            else => |leftover| return leftover,
        };
        defer allocator.free(answer);
        try out.print("{s}\n", .{answer});
        return 0;
    }
    if (std.mem.eql(u8, mode, "skipped-count") or std.mem.eql(u8, mode, "cancelled-count")) {
        const conclusion: []const u8 = if (mode[0] == 's') "skipped" else "cancelled";
        const total = try implementation.conclusionCount(allocator, runs, arg, conclusion);
        try out.print("{d}\n", .{total});
        return 0;
    }

    // `lines-sha`: the last iterating mode.
    const got = try implementation.matching(allocator, runs, arg);
    defer allocator.free(got);
    for (got) |entry| {
        const line = try implementation.renderRun(allocator, entry, false);
        defer allocator.free(line);
        try out.print("{s}\n", .{line});
    }
    return 0;
}

/// One run as a one-element slice, so a single entry can be validated with
/// the same rule the whole list is.
fn entry_slice(entry: *const Value) []const Value {
    return entry[0..1];
}
