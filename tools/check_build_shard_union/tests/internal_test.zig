// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Behavioural regression tests for the pure rules of the cross-build
// shard-union gate (#858, #1159). Every expectation here was taken from the
// Python this replaced, not from what the rules ought to be: the manifest
// reader's ASCII contract, Python's line-break and strip sets, `sorted()`
// ordering over names the filesystem handed back, and the exact problem
// strings the gate prints.

const std = @import("std");
const testing = std.testing;
const implementation = @import("implementation");

fn lines(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    var collected = std.ArrayList([]const u8).init(allocator);
    var it = implementation.LineIterator{ .text = text };
    while (it.next()) |line| try collected.append(line);
    return collected.toOwnedSlice();
}

test "the C0 whitespace block plus the information separators strip" {
    for ([_]u8{ 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x1f, 0x20 }) |byte| {
        try testing.expect(implementation.isPythonSpace(byte));
    }
    for ([_]u8{ 0x00, 0x01, 0x1b, '!', 'a', '0' }) |byte| {
        try testing.expect(!implementation.isPythonSpace(byte));
    }
}

test "0x1f is whitespace but is NOT a line break" {
    try testing.expect(implementation.isPythonSpace(0x1f));
    try testing.expect(!implementation.isLineBreak(0x1f));
}

test "splitlines breaks on the C0 set the Python inherited" {
    for ([_]u8{ 0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e }) |byte| {
        try testing.expect(implementation.isLineBreak(byte));
    }
}

test "a CRLF pair is one break, not two" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try lines(arena.allocator(), "a\r\nb\r\n");
    try testing.expectEqual(@as(usize, 2), got.len);
    try testing.expectEqualStrings("a", got[0]);
    try testing.expectEqualStrings("b", got[1]);
}

test "a lone CR and a lone LF each break once" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try lines(arena.allocator(), "a\rb\nc");
    try testing.expectEqual(@as(usize, 3), got.len);
    try testing.expectEqualStrings("c", got[2]);
}

test "a vertical tab and a form feed break lines" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try lines(arena.allocator(), "a\x0bb\x0cc");
    try testing.expectEqual(@as(usize, 3), got.len);
}

test "a trailing break yields no final empty line" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try lines(arena.allocator(), "only\n");
    try testing.expectEqual(@as(usize, 1), got.len);
}

test "an empty text has no lines" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try lines(arena.allocator(), "");
    try testing.expectEqual(@as(usize, 0), got.len);
}

test "strip removes both ends and leaves the interior alone" {
    try testing.expectEqualStrings("a b", implementation.strip("  \t a b \r\n"));
    try testing.expectEqualStrings("", implementation.strip(" \x1f \t "));
    try testing.expectEqualStrings("x", implementation.strip("x"));
}

test "an ASCII text is ASCII and one high byte is not" {
    try testing.expect(implementation.isAscii("tier::app"));
    try testing.expect(!implementation.isAscii("tier::\xc3\xa9"));
}

test "a manifest drops blank and whitespace-only lines" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try implementation.parseManifest(arena.allocator(), "a\n\n  \nb\n\t\n");
    try testing.expectEqual(@as(usize, 2), got.len);
    try testing.expectEqualStrings("a", got[0]);
    try testing.expectEqualStrings("b", got[1]);
}

test "a manifest keeps file order rather than sorting" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try implementation.parseManifest(arena.allocator(), "tier::z\ntier::a\n");
    try testing.expectEqualStrings("tier::z", got[0]);
    try testing.expectEqualStrings("tier::a", got[1]);
}

test "a manifest entry is stripped, so a CRLF file still compares equal" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try implementation.parseManifest(arena.allocator(), "tier::a\r\ntier::b\r\n");
    try testing.expectEqualStrings("tier::a", got[0]);
    try testing.expectEqualStrings("tier::b", got[1]);
}

test "a non-ASCII manifest is an error, never a tolerated entry" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    try testing.expectError(
        error.NonAsciiManifest,
        implementation.parseManifest(arena.allocator(), "tier::\xff\n"),
    );
}

test "an empty manifest parses to no entries rather than one blank" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const got = try implementation.parseManifest(arena.allocator(), "");
    try testing.expectEqual(@as(usize, 0), got.len);
}

test "name ordering is by code point, so a prefix sorts first" {
    try testing.expectEqual(std.math.Order.lt, implementation.pythonOrder("tier", "tier::a"));
    try testing.expectEqual(std.math.Order.eq, implementation.pythonOrder("same", "same"));
    try testing.expectEqual(std.math.Order.gt, implementation.pythonOrder("b", "a"));
}

test "uppercase sorts before lowercase, as sorted() does" {
    try testing.expect(implementation.pythonLessThan({}, "Board", "board"));
}

test "a valid multi-byte name compares by code point, not by byte" {
    // U+00E9 is one code point below U+DC80, the escape for the byte 0x80,
    // even though its first byte (0xc3) is the larger of the two.
    try testing.expect(implementation.pythonLessThan({}, "\xc3\xa9", "\x80"));
}

test "an invalid byte becomes a surrogate escape above every ASCII name" {
    try testing.expect(implementation.pythonLessThan({}, "zzz", "\xff"));
}

test "a truncated multi-byte sequence escapes byte by byte" {
    try testing.expectEqual(std.math.Order.eq, implementation.pythonOrder("\xc3", "\xc3"));
    try testing.expect(implementation.pythonLessThan({}, "\xc3", "\xc4"));
}

test "sortNames orders a discovered set the way the gate compares it" {
    var names = [_][]const u8{ "tier::b", "board::stand_alone::ra8d2-ereader@ns-xip", "tier::a" };
    implementation.sortNames(&names);
    try testing.expectEqualStrings("board::stand_alone::ra8d2-ereader@ns-xip", names[0]);
    try testing.expectEqualStrings("tier::a", names[1]);
    try testing.expectEqualStrings("tier::b", names[2]);
}

test "list equality is order sensitive, which is what the manifest test was" {
    const left = [_][]const u8{ "a", "b" };
    const same = [_][]const u8{ "a", "b" };
    const swapped = [_][]const u8{ "b", "a" };
    const longer = [_][]const u8{ "a", "b", "c" };
    try testing.expect(implementation.sameList(&left, &same));
    try testing.expect(!implementation.sameList(&left, &swapped));
    try testing.expect(!implementation.sameList(&left, &longer));
}

test "an example needs a tier and an app directory" {
    try testing.expect(implementation.isExampleSelected(&[_][]const u8{ "tier", "app" }));
    try testing.expect(!implementation.isExampleSelected(&[_][]const u8{"app"}));
    try testing.expect(!implementation.isExampleSelected(&[_][]const u8{}));
}

test "the shared tier is not a build configuration" {
    try testing.expect(!implementation.isExampleSelected(&[_][]const u8{ "shared", "thing" }));
    try testing.expect(implementation.isExampleSelected(&[_][]const u8{ "shared_things", "thing" }));
}

test "a deeper example keeps every part in its identifier" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const parts = try implementation.splitParts(arena.allocator(), "tier/group/app");
    try testing.expectEqual(@as(usize, 3), parts.len);
    const name = try implementation.exampleConfig(arena.allocator(), parts);
    try testing.expectEqualStrings("tier::group::app", name);
}

test "splitParts drops empty and dot segments the way pathlib does" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const parts = try implementation.splitParts(arena.allocator(), "./tier//app");
    try testing.expectEqual(@as(usize, 2), parts.len);
    try testing.expectEqualStrings("tier", parts[0]);
    try testing.expectEqualStrings("app", parts[1]);
}

test "a dot-prefixed directory is a part like any other" {
    // pathlib's rglob hides no dot-prefixed name, so neither may this.
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const parts = try implementation.splitParts(arena.allocator(), ".hidden/app");
    const name = try implementation.exampleConfig(arena.allocator(), parts);
    try testing.expectEqualStrings(".hidden::app", name);
}

test "the e-reader is renamed to its board identifier" {
    try testing.expectEqualStrings("ra8d2-ereader", implementation.boardName("ereader"));
    try testing.expect(implementation.requiresNsXip("ereader"));
}

test "any other board product keeps its own last path component" {
    try testing.expectEqualStrings("bringup", implementation.boardName("bringup"));
    try testing.expectEqualStrings("leaf", implementation.boardName("nested/leaf"));
    try testing.expect(!implementation.requiresNsXip("nested/ereader"));
}

test "a board identifier carries the stand_alone prefix" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const name = try implementation.boardConfig(arena.allocator(), "ereader");
    try testing.expectEqualStrings("board::stand_alone::ra8d2-ereader", name);
}

test "a board directory at the root renders the empty name Python produced" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const name = try implementation.boardConfig(arena.allocator(), "");
    try testing.expectEqualStrings("board::stand_alone::", name);
}

test "path normalisation matches str(Path(...))" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try testing.expectEqualStrings("a/b", try implementation.normalizePath(allocator, "a//b"));
    try testing.expectEqualStrings("a/b", try implementation.normalizePath(allocator, "a/./b"));
    try testing.expectEqualStrings("x", try implementation.normalizePath(allocator, "./x"));
    try testing.expectEqualStrings("x", try implementation.normalizePath(allocator, "x/"));
    try testing.expectEqualStrings(".", try implementation.normalizePath(allocator, ""));
    try testing.expectEqualStrings("/a", try implementation.normalizePath(allocator, "/a"));
}

test "POSIX keeps exactly two leading slashes and collapses three" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try testing.expectEqualStrings("//a", try implementation.normalizePath(allocator, "//a"));
    try testing.expectEqualStrings("/a", try implementation.normalizePath(allocator, "///a"));
}

test "a parent keeps its .. segment" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    try testing.expectEqualStrings("../a", try implementation.normalizePath(arena.allocator(), "../a"));
}

test "joining the shard subdirectory renders one clean path" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try testing.expectEqualStrings(
        "root/build/build_all_examples/.shard",
        try implementation.joinPath(allocator, "root", implementation.shard_subdir),
    );
    try testing.expectEqualStrings(
        "build/build_all_examples/.shard",
        try implementation.joinPath(allocator, ".", implementation.shard_subdir),
    );
    try testing.expectEqualStrings(
        "/abs/build/build_all_examples/.shard",
        try implementation.joinPath(allocator, "/abs/", implementation.shard_subdir),
    );
}

test "a joined manifest path names the file under the shard directory" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const path = try implementation.joinPath(arena.allocator(), "r/build/build_all_examples/.shard", implementation.all_configs_name);
    try testing.expectEqualStrings("r/build/build_all_examples/.shard/all-configs.txt", path);
}

test "shard manifest names carry the 1-based index and the total" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try testing.expectEqualStrings("shard-1-of-1.txt", try implementation.shardFileName(allocator, 1, 1));
    try testing.expectEqualStrings("shard-7-of-12.txt", try implementation.shardFileName(allocator, 7, 12));
}

test "a complete union reports no problem at all" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{ "tier::a", "tier::b" };
    const shards = [_]implementation.Shard{
        .{ .index = 1, .apps = &[_][]const u8{"tier::a"} },
        .{ .index = 2, .apps = &[_][]const u8{"tier::b"} },
    };
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

test "a configuration claimed twice names both shards, first claimer first" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{ "tier::a", "tier::b" };
    const shards = [_]implementation.Shard{
        .{ .index = 1, .apps = &[_][]const u8{ "tier::a", "tier::b" } },
        .{ .index = 2, .apps = &[_][]const u8{"tier::a"} },
    };
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
    try testing.expectEqualStrings(
        "app 'tier::a' claimed by both shard 1 and shard 2",
        problems.items[0],
    );
}

test "an unbuilt configuration is counted, never named away" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{ "tier::a", "tier::b", "tier::c" };
    const shards = [_]implementation.Shard{.{ .index = 1, .apps = &[_][]const u8{"tier::a"} }};
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
    try testing.expectEqualStrings(
        "2 firmware configuration(s) never built by any shard",
        problems.items[0],
    );
}

test "a configuration nobody discovered is reported as not structural" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{"tier::a"};
    const shards = [_]implementation.Shard{
        .{ .index = 1, .apps = &[_][]const u8{ "tier::a", "ghost" } },
    };
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
    try testing.expectEqualStrings(
        "1 configuration(s) claimed in manifests are not structural",
        problems.items[0],
    );
}

test "missing and extra are two problems in that order" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{ "tier::a", "tier::b" };
    const shards = [_]implementation.Shard{
        .{ .index = 1, .apps = &[_][]const u8{ "tier::a", "ghost" } },
    };
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 2), problems.items.len);
    try testing.expect(std.mem.indexOf(u8, problems.items[0], "never built") != null);
    try testing.expect(std.mem.indexOf(u8, problems.items[1], "not structural") != null);
}

test "an empty shard is a shard that covered nothing, so the set disagrees" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{ "tier::a", "tier::b" };
    const shards = [_]implementation.Shard{
        .{ .index = 1, .apps = &[_][]const u8{"tier::a"} },
        .{ .index = 2, .apps = &[_][]const u8{} },
    };
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
}

test "a duplicate inside one shard is still reported against that shard" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var problems = std.ArrayList([]const u8).init(allocator);
    const expected = [_][]const u8{"tier::a"};
    const shards = [_]implementation.Shard{
        .{ .index = 1, .apps = &[_][]const u8{ "tier::a", "tier::a" } },
    };
    try implementation.auditShardContents(allocator, &shards, &expected, &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
    try testing.expectEqualStrings(
        "app 'tier::a' claimed by both shard 1 and shard 1",
        problems.items[0],
    );
}

test "membership over a name list is exact, never a prefix" {
    const names = [_][]const u8{ "tier::a", "tier::ab" };
    try testing.expect(implementation.containsName(&names, "tier::ab"));
    try testing.expect(!implementation.containsName(&names, "tier::"));
}

test "the clean line names the shard count and the configuration count" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const line = try implementation.renderCleanLine(arena.allocator(), 4, 187);
    try testing.expectEqualStrings(
        "check_build_shard_union: 4 shard(s) covered all 187 firmware configuration(s) exactly once.",
        line,
    );
}

test "the missing-directory problem names the whole path and ends in a full stop" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const line = try implementation.renderMissingShardDir(arena.allocator(), "r/build/build_all_examples/.shard");
    try testing.expectEqualStrings(
        "shard manifest directory r/build/build_all_examples/.shard does not exist.",
        line,
    );
}

test "a missing all-configs names its path while a missing shard names its file" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try testing.expectEqualStrings(
        "missing r/.shard/all-configs.txt",
        try implementation.renderMissingAllConfigs(allocator, "r/.shard/all-configs.txt"),
    );
    try testing.expectEqualStrings(
        "missing shard manifest shard-2-of-3.txt",
        try implementation.renderMissingShard(allocator, "shard-2-of-3.txt"),
    );
}

test "the empty-discovery problem refuses to vouch for a union" {
    try testing.expect(std.mem.startsWith(
        u8,
        implementation.empty_discovery_problem,
        "no firmware configurations discovered under examples/ or apps/board/stand_alone/",
    ));
    try testing.expect(std.mem.endsWith(
        u8,
        implementation.empty_discovery_problem,
        "cannot vouch for a union it has no truth to compare against.",
    ));
}

test "the advisory keeps the instruction not to relax the gate" {
    try testing.expect(std.mem.indexOf(u8, implementation.failure_advisory, "Do NOT relax this") != null);
    try testing.expect(std.mem.indexOf(u8, implementation.failure_advisory, "an unbuilt app is an unchecked app") != null);
}

test "the constants still spell the paths all_examples.sh writes" {
    try testing.expectEqualStrings("build/build_all_examples/.shard", implementation.shard_subdir);
    try testing.expectEqualStrings("all-configs.txt", implementation.all_configs_name);
    try testing.expectEqual(@as(usize, 2), implementation.min_example_path_parts);
    try testing.expectEqual(@as(u8, 0), implementation.rc_ok);
    try testing.expectEqual(@as(u8, 1), implementation.rc_violation);
}
