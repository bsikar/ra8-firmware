//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the host-build-entrypoint gate (RA8FW-335). The repository root
//! comes from RA8_REPO_ROOT when the trusted launcher sets it, otherwise the
//! working directory, mirroring the predecessor's parents[2] resolution.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.gpa;
    const arena = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(arena);
    const argv = try arena.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    const cwd = try std.process.currentPathAlloc(io, arena);
    const repo_root = init.environ_map.get("RA8_REPO_ROOT") orelse cwd;

    var stdout_buffer: [4096]u8 = undefined;
    var stderr_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buffer);
    var stderr = std.Io.File.stderr().writer(io, &stderr_buffer);
    const status = try cli.run(
        allocator,
        io,
        argv[1..],
        repo_root,
        &stdout.interface,
        &stderr.interface,
    );
    try stdout.interface.flush();
    try stderr.interface.flush();
    return status;
}
