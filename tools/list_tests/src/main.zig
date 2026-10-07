//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Entry point of the `list_tests` build tool (RA8FW-335). Everything decidable
//! lives in `cli.zig`, so the process boundary here stays a thin shell around
//! it: collect argv and the one environment variable the root resolution
//! reads, hand over the real cwd and streams, exit with the status `run`
//! returned.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    const repo_root_env = init.environ_map.get("RA8_REPO_ROOT");

    var stdout_buffer: [4096]u8 = undefined;
    var stderr_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buffer);
    var stderr = std.Io.File.stderr().writer(io, &stderr_buffer);

    const status = try cli.run(
        allocator,
        io,
        std.Io.Dir.cwd(),
        argv,
        repo_root_env,
        &stdout.interface,
        &stderr.interface,
    );

    try stdout.interface.flush();
    try stderr.interface.flush();
    return status;
}
