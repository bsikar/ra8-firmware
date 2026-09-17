//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract of `merge_ihex` (#858). The build callers in
//! `apps/board/stand_alone/ereader`, `tz_nsc_cgc_usb`, `tz_threadx_demo` and
//! `secure_boot_ns_hil` treat a non-zero status as a failed image, so these
//! cases pin 0 / 1 / 2 and the stream each message lands on.

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

test "exit 2 and usage on stderr when the argument count is wrong" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{ "merge_ihex", "only_one.hex" });

    try std.testing.expectEqual(cli.exit_usage, status);
    try std.testing.expectEqualStrings(cli.usage ++ "\n", streams.err.items);
    try std.testing.expectEqualStrings("", streams.out.items);
}

test "exit 2 when an extra argument is supplied" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    const status = try run(tmp.dir, &streams, &.{ "merge_ihex", "a.hex", "b.hex", "out.hex", "extra" });

    try std.testing.expectEqual(cli.exit_usage, status);
}

test "exit 1 and a named path on stderr when an input is missing" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    try tmp.dir.writeFile(.{ .sub_path = "a.hex", .data = ":00000001FF\n" });

    const status = try run(tmp.dir, &streams, &.{ "merge_ihex", "a.hex", "missing.hex", "out.hex" });

    try std.testing.expectEqual(cli.exit_io_error, status);
    try std.testing.expect(std.mem.startsWith(u8, streams.err.items, "merge_ihex: missing.hex:"));
    try std.testing.expectEqualStrings("", streams.out.items);
}

test "a failed merge writes no output file" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    _ = try run(tmp.dir, &streams, &.{ "merge_ihex", "missing.hex", "missing.hex", "out.hex" });

    try std.testing.expectError(error.FileNotFound, tmp.dir.access("out.hex", .{}));
}

test "exit 0 writes the merged image and reports the record count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    try tmp.dir.writeFile(.{ .sub_path = "secure.hex", .data = ":020000040200F8\n:10000000AA33\n:00000001FF\n" });
    try tmp.dir.writeFile(.{ .sub_path = "ns.hex", .data = ":020000040208F0\n:10000000BB22\n:00000001FF\n" });

    const status = try run(tmp.dir, &streams, &.{ "merge_ihex", "secure.hex", "ns.hex", "merged.hex" });

    try std.testing.expectEqual(cli.exit_ok, status);
    try std.testing.expectEqualStrings("merge_ihex: wrote 4 records -> merged.hex\n", streams.out.items);
    try std.testing.expectEqualStrings("", streams.err.items);

    const written = try tmp.dir.readFileAlloc(std.testing.allocator, "merged.hex", 4096);
    defer std.testing.allocator.free(written);
    try std.testing.expectEqualStrings(
        ":020000040200F8\n:10000000AA33\n:020000040208F0\n:10000000BB22\n:00000001FF\n",
        written,
    );
}

test "the output path may be one of the inputs" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    try tmp.dir.writeFile(.{ .sub_path = "app.hex", .data = ":10000000AA33\n:00000001FF\n" });
    try tmp.dir.writeFile(.{ .sub_path = "ns.hex", .data = ":10001000BB22\n:00000001FF\n" });

    const status = try run(tmp.dir, &streams, &.{ "merge_ihex", "app.hex", "ns.hex", "app.hex" });

    try std.testing.expectEqual(cli.exit_ok, status);
    const written = try tmp.dir.readFileAlloc(std.testing.allocator, "app.hex", 4096);
    defer std.testing.allocator.free(written);
    try std.testing.expectEqualStrings(":10000000AA33\n:10001000BB22\n:00000001FF\n", written);
}

test "an unwritable output path exits 1 rather than reporting success" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var streams = Streams.init(std.testing.allocator);
    defer streams.deinit();

    try tmp.dir.writeFile(.{ .sub_path = "a.hex", .data = ":00000001FF\n" });

    const status = try run(tmp.dir, &streams, &.{ "merge_ihex", "a.hex", "a.hex", "no_such_dir/out.hex" });

    try std.testing.expectEqual(cli.exit_io_error, status);
    try std.testing.expect(std.mem.startsWith(u8, streams.err.items, "merge_ihex: no_such_dir/out.hex:"));
}
