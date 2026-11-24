//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

const expected_size: u64 = 67_108_864;
const golden_sha256 = "d3068af77071871baeaf77748519aaba2d12cdd236b9cd32fd22338aa477c771";

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

test "positional output writes golden 64 MiB exFAT image" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "showcase.img" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "wrote showcase.img") != null);

    const summary = try fileSha256AndLen(tmp.dir, "showcase.img");
    try std.testing.expectEqual(expected_size, summary.size);
    try std.testing.expectEqualStrings(golden_sha256, &summary.hex);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".exfat_mkimage.tmp."));
}

test "omitted argument writes default exfat_showcase.img matching golden hash" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    const summary = try fileSha256AndLen(tmp.dir, "exfat_showcase.img");
    try std.testing.expectEqual(expected_size, summary.size);
    try std.testing.expectEqualStrings(golden_sha256, &summary.hex);
}

test "extra argument exits with status 2 without creating destination" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "unwritten.img", "extra" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 2), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "usage: exfat_mkimage") != null);
    try std.testing.expectError(
        error.FileNotFound,
        tmp.dir.statFile(std.testing.io, "unwritten.img", .{ .follow_symlinks = false }),
    );
}

test "missing parent directory fails cleanly with status 1" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "missing_dir/out.img" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".exfat_mkimage.tmp."));
}

test "directory target fails while preserving directory and leaving no temp" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDir(std.testing.io, "out_dir", .default_dir);
    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "out_dir" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "out_dir", .{ .follow_symlinks = false });
    try std.testing.expectEqual(std.Io.File.Kind.directory, stat.kind);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".exfat_mkimage.tmp."));
}

test "existing regular file is atomically replaced with golden image" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var file = try tmp.dir.createFile(std.testing.io, "replace.img", .{});
    try file.writeStreamingAll(std.testing.io, "stale-content");
    file.close(std.testing.io);

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "replace.img" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    const summary = try fileSha256AndLen(tmp.dir, "replace.img");
    try std.testing.expectEqual(expected_size, summary.size);
    try std.testing.expectEqualStrings(golden_sha256, &summary.hex);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".exfat_mkimage.tmp."));
}
