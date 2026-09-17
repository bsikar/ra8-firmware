//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the cross-build shard-union gate (#858, #1159): hand
//! argv, the working directory, a scratch directory for `--selftest` and the
//! real streams to `cli.run`, return its status.
//!
//! A compiled tool has no `__file__`, so the default repository root comes
//! from `RA8_REPO_ROOT` (the trusted launcher sets it) and falls back to the
//! working directory.

const std = @import("std");
const cli = @import("cli.zig");

fn scratchName(allocator: std.mem.Allocator) ![]const u8 {
    var seed: [8]u8 = undefined;
    std.crypto.random.bytes(&seed);
    return std.fmt.allocPrint(allocator, "ra8-shard-union-selftest-{s}", .{
        std.fmt.bytesToHex(seed, .lower),
    });
}

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);
    const default_root = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch
        try allocator.dupe(u8, ".");

    const temporary_root = std.process.getEnvVarOwned(allocator, "TMPDIR") catch
        try allocator.dupe(u8, "/tmp");
    const name = try scratchName(allocator);
    const scratch_path = try std.fs.path.join(allocator, &.{ temporary_root, name });

    var temporary = std.fs.cwd().openDir(temporary_root, .{}) catch null;
    var scratch: ?std.fs.Dir = null;
    if (temporary) |*base| {
        base.makePath(name) catch {};
        scratch = base.openDir(name, .{}) catch null;
    }
    defer {
        if (scratch) |*open| open.close();
        std.fs.cwd().deleteTree(scratch_path) catch {};
        if (temporary) |*base| base.close();
    }

    var out = std.io.bufferedWriter(std.io.getStdOut().writer());
    var err = std.io.bufferedWriter(std.io.getStdErr().writer());
    const status = try cli.run(
        allocator,
        std.fs.cwd(),
        scratch,
        argv[1..],
        default_root,
        out.writer(),
        err.writer(),
    );
    try out.flush();
    try err.flush();
    return status;
}
