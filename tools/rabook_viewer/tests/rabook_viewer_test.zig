//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

const sample_cbz_ppm_sha256 = "003c60d6ff5b4f1ea4671193c278817878223605aabcafc2dab7e47927a4665d";

fn runTool(dir: std.Io.Dir, argv: []const []const u8) !std.process.RunResult {
    const io = std.testing.io;
    const cwd = std.Io.Dir.cwd();
    const exe_abs = try cwd.realPathFileAlloc(io, tool_options.tool_exe, std.testing.allocator);
    defer std.testing.allocator.free(exe_abs);
    const cbz_abs = try cwd.realPathFileAlloc(io, tool_options.sample_cbz, std.testing.allocator);
    defer std.testing.allocator.free(cbz_abs);

    const resolved = try std.testing.allocator.dupe([]const u8, argv);
    defer std.testing.allocator.free(resolved);
    for (resolved, 0..) |arg, idx| {
        if (idx == 0 and std.mem.eql(u8, arg, tool_options.tool_exe)) {
            resolved[idx] = exe_abs;
        } else if (std.mem.eql(u8, arg, tool_options.sample_cbz)) {
            resolved[idx] = cbz_abs;
        }
    }

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

fn fileSha256(dir: std.Io.Dir, sub_path: []const u8) ![64]u8 {
    const io = std.testing.io;
    const file = try dir.openFile(io, sub_path, .{});
    defer file.close(io);

    var hasher = std.crypto.hash.sha2.Sha256.init(.{});
    var buf: [65_536]u8 = undefined;
    while (true) {
        const n = file.readStreaming(io, &.{&buf}) catch |err| switch (err) {
            error.EndOfStream => break,
            else => |other| return other,
        };
        if (n == 0) break;
        hasher.update(buf[0..n]);
    }
    var digest: [32]u8 = undefined;
    hasher.final(&digest);
    return std.fmt.bytesToHex(digest, .lower);
}

fn readPrefix(dir: std.Io.Dir, sub_path: []const u8, out: []u8) ![]u8 {
    const io = std.testing.io;
    const file = try dir.openFile(io, sub_path, .{});
    defer file.close(io);
    var total: usize = 0;
    while (total < out.len) {
        const n = file.readStreaming(io, &.{out[total..]}) catch |err| switch (err) {
            error.EndOfStream => break,
            else => |other| return other,
        };
        if (n == 0) break;
        total += n;
    }
    return out[0..total];
}

test "headless CBZ render writes golden P6 PPM" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        tool_options.sample_cbz,
        "--headless",
        "--dump-ppm",
        "page0.ppm",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    var head: [3]u8 = undefined;
    const prefix = try readPrefix(tmp.dir, "page0.ppm", &head);
    try std.testing.expectEqualStrings("P6\n", prefix);
    const hex = try fileSha256(tmp.dir, "page0.ppm");
    try std.testing.expectEqualStrings(sample_cbz_ppm_sha256, &hex);
}

test "out-of-range --page clamps cleanly to final CBZ page" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        tool_options.sample_cbz,
        "--page",
        "999999",
        "--headless",
        "--dump-ppm",
        "clamped.ppm",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    var head: [3]u8 = undefined;
    const prefix = try readPrefix(tmp.dir, "clamped.ppm", &head);
    try std.testing.expectEqualStrings("P6\n", prefix);
}

test "missing CLI arguments exit with status 2 and usage" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 2), result.term.exited);
    try std.testing.expect(result.stderr.len > 0);
}

test "unknown or incomplete CLI flags exit with status 2" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const r_unk = try runTool(tmp.dir, &.{ tool_options.tool_exe, tool_options.sample_cbz, "--bogus" });
    defer freeResult(r_unk);
    try std.testing.expectEqual(@as(u8, 2), r_unk.term.exited);

    const r_val = try runTool(tmp.dir, &.{ tool_options.tool_exe, tool_options.sample_cbz, "--dump-ppm" });
    defer freeResult(r_val);
    try std.testing.expectEqual(@as(u8, 2), r_val.term.exited);
}

test "missing input file exits with status 1 without creating PPM" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        "missing.cbz",
        "--headless",
        "--dump-ppm",
        "out.ppm",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    try std.testing.expectError(
        error.FileNotFound,
        tmp.dir.statFile(std.testing.io, "out.ppm", .{ .follow_symlinks = false }),
    );
}

test "directory --dump-ppm target fails cleanly without damaging directory" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDir(std.testing.io, "ppm_dir", .default_dir);
    const result = try runTool(tmp.dir, &.{
        tool_options.tool_exe,
        tool_options.sample_cbz,
        "--headless",
        "--dump-ppm",
        "ppm_dir",
    });
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 1), result.term.exited);
    const stat = try tmp.dir.statFile(std.testing.io, "ppm_dir", .{ .follow_symlinks = false });
    try std.testing.expectEqual(std.Io.File.Kind.directory, stat.kind);
}
