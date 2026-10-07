//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the ci-monitor status reader (RA8FW-335): hand argv,
//! the working directory and the real streams to `cli.run`, return its
//! status. The state file is named on the command line, so this tool needs no
//! repository root.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    var out_buffer: [4096]u8 = undefined;
    var err_buffer: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buffer);
    var err = std.Io.File.stderr().writer(io, &err_buffer);
    const status = try cli.run(
        allocator,
        io,
        std.Io.Dir.cwd(),
        argv[1..],
        &out.interface,
        &err.interface,
    );
    try out.interface.flush();
    try err.interface.flush();
    return status;
}
