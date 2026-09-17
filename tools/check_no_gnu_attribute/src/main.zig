//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the GNU-attribute gate (#858, #1178): it owns argv, the
//! working directory and the two streams, and nothing else. Every decision
//! lives in `src/cli.zig`, which is why the exit contract can be tested
//! without spawning anything.
//!
//! The scan root is `RA8_REPO_ROOT` when set, else the current directory:
//! the predecessor's `ROOTS` are relative, so it walked nothing when run from
//! elsewhere and leant on the floor to catch it. The floor still catches it.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);
    const args = argv[1..];

    const repo_root = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch
        try std.process.getCwdAlloc(allocator);

    var dir = try std.fs.cwd().openDir(repo_root, .{});
    defer dir.close();

    var stdout_buffer = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr_buffer = std.io.bufferedWriter(std.io.getStdErr().writer());

    const outcome = try cli.run(
        allocator,
        dir,
        repo_root,
        args,
        stdout_buffer.writer(),
        stderr_buffer.writer(),
    );

    try stdout_buffer.flush();
    try stderr_buffer.flush();
    return outcome.status;
}
