//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Command-line membrane for `gen_jpeg_fixture` (#858), replacing the Python
//! implementation this change deletes.
//!
//! `scripts/builders/init_fuzz_corpora.sh` runs this once per seed and reads
//! the result from a file it names with `-o`, so the exit-status contract
//! pinned by `tests/cli_test.zig` is:
//!
//!   0  the seed was written (to the named file, or to stdout for `-`)
//!   1  a dimension outside 1..65535, or an output path that cannot be written
//!   2  wrong arguments (unknown flag, missing value, non-integer value)
//!
//! That split is inherited rather than invented: under argparse a malformed
//! argument exited 2, while an out-of-range dimension raised `ValueError` and
//! reached the process boundary as an exit-1 traceback. This tool keeps both
//! statuses and replaces the traceback with one message on stderr.
//!
//! Three argparse spellings are deliberately NOT accepted, all measured
//! against the deleted implementation rather than assumed: a prefix
//! abbreviation (`--wid 8`, argparse exit 0), an attached short-option value
//! (`-oseed.jpg`, argparse exit 0) and `-o=seed.jpg`, which argparse read as a
//! path literally named `=seed.jpg`. Each is a usage error here, exit 2, and
//! `tests/cli_test.zig` pins all three. `scripts/builders/init_fuzz_corpora.sh`
//! is the only caller and uses the long spellings, so nothing in the tree
//! depends on the argparse behaviour; rejecting an abbreviation beats silently
//! guessing which option a future `--he` meant.
//!
//! `run` takes the directory output paths resolve against and both streams, so
//! the contract is provable in a temporary directory without spawning a
//! process. `main` supplies the real cwd, stdout and stderr.

const std = @import("std");
const jpeg = @import("internal/root.zig");

pub const usage =
    "usage: gen_jpeg_fixture [--width N] [--height N] [-o|--output PATH]\n" ++
    "       N is 1..65535 (default 8); PATH defaults to - (stdout)";

pub const exit_ok: u8 = 0;
pub const exit_error: u8 = 1;
pub const exit_usage: u8 = 2;

const default_dimension: i64 = 8;

/// One `--name value` / `--name=value` option, read from `argv` at `index`.
const Option = struct {
    value: []const u8,
    next_index: usize,
};

/// Read the value of `name` at `argv[index]`, or null when the argument is
/// some other option. Both spellings argparse accepted are accepted here:
/// `--width 8` and `--width=8`.
fn optionValue(
    argv: []const []const u8,
    index: usize,
    name: []const u8,
    short: ?[]const u8,
) ?Option {
    const arg = argv[index];
    const is_long = std.mem.eql(u8, arg, name);
    const is_short = if (short) |alias| std.mem.eql(u8, arg, alias) else false;
    if (is_long or is_short) {
        if (index + 1 >= argv.len) return .{ .value = "", .next_index = index };
        return .{ .value = argv[index + 1], .next_index = index + 1 };
    }
    if (std.mem.startsWith(u8, arg, name) and
        arg.len > name.len and
        arg[name.len] == '=')
    {
        return .{ .value = arg[name.len + 1 ..], .next_index = index };
    }
    return null;
}

fn usageError(stderr: anytype, message: []const u8, detail: []const u8) !u8 {
    try stderr.print("gen_jpeg_fixture: {s}{s}\n{s}\n", .{ message, detail, usage });
    return exit_usage;
}

/// Write one minimal baseline JPEG seed, returning the exit status.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var width: i64 = default_dimension;
    var height: i64 = default_dimension;
    var output: []const u8 = "-";

    var index: usize = 1;
    while (index < argv.len) : (index += 1) {
        const arg = argv[index];
        if (std.mem.eql(u8, arg, "--help") or std.mem.eql(u8, arg, "-h")) {
            try stdout.print("{s}\n", .{usage});
            return exit_ok;
        }
        if (optionValue(argv, index, "--width", null)) |option| {
            if (option.next_index == index and option.value.len == 0) {
                return usageError(stderr, "missing value for ", "--width");
            }
            width = std.fmt.parseInt(i64, option.value, 10) catch {
                return usageError(stderr, "--width expects an integer, got ", option.value);
            };
            index = option.next_index;
            continue;
        }
        if (optionValue(argv, index, "--height", null)) |option| {
            if (option.next_index == index and option.value.len == 0) {
                return usageError(stderr, "missing value for ", "--height");
            }
            height = std.fmt.parseInt(i64, option.value, 10) catch {
                return usageError(stderr, "--height expects an integer, got ", option.value);
            };
            index = option.next_index;
            continue;
        }
        if (optionValue(argv, index, "--output", "-o")) |option| {
            if (option.next_index == index and option.value.len == 0) {
                return usageError(stderr, "missing value for ", "--output");
            }
            output = option.value;
            index = option.next_index;
            continue;
        }
        return usageError(stderr, "unrecognised argument: ", arg);
    }

    if (width < jpeg.min_dimension or width > jpeg.max_dimension or
        height < jpeg.min_dimension or height > jpeg.max_dimension)
    {
        try stderr.print(
            "gen_jpeg_fixture: width/height must be in {d}..{d}, got {d}x{d}\n",
            .{ jpeg.min_dimension, jpeg.max_dimension, width, height },
        );
        return exit_error;
    }

    const blob = jpeg.buildMinimalJpeg(
        allocator,
        @intCast(width),
        @intCast(height),
    ) catch return exit_error;
    defer allocator.free(blob);

    if (std.mem.eql(u8, output, "-")) {
        stdout.writeAll(blob) catch return exit_error;
        return exit_ok;
    }

    const file = dir.createFile(output, .{ .truncate = true }) catch |err| {
        try stderr.print(
            "gen_jpeg_fixture: cannot write {s}: {s}\n",
            .{ output, @errorName(err) },
        );
        return exit_error;
    };
    defer file.close();
    file.writeAll(blob) catch |err| {
        try stderr.print(
            "gen_jpeg_fixture: cannot write {s}: {s}\n",
            .{ output, @errorName(err) },
        );
        return exit_error;
    };

    try stdout.print(
        "gen_jpeg_fixture: wrote {s} ({d}x{d}, {d} bytes)\n",
        .{ output, width, height, blob.len },
    );
    return exit_ok;
}
