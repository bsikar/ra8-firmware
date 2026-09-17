//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and enumeration tests for the GNU-attribute gate (#858,
//! #1178). Each case drives `cli.run` against a temporary tree with both
//! streams captured, so the contract
//! `scripts/builders/check_no_gnu_attribute.sh` passes through is pinned
//! without spawning a process.

const std = @import("std");
const cli = @import("cli");
const testing = std.testing;

const Captured = struct {
    status: u8,
    scanned: usize,
    findings: usize,
    out: []u8,
    err: []u8,

    fn deinit(self: *Captured) void {
        testing.allocator.free(self.out);
        testing.allocator.free(self.err);
    }
};

fn runIn(dir: std.fs.Dir, root: []const u8, args: []const []const u8) !Captured {
    var out = std.ArrayList(u8).init(testing.allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    errdefer err.deinit();
    const outcome = try cli.run(testing.allocator, dir, root, args, out.writer(), err.writer());
    return .{
        .status = outcome.status,
        .scanned = outcome.scanned,
        .findings = outcome.findings,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

fn write(dir: std.fs.Dir, path: []const u8, body: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = path, .data = body });
}

const bad_line = "void f(void) __attribute__((weak));\n";
const good_line = "[[gnu::weak]] void f(void);\n";

// --- the selftest ---------------------------------------------------------

test "a lone --selftest passes and exits 0" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{"--selftest"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "[ok] migratable GNU attribute fires") != null);
    try testing.expect(std.mem.indexOf(u8, result.out, "all cases pass (both directions)") != null);
    try testing.expectEqualStrings("", result.err);
}

test "the selftest needs no tree on disk" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{"--selftest"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 0), result.scanned);
}

// --- usage ---------------------------------------------------------------

test "--selftest with a trailing argument is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{ "--selftest", "libs/a.c" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expect(std.mem.startsWith(u8, result.err, "usage: check_no_gnu_attribute"));
    try testing.expectEqualStrings("", result.out);
}

test "an unknown flag is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{"--all"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
}

test "a bare dash is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{"-"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
}

test "a flag anywhere in a file list is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", good_line);
    var result = try runIn(tmp.dir, "/repo", &.{ "libs/a.c", "--selftest" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
}

test "usage precedes any scanning" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{ "libs/a.c", "-x" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expectEqualStrings("", result.out);
}

// --- an explicit file list ------------------------------------------------

test "a clean file list exits 0 with the clean line on stdout" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", good_line);
    var result = try runIn(tmp.dir, "/repo", &.{"libs/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 1), result.scanned);
    try testing.expect(std.mem.indexOf(u8, result.out, "clean -- all attributes use the C23") != null);
}

test "a finding exits 1 and prints the site then the summary on stdout" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{"libs/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqual(@as(usize, 1), result.findings);
    try testing.expect(std.mem.startsWith(u8, result.out, "libs/a.c:1: GNU __attribute__"));
    try testing.expect(std.mem.indexOf(u8, result.out, "check_no_gnu_attribute: 1 violation(s)") != null);
    try testing.expectEqualStrings("", result.err);
}

test "findings across files are counted together" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", bad_line);
    try write(tmp.dir, "libs/b.c", bad_line ++ bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{ "libs/a.c", "libs/b.c" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqual(@as(usize, 3), result.findings);
}

test "an argv list is sorted and de-duplicated" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", bad_line);
    try write(tmp.dir, "libs/b.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{ "libs/b.c", "libs/a.c", "libs/b.c" });
    defer result.deinit();
    try testing.expectEqual(@as(usize, 2), result.scanned);
    try testing.expectEqual(@as(usize, 2), result.findings);
    const first = std.mem.indexOf(u8, result.out, "libs/a.c").?;
    const second = std.mem.indexOf(u8, result.out, "libs/b.c").?;
    try testing.expect(first < second);
}

test "an argv list is exempt from the floor" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", good_line);
    var result = try runIn(tmp.dir, "/repo", &.{"libs/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "an argv list that filters to nothing exits 0" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/notes.md", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{"libs/notes.md"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 0), result.scanned);
}

test "a non-existent path with a scanned suffix is counted and stays quiet" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{"libs/missing.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 1), result.scanned);
}

test "a directory named like a source is read-failed and skipped" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("libs/weird.c");
    var result = try runIn(tmp.dir, "/repo", &.{"libs/weird.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "an undecodable source is skipped rather than reported" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/bin.c", "\xffvoid f(void) __attribute__((weak));\n");
    var result = try runIn(tmp.dir, "/repo", &.{"libs/bin.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 0), result.findings);
}

test "build output named on argv is filtered out" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "tests/build/a.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{"tests/build/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 0), result.scanned);
}

test "vendored SOUP named on argv is filtered out" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/third_party/x/a.c", bad_line);
    try write(tmp.dir, "libs/ra8_fonts/f.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{ "libs/third_party/x/a.c", "libs/ra8_fonts/f.c" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 0), result.scanned);
}

test "a source directory called build is NOT build output" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/build/a.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{"libs/build/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
}

test "an absolute path is normalised against the repo root before filtering" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", good_line);
    var result = try runIn(tmp.dir, "/repo", &.{"/repo/tests/build/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqual(@as(usize, 0), result.scanned);
}

test "line numbers in a report follow the file" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", good_line ++ good_line ++ bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{"libs/a.c"});
    defer result.deinit();
    try testing.expect(std.mem.startsWith(u8, result.out, "libs/a.c:3: GNU"));
}

test "a waived file in an argv list stays quiet" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", "void g(void) __attribute__((packed)); /* ATTR-OK: wire ABI */\n");
    var result = try runIn(tmp.dir, "/repo", &.{"libs/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

// --- the whole-tree sweep -------------------------------------------------

test "a sweep of a small tree collapses to exit 2" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", bad_line);
    var result = try runIn(tmp.dir, "/repo", &.{});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expect(std.mem.indexOf(u8, result.err, "FATAL -- only 1 first-party source file(s)") != null);
    try testing.expectEqualStrings("", result.out);
}

test "a collapsed sweep never reports the tree clean" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "clean") == null);
}

test "the collapse line names the floor" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var result = try runIn(tmp.dir, "/repo", &.{});
    defer result.deinit();
    try testing.expect(std.mem.indexOf(u8, result.err, "floor is 1700") != null);
}

// --- enumeration ----------------------------------------------------------

test "discovery finds sources under every root, at any depth" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", good_line);
    try write(tmp.dir, "tests/deep/nested/b.hpp", good_line);
    try write(tmp.dir, "apps/product/c.h", good_line);
    try write(tmp.dir, "port/d.cpp", good_line);
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 4), found.items.len);
}

test "discovery ignores unscanned suffixes and absent roots" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.md", good_line);
    try write(tmp.dir, "libs/a.C", good_line);
    try write(tmp.dir, "scripts/a.c", good_line);
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 0), found.items.len);
}

test "discovery drops build output and vendored trees" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/keep.c", good_line);
    try write(tmp.dir, "tests/build/drop.c", good_line);
    try write(tmp.dir, "tools/x/__pycache__/drop.c", good_line);
    try write(tmp.dir, "libs/third_party/drop.c", good_line);
    try write(tmp.dir, "libs/ra8_fonts/drop.c", good_line);
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 1), found.items.len);
    try testing.expectEqualStrings("libs/keep.c", found.items[0]);
}

test "discovery does not hide dot-prefixed directories or files" {
    // pathlib globbing hides nothing, so neither does this.
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/.hidden/a.c", good_line);
    try write(tmp.dir, "libs/.b.c", good_line);
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 2), found.items.len);
}

test "discovery yields a directory whose name ends in a scanned suffix" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("libs/oops.c");
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 1), found.items.len);
}

test "a source directory named build survives discovery" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/build/a.c", good_line);
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 1), found.items.len);
}

test "discovery drops a cmake-build- tree under a build root" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "tools/x/cmake-build-debug/a.c", good_line);
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer {
        for (found.items) |item| testing.allocator.free(item);
        found.deinit();
    }
    try testing.expectEqual(@as(usize, 0), found.items.len);
}

test "an empty tree discovers nothing without erroring" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var found = try cli.discover(testing.allocator, tmp.dir, "/repo");
    defer found.deinit();
    try testing.expectEqual(@as(usize, 0), found.items.len);
}

test "a source past any read ceiling still reports its attribute" {
    // A ceiling on the read is a fail-OPEN divergence: readFileAlloc answers
    // error.FileTooBig rather than truncating, and that error lands in the
    // same catch as a missing file, so a large source would be reported as
    // carrying no attribute while still counting as scanned. Written sparse,
    // so the fixture costs a seek rather than 64 MiB of disk.
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("libs");
    {
        var file = try tmp.dir.createFile("libs/huge.c", .{});
        defer file.close();
        try file.seekTo(65 * 1024 * 1024);
        try file.writeAll(bad_line);
    }
    var result = try runIn(tmp.dir, "/repo", &.{"libs/huge.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqual(@as(usize, 1), result.findings);
    try testing.expect(std.mem.indexOf(u8, result.out, "libs/huge.c:1: GNU __attribute__") != null);
}
