//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the example board-pin gate (RA8FW-335).  Resolves the
//! repository root (RA8_REPO_ROOT, else the working directory), hands argv and
//! both streams to `cli.run`, and exits with its status.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    const repo_root: []const u8 = init.environ_map.get("RA8_REPO_ROOT") orelse
        try std.process.currentPathAlloc(io, allocator);

    var dir = try std.Io.Dir.cwd().openDir(io, repo_root, .{});
    defer dir.close(io);

    var stdout_buffer: [4096]u8 = undefined;
    var stderr_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buffer);
    var stderr = std.Io.File.stderr().writer(io, &stderr_buffer);

    const status = try cli.run(
        allocator,
        io,
        dir,
        repo_root,
        argv[1..],
        &stdout.interface,
        &stderr.interface,
    );
    try stdout.interface.flush();
    try stderr.interface.flush();
    return status;
}
