//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the example board-pin gate (#858).  Resolves the
//! repository root (RA8_REPO_ROOT, else the working directory), hands argv and
//! both streams to `cli.run`, and exits with its status.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);
    const repo_root = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch
        try std.process.getCwdAlloc(allocator);

    var dir = try std.fs.cwd().openDir(repo_root, .{});
    defer dir.close();

    var stdout_buffer = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr_buffer = std.io.bufferedWriter(std.io.getStdErr().writer());

    const status = try cli.run(
        allocator,
        dir,
        repo_root,
        argv[1..],
        stdout_buffer.writer(),
        stderr_buffer.writer(),
    );
    try stdout_buffer.flush();
    try stderr_buffer.flush();
    std.process.exit(status);
}
