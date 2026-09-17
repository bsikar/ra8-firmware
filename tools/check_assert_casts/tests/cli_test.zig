//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and stream contract tests for the redundant-cast gate (#858).
//! `cli.run` takes a directory handle, a repository root and both streams, so
//! each status below is proved against a real tree with no process involved.

const std = @import("std");
const cli = @import("cli");

const Result = struct {
    status: u8,
    out: []const u8,
    err: []const u8,
};

fn invoke(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
) !Result {
    var out = std.ArrayList(u8).init(allocator);
    var err = std.ArrayList(u8).init(allocator);
    const status = try cli.run(allocator, dir, repo_root, argv, out.writer(), err.writer());
    return .{ .status = status, .out = try out.toOwnedSlice(), .err = try err.toOwnedSlice() };
}

const bad_source = "TEST_ASSERT_EQ((int)value, (uint32_t)expected);\n";
const good_source = "TEST_ASSERT_EQ(value, expected);\nTEST_ASSERT_EQ(load((int)value), expected);\n";

test "no arguments prints usage and exits 1" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "usage: check_assert_casts <file> [...] or check_assert_casts --all\n",
        result.err,
    );
}

test "the selftest proves both directions and exits 0" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--selftest"});
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "  [ok] leading casts on both arguments fire\n" ++
            "  [ok] clean and nested casts stay quiet\n" ++
            "check_assert_casts --selftest: all cases pass (both directions).\n",
        result.out,
    );
}

test "an unknown flag exits 2" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--jobs"});
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("check_assert_casts: unknown or incompatible arguments\n", result.err);
}

test "--all beside a file is incompatible and exits 2" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{ "--all", "x.c" });
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a clean file exits 0 with no output" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "good.c", .data = good_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"good.c"});
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings("", result.out);
    try std.testing.expectEqualStrings("", result.err);
}

test "a file with leading casts exits 1 and prints both rows" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "bad.c", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"bad.c"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "bad.c:1: cast in first arg of TEST_ASSERT_EQ: TEST_ASSERT_EQ((int)value...\n" ++
            "bad.c:1: cast in second arg of TEST_ASSERT_EQ: ...(uint32_t)expected\n",
        result.out,
    );
}

test "the failure summary names the fixer" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "bad.c", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"bad.c"});
    try std.testing.expectEqualStrings(
        "\n2 redundant cast(s) in TEST_ASSERT_EQ.\nRun scripts/fix/strip_assert_casts.py to fix automatically.\n",
        result.err,
    );
}

test "an unreadable source exits 1 rather than reporting clean" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"absent.c"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings("check_assert_casts: cannot read absent.c\n", result.err);
}

test "a row quotes the path as normalised, not as typed" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "bad.c", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"./bad.c"});
    try std.testing.expect(std.mem.startsWith(u8, result.out, "bad.c:1:"));
}

test "a CRLF source numbers its lines after translation" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "crlf.c", .data = "one\r\ntwo\r\nTEST_ASSERT_EQ((int)a, b);\r\n" });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"crlf.c"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.startsWith(u8, result.out, "crlf.c:3:"));
}

test "several files aggregate into one report" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "one.c", .data = "TEST_ASSERT_EQ((int)a, b);\n" });
    try tmp.dir.writeFile(.{ .sub_path = "two.c", .data = "TEST_ASSERT_EQ(a, (int)b);\n" });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{ "one.c", "two.c" });
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "one.c:1:") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "two.c:1:") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "2 redundant cast(s)") != null);
}

test "a clean file beside a dirty one still reports only the dirty rows" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "good.c", .data = good_source });
    try tmp.dir.writeFile(.{ .sub_path = "bad.c", .data = "TEST_ASSERT_EQ((int)a, b);\n" });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{ "good.c", "bad.c" });
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "good.c") == null);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "1 redundant cast(s)") != null);
}

test "--all sweeps the tests tree" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("tests/unit");
    try tmp.dir.writeFile(.{ .sub_path = "tests/unit/test_a.c", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--all"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "tests/unit/test_a.c:1:") != null);
}

test "--all ignores sources outside the tests tree" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("tests");
    try tmp.dir.writeFile(.{ .sub_path = "tests/test_ok.c", .data = good_source });
    try tmp.dir.writeFile(.{ .sub_path = "elsewhere.c", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--all"});
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "--all with no tests tree exits 1 with usage" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--all"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "usage: check_assert_casts") != null);
}

test "--all reports its targets in sorted order" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("tests/b");
    try tmp.dir.makePath("tests/a");
    try tmp.dir.writeFile(.{ .sub_path = "tests/b/test_b.c", .data = "TEST_ASSERT_EQ((int)b, x);\n" });
    try tmp.dir.writeFile(.{ .sub_path = "tests/a/test_a.c", .data = "TEST_ASSERT_EQ((int)a, x);\n" });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--all"});
    const first = std.mem.indexOf(u8, result.out, "tests/a/test_a.c").?;
    const second = std.mem.indexOf(u8, result.out, "tests/b/test_b.c").?;
    try std.testing.expect(first < second);
}

test "--all ignores non-C files in the tests tree" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("tests");
    try tmp.dir.writeFile(.{ .sub_path = "tests/notes.md", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--all"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "usage: check_assert_casts") != null);
}

test "an invocation with no top-level comma passes through the CLI quietly" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "odd.c", .data = "TEST_ASSERT_EQ((int)value);\n" });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"odd.c"});
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a cast on the second argument alone reports once" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "one.c", .data = "TEST_ASSERT_EQ(value, (size_t)expected);\n" });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"one.c"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "one.c:1: cast in second arg of TEST_ASSERT_EQ: ...(size_t)expected\n",
        result.out,
    );
}

test "--all orders siblings by path component, not by joined bytes" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    // `a-b` sorts BELOW `a` on joined bytes ('-' < '/') and ABOVE it on the
    // component tuple pathlib compared, so this pair pins the ordering the
    // deleted Python gate produced.
    try tmp.dir.makePath("tests/a");
    try tmp.dir.makePath("tests/a-b");
    try tmp.dir.writeFile(.{ .sub_path = "tests/a/test_inner.c", .data = bad_source });
    try tmp.dir.writeFile(.{ .sub_path = "tests/a-b/test_outer.c", .data = bad_source });
    const result = try invoke(arena.allocator(), tmp.dir, ".", &[_][]const u8{"--all"});
    try std.testing.expectEqual(@as(u8, 1), result.status);
    const inner = std.mem.indexOf(u8, result.out, "tests/a/test_inner.c").?;
    const outer = std.mem.indexOf(u8, result.out, "tests/a-b/test_outer.c").?;
    try std.testing.expect(inner < outer);
}
