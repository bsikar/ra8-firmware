//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the ci-monitor status reader (#858, #1144): hand argv,
//! the working directory and the real streams to `cli.run`, return its
//! status. The state file is named on the command line, so this tool needs no
//! repository root.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);

    var out = std.io.bufferedWriter(std.io.getStdOut().writer());
    var err = std.io.bufferedWriter(std.io.getStdErr().writer());
    const status = try cli.run(allocator, std.fs.cwd(), argv[1..], out.writer(), err.writer());
    try out.flush();
    try err.flush();
    return status;
}
