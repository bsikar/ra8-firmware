//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for `gen_jlink_w4` (#858): collect argv, hand the current
//! working directory and both streams to the CLI, and exit with its status.
//! Relative image paths resolve against the caller's working directory, as
//! the predecessor's `Path(bin_file)` did.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var gpa_state = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa_state.deinit();
    const gpa = gpa_state.allocator();

    const argv = try std.process.argsAlloc(gpa);
    defer std.process.argsFree(gpa, argv);

    var stdout_buffered = std.io.bufferedWriter(std.io.getStdOut().writer());
    const stdout = stdout_buffered.writer().any();
    const stderr = std.io.getStdErr().writer().any();

    const program_name = if (argv.len > 0) argv[0] else "gen_jlink_w4";
    const status = try cli.run(
        gpa,
        argv[@min(argv.len, 1)..],
        .{ .dir = std.fs.cwd(), .program_name = program_name },
        .{ .out = stdout, .err = stderr },
    );
    try stdout_buffered.flush();
    return status;
}
