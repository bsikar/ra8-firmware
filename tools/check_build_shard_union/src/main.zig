//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the cross-build shard-union gate (RA8FW-335): hand
//! argv, the working directory, a scratch directory for `--selftest` and the
//! real streams to `cli.run`, return its status.
//!
//! A compiled tool has no `__file__`, so the default repository root comes
//! from `RA8_REPO_ROOT` (the trusted launcher sets it) and falls back to the
//! working directory.

const std = @import("std");
const cli = @import("cli.zig");

fn scratchName(allocator: std.mem.Allocator, io: std.Io) ![]const u8 {
    var seed: [8]u8 = undefined;
    io.random(&seed);
    return std.fmt.allocPrint(allocator, "ra8-shard-union-selftest-{s}", .{
        std.fmt.bytesToHex(seed, .lower),
    });
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;
    const default_root = init.environ_map.get("RA8_REPO_ROOT") orelse ".";

    const temporary_root = init.environ_map.get("TMPDIR") orelse "/tmp";
    const name = try scratchName(allocator, io);
    const scratch_path = try std.fs.path.join(allocator, &.{ temporary_root, name });

    const cwd = std.Io.Dir.cwd();
    const temporary: ?std.Io.Dir = cwd.openDir(io, temporary_root, .{}) catch null;
    var scratch: ?std.Io.Dir = null;
    if (temporary) |base| {
        base.createDirPath(io, name) catch {};
        scratch = base.openDir(io, name, .{}) catch null;
    }
    defer {
        if (scratch) |open| open.close(io);
        cwd.deleteTree(io, scratch_path) catch {};
        if (temporary) |base| base.close(io);
    }

    var out_buffer: [4096]u8 = undefined;
    var err_buffer: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buffer);
    var err = std.Io.File.stderr().writer(io, &err_buffer);
    const status = try cli.run(
        allocator,
        io,
        cwd,
        scratch,
        argv[1..],
        default_root,
        &out.interface,
        &err.interface,
    );
    try out.interface.flush();
    try err.interface.flush();
    return status;
}
