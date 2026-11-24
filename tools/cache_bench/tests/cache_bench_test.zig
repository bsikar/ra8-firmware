//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

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
        .stdout_limit = .limited(4 * 1024 * 1024),
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
    return reader.interface.allocRemaining(std.testing.allocator, .limited(4 * 1024 * 1024));
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

test "captured trace and --output write complete benchmark markdown" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try writeFile(tmp.dir, "trace.txt", "0 1\n0 2\n0 1\n0 3\n");
    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "custom_workload=trace.txt",
        "--output=report.md",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expectEqual(@as(usize, 0), result.stderr.len);

    const report = try readAll(tmp.dir, "report.md");
    defer std.testing.allocator.free(report);
    try std.testing.expect(std.mem.indexOf(u8, report, "# Eviction-policy benchmark") != null);
    try std.testing.expect(std.mem.indexOf(u8, report, "### custom_workload") != null);
    try std.testing.expect(std.mem.indexOf(u8, report, "4 accesses") != null);
    try std.testing.expect(std.mem.indexOf(u8, report, "SLRU") != null);
    try std.testing.expect(std.mem.indexOf(u8, report, "SRRIP") != null);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".cache_bench.tmp."));
}

test "omitting --output prints synthetic workloads to stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "# Eviction-policy benchmark") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "accesses, footprint") != null);
}

test "empty --output fails with exit 1 and error diagnostic" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--output=" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "output open failed") != null);
}

test "missing parent directory --output fails without temp leak" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--output=missing/report.md" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "output open failed") != null);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".cache_bench.tmp."));
}

test "directory --output fails while preserving directory intact" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDir(std.testing.io, "out_dir", .default_dir);
    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--output=out_dir" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "out_dir", .{ .follow_symlinks = false });
    try std.testing.expectEqual(std.Io.File.Kind.directory, stat.kind);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".cache_bench.tmp."));
}

test "--sweep-block rejects unwritable output path cleanly" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "--sweep-block",
        "--output=missing_dir/sweep.md",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "output open failed") != null);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".cache_bench.tmp."));
}
