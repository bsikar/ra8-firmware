//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for `gen_jlink_w4` (RA8FW-335): collect argv, hand the current
//! working directory and both streams to the CLI, and exit with its status.
//! Relative image paths resolve against the caller's working directory, as
//! the predecessor's `Path(bin_file)` did.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const gpa = init.gpa;

    const sentinel_argv = try init.minimal.args.toSlice(init.arena.allocator());
    const argv = try init.arena.allocator().alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    var stdout_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buffer);
    var stderr = std.Io.File.stderr().writer(io, &.{});

    const program_name = if (argv.len > 0) argv[0] else "gen_jlink_w4";
    const status = try cli.run(
        gpa,
        argv[@min(argv.len, 1)..],
        .{ .io = io, .dir = std.Io.Dir.cwd(), .program_name = program_name },
        .{ .out = &stdout.interface, .err = &stderr.interface },
    );
    try stdout.interface.flush();
    return status;
}
