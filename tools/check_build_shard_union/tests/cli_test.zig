// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Behavioural regression tests for the argv membrane, the discovery walk and
// the exit-status contract of the cross-build shard-union gate (#858, #1159).
//
// Every status asserted here is the status the Python returned: 0 clean or a
// passing selftest, 1 a discrepancy or an unreadable manifest, 2 a usage
// error. The fail-closed cases (an empty discovery, an absent manifest
// directory, a missing shard) are pinned in BOTH directions so a detector
// that stopped detecting cannot pass as a clean gate.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

const Captured = struct {
    status: u8,
    out: []const u8,
    err: []const u8,
};

fn writeFile(dir: std.fs.Dir, path: []const u8, data: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = path, .data = data });
}

/// One example app: examples/<tier>/<app>/src/main.c plus its CMakeLists.
fn addExample(allocator: std.mem.Allocator, dir: std.fs.Dir, root: []const u8, tier: []const u8, app: []const u8) !void {
    const main_c = try std.fmt.allocPrint(allocator, "{s}/examples/{s}/{s}/src/main.c", .{ root, tier, app });
    try writeFile(dir, main_c, "int main(void){return 0;}\n");
    const lists = try std.fmt.allocPrint(allocator, "{s}/examples/{s}/{s}/CMakeLists.txt", .{ root, tier, app });
    try writeFile(dir, lists, "add_executable(test src/main.c)\n");
}

fn addBoard(allocator: std.mem.Allocator, dir: std.fs.Dir, root: []const u8, product: []const u8) !void {
    const main_c = try std.fmt.allocPrint(allocator, "{s}/apps/board/stand_alone/{s}/src/main.c", .{ root, product });
    try writeFile(dir, main_c, "void main(void) {}\n");
    const lists = try std.fmt.allocPrint(allocator, "{s}/apps/board/stand_alone/{s}/CMakeLists.txt", .{ root, product });
    try writeFile(dir, lists, "add_executable(p src/main.c)\n");
}

fn addManifest(allocator: std.mem.Allocator, dir: std.fs.Dir, root: []const u8, name: []const u8, body: []const u8) !void {
    const path = try std.fmt.allocPrint(allocator, "{s}/build/build_all_examples/.shard/{s}", .{ root, name });
    try writeFile(dir, path, body);
}

fn invoke(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    scratch: ?std.fs.Dir,
    argv: []const []const u8,
) !Captured {
    var out = std.ArrayList(u8).init(allocator);
    var err = std.ArrayList(u8).init(allocator);
    const status = try cli.run(allocator, dir, scratch, argv, ".", out.writer(), err.writer());
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

test "int parsing accepts what Python's int() accepts" {
    try testing.expectEqual(@as(?i64, 3), cli.pythonInt("3"));
    try testing.expectEqual(@as(?i64, 3), cli.pythonInt("  3\t"));
    try testing.expectEqual(@as(?i64, 3), cli.pythonInt("+3"));
    try testing.expectEqual(@as(?i64, -3), cli.pythonInt("-3"));
    try testing.expectEqual(@as(?i64, 10), cli.pythonInt("1_0"));
}

test "int parsing rejects what Python's int() rejects" {
    try testing.expectEqual(@as(?i64, null), cli.pythonInt(""));
    try testing.expectEqual(@as(?i64, null), cli.pythonInt("3.5"));
    try testing.expectEqual(@as(?i64, null), cli.pythonInt("_3"));
    try testing.expectEqual(@as(?i64, null), cli.pythonInt("3_"));
    try testing.expectEqual(@as(?i64, null), cli.pythonInt("1__0"));
    try testing.expectEqual(@as(?i64, null), cli.pythonInt("two"));
}

test "a separate and an attached shard value parse the same" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const separate = try cli.parseArgs(allocator, &.{ "--shards", "4" });
    const attached = try cli.parseArgs(allocator, &.{"--shards=4"});
    try testing.expectEqual(@as(?i64, 4), separate.options.shards);
    try testing.expectEqual(@as(?i64, 4), attached.options.shards);
}

test "an unambiguous option prefix is accepted, as argparse accepts it" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const parsed = try cli.parseArgs(allocator, &.{ "--shard", "2", "--repo", "tree" });
    try testing.expectEqual(@as(?i64, 2), parsed.options.shards);
    try testing.expectEqualStrings("tree", parsed.options.repo_root.?);
}

test "an ambiguous prefix is a usage error, not a guess" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const parsed = try cli.parseArgs(arena.allocator(), &.{"--s"});
    try testing.expect(parsed == .usage_error);
}

test "selftest is a flag and takes no value" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const parsed = try cli.parseArgs(allocator, &.{"--selftest"});
    try testing.expect(parsed.options.selftest);
    const valued = try cli.parseArgs(allocator, &.{"--selftest=1"});
    try testing.expect(valued == .usage_error);
}

test "a shard option with no value is a usage error" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const parsed = try cli.parseArgs(arena.allocator(), &.{"--shards"});
    try testing.expect(parsed == .usage_error);
}

test "help is requested by either spelling" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try testing.expect(try cli.parseArgs(allocator, &.{"-h"}) == .help);
    try testing.expect(try cli.parseArgs(allocator, &.{"--help"}) == .help);
}

test "help prints the usage on stdout and exits 0" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, null, &.{"--help"});
    try testing.expectEqual(@as(u8, 0), got.status);
    try testing.expect(std.mem.indexOf(u8, got.out, "usage: check_build_shard_union") != null);
    try testing.expectEqualStrings("", got.err);
}

test "no arguments at all is a usage error, never a vacuous pass" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, null, &.{});
    try testing.expectEqual(@as(u8, 2), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "--shards must be a positive integer") != null);
}

test "zero and negative shard counts are usage errors" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const zero = try invoke(allocator, tmp.dir, null, &.{ "--shards", "0" });
    const negative = try invoke(allocator, tmp.dir, null, &.{ "--shards", "-2" });
    try testing.expectEqual(@as(u8, 2), zero.status);
    try testing.expectEqual(@as(u8, 2), negative.status);
}

test "a non-integer shard count is a usage error naming the value" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, null, &.{ "--shards", "two" });
    try testing.expectEqual(@as(u8, 2), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "invalid int value: 'two'") != null);
}

test "an unknown option and a stray positional are both usage errors" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const unknown = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--nope" });
    const positional = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "extra" });
    try testing.expectEqual(@as(u8, 2), unknown.status);
    try testing.expectEqual(@as(u8, 2), positional.status);
}

test "a complete two-way matrix passes and reports the counts" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addExample(allocator, tmp.dir, "r", "tier", "b");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\ntier::b\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-2.txt", "tier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-2-of-2.txt", "tier::b\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "2", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
    try testing.expect(std.mem.indexOf(u8, got.out, "2 shard(s) covered all 2 firmware configuration(s) exactly once.") != null);
    try testing.expectEqualStrings("", got.err);
}

test "the e-reader contributes its board configuration and its XIP variant" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addBoard(allocator, tmp.dir, "r", "ereader");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "board::stand_alone::ra8d2-ereader\nboard::stand_alone::ra8d2-ereader@ns-xip\ntier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "board::stand_alone::ra8d2-ereader\nboard::stand_alone::ra8d2-ereader@ns-xip\ntier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
    try testing.expect(std.mem.indexOf(u8, got.out, "all 3 firmware configuration(s)") != null);
}

test "omitting the XIP variant fails, which is the #530 shape of a gate checking less" {
    // The matrix a shard wrote is checked BEFORE the union is audited, so a
    // tree whose all-configs.txt is missing the variant fails on the
    // disagreement and never reaches the count lines. That ordering is the
    // Python's: the first non-empty problem list returns.
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addBoard(allocator, tmp.dir, "r", "ereader");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "board::stand_alone::ra8d2-ereader\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "board::stand_alone::ra8d2-ereader\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "all-configs.txt disagrees with fresh discovery") != null);
    try testing.expect(std.mem.indexOf(u8, got.err, "the cross-build shards did NOT cover the tree") != null);
    try testing.expect(std.mem.indexOf(u8, got.err, "Do NOT relax this") != null);
}

test "a shard that skipped the XIP variant is the configuration never built" {
    // Same omission, but with an honest all-configs.txt, so the union audit
    // is reached and reports the unbuilt configuration itself.
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addBoard(allocator, tmp.dir, "r", "ereader");
    try addManifest(
        allocator,
        tmp.dir,
        "r",
        "all-configs.txt",
        "board::stand_alone::ra8d2-ereader\nboard::stand_alone::ra8d2-ereader@ns-xip\n",
    );
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "board::stand_alone::ra8d2-ereader\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "1 firmware configuration(s) never built by any shard") != null);
}

test "an empty discovery fails rather than vouching for nothing" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "cannot vouch for a union") != null);
}

test "an absent shard directory fails and names the path it looked for" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "shard manifest directory r/build/build_all_examples/.shard does not exist.") != null);
}

test "a missing shard manifest is a shard that did not run" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addExample(allocator, tmp.dir, "r", "tier", "b");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\ntier::b\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-2.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "2", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "missing shard manifest shard-2-of-2.txt") != null);
}

test "a missing all-configs is reported with its whole path" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "missing r/build/build_all_examples/.shard/all-configs.txt") != null);
}

test "an all-configs that disagrees with a fresh discovery fails" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\ntier::ghost\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "all-configs.txt disagrees with fresh discovery") != null);
}

test "an all-configs in a different order disagrees, because the compare is a list" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addExample(allocator, tmp.dir, "r", "tier", "b");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::b\ntier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\ntier::b\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "disagrees with fresh discovery") != null);
}

test "a configuration claimed by two shards fails with both shard numbers" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addExample(allocator, tmp.dir, "r", "tier", "b");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\ntier::b\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-2.txt", "tier::a\ntier::b\n");
    try addManifest(allocator, tmp.dir, "r", "shard-2-of-2.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "2", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "app 'tier::a' claimed by both shard 1 and shard 2") != null);
}

test "an empty shard manifest fails on the configuration nobody built" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addExample(allocator, tmp.dir, "r", "tier", "b");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\ntier::b\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-2.txt", "tier::a\ntier::b\n");
    try addManifest(allocator, tmp.dir, "r", "shard-2-of-2.txt", "");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "2", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
    try testing.expectEqualStrings("", got.err);
}

test "a manifest naming something unstructural fails as not structural" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\nghost\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "1 configuration(s) claimed in manifests are not structural") != null);
}

test "a non-ASCII manifest is status 1, not a tolerated read" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::\xff\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "not ASCII") != null);
}

test "the shared tier is not discovered, so it need not be built" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addExample(allocator, tmp.dir, "r", "shared", "helper");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
}

test "an app with no CMakeLists is not a configuration" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try writeFile(tmp.dir, "r/examples/tier/b/src/main.c", "int main(void){return 0;}\n");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
}

test "a main.c outside a src directory is not a configuration" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try writeFile(tmp.dir, "r/examples/tier/b/main.c", "int main(void){return 0;}\n");
    try writeFile(tmp.dir, "r/examples/tier/b/CMakeLists.txt", "add_executable(b main.c)\n");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
}

test "a one-part example path is below the minimum and is skipped" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try writeFile(tmp.dir, "r/examples/loose/src/main.c", "int main(void){return 0;}\n");
    try writeFile(tmp.dir, "r/examples/loose/CMakeLists.txt", "add_executable(l src/main.c)\n");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
}

test "a dot-prefixed tier is discovered, because pathlib's rglob hides nothing" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", ".hidden", "app");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", ".hidden::app\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", ".hidden::app\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
    try testing.expect(std.mem.indexOf(u8, got.out, "all 1 firmware configuration(s)") != null);
}

test "a non-ereader board product keeps its own name and needs no XIP variant" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addBoard(allocator, tmp.dir, "r", "bringup");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "board::stand_alone::bringup\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "board::stand_alone::bringup\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
}

test "a manifest entry with surrounding whitespace still matches" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "  tier::a  \n\n");
    try addManifest(allocator, tmp.dir, "r", "shard-1-of-1.txt", "\ttier::a\r\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "1", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 0), got.status);
}

test "a repository root that does not exist discovers nothing and fails" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, null, &.{ "--shards", "1", "--repo-root", "absent" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "cannot vouch for a union") != null);
}

test "more shards than manifests fails on every absent one" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try addExample(allocator, tmp.dir, "r", "tier", "a");
    try addManifest(allocator, tmp.dir, "r", "all-configs.txt", "tier::a\n");
    const got = try invoke(allocator, tmp.dir, null, &.{ "--shards", "3", "--repo-root", "r" });
    try testing.expectEqual(@as(u8, 1), got.status);
    try testing.expect(std.mem.indexOf(u8, got.err, "shard-1-of-3.txt") != null);
    try testing.expect(std.mem.indexOf(u8, got.err, "shard-2-of-3.txt") != null);
    try testing.expect(std.mem.indexOf(u8, got.err, "shard-3-of-3.txt") != null);
}

test "the selftest passes and says so" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var scratch = testing.tmpDir(.{});
    defer scratch.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, scratch.dir, &.{"--selftest"});
    try testing.expectEqual(@as(u8, 0), got.status);
    try testing.expect(std.mem.indexOf(u8, got.out, "--selftest: all cases pass.") != null);
    try testing.expect(std.mem.indexOf(u8, got.out, "[FAIL]") == null);
}

test "the selftest reports every case, both boundaries included" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var scratch = testing.tmpDir(.{});
    defer scratch.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, scratch.dir, &.{"--selftest"});
    try testing.expect(std.mem.indexOf(u8, got.out, "complete examples plus board variants: expected to pass, rc=0") != null);
    try testing.expect(std.mem.indexOf(u8, got.out, "an app built twice: expected to fire, rc=1") != null);
    try testing.expect(std.mem.indexOf(u8, got.out, "absent manifest dir: expected to fire, rc=1") != null);
    try testing.expect(std.mem.indexOf(u8, got.out, "empty tree: expected to fire, rc=1") != null);
    try testing.expectEqual(@as(usize, 9), std.mem.count(u8, got.out, "  [ok] "));
}

test "the selftest wins over a shard count, as it did in argparse" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var scratch = testing.tmpDir(.{});
    defer scratch.cleanup();
    const got = try invoke(arena.allocator(), tmp.dir, scratch.dir, &.{ "--selftest", "--shards", "0" });
    try testing.expectEqual(@as(u8, 0), got.status);
}
