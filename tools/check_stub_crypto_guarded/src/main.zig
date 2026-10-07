//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the stub-crypto gate (RA8FW-335): resolve the repository
//! root, hand argv, the live tree and the real streams to `cli.run`, return
//! its status.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    // A compiled tool has no `__file__.parents[2]`, so the root comes from the
    // launcher and falls back to the working directory when the gate is run
    // by hand from the repository root.
    const repo_root = init.environ_map.get("RA8_REPO_ROOT") orelse ".";

    var out_buffer: [4096]u8 = undefined;
    var err_buffer: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buffer);
    var err = std.Io.File.stderr().writer(io, &err_buffer);
    const status = try cli.run(
        allocator,
        io,
        std.Io.Dir.cwd(),
        repo_root,
        argv[1..],
        cli.default_stubs,
        &out.interface,
        &err.interface,
    );
    try out.interface.flush();
    try err.interface.flush();
    return status;
}
