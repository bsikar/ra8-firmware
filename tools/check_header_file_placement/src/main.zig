//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for `check_header_file_placement` (#858, #1219). It owns the
//! allocator, the real file system, the environment and the exit status, and
//! nothing else: every decision is in `src/cli.zig` and
//! `src/internal/root.zig`, so the tests drive the gate without a process.
//!
//! The predecessor derived REPO_ROOT from its own location (`parents[2]`),
//! which a compiled binary in tools/<name>/build/bin cannot do meaningfully,
//! so the root is `RA8_REPO_ROOT` when set and the working directory
//! otherwise, the same resolution the other tools migrated under #858 use.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const args = try std.process.argsAlloc(allocator);

    const override = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch null;
    const repo_root = if (override) |value|
        try std.fs.cwd().realpathAlloc(allocator, value)
    else
        try std.fs.cwd().realpathAlloc(allocator, ".");

    // The selftest builds its fixture tree in a private directory and removes
    // it on the way out, as tempfile.TemporaryDirectory did.
    const temp_dir = std.process.getEnvVarOwned(allocator, "TMPDIR") catch
        try allocator.dupe(u8, "/tmp");
    var seed: [8]u8 = undefined;
    std.crypto.random.bytes(&seed);
    const scratch_root = try std.fmt.allocPrint(
        allocator,
        "{s}/ra8-header-placement-{x}",
        .{ std.mem.trimRight(u8, temp_dir, "/"), std.mem.readInt(u64, &seed, .little) },
    );

    // Every path this tool handles is absolute, as the predecessor's were:
    // REPO_ROOT-joined arguments, the scan roots beneath it, and the
    // selftest's fixture tree. A POSIX *at call ignores its directory handle
    // for an absolute path, so the working directory stays irrelevant.
    var root_dir = std.fs.cwd();

    var stdout_buffered = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr_buffered = std.io.bufferedWriter(std.io.getStdErr().writer());

    const status = cli.run(
        allocator,
        root_dir,
        repo_root,
        scratch_root,
        args[1..],
        stdout_buffered.writer(),
        stderr_buffered.writer(),
    ) catch |err| {
        std.debug.print("check_header_file_placement: {s}\n", .{@errorName(err)});
        stdout_buffered.flush() catch {};
        stderr_buffered.flush() catch {};
        return 1;
    };

    root_dir.deleteTree(scratch_root) catch {};

    try stdout_buffered.flush();
    try stderr_buffered.flush();
    return status;
}
