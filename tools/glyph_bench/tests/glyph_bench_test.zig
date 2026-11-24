//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tool_options = @import("tool_options");

const expected_budgets = [_][]const u8{
    "|    16 |          576 KiB |",
    "|    32 |         1152 KiB |",
    "|    48 |         1728 KiB |",
    "|    64 |         2304 KiB |",
    "|    96 |         3456 KiB |",
    "|   128 |         4608 KiB |",
    "|   192 |         6912 KiB |",
    "|   256 |         9216 KiB |",
};

fn runTool(argv: []const []const u8) !std.process.RunResult {
    return std.process.run(std.testing.allocator, std.testing.io, .{
        .argv = argv,
        .stderr_limit = .limited(1024 * 1024),
        .stdout_limit = .limited(1024 * 1024),
    });
}

fn freeResult(result: std.process.RunResult) void {
    std.testing.allocator.free(result.stdout);
    std.testing.allocator.free(result.stderr);
}

fn parseLastNumber(row: []const u8) !u64 {
    var it = std.mem.tokenizeAny(u8, row, "| \t\r\n");
    var last: ?[]const u8 = null;
    while (it.next()) |tok| last = tok;
    return std.fmt.parseInt(u64, last orelse return error.MissingToken, 10);
}

test "glyph_bench exits 0 with clean stderr and workload header" {
    const result = try runTool(&.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expectEqual(@as(usize, 0), result.stderr.len);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "# Glyph-cache budget sweep") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "1080000 glyph gets") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "Without a cache") != null);
}

test "report prints all eight cell budgets and exact RAM costs" {
    const result = try runTool(&.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    for (expected_budgets) |prefix| {
        try std.testing.expect(std.mem.indexOf(u8, result.stdout, prefix) != null);
    }
}

test "rasterisations decrease monotonically across larger cell budgets" {
    const result = try runTool(&.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    var prev_rasters: u64 = 1_080_001;
    var lines = std.mem.splitScalar(u8, result.stdout, '\n');
    var rows_seen: usize = 0;
    while (lines.next()) |line| {
        if (!std.mem.startsWith(u8, line, "|    ") and !std.mem.startsWith(u8, line, "|   ")) continue;
        const rasters = try parseLastNumber(line);
        try std.testing.expect(rasters <= prev_rasters);
        try std.testing.expect(rasters <= 1_080_000);
        prev_rasters = rasters;
        rows_seen += 1;
    }
    try std.testing.expectEqual(@as(usize, expected_budgets.len), rows_seen);
}

test "consecutive runs emit byte-identical deterministic reports" {
    const r1 = try runTool(&.{tool_options.tool_exe});
    defer freeResult(r1);
    const r2 = try runTool(&.{tool_options.tool_exe});
    defer freeResult(r2);

    try std.testing.expectEqual(@as(u8, 0), r1.term.exited);
    try std.testing.expectEqual(@as(u8, 0), r2.term.exited);
    try std.testing.expectEqualStrings(r1.stdout, r2.stdout);
}

test "extra CLI argument preserves deterministic zero-exit sweep" {
    const r1 = try runTool(&.{tool_options.tool_exe});
    defer freeResult(r1);
    const r2 = try runTool(&.{ tool_options.tool_exe, "--ignored" });
    defer freeResult(r2);

    try std.testing.expectEqual(@as(u8, 0), r2.term.exited);
    try std.testing.expectEqual(@as(usize, 0), r2.stderr.len);
    try std.testing.expectEqualStrings(r1.stdout, r2.stdout);
}

test "256-cell budget keeps exact floor of 142 unique rasterisations" {
    const result = try runTool(&.{tool_options.tool_exe});
    defer freeResult(result);

    try std.testing.expectEqual(@as(u8, 0), result.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, result.stdout, "|   256 |         9216 KiB |      99.99 |            142 |") != null);
}
