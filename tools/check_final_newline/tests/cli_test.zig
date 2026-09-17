//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract tests for the trailing-newline gate (#858). Every
//! status the launcher can pass through is proved here with a census supplied
//! by the test and a temporary tree on disk, so no real repository and no
//! process are involved.

const std = @import("std");
const cli = @import("cli");

/// A throwaway repository: a real directory the gate reads through, plus
/// captured stdout and stderr.
const Harness = struct {
    tmp: std.testing.TmpDir,
    root: []const u8,
    out: std.ArrayList(u8),
    err: std.ArrayList(u8),
    allocator: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) !Harness {
        var tmp = std.testing.tmpDir(.{});
        const root = try tmp.dir.realpathAlloc(allocator, ".");
        return .{
            .tmp = tmp,
            .root = root,
            .out = std.ArrayList(u8).init(allocator),
            .err = std.ArrayList(u8).init(allocator),
            .allocator = allocator,
        };
    }

    fn deinit(self: *Harness) void {
        self.allocator.free(self.root);
        self.out.deinit();
        self.err.deinit();
        self.tmp.cleanup();
    }

    fn write(self: *Harness, rel: []const u8, body: []const u8) !void {
        if (std.fs.path.dirname(rel)) |parent| try self.tmp.dir.makePath(parent);
        try self.tmp.dir.writeFile(.{ .sub_path = rel, .data = body });
    }

    fn run(self: *Harness, argv: []const []const u8, census: []const []const u8, policy: cli.Policy) !u8 {
        return cli.run(
            self.allocator,
            std.fs.cwd(),
            self.root,
            argv,
            .{ .provided = census },
            policy,
            self.out.writer(),
            self.err.writer(),
        );
    }
};

/// A policy with both floors lowered, so a fixture tree can exercise the
/// statuses the live floors reserve for the real repository.
const relaxed = cli.Policy{ .file_floor = 1, .tracked_floor = 1 };

test "a clean sweep exits 0 and reports the scanned count" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int main(void) { return 0; }\n");
    try harness.write("scripts/dev/tool.py", "x = 1\n");
    const census = [_][]const u8{ "libs/a/x.c", "scripts/dev/tool.py" };

    const status = try harness.run(&.{}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "2 file(s) scanned, all end in a newline.") != null);
    try std.testing.expectEqualStrings("", harness.err.items);
}

test "a file with no trailing newline exits 1 and is listed" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int main(void) { return 0; }\n");
    try harness.write("scripts/dev/tool.py", "x = 1");
    const census = [_][]const u8{ "libs/a/x.c", "scripts/dev/tool.py" };

    const status = try harness.run(&.{}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "1 file(s) missing a trailing newline:") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "  scripts/dev/tool.py\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "Add a single newline at end of file.") != null);
}

test "findings are listed in sorted order" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/z/z.c", "z");
    try harness.write("libs/a/a.c", "a");
    const census = [_][]const u8{ "libs/z/z.c", "libs/a/a.c" };

    const status = try harness.run(&.{}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 1), status);
    const first = std.mem.indexOf(u8, harness.err.items, "libs/a/a.c").?;
    const second = std.mem.indexOf(u8, harness.err.items, "libs/z/z.c").?;
    try std.testing.expect(first < second);
}

test "an empty file is not a finding" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/empty.c", "");
    const census = [_][]const u8{"libs/a/empty.c"};

    try std.testing.expectEqual(@as(u8, 0), try harness.run(&.{}, &census, relaxed));
}

test "a collapsed sweep exits 2 rather than reporting a clean tree" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;\n");
    const census = [_][]const u8{"libs/a/x.c"};

    const status = try harness.run(&.{}, &census, .{ .file_floor = 2200, .tracked_floor = 1 });
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "only 1 file(s) in scope, floor is 2200") != null);
    try std.testing.expectEqualStrings("", harness.out.items);
}

test "a collapsed census exits 2 before any file is read" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;");
    const census = [_][]const u8{"libs/a/x.c"};

    const status = try harness.run(&.{}, &census, .{ .file_floor = 1, .tracked_floor = 1000 });
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "only 1 tracked path(s), floor is 1000") != null);
}

test "a sweep whose scope is empty exits 2, never 0" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    const census = [_][]const u8{ "docs/a.md", "docs/b.json" };

    const status = try harness.run(&.{}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 2), status);
}

test "an argv list that filters to nothing exits 0" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("docs/guide.md", "prose");
    const census = [_][]const u8{"docs/guide.md"};

    const status = try harness.run(&.{"docs/guide.md"}, &census, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "no files to scan") != null);
}

test "an argv list bypasses the sweep floor entirely" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;\n");

    const status = try harness.run(&.{"libs/a/x.c"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "1 file(s) scanned") != null);
}

test "an argv file with no trailing newline exits 1" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;");

    const status = try harness.run(&.{"libs/a/x.c"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "  libs/a/x.c\n") != null);
}

test "an argv directory expands to its source files at any depth" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;\n");
    try harness.write("libs/a/deep/y.py", "y = 1");
    try harness.write("libs/a/notes.md", "prose");

    const status = try harness.run(&.{"libs/a"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "1 file(s) missing a trailing newline:") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "libs/a/deep/y.py") != null);
}

test "an argv path that does not exist is not a finding" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();

    const status = try harness.run(&.{"libs/a/absent.c"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "1 file(s) scanned") != null);
}

test "an unknown flag is treated as a path, not a usage error" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();

    const status = try harness.run(&.{"--all"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "no files to scan") != null);
}

test "an excluded vendored path is dropped from an argv list" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/third_party/lz4/lz4.c", "int x;");

    const status = try harness.run(&.{"libs/third_party/lz4/lz4.c"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "no files to scan") != null);
}

test "build output is dropped from an argv directory expansion" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("tools/demo/build/generated.c", "int x;");
    try harness.write("tools/demo/src/real.c", "int y;\n");

    const status = try harness.run(&.{"tools/demo"}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "1 file(s) scanned") != null);
}

test "the sweep drops vendored trees the census still carries" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("port/threadx/CMakeLists.txt", "project(tx)");
    try harness.write("libs/a/x.c", "int x;\n");
    const census = [_][]const u8{ "port/threadx/CMakeLists.txt", "libs/a/x.c" };

    const status = try harness.run(&.{}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "1 file(s) scanned") != null);
}

test "a listfile with no suffix is in the sweep" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("justfile", "default:\n");
    try harness.write("apps/a/CMakeLists.txt", "project(a)");
    const census = [_][]const u8{ "justfile", "apps/a/CMakeLists.txt" };

    const status = try harness.run(&.{}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "apps/a/CMakeLists.txt") != null);
}

test "an absolute argv path is accepted and printed repo-relative" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;");
    const absolute = try std.fs.path.join(std.testing.allocator, &.{ harness.root, "libs/a/x.c" });
    defer std.testing.allocator.free(absolute);

    const status = try harness.run(&.{absolute}, &.{}, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "  libs/a/x.c\n") != null);
}

test "the selftest passes and prints both directions when the scope is real" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("just/tests.just", "x:\n");
    try harness.write("infra/site.yml", "- hosts: all\n");
    const census = [_][]const u8{ "just/tests.just", "infra/site.yml" };

    const status = try harness.run(&.{"--selftest"}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[ok] MUST NOT FIRE: a newline-terminated file") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[ok] MUST NOT FIRE: an empty file") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[ok] MUST FIRE: a file with no trailing newline") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[ok] the derived scope reaches just/ (previously omitted)") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[ok] the derived scope reaches infra/ (previously omitted)") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "selftest: all assertions held (both directions).") != null);
}

test "the selftest fails when the derived scope never reaches the dropped roots" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;\n");
    const census = [_][]const u8{"libs/a/x.c"};

    const status = try harness.run(&.{"--selftest"}, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[FAIL] the derived scope reaches just/") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "SELFTEST FAILED: 2 assertion(s)") != null);
}

test "the selftest fails when the derived scope collapses below the floor" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("just/tests.just", "x:\n");
    try harness.write("infra/site.yml", "- hosts: all\n");
    const census = [_][]const u8{ "just/tests.just", "infra/site.yml" };

    const status = try harness.run(&.{"--selftest"}, &census, .{ .file_floor = 2200, .tracked_floor = 1 });
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "[FAIL] derived scope sees 2 file(s) (floor 2200)") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "SELFTEST FAILED: 1 assertion(s)") != null);
}

test "the selftest wins over a file list, wherever it sits in argv" {
    var harness = try Harness.init(std.testing.allocator);
    defer harness.deinit();
    try harness.write("libs/a/x.c", "int x;");
    try harness.write("just/tests.just", "x:\n");
    try harness.write("infra/site.yml", "- hosts: all\n");
    const census = [_][]const u8{ "just/tests.just", "infra/site.yml" };

    const status = try harness.run(&.{ "libs/a/x.c", "--selftest" }, &census, relaxed);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, harness.out.items, "selftest: all assertions held (both directions).") != null);
    try std.testing.expect(std.mem.indexOf(u8, harness.err.items, "missing a trailing newline") == null);
}

test "the live policy carries the inherited floors" {
    try std.testing.expectEqual(@as(usize, 2200), cli.Policy.default.file_floor);
    try std.testing.expectEqual(@as(usize, 1000), cli.Policy.default.tracked_floor);
}
