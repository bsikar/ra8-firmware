//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract of `gen_jpeg_fixture` (#858).
//!
//! `scripts/builders/init_fuzz_corpora.sh` runs under `set -e`, so a status
//! this tool gets wrong stops the fuzz sweep or, worse, leaves a truncated
//! seed behind and carries on. These cases pin 0 / 1 / 2, which stream each
//! message lands on, and that nothing but the blob reaches stdout when the
//! seed is piped rather than written to a file.

const std = @import("std");
const cli = @import("cli");

const Streams = struct {
    out: std.ArrayList(u8),
    err: std.ArrayList(u8),

    fn init(allocator: std.mem.Allocator) Streams {
        return .{
            .out = std.ArrayList(u8).init(allocator),
            .err = std.ArrayList(u8).init(allocator),
        };
    }

    fn deinit(self: *Streams) void {
        self.out.deinit();
        self.err.deinit();
    }
};

fn run(dir: std.fs.Dir, streams: *Streams, argv: []const []const u8) !u8 {
    return cli.run(
        std.testing.allocator,
        dir,
        argv,
        streams.out.writer(),
        streams.err.writer(),
    );
}

test "no arguments writes the default 8x8 seed to stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{"gen_jpeg_fixture"});
    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqual(@as(usize, 346), streams.out.items.len);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xD8 }, streams.out.items[0..2]);
    try std.testing.expectEqualStrings("", streams.err.items);
}

test "an explicit - output also writes the blob to stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--width", "16", "--height", "16", "-o", "-",
    });
    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqual(@as(usize, 346), streams.out.items.len);
}

test "the requested dimensions reach SOF0" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--width", "32", "--height", "24",
    });
    try std.testing.expectEqual(cli.exit_ok, status);
    const sof0 = std.mem.indexOf(u8, streams.out.items, &[_]u8{ 0xFF, 0xC0 }).?;
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0x00, 0x18, 0x00, 0x20 },
        streams.out.items[sof0 + 5 ..][0..4],
    );
}

test "the --width=N spelling is accepted" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--width=64", "--height=64",
    });
    try std.testing.expectEqual(cli.exit_ok, status);
    const sof0 = std.mem.indexOf(u8, streams.out.items, &[_]u8{ 0xFF, 0xC0 }).?;
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0x00, 0x40, 0x00, 0x40 },
        streams.out.items[sof0 + 5 ..][0..4],
    );
}

test "-o writes the seed to a file and keeps the blob off stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--width", "8", "--height", "8", "-o", "seed_8x8.jpg",
    });
    try std.testing.expectEqual(cli.exit_ok, status);

    const written = try tmp.dir.readFileAlloc(std.testing.allocator, "seed_8x8.jpg", 4096);
    defer std.testing.allocator.free(written);
    try std.testing.expectEqual(@as(usize, 346), written.len);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xD9 }, written[written.len - 2 ..]);

    try std.testing.expect(std.mem.indexOf(u8, streams.out.items, "seed_8x8.jpg") != null);
    try std.testing.expect(std.mem.indexOf(u8, streams.out.items, &[_]u8{0xFF}) == null);
    try std.testing.expectEqualStrings("", streams.err.items);
}

test "the --output=PATH spelling is accepted" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--output=seed.jpg",
    });
    try std.testing.expectEqual(cli.exit_ok, status);
    const written = try tmp.dir.readFileAlloc(std.testing.allocator, "seed.jpg", 4096);
    defer std.testing.allocator.free(written);
    try std.testing.expectEqual(@as(usize, 346), written.len);
}

test "an existing seed is overwritten rather than appended to" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    try tmp.dir.writeFile(.{ .sub_path = "seed.jpg", .data = "x" ** 5000 });
    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "-o", "seed.jpg",
    });
    try std.testing.expectEqual(cli.exit_ok, status);
    const written = try tmp.dir.readFileAlloc(std.testing.allocator, "seed.jpg", 8192);
    defer std.testing.allocator.free(written);
    try std.testing.expectEqual(@as(usize, 346), written.len);
}

test "an unrecognised flag is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--depth", "8",
    });
    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectEqualStrings("", streams.out.items);
    try std.testing.expect(std.mem.indexOf(u8, streams.err.items, "--depth") != null);
}

test "a stray positional argument is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{ "gen_jpeg_fixture", "seed.jpg" });
    try std.testing.expectEqual(cli.exit_usage, status);
}

test "a --width with no value is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{ "gen_jpeg_fixture", "--width" });
    try std.testing.expectEqual(cli.exit_usage, status);
}

test "an -o with no value is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{ "gen_jpeg_fixture", "-o" });
    try std.testing.expectEqual(cli.exit_usage, status);
}

test "a non-integer dimension is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--height", "eight",
    });
    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expect(std.mem.indexOf(u8, streams.err.items, "eight") != null);
}

test "a zero dimension fails, and fails differently from a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{ "gen_jpeg_fixture", "--width", "0" });
    try std.testing.expectEqual(cli.exit_error, status);
    try std.testing.expectEqualStrings("", streams.out.items);
    try std.testing.expect(std.mem.indexOf(u8, streams.err.items, "1..65535") != null);
}

test "a dimension above the SOF0 field fails" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--height", "65536",
    });
    try std.testing.expectEqual(cli.exit_error, status);
}

test "a negative dimension fails as out of range, not as a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--width", "-1",
    });
    try std.testing.expectEqual(cli.exit_error, status);
}

test "an unwritable output path fails without touching stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "-o", "no_such_dir/seed.jpg",
    });
    try std.testing.expectEqual(cli.exit_error, status);
    try std.testing.expectEqualStrings("", streams.out.items);
    try std.testing.expect(std.mem.indexOf(u8, streams.err.items, "no_such_dir/seed.jpg") != null);
}

test "no seed is left behind when the dimensions are refused" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--width", "0", "-o", "seed.jpg",
    });
    try std.testing.expectEqual(cli.exit_error, status);
    try std.testing.expectError(error.FileNotFound, tmp.dir.access("seed.jpg", .{}));
}

test "--help prints the usage line and succeeds" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{ "gen_jpeg_fixture", "--help" });
    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expect(std.mem.indexOf(u8, streams.out.items, "usage:") != null);
    try std.testing.expectEqualStrings("", streams.err.items);
}

test "the five committed corpus sizes all succeed" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const sizes = [_][2][]const u8{
        .{ "8", "8" },
        .{ "16", "16" },
        .{ "32", "24" },
        .{ "64", "64" },
        .{ "1", "1" },
    };
    for (sizes) |size| {
        var streams = Streams.init(std.testing.allocator);
        defer streams.deinit();
        const status = try run(tmp.dir, &streams, &[_][]const u8{
            "gen_jpeg_fixture", "--width", size[0], "--height", size[1], "-o", "seed.jpg",
        });
        try std.testing.expectEqual(cli.exit_ok, status);
        const written = try tmp.dir.readFileAlloc(std.testing.allocator, "seed.jpg", 4096);
        defer std.testing.allocator.free(written);
        try std.testing.expectEqual(@as(usize, 346), written.len);
    }
}

test "an abbreviated option is refused rather than guessed at" {
    // argparse accepted prefix abbreviations and exited 0 here (measured
    // against the deleted implementation). Guessing which option `--wid` meant
    // is worse than refusing it, and no caller in the tree uses one.
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "--wid", "8",
    });
    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectEqualStrings("", streams.out.items);
    try std.testing.expect(std.mem.indexOf(u8, streams.err.items, "--wid") != null);
}

test "an attached short-option value is refused" {
    // argparse read `-oseed.jpg` as the path `seed.jpg` and exited 0; here it
    // is a usage error, so a seed is never written to a surprising path.
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "-oseed.jpg",
    });
    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectError(error.FileNotFound, tmp.dir.access("seed.jpg", .{}));
}

test "-o=PATH is refused rather than written to a literal =PATH" {
    // argparse handed `-o=seed.jpg` through as the path `=seed.jpg`, which is
    // a file nobody asked for. Exit 2 and write nothing.
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &[_][]const u8{
        "gen_jpeg_fixture", "-o=seed.jpg",
    });
    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectError(error.FileNotFound, tmp.dir.access("=seed.jpg", .{}));
}
