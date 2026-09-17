//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract of the pointer-only comment gate (#858). `cli.run` is
//! parameterised on a directory handle, the repository root, the census, the
//! scope policy and both streams, so every status below is proved here with
//! no process and no real repository.

const std = @import("std");
const cli = @import("cli");

/// One run's captured streams and status.
const Run = struct {
    status: u8,
    out: []const u8,
    err: []const u8,

    fn deinit(self: *Run, allocator: std.mem.Allocator) void {
        allocator.free(self.out);
        allocator.free(self.err);
    }
};

/// Drive the gate over a temporary tree.
fn runGate(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    census: []const []const u8,
    floor: usize,
) !Run {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    errdefer err.deinit();
    const status = try cli.run(
        allocator,
        dir,
        ".",
        argv,
        .{ .provided = census },
        .{ .floor = floor },
        out.writer(),
        err.writer(),
    );
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

/// Write one file, creating its directories.
fn writeFile(dir: std.fs.Dir, rel: []const u8, contents: []const u8) !void {
    if (std.fs.path.dirname(rel)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = rel, .data = contents });
}

const clean_source = "void f(void) { return; }\n";
const banned_source = "void f(void) {\n/* see header for the documented contract. */\n}\n";

test "a clean tree exits 0 and reports the scanned count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);
    try writeFile(tmp.dir, "examples/b/src/main.c", clean_source);

    var result = try runGate(
        std.testing.allocator,
        tmp.dir,
        &.{},
        &.{ "apps/a/src/main.c", "examples/b/src/main.c" },
        1,
    );
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_pointer_boilerplate: clean (2 app/example source files)\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "a banned comment exits 1 and names path and line on stderr" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", banned_source);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings("", result.out);
    try std.testing.expectEqualStrings(
        "Generated pointer-only definition comment(s):\n" ++
            "  apps/a/src/main.c:2\n" ++
            "Delete the comment; the declaration owns the contract.\n",
        result.err,
    );
}

test "findings are reported in sorted path order" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "examples/b/src/main.c", banned_source);
    try writeFile(tmp.dir, "apps/a/src/main.c", banned_source);

    var result = try runGate(
        std.testing.allocator,
        tmp.dir,
        &.{},
        &.{ "examples/b/src/main.c", "apps/a/src/main.c" },
        1,
    );
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    const apps = std.mem.indexOf(u8, result.err, "apps/a/src/main.c:2").?;
    const examples = std.mem.indexOf(u8, result.err, "examples/b/src/main.c:2").?;
    try std.testing.expect(apps < examples);
}

test "several findings in one file are all reported" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(
        tmp.dir,
        "apps/a/src/main.c",
        "/* see header for the documented contract. */\n" ++
            "int x;\n" ++
            "/* See the internal header for the documented contract. */\n",
    );

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "apps/a/src/main.c:1") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "apps/a/src/main.c:3") != null);
}

test "a library source is out of scope even when it carries the comment" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "libs/c/src/lib.c", banned_source);
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);

    var result = try runGate(
        std.testing.allocator,
        tmp.dir,
        &.{},
        &.{ "libs/c/src/lib.c", "apps/a/src/main.c" },
        1,
    );
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_pointer_boilerplate: clean (1 app/example source files)\n",
        result.out,
    );
}

test "a non-source suffix under apps is not scanned" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/README.md", banned_source);
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);

    var result = try runGate(
        std.testing.allocator,
        tmp.dir,
        &.{},
        &.{ "apps/a/README.md", "apps/a/src/main.c" },
        1,
    );
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "an upper case suffix is scanned" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.C", banned_source);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.C"}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "a census path that is not on disk is skipped" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);

    var result = try runGate(
        std.testing.allocator,
        tmp.dir,
        &.{},
        &.{ "apps/a/src/main.c", "apps/a/src/deleted.c" },
        1,
    );
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_pointer_boilerplate: clean (1 app/example source files)\n",
        result.out,
    );
}

test "a directory named like a source is skipped" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("apps/a/src/fake.c");
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);

    var result = try runGate(
        std.testing.allocator,
        tmp.dir,
        &.{},
        &.{ "apps/a/src/fake.c", "apps/a/src/main.c" },
        1,
    );
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a collapsed scope exits 2 rather than passing" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 850);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("", result.out);
    try std.testing.expectEqualStrings(
        "check_pointer_boilerplate: scope collapsed to 1 file(s); expected at least 850\n",
        result.err,
    );
}

test "an empty census is a collapsed scope, never a quiet pass" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{}, 850);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a collapsed scope outranks a finding" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", banned_source);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 850);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "an unreadable source stops the sweep with status 2" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", clean_source);
    {
        const file = tmp.dir.openFile("apps/a/src/main.c", .{}) catch return error.SkipZigTest;
        defer file.close();
        file.chmod(0o000) catch return error.SkipZigTest;
    }
    // A privileged runner ignores the mode bits, and then there is nothing
    // here to prove: skip rather than assert a permission that does not apply.
    if (tmp.dir.openFile("apps/a/src/main.c", .{})) |probe| {
        probe.close();
        return error.SkipZigTest;
    } else |_| {}

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "cannot scan source tree") != null);
}

test "an undecodable source stops the sweep with status 2" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", "int x;\n\xff\xfe\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "not valid UTF-8") != null);
}

test "a crlf source reports the same line number as its lf twin" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(
        tmp.dir,
        "apps/a/src/main.c",
        "void f(void) {\r\n/* see header for the documented contract. */\r\n}\r\n",
    );

    var result = try runGate(std.testing.allocator, tmp.dir, &.{}, &.{"apps/a/src/main.c"}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "apps/a/src/main.c:2") != null);
}

test "the selftest passes and prints its case count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--selftest"}, &.{}, 850);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_pointer_boilerplate --selftest: PASS (5 both-direction cases)\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "the selftest never reads the census" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "apps/a/src/main.c", banned_source);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--selftest"}, &.{"apps/a/src/main.c"}, 850);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "an unknown flag is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--all"}, &.{}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("usage: check_pointer_boilerplate [--selftest]\n", result.err);
}

test "a path argument is a usage error, the gate takes no file list" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"apps/a/src/main.c"}, &.{}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "selftest beside another argument is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{ "--selftest", "--selftest" }, &.{}, 1);
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("usage: check_pointer_boilerplate [--selftest]\n", result.err);
}

test "the default policy carries the documented floor" {
    try std.testing.expectEqual(@as(usize, 850), cli.Policy.default.floor);
}

test "the tool names itself without the old extension" {
    try std.testing.expectEqualStrings("check_pointer_boilerplate", cli.tool);
}
