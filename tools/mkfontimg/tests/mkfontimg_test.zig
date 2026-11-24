//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

const expected_size: u64 = 4_194_304;
const blank_sha256 = "1f5720358302c8a9d329b85f698fa1e86aae6d29042e294f48ebfd5b253107c1";
const font_sha256 = "c293f3bff8508dcc81d74e1d6b427f372d89baac9fc4ea4aac8af75a2b876567";

fn runTool(dir: std.Io.Dir, argv: []const []const u8) !std.process.RunResult {
    const io = std.testing.io;
    const exe_abs = try std.Io.Dir.cwd().realPathFileAlloc(io, argv[0], std.testing.allocator);
    defer std.testing.allocator.free(exe_abs);

    const resolved = try std.testing.allocator.dupe([]const u8, argv);
    defer std.testing.allocator.free(resolved);
    resolved[0] = exe_abs;

    return std.process.run(std.testing.allocator, io, .{
        .argv = resolved,
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

test "--blank writes golden 4 MiB blank FAT16 image" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--blank", "blank.img" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    const summary = try fileSha256AndLen(tmp.dir, "blank.img");
    try std.testing.expectEqual(expected_size, summary.size);
    try std.testing.expectEqualStrings(blank_sha256, &summary.hex);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkfontimg.tmp."));
}

test "valid font payload writes golden 4 MiB FAT16 image" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const font_bytes: [9000]u8 = @splat('F');
    try writeFile(tmp.dir, "font.bin", &font_bytes);

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "font.bin",
        "font.img",
        "FONT.OTF",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    const summary = try fileSha256AndLen(tmp.dir, "font.img");
    try std.testing.expectEqual(expected_size, summary.size);
    try std.testing.expectEqualStrings(font_sha256, &summary.hex);
}

test "invalid argument counts exit with status 2 and usage" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const r1 = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--blank" });
    defer freeResult(r1);
    try std.testing.expectEqual(@as(u8, 2), r1.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, r1.stderr, "usage: mkfontimg --blank") != null);

    const r2 = try runTool(tmp.dir, &.{ tool_options.tool_exe, "only_one" });
    defer freeResult(r2);
    try std.testing.expectEqual(@as(u8, 2), r2.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, r2.stderr, "usage: mkfontimg <font-in>") != null);
}

test "undersized and oversized font inputs fail and preserve output" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const sentinel = "preserve-on-bad-font\n";
    try writeFile(tmp.dir, "preserved.img", sentinel);
    try writeFile(tmp.dir, "small.bin", "too-small");

    const small_res = try runTool(tmp.dir, &.{ tool_options.tool_exe, "small.bin", "preserved.img" });
    defer freeResult(small_res);
    try std.testing.expectEqual(@as(u8, 1), small_res.term.exited);

    var big_file = try tmp.dir.createFile(std.testing.io, "big.bin", .{});
    const zeros: [4096]u8 = @splat(0);
    for (0..1024) |_| try big_file.writeStreamingAll(std.testing.io, &zeros);
    try big_file.writeStreamingAll(std.testing.io, &.{0});
    big_file.close(std.testing.io);

    const big_res = try runTool(tmp.dir, &.{ tool_options.tool_exe, "big.bin", "preserved.img" });
    defer freeResult(big_res);
    try std.testing.expectEqual(@as(u8, 1), big_res.term.exited);

    const after = try readAll(tmp.dir, "preserved.img");
    defer std.testing.allocator.free(after);
    try std.testing.expectEqualStrings(sentinel, after);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkfontimg.tmp."));
}

test "invalid FAT filename exits 1 and preserves existing output" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const sentinel = "preserve-on-bad-name\n";
    try writeFile(tmp.dir, "preserved.img", sentinel);
    const font_bytes: [64]u8 = @splat('G');
    try writeFile(tmp.dir, "font.bin", &font_bytes);

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "font.bin",
        "preserved.img",
        "NOT/A/NAME",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const after = try readAll(tmp.dir, "preserved.img");
    defer std.testing.allocator.free(after);
    try std.testing.expectEqualStrings(sentinel, after);
}

test "directory output exits 1 and preserves directory without temp leak" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDir(std.testing.io, "out_dir", .default_dir);
    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--blank", "out_dir" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "out_dir", .{ .follow_symlinks = false });
    try std.testing.expectEqual(std.Io.File.Kind.directory, stat.kind);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".mkfontimg.tmp."));
}
