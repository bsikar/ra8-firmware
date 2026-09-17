//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and stream contract for `check_tz_boundary_discard` (#1250).
//! CI reads these statuses (`scripts/ci/gates/checks.sh` runs the selftest and
//! then the sweep), so each one is pinned here: 0 clean, 1 a finding or a
//! failing selftest case, 2 usage or a collapsed sweep.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

const Run = struct {
    status: u8,
    out: []u8,
    err: []u8,

    fn deinit(self: Run) void {
        testing.allocator.free(self.out);
        testing.allocator.free(self.err);
    }
};

fn runIn(dir: std.fs.Dir, args: []const []const u8) !Run {
    var out = std.ArrayList(u8).init(testing.allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    errdefer err.deinit();
    const status = try cli.run(testing.allocator, dir, "/repo", args, out.writer(), err.writer());
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

fn write(dir: std.fs.Dir, path: []const u8, data: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = path, .data = data });
}

const family_discard = "void f(void) { (void)ra8_tz_secure_boot_verify(); }\n";
const boot_discard = "void SystemInit(void) { (void)ra8_cgc_init(); }\n";
const handled = "void f(void) { if (ra8_cgc_init() != k_ra8_ok) { halt(); } }\n";

// --- selftest -------------------------------------------------------------

test "--selftest alone passes and exits zero" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{"--selftest"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "[ok] world-switch") != null);
    try testing.expect(std.mem.indexOf(u8, result.out, "[ok] handled results") != null);
    try testing.expectEqualStrings("", result.err);
}

test "--selftest with a second argument is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{ "--selftest", "libs/a.c" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expectEqualStrings("usage: check_tz_boundary_discard [--selftest] [file ...]\n", result.err);
}

test "--selftest after a path is a usage error too" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{ "libs/a.c", "--selftest" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
}

// --- usage ----------------------------------------------------------------

test "an unknown flag is a usage error on stderr" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{"--all"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expect(std.mem.startsWith(u8, result.err, "usage: "));
}

test "a lone dash is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{"-"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
}

test "a flag anywhere in the list is a usage error" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{ "libs/a.c", "-q", "libs/b.c" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
}

// --- explicit path lists --------------------------------------------------

test "a clean explicit file exits zero with the clean line on stdout" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/ok.c", handled);
    const result = try runIn(tmp.dir, &.{"libs/ok.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.startsWith(u8, result.out, "check_tz_boundary_discard: clean"));
    try testing.expectEqualStrings("", result.err);
}

test "a world-switch discard exits one and prints the site on stdout" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/bad.c", family_discard);
    const result = try runIn(tmp.dir, &.{"libs/bad.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expect(std.mem.startsWith(u8, result.out, "libs/bad.c:1: [rule A] world-switch result discarded"));
    try testing.expect(std.mem.indexOf(u8, result.out, ": 1 violation(s).") != null);
    try testing.expectEqualStrings("", result.err);
}

test "a boot translation unit reports rule B" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/boot.c", boot_discard);
    const result = try runIn(tmp.dir, &.{"libs/boot.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "[rule B] boot-TU ra8_* result discarded") != null);
}

test "the same source in a header reports nothing" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/boot.h", boot_discard);
    const result = try runIn(tmp.dir, &.{"libs/boot.h"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "violations from several files are counted together" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", family_discard);
    try write(tmp.dir, "libs/b.c", family_discard);
    const result = try runIn(tmp.dir, &.{ "libs/b.c", "libs/a.c" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, ": 2 violation(s).") != null);
}

test "paths are reported in sorted order whatever argv order was" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", family_discard);
    try write(tmp.dir, "libs/b.c", family_discard);
    const result = try runIn(tmp.dir, &.{ "libs/b.c", "libs/a.c" });
    defer result.deinit();
    const first = std.mem.indexOf(u8, result.out, "libs/a.c").?;
    const second = std.mem.indexOf(u8, result.out, "libs/b.c").?;
    try testing.expect(first < second);
}

test "a repeated path is scanned once" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", family_discard);
    const result = try runIn(tmp.dir, &.{ "libs/a.c", "libs/a.c" });
    defer result.deinit();
    try testing.expect(std.mem.indexOf(u8, result.out, ": 1 violation(s).") != null);
}

test "a missing path is skipped rather than fatal" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{"libs/gone.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "a directory handed in as a path is skipped" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("libs/dir.c");
    const result = try runIn(tmp.dir, &.{"libs/dir.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "a file that is not valid UTF-8 is scanned as nothing" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/bin.c", "(void)ra8_tz_secure_boot_verify();\xff\n");
    const result = try runIn(tmp.dir, &.{"libs/bin.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "an out-of-scope suffix is filtered out of an explicit list" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.cc", family_discard);
    const result = try runIn(tmp.dir, &.{"libs/a.cc"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "a build tree path is filtered out of an explicit list" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "tests/build/a.c", family_discard);
    const result = try runIn(tmp.dir, &.{"tests/build/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "vendored SOUP is filtered out of an explicit list" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/third_party/x/a.c", family_discard);
    const result = try runIn(tmp.dir, &.{"libs/third_party/x/a.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "a waived site keeps an explicit list clean" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/w.c", "(void)ra8_tz_secure_boot_verify(); /* TZ-DISCARD-OK: audited */\n");
    const result = try runIn(tmp.dir, &.{"libs/w.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

test "an explicit list is exempt from the file floor" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/ok.c", handled);
    const result = try runIn(tmp.dir, &.{"libs/ok.c"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
}

// --- the whole-tree sweep -------------------------------------------------

test "a collapsed sweep is fatal on stderr with status two" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.c", handled);
    const result = try runIn(tmp.dir, &.{});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expect(std.mem.indexOf(u8, result.err, "FATAL -- only 1 first-party source file(s)") != null);
    try testing.expect(std.mem.indexOf(u8, result.err, "floor is 1700") != null);
}

test "an empty tree reports a zero-file sweep" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 2), result.status);
    try testing.expect(std.mem.indexOf(u8, result.err, "only 0 first-party source file(s)") != null);
}

// --- discovery ------------------------------------------------------------

fn discovered(dir: std.fs.Dir) ![][]const u8 {
    return cli.discover(testing.allocator, dir, "/repo");
}

fn freeAll(paths: [][]const u8) void {
    for (paths) |path| testing.allocator.free(path);
    testing.allocator.free(paths);
}

fn contains(paths: [][]const u8, wanted: []const u8) bool {
    for (paths) |path| {
        if (std.mem.eql(u8, path, wanted)) return true;
    }
    return false;
}

test "discovery walks the roots recursively" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/ra8_tz/deep/a.c", handled);
    try write(tmp.dir, "apps/reader/main.cpp", handled);
    const paths = try discovered(tmp.dir);
    defer freeAll(paths);
    try testing.expect(contains(paths, "libs/ra8_tz/deep/a.c"));
    try testing.expect(contains(paths, "apps/reader/main.cpp"));
}

test "discovery ignores directories that are not roots" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "scripts/a.c", handled);
    try write(tmp.dir, "libs/a.c", handled);
    const paths = try discovered(tmp.dir);
    defer freeAll(paths);
    try testing.expectEqual(@as(usize, 1), paths.len);
    try testing.expectEqualStrings("libs/a.c", paths[0]);
}

test "discovery drops build output and exempt trees" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "tests/build/gen.c", handled);
    try write(tmp.dir, "tools/vela/__pycache__/x.c", handled);
    try write(tmp.dir, "libs/third_party/lv/lv.c", handled);
    try write(tmp.dir, "libs/ra8_fonts/f.c", handled);
    try write(tmp.dir, "libs/keep.c", handled);
    const paths = try discovered(tmp.dir);
    defer freeAll(paths);
    try testing.expectEqual(@as(usize, 1), paths.len);
    try testing.expectEqualStrings("libs/keep.c", paths[0]);
}

test "discovery keeps hidden files and hidden directories, as rglob did" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/.hidden/a.c", handled);
    try write(tmp.dir, "libs/.dot.c", handled);
    const paths = try discovered(tmp.dir);
    defer freeAll(paths);
    try testing.expectEqual(@as(usize, 2), paths.len);
}

test "discovery keeps a source-directory build name outside a build root" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/builders/a.c", handled);
    const paths = try discovered(tmp.dir);
    defer freeAll(paths);
    try testing.expectEqual(@as(usize, 1), paths.len);
}

test "discovery ignores out-of-scope suffixes" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "libs/a.zig", handled);
    try write(tmp.dir, "libs/a.md", handled);
    const paths = try discovered(tmp.dir);
    defer freeAll(paths);
    try testing.expectEqual(@as(usize, 0), paths.len);
}
