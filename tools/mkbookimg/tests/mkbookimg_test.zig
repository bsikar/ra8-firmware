//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

const expected_size: u64 = 67_108_864;
const golden_sha256 = "0a028a558cac7ced26b928f1f9c9d8d8cd36dc364f970a387eb5c429e48e4f73";

fn runTool(dir: std.Io.Dir, argv: []const []const u8) !std.process.RunResult {
    return std.process.run(std.testing.allocator, std.testing.io, .{
        .argv = argv,
        .cwd = .{ .dir = dir },
        .stderr_limit = .limited(1024 * 1024),
        .stdout_limit = .limited(1024 * 1024),
    });
}

fn freeResult(result: std.process.RunResult) void {
    std.testing.allocator.free(result.stdout);
    std.testing.allocator.free(result.stderr);
}

fn writeFile(dir: std.Io.Dir, name: []const u8, bytes: []const u8) !void {
    const io = std.testing.io;
    var file = try dir.createFile(io, name, .{});
    defer file.close(io);
    try file.writeStreamingAll(io, bytes);
}

fn readAll(dir: std.Io.Dir, name: []const u8) ![]u8 {
    const io = std.testing.io;
    const file = try dir.openFile(io, name, .{});
    defer file.close(io);
    var buffer: [4096]u8 = undefined;
    var reader = file.reader(io, &buffer);
    return reader.interface.allocRemaining(std.testing.allocator, .limited(1024 * 1024));
}

fn fileSha256AndLen(dir: std.Io.Dir, sub_path: []const u8) !struct { hex: [64]u8, size: u64 } {
    const io = std.testing.io;
    const file = try dir.openFile(io, sub_path, .{});
    defer file.close(io);

    var hasher = std.crypto.hash.sha2.Sha256.init(.{});
    var buf: [65_536]u8 = undefined;
    var total: u64 = 0;
    while (true) {
        const n = file.readStreaming(io, &.{&buf}) catch |err| switch (err) {
            error.EndOfStream => break,
            else => |other| return other,
        };
        if (n == 0) break;
        hasher.update(buf[0..n]);
        total += n;
    }
    var digest: [32]u8 = undefined;
    hasher.final(&digest);
    return .{ .hex = std.fmt.bytesToHex(digest, .lower), .size = total };
}

fn hasTempPrefix(dir: std.Io.Dir, prefix: []const u8) !bool {
    const io = std.testing.io;
    var iter_dir = try dir.openDir(io, ".", .{ .iterate = true });
    defer iter_dir.close(io);
    var it = iter_dir.iterate();
    while (try it.next(io)) |entry| {
        if (std.mem.startsWith(u8, entry.name, prefix)) return true;
    }
    return false;
}

test "two valid rabook inputs produce golden 64 MiB FAT32 image" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try writeFile(tmp.dir, "alpha.rabook", "RABOOK-GOLDEN-A\nline-two\n");
    const book_b: [9000]u8 = @splat('Z');
    try writeFile(tmp.dir, "Second Book.rabook", &book_b);

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "golden.img",
        "alpha.rabook",
        "Second Book.rabook",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "2 book(s), 64 MiB FAT32") != null);

    const summary = try fileSha256AndLen(tmp.dir, "golden.img");
    try std.testing.expectEqual(expected_size, summary.size);
    try std.testing.expectEqualStrings(golden_sha256, &summary.hex);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkbookimg.tmp."));
}

test "missing CLI arguments exit with status 2 and usage" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const r0 = try runTool(tmp.dir, &.{tool_options.tool_exe});
    defer freeResult(r0);
    try std.testing.expectEqual(@as(u8, 2), r0.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, r0.stderr, "usage: mkbookimg") != null);

    const r1 = try runTool(tmp.dir, &.{ tool_options.tool_exe, "out.img" });
    defer freeResult(r1);
    try std.testing.expectEqual(@as(u8, 2), r1.term.exited);
}

test "more than 32 books exits with status 2" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var argv: [35][]const u8 = undefined;
    argv[0] = tool_options.tool_exe;
    argv[1] = "out.img";
    for (argv[2..]) |*arg| arg.* = "book.rabook";

    const result = try runTool(tmp.dir, &argv);
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 2), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "too many books") != null);
}

test "missing input file exits 1 and preserves existing output" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const sentinel = "keep-existing-book-image\n";
    try writeFile(tmp.dir, "preserved.img", sentinel);

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "preserved.img",
        "missing.rabook",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const after = try readAll(tmp.dir, "preserved.img");
    defer std.testing.allocator.free(after);
    try std.testing.expectEqualStrings(sentinel, after);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkbookimg.tmp."));
}

test "empty input file exits 1 and preserves existing output" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const sentinel = "keep-on-empty-input\n";
    try writeFile(tmp.dir, "preserved.img", sentinel);
    try writeFile(tmp.dir, "empty.rabook", "");

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "preserved.img",
        "empty.rabook",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const after = try readAll(tmp.dir, "preserved.img");
    defer std.testing.allocator.free(after);
    try std.testing.expectEqualStrings(sentinel, after);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkbookimg.tmp."));
}

test "directory target exits 1 and preserves directory without temp leak" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try writeFile(tmp.dir, "alpha.rabook", "RABOOK-DATA\n");
    try tmp.dir.createDir(std.testing.io, "out_dir", .default_dir);

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "out_dir", "alpha.rabook" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "out_dir", .{ .follow_symlinks = false });
    try std.testing.expectEqual(std.Io.File.Kind.directory, stat.kind);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkbookimg.tmp."));
}
