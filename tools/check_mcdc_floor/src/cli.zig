//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit contract for the per-file MC/DC floor gate
//! (#858, #1205). Everything that can fail against a real tree lives here:
//! locating the coverage document, reading it, parsing it and mapping each
//! failure onto the status `scripts/report/mcdc_report.sh` branches on.
//!
//! `run` is parameterised on the directory the document is resolved against,
//! the repo root path used to spell it in the not-found line, the checkout
//! basename `normalize` splits absolute paths on, and both output streams, so
//! the whole contract is testable without a process or a real repository.
//!
//! EXIT CONTRACT, inherited from scripts/checks/check_mcdc_floor.py and NOT
//! extended. The predecessor had exactly two statuses, so no usage status is
//! invented here:
//!
//!   0  every in-scope file with a reachable decision is at or above the
//!      floor, or `--selftest` held.
//!   1  an offender, a required production root with no reachable decision, a
//!      missing report, a report that cannot be read or parsed, a report whose
//!      `files` is falsy or misshapen, a field `int()` would have rejected, or
//!      a failing selftest case.
//!
//! Argv is read the predecessor's way: the selftest runs when argv[1:] is
//! EXACTLY ["--selftest"], and any other argv (an unknown flag, a stray
//! positional, several arguments) runs the gate and is otherwise ignored.
//!
//! Findings print on STDOUT, because the predecessor used bare `print()`
//! throughout. Only the malformed-document lines, which replaced a CPython
//! traceback, go to stderr.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Refuse a document larger than any MC/DC report: the tree's is ~2 MiB.
pub const max_report_bytes: usize = 256 * 1024 * 1024;

pub fn run(
    allocator: std.mem.Allocator,
    args: []const []const u8,
    dir: std.fs.Dir,
    repo_root: []const u8,
    root_name: []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    if (args.len == 1 and std.mem.eql(u8, args[0], "--selftest")) {
        return selftest(arena, root_name, stdout);
    }
    return gate(arena, dir, repo_root, root_name, stdout, stderr);
}

fn selftest(arena: std.mem.Allocator, root_name: []const u8, stdout: anytype) !u8 {
    const failures = try implementation.selftestFailures(arena, root_name);
    if (failures.items.len != 0) {
        for (failures.items) |failure| try implementation.renderSelftestFailure(stdout, failure);
        return 1;
    }
    try implementation.renderSelftestPass(stdout);
    return 0;
}

fn gate(
    arena: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    root_name: []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    const json_path = try std.fs.path.join(arena, &.{ repo_root, implementation.mcdc_json_rel });

    // `Path.is_file()`: a missing path AND a directory both take this branch,
    // so a half-written report directory cannot pass vacuously.
    const stat = dir.statFile(implementation.mcdc_json_rel) catch {
        try implementation.renderMissingReport(stdout, json_path);
        return 1;
    };
    if (stat.kind != .file) {
        try implementation.renderMissingReport(stdout, json_path);
        return 1;
    }

    const text = dir.readFileAlloc(arena, implementation.mcdc_json_rel, max_report_bytes) catch |err| {
        try implementation.renderUnreadableReport(stdout, @errorName(err));
        return 1;
    };

    var parsed = std.json.parseFromSlice(std.json.Value, arena, text, .{}) catch |err| {
        try implementation.renderUnreadableReport(stdout, @errorName(err));
        return 1;
    };
    defer parsed.deinit();

    // `data.get("files", [])` needs a mapping; the predecessor died on an
    // AttributeError here, with its traceback on stderr.
    const document = switch (parsed.value) {
        .object => |object| object,
        else => {
            try stderr.print(
                "{s}: ERROR -- MC/DC JSON is not an object.\n",
                .{implementation.tool_name},
            );
            return 1;
        },
    };

    const files_value = document.get("files") orelse std.json.Value{ .array = std.json.Array.init(arena) };
    if (!truthy(files_value)) {
        try implementation.renderNoFiles(stdout);
        return 1;
    }
    const files = switch (files_value) {
        .array => |array| array.items,
        else => {
            try stderr.print(
                "{s}: ERROR -- MC/DC JSON `files` is not a list.\n",
                .{implementation.tool_name},
            );
            return 1;
        },
    };

    const collected = implementation.collectOffenders(arena, files, root_name) catch |err| switch (err) {
        error.OutOfMemory => return err,
        else => {
            try stderr.print(
                "{s}: ERROR -- MC/DC JSON has a per-file entry this gate cannot read ({s}).\n",
                .{ implementation.tool_name, @errorName(err) },
            );
            return 1;
        },
    };

    const missing = try implementation.missingScopes(arena, collected.census);
    if (missing.len != 0) {
        try implementation.renderMissingScopes(stdout, missing);
        return 1;
    }

    if (collected.offenders.len != 0) {
        try implementation.renderOffenders(stdout, collected.offenders);
        return 1;
    }

    try implementation.renderPass(stdout, collected.checked());
    return 0;
}

/// Python truthiness, which is what `if not files:` tested. An empty list,
/// an empty object, an empty string, 0, false and null are all "no files"
/// rather than an error.
pub fn truthy(value: std.json.Value) bool {
    return switch (value) {
        .null => false,
        .bool => |flag| flag,
        .integer => |integer| integer != 0,
        .float => |float| float != 0.0,
        .number_string => |text| text.len != 0,
        .string => |text| text.len != 0,
        .array => |array| array.items.len != 0,
        .object => |object| object.count() != 0,
    };
}

/// Re-exported so the CLI tests can build documents without reaching into the
/// implementation module directly.
pub const tool_name = implementation.tool_name;
pub const mcdc_json_rel = implementation.mcdc_json_rel;
