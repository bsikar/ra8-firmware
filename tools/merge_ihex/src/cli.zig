//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Command-line membrane for `merge_ihex` (#858), replacing the Python
//! implementation this change deletes.
//!
//! The exit-status contract is the part callers depend on and is pinned by
//! `tests/cli_test.zig`:
//!
//!   0  the merged image was written
//!   1  an input could not be read, or the output could not be written
//!   2  wrong argument count (usage error)
//!
//! `run` takes the directory paths resolve against and the two output streams,
//! so the contract is provable in a temporary directory without spawning a
//! process. `main` supplies the real cwd, stdout and stderr.

const std = @import("std");
const merge_ihex = @import("internal/root.zig");

/// Argument count of the supported invocation: tool + in_a + in_b + out.
const expected_argc = 4;

pub const usage = "usage: merge_ihex <in_a.hex> <in_b.hex> <out.hex>";

pub const exit_ok: u8 = 0;
pub const exit_io_error: u8 = 1;
pub const exit_usage: u8 = 2;

/// Largest input accepted, 64 MiB: far above any RA8 image and small enough
/// that a wrong argument cannot exhaust the build machine's memory.
const max_input_bytes = 64 * 1024 * 1024;

/// Merge two Intel HEX files, returning the process exit status.
///
/// `dir` is the directory relative paths resolve against. The output path may
/// name one of the inputs: both inputs are read fully before anything is
/// written, exactly as the Python tool documented.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    if (argv.len != expected_argc) {
        try stderr.print("{s}\n", .{usage});
        return exit_usage;
    }

    const in_a = argv[1];
    const in_b = argv[2];
    const out = argv[3];

    const first = dir.readFileAlloc(allocator, in_a, max_input_bytes) catch |err| {
        try stderr.print("merge_ihex: {s}: {s}\n", .{ in_a, @errorName(err) });
        return exit_io_error;
    };
    defer allocator.free(first);

    const second = dir.readFileAlloc(allocator, in_b, max_input_bytes) catch |err| {
        try stderr.print("merge_ihex: {s}: {s}\n", .{ in_b, @errorName(err) });
        return exit_io_error;
    };
    defer allocator.free(second);

    const merged = try merge_ihex.merge(allocator, first, second);
    defer merged.deinit(allocator);

    dir.writeFile(.{ .sub_path = out, .data = merged.text }) catch |err| {
        try stderr.print("merge_ihex: {s}: {s}\n", .{ out, @errorName(err) });
        return exit_io_error;
    };

    try stdout.print("merge_ihex: wrote {d} records -> {s}\n", .{ merged.record_count, out });
    return exit_ok;
}
