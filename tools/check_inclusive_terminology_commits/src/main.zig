//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the commit-message terminology gate (RA8FW-335): read the
//! commit messages from stdin, hand argv and the real streams to `cli.run`,
//! return its status.

const std = @import("std");
const cli = @import("cli.zig");

/// Ceiling on one scan, far above any plausible push.
const max_input_bytes = 64 * 1024 * 1024;

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();

    const sentinel_argv = try init.minimal.args.toSlice(allocator);
    const argv = try allocator.alloc([]const u8, sentinel_argv.len);
    for (argv, sentinel_argv) |*arg, sentinel_arg| arg.* = sentinel_arg;

    // Read stdin up front, exactly as the predecessor's `sys.stdin.read()`
    // did, so an empty pipe scans empty text rather than blocking a rule.
    // `allocRemaining` fails only once the limit is exceeded, so exactly
    // `max_input_bytes` is still accepted, as `readAllAlloc` accepted it.
    var stdin_buffer: [4096]u8 = undefined;
    var stdin = std.Io.File.stdin().readerStreaming(io, &stdin_buffer);
    const input = try stdin.interface.allocRemaining(allocator, .limited(max_input_bytes));

    var out_buffer: [4096]u8 = undefined;
    var err_buffer: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buffer);
    var err = std.Io.File.stderr().writer(io, &err_buffer);
    const status = try cli.run(allocator, argv[1..], input, &out.interface, &err.interface);
    try out.interface.flush();
    try err.interface.flush();
    return status;
}
