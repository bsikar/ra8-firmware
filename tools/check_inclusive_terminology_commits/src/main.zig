//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the commit-message terminology gate (#858): read the
//! commit messages from stdin, hand argv and the real streams to `cli.run`,
//! return its status.

const std = @import("std");
const cli = @import("cli.zig");

/// Ceiling on one scan, far above any plausible push.
const max_input_bytes = 64 * 1024 * 1024;

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);

    // Read stdin up front, exactly as the predecessor's `sys.stdin.read()`
    // did, so an empty pipe scans empty text rather than blocking a rule.
    const input = try std.io.getStdIn().reader().readAllAlloc(allocator, max_input_bytes);

    var out = std.io.bufferedWriter(std.io.getStdOut().writer());
    var err = std.io.bufferedWriter(std.io.getStdErr().writer());
    const status = try cli.run(allocator, argv[1..], input, out.writer(), err.writer());
    try out.flush();
    try err.flush();
    return status;
}
