//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

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
    return reader.interface.allocRemaining(std.testing.allocator, .limited(8 * 1024 * 1024));
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

test "--help exits 0 and prints usage with budget ceiling" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "--help" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "usage: reader_vmem") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stderr, "compiled maximum frame budget:") != null);
    try std.testing.expectError(
        error.FileNotFound,
        tmp.dir.statFile(std.testing.io, "reader_vmem.trace", .{ .follow_symlinks = false }),
    );
}

test "explicit trace output and frame budget emit deterministic trace" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const r1 = try runTool(tmp.dir, &.{ tool_options.tool_exe, "a.trace", "64" });
    defer freeResult(r1);
    try std.testing.expectEqual(@as(u8, 0), r1.term.exited);

    const r2 = try runTool(tmp.dir, &.{ tool_options.tool_exe, "b.trace", "64" });
    defer freeResult(r2);
    try std.testing.expectEqual(@as(u8, 0), r2.term.exited);

    const trace_a = try readAll(tmp.dir, "a.trace");
    defer std.testing.allocator.free(trace_a);
    const trace_b = try readAll(tmp.dir, "b.trace");
    defer std.testing.allocator.free(trace_b);

    try std.testing.expect(trace_a.len > 0);
    try std.testing.expectEqualStrings(trace_a, trace_b);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".reader_vmem.tmp."));
}

test "default invocation writes reader_vmem.trace in working dir" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "reader_vmem.trace", .{ .follow_symlinks = false });
    try std.testing.expect(stat.size > 0);
}

test "invalid or extra arguments exit with status 2" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const r_zero = try runTool(tmp.dir, &.{ tool_options.tool_exe, "out.trace", "0" });
    defer freeResult(r_zero);
    try std.testing.expectEqual(@as(u8, 2), r_zero.term.exited);

    const r_bad = try runTool(tmp.dir, &.{ tool_options.tool_exe, "out.trace", "abc" });
    defer freeResult(r_bad);
    try std.testing.expectEqual(@as(u8, 2), r_bad.term.exited);

    const r_extra = try runTool(tmp.dir, &.{ tool_options.tool_exe, "out.trace", "64", "extra" });
    defer freeResult(r_extra);
    try std.testing.expectEqual(@as(u8, 2), r_extra.term.exited);
    try std.testing.expectError(
        error.FileNotFound,
        tmp.dir.statFile(std.testing.io, "out.trace", .{ .follow_symlinks = false }),
    );
}

test "excessive budget exits 1 and preserves existing destination" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const sentinel = "preserve-trace-contents\n";
    try writeFile(tmp.dir, "keep.trace", sentinel);

    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "keep.trace", "999999" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const after = try readAll(tmp.dir, "keep.trace");
    defer std.testing.allocator.free(after);
    try std.testing.expectEqualStrings(sentinel, after);
}

test "directory target fails while preserving directory without temp leak" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDir(std.testing.io, "trace_dir", .default_dir);
    const result = try runTool(tmp.dir, &.{ tool_options.tool_exe, "trace_dir", "64" });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "trace_dir", .{ .follow_symlinks = false });
    try std.testing.expectEqual(std.Io.File.Kind.directory, stat.kind);
    try std.testing.expect(!try hasTempPrefix(tmp.dir, ".reader_vmem.tmp."));
}
