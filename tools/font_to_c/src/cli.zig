//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Command-line membrane for `font_to_c` (#858), replacing the Python
//! implementation this change deletes.
//!
//! The exit-status contract is what the three CMake callers depend on and is
//! pinned by `tests/cli_test.zig`:
//!
//!   0  the C source was written
//!   1  the font could not be read, it was EMPTY, or the output could not be
//!      written
//!   2  wrong argument count (usage error)
//!
//! An empty font is rejected rather than baked into a zero-length array, for
//! the reason the Python tool gave: a zero-length font links cleanly and fails
//! only at render time, far from its cause.
//!
//! `run` takes the directory paths resolve against and the two output streams,
//! so the contract is provable in a temporary directory without spawning a
//! process. `main` supplies the real cwd, stdout and stderr.

const std = @import("std");
const font_to_c = @import("internal/root.zig");

/// Argument count of the supported invocation: tool + input + output + symbol
/// + header.
const expected_argc = 5;

pub const usage = "usage: font_to_c <input-font> <output.c> <symbol_name> <header_name>";

pub const exit_ok: u8 = 0;
pub const exit_error: u8 = 1;
pub const exit_usage: u8 = 2;

/// Largest font accepted, 64 MiB: far above any baked subset and small enough
/// that a wrong argument cannot exhaust the build machine's memory.
const max_font_bytes = 64 * 1024 * 1024;

/// Bake a font file into a C translation unit, returning the exit status.
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

    const source = argv[1];
    const destination = argv[2];
    const symbol = argv[3];
    const header = argv[4];

    const data = dir.readFileAlloc(allocator, source, max_font_bytes) catch |err| {
        try stderr.print("font_to_c: {s}: {s}\n", .{ source, @errorName(err) });
        return exit_error;
    };
    defer allocator.free(data);

    if (data.len == 0) {
        try stderr.print("font_to_c: empty input {s}\n", .{source});
        return exit_error;
    }

    const text = try font_to_c.render(
        allocator,
        font_to_c.basename(source),
        data,
        symbol,
        header,
    );
    defer allocator.free(text);

    dir.writeFile(.{ .sub_path = destination, .data = text }) catch |err| {
        try stderr.print("font_to_c: {s}: {s}\n", .{ destination, @errorName(err) });
        return exit_error;
    };

    try stdout.print("font_to_c: baked {d} bytes -> {s}\n", .{ data.len, destination });
    return exit_ok;
}
