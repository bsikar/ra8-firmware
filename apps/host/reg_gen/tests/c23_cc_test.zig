//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Unit tests for the C23 front-end resolution order (#899).

const std = @import("std");
const c23_cc = @import("c23_cc.zig");

fn joined(allocator: std.mem.Allocator, argv: []const []const u8) ![]u8 {
    return std.mem.join(allocator, " ", argv);
}

test "clang-18 stays the first candidate when nothing is pinned" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const list = try c23_cc.candidates(arena.allocator(), .{});
    try std.testing.expect(list.len >= 1);
    try std.testing.expectEqual(@as(usize, 1), list[0].len);
    try std.testing.expectEqualStrings("clang-18", list[0][0]);
}

test "zig cc is appended last when the build passes a zig path" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const without = try c23_cc.candidates(allocator, .{});
    const with = try c23_cc.candidates(allocator, .{ .zig_exe = "/opt/zig/zig" });
    try std.testing.expectEqual(without.len + 1, with.len);
    const last = with[with.len - 1];
    try std.testing.expectEqual(@as(usize, 2), last.len);
    try std.testing.expectEqualStrings("/opt/zig/zig", last[0]);
    try std.testing.expectEqualStrings("cc", last[1]);
}

test "an empty zig path adds no candidate" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const without = try c23_cc.candidates(allocator, .{});
    const empty = try c23_cc.candidates(allocator, .{ .zig_exe = "" });
    try std.testing.expectEqual(without.len, empty.len);
}

test "a pinned RA8_C23_CC is the only candidate" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const list = try c23_cc.candidates(allocator, .{
        .override = "xcrun clang",
        .zig_exe = "/opt/zig/zig",
    });
    try std.testing.expectEqual(@as(usize, 1), list.len);
    try std.testing.expectEqualStrings("xcrun clang", try joined(allocator, list[0]));
}

test "a pinned value keeps its arguments and collapses runs of whitespace" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const argv = try c23_cc.splitCommand(arena.allocator(), "  ccache \t clang-18  --target=arm64-apple-macos\n");
    try std.testing.expectEqual(@as(usize, 3), argv.len);
    try std.testing.expectEqualStrings("ccache", argv[0]);
    try std.testing.expectEqualStrings("clang-18", argv[1]);
    try std.testing.expectEqualStrings("--target=arm64-apple-macos", argv[2]);
}

test "a blank RA8_C23_CC falls through to the default order" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const list = try c23_cc.candidates(arena.allocator(), .{ .override = "   \t\n" });
    try std.testing.expectEqual(c23_cc.default_names.len, list.len);
    try std.testing.expectEqualStrings("clang-18", list[0][0]);
}

test "the default order is clang-18 first then widening fallbacks" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const list = try c23_cc.candidates(arena.allocator(), .{});
    try std.testing.expectEqual(c23_cc.default_names.len, list.len);
    for (list, c23_cc.default_names) |argv, expected| {
        try std.testing.expectEqual(@as(usize, 1), argv.len);
        try std.testing.expectEqualStrings(expected, argv[0]);
    }
}

test "the probe source uses the C23 static_assert keyword without assert.h" {
    try std.testing.expect(std.mem.indexOf(u8, c23_cc.probe_source, "static_assert(") != null);
    try std.testing.expect(std.mem.indexOf(u8, c23_cc.probe_source, "assert.h") == null);
}

test "the compile flags keep -std=c23 and the warnings-as-errors contract" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const text = try joined(arena.allocator(), &c23_cc.compile_args);
    try std.testing.expectEqualStrings("-std=c23 -Wall -Wextra -Werror -fsyntax-only -x c-header -", text);
}
