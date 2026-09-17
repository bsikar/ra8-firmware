//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for `check_tz_boundary_discard` (#1250). The roots this gate
//! walks are relative, exactly as the predecessor's were, so it scans the
//! WORKING DIRECTORY and nothing else: run from anywhere but the repo root and
//! the sweep collapses, which is what `file_floor` exists to catch.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);
    const args = if (argv.len > 1) argv[1..] else &[_][]const u8{};

    const cwd_path = try std.process.getCwdAlloc(allocator);

    var stdout_buffer = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr_buffer = std.io.bufferedWriter(std.io.getStdErr().writer());

    const status = try cli.run(
        allocator,
        std.fs.cwd(),
        cwd_path,
        args,
        stdout_buffer.writer(),
        stderr_buffer.writer(),
    );

    try stdout_buffer.flush();
    try stderr_buffer.flush();
    std.process.exit(status);
}
