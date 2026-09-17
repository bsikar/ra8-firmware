//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the obsolete-standards gate (#858): resolve the
//! repository root, hand argv, the Git census, the Git index and the real
//! streams to `cli.run`, return its status.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);

    // A compiled tool has no `__file__.parents[2]`, so the root the census is
    // taken at comes from the launcher, and falls back to the working
    // directory when the gate is run by hand from the repository root.
    const repo_root = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch ".";

    var out = std.io.bufferedWriter(std.io.getStdOut().writer());
    var err = std.io.bufferedWriter(std.io.getStdErr().writer());
    const status = try cli.run(
        allocator,
        std.fs.cwd(),
        repo_root,
        argv[1..],
        .git,
        .git,
        cli.Policy.default,
        out.writer(),
        err.writer(),
    );
    try out.flush();
    try err.flush();
    return status;
}
