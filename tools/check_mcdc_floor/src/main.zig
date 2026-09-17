//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the per-file MC/DC floor gate (#858, #1205). It resolves
//! the repository root, opens it, and hands argv, the streams and the exit
//! status to the CLI module.
//!
//! The predecessor derived its root from its own location (`parents[2]`), which
//! a compiled binary in tools/<name>/build/bin cannot do meaningfully, so the
//! root is `RA8_REPO_ROOT` when set and the working directory otherwise, the
//! same resolution the other migrated tools under #858 use. The checkout
//! BASENAME matters as well as the path: `normalize` splits absolute paths in
//! the coverage document on it.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var gpa_state = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa_state.deinit();
    const gpa = gpa_state.allocator();

    const args = try std.process.argsAlloc(gpa);
    defer std.process.argsFree(gpa, args);

    const override = std.process.getEnvVarOwned(gpa, "RA8_REPO_ROOT") catch null;
    defer if (override) |value| gpa.free(value);

    const repo_root = if (override) |value|
        try std.fs.cwd().realpathAlloc(gpa, value)
    else
        try std.fs.cwd().realpathAlloc(gpa, ".");
    defer gpa.free(repo_root);

    var dir = try std.fs.cwd().openDir(repo_root, .{});
    defer dir.close();

    var stdout_buffered = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr_buffered = std.io.bufferedWriter(std.io.getStdErr().writer());

    const status = try cli.run(
        gpa,
        args[1..],
        dir,
        repo_root,
        std.fs.path.basename(repo_root),
        stdout_buffered.writer(),
        stderr_buffered.writer(),
    );

    try stdout_buffered.flush();
    try stderr_buffered.flush();
    return status;
}
