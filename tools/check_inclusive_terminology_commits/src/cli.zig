//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the commit-message terminology
//! gate (#858).
//!
//! Exit 0 when the scanned commit messages are clean, and 1 when a banned term
//! survives its paragraph's opt-out or the detector selftest fails. There is
//! no usage status: the predecessor read `sys.argv[1:]` only to look for
//! `--selftest` and scanned stdin whatever else it was handed, so an unknown
//! flag is not an error here.
//!
//! `run` is parameterised on the input text and both output streams, so every
//! status above is provable in a test with no process and no pipe.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_inclusive_terminology_commits";

/// Run the gate over `input`. Returns the process exit status.
pub fn run(
    caller_allocator: std.mem.Allocator,
    argv: []const []const u8,
    input: []const u8,
    out: anytype,
    err: anytype,
) !u8 {
    // One arena per run: the decoded text, the findings and the rendered
    // report all live exactly as long as the run does.
    var arena = std.heap.ArenaAllocator.init(caller_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    for (argv) |argument| {
        if (std.mem.eql(u8, argument, "--selftest")) return selftest(allocator, out, err);
    }

    const violations = try implementation.findViolations(allocator, input);
    if (violations.len == 0) {
        try out.print("[PASS] Commit message terminology clean.\n", .{});
        return 0;
    }

    try out.print("[FAIL] Non-inclusive terminology in commit message(s):\n", .{});
    for (violations) |violation| {
        try out.print("{s}\n", .{try implementation.renderViolation(allocator, violation)});
    }
    return 1;
}

/// Prove the detector fires, honours a wrapped opt-out, and does not leak.
///
/// Run FIRST by the gate: a detector that has stopped firing reports a clean
/// history for exactly the wrong reason, which is the failure the selftest
/// exists to catch.
pub fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
    _ = err;

    const fires = try implementation.findViolations(allocator, implementation.selftest_fires);
    if (fires.len == 0) {
        try out.print(
            "[SELFTEST FAIL] an un-annotated {s} in a commit message was not flagged.\n",
            .{"MO" ++ "SI"},
        );
        return 1;
    }

    const quiet = try implementation.findViolations(allocator, implementation.selftest_quiet);
    if (quiet.len != 0) {
        try out.print("[SELFTEST FAIL] a paragraph-scoped LEGACY-OK opt-out did not cover its\n", .{});
        try out.print("                whole paragraph:\n", .{});
        for (quiet) |violation| {
            try out.print("{s}\n", .{try implementation.renderViolation(allocator, violation)});
        }
        return 1;
    }

    const cross = try implementation.findViolations(allocator, implementation.selftest_cross_paragraph);
    if (cross.len == 0) {
        try out.print("[SELFTEST FAIL] LEGACY-OK in one paragraph suppressed a violation in a\n", .{});
        try out.print("                different paragraph.\n", .{});
        return 1;
    }

    try out.print("[SELFTEST OK] fires on an un-annotated term, stays quiet on a wrapped\n", .{});
    try out.print("              paragraph-scoped LEGACY-OK, and does not leak across paragraphs.\n", .{});
    return 0;
}
