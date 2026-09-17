//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the per-file MC/DC FLOOR gate (#858): resolve the
//! repository root and its checkout basename, hand argv and the real stream
//! to `cli.run`, return its status.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);

    // A compiled tool has no `__file__.parents[2]`, so the root comes from
    // the launcher and falls back to the working directory when the gate is
    // run by hand from the repository root.
    const repo_root = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch ".";

    // The predecessor derived its absolute-path split marker from the
    // checkout directory basename, so resolve the root before taking it: a
    // relative root would otherwise yield the marker "/./".
    const resolved = std.fs.cwd().realpathAlloc(allocator, repo_root) catch repo_root;
    const repo_name = std.fs.path.basename(resolved);

    var out = std.io.bufferedWriter(std.io.getStdOut().writer());
    const status = try cli.run(allocator, std.fs.cwd(), repo_root, repo_name, argv[1..], out.writer());
    try out.flush();
    return status;
}
