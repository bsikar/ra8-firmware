//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for `check_header_file_placement` (RA8FW-335). It owns the
//! allocator, the real file system, the environment and the exit status, and
//! nothing else: every decision is in `src/cli.zig` and
//! `src/internal/root.zig`, so the tests drive the gate without a process.
//!
//! The predecessor derived REPO_ROOT from its own location (`parents[2]`),
//! which a compiled binary in tools/<name>/build/bin cannot do meaningfully,
//! so the root is `RA8_REPO_ROOT` when set and the working directory
//! otherwise, the same resolution the other tools migrated under RA8FW-335 use.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_args = try init.minimal.args.toSlice(allocator);
    const args = try allocator.alloc([]const u8, sentinel_args.len);
    for (args, sentinel_args) |*arg, sentinel_arg| arg.* = sentinel_arg;

    const cwd = std.Io.Dir.cwd();
    const repo_root = try cwd.realPathFileAlloc(io, init.environ_map.get("RA8_REPO_ROOT") orelse ".", allocator);

    // The selftest builds its fixture tree in a private directory and removes
    // it on the way out, as tempfile.TemporaryDirectory did.
    const temp_dir = init.environ_map.get("TMPDIR") orelse "/tmp";
    var seed: [8]u8 = undefined;
    io.random(&seed);
    const scratch_root = try std.fmt.allocPrint(
        allocator,
        "{s}/ra8-header-placement-{x}",
        .{ std.mem.trimEnd(u8, temp_dir, "/"), std.mem.readInt(u64, &seed, .little) },
    );

    // Every path this tool handles is absolute, as the predecessor's were:
    // REPO_ROOT-joined arguments, the scan roots beneath it, and the
    // selftest's fixture tree. A POSIX *at call ignores its directory handle
    // for an absolute path, so the working directory stays irrelevant.
    var stdout_buffer: [4096]u8 = undefined;
    var stderr_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buffer);
    var stderr = std.Io.File.stderr().writer(io, &stderr_buffer);

    const status = cli.run(
        allocator,
        io,
        cwd,
        repo_root,
        scratch_root,
        args[1..],
        &stdout.interface,
        &stderr.interface,
    ) catch |err| {
        std.debug.print("check_header_file_placement: {s}\n", .{@errorName(err)});
        stdout.interface.flush() catch {};
        stderr.interface.flush() catch {};
        return 1;
    };

    cwd.deleteTree(io, scratch_root) catch {};

    try stdout.interface.flush();
    try stderr.interface.flush();
    return status;
}
