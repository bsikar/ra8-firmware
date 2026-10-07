//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract tests for the legacy-task-runner gate (RA8FW-335).
//!
//! `cli.run` takes a directory handle, a census and both streams, so every
//! status the predecessor could return is provable here with no process, no
//! git and no real repository.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

const Streams = struct {
    out: std.Io.Writer.Allocating,
    err: std.Io.Writer.Allocating,

    fn init() Streams {
        return .{
            .out = .init(testing.allocator),
            .err = .init(testing.allocator),
        };
    }

    fn deinit(self: *Streams) void {
        self.out.deinit();
        self.err.deinit();
    }
};

/// A temporary tree holding the files a case needs, plus this gate's own
/// source so the floor check can pass.
const Tree = struct {
    dir: std.testing.TmpDir,

    fn init() Tree {
        return .{ .dir = std.testing.tmpDir(.{}) };
    }

    fn deinit(self: *Tree) void {
        self.dir.cleanup();
    }

    fn write(self: *Tree, rel: []const u8, text: []const u8) !void {
        if (std.fs.path.dirname(rel)) |parent| try self.dir.dir.createDirPath(testing.io, parent);
        try self.dir.dir.writeFile(testing.io, .{ .sub_path = rel, .data = text });
    }
};

fn runWith(
    tree: *Tree,
    argv: []const []const u8,
    census: []const []const u8,
    policy: cli.Policy,
    streams: *Streams,
) !u8 {
    return cli.run(
        testing.allocator,
        testing.io,
        tree.dir.dir,
        ".",
        argv,
        .{ .provided = census },
        policy,
        &streams.out.writer,
        &streams.err.writer,
    );
}

const self_rel = "tools/check_no_legacy_make/src/internal/root.zig";

fn seededTree() !Tree {
    var tree = Tree.init();
    try tree.write(self_rel, "const std = @import(\"std\");\n");
    return tree;
}

test "a lone --selftest passes and reports its case count" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{"--selftest"}, &.{}, .{}, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings(
        "check_no_legacy_make --selftest: PASS (19 both-direction cases)\n",
        streams.out.written(),
    );
    try testing.expectEqualStrings("", streams.err.written());
}

test "an unknown argument is a usage error" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{"--all"}, &.{}, .{}, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expectEqualStrings("usage: check_no_legacy_make [--selftest]\n", streams.err.written());
}

test "--selftest beside another argument is a usage error" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{ "--selftest", "extra" }, &.{}, .{}, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expectEqualStrings("usage: check_no_legacy_make [--selftest]\n", streams.err.written());
}

test "a positional path is a usage error, this gate takes no file list" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{"scripts/a.sh"}, &.{}, .{}, &streams);
    try testing.expectEqual(@as(u8, 2), status);
}

test "a clean scope exits 0 and prints the file count" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "set -eu\njust ci\n");
    try tree.write("docs/a.md", "GNU Make is an upstream dependency.\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(
        &tree,
        &.{},
        &.{ "scripts/a.sh", "docs/a.md" },
        .{ .floor = 2 },
        &streams,
    );
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (3 authored files)\n", streams.out.written());
    try testing.expectEqualStrings("", streams.err.written());
}

test "a finding exits 1 and names the path, line and invocation" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "set -eu\nmake ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"scripts/a.sh"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expectEqualStrings("", streams.out.written());
    try testing.expectEqualStrings(
        "check_no_legacy_make: legacy repository task references:\n" ++
            "  scripts/a.sh:2: legacy repository task: make ci\n" ++
            "Use the authoritative namespaced Just recipe instead.\n",
        streams.err.written(),
    );
}

test "findings are reported in sorted path order" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/b.sh", "make b\n");
    try tree.write("docs/a.md", "Please run make a\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(
        &tree,
        &.{},
        &.{ "scripts/b.sh", "docs/a.md" },
        .{ .floor = 1 },
        &streams,
    );
    try testing.expectEqual(@as(u8, 1), status);
    const first = std.mem.indexOf(u8, streams.err.written(), "docs/a.md").?;
    const second = std.mem.indexOf(u8, streams.err.written(), "scripts/b.sh").?;
    try testing.expect(first < second);
}

test "a scope below the floor is an error, never a pass" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "just ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"scripts/a.sh"}, .{ .floor = 650 }, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.written(), "scope collapsed to 2 file(s)") != null);
    try testing.expect(std.mem.indexOf(u8, streams.err.written(), self_rel) != null);
}

test "a scope missing this gate's own source is an error" {
    var tree = Tree.init();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "just ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"scripts/a.sh"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 2), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.written(), "scope collapsed to 1 file(s)") != null);
}

test "this gate's own source is force-added even when the census omits it" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (1 authored files)\n", streams.out.written());
}

test "an unselected path is not scanned" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("libs/ra8_ui/src/ui.c", "make ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"libs/ra8_ui/src/ui.c"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (1 authored files)\n", streams.out.written());
}

test "a vendored path is excluded before selection" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("libs/third_party/x/README.md", "Please run make ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(
        &tree,
        &.{},
        &.{"libs/third_party/x/README.md"},
        .{ .floor = 1 },
        &streams,
    );
    try testing.expectEqual(@as(u8, 0), status);
}

test "a census entry that is not a regular file is dropped" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.dir.dir.createDirPath(testing.io, "scripts/subdir.sh");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(
        &tree,
        &.{ "scripts/subdir.sh", "scripts/missing.sh" },
        &.{},
        .{ .floor = 1 },
        &streams,
    );
    try testing.expectEqual(@as(u8, 2), status);
}

test "an exact-named surface is selected" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("justfile", "ci:\n    just build\n");
    try tree.write("CMakePresets.json", "{}\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(
        &tree,
        &.{},
        &.{ "justfile", "CMakePresets.json" },
        .{ .floor = 3 },
        &streams,
    );
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (3 authored files)\n", streams.out.written());
}

test "a Dockerfile anywhere is selected and scanned as an active surface" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("ci/images/Dockerfile", "FROM debian\nRUN make coverage\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"ci/images/Dockerfile"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(
        u8,
        streams.err.written(),
        "ci/images/Dockerfile:2: legacy repository task: make coverage",
    ) != null);
}

test "a baseline text file under .github is selected" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write(".github/misra-baseline.txt", "# make ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{".github/misra-baseline.txt"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
}

test "a documentation source is scanned without the active command forms" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("docs/a.md", "make ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"docs/a.md"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
}

test "a source that does not decode is skipped, not scanned" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("docs/binary.md", "\xff\xfe make ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"docs/binary.md"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (2 authored files)\n", streams.out.written());
}

test "a duplicated census entry is counted once" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "just ci\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(
        &tree,
        &.{},
        &.{ "scripts/a.sh", "scripts/a.sh" },
        .{ .floor = 2 },
        &streams,
    );
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (2 authored files)\n", streams.out.written());
}

test "the empty census entry git prints after its final NUL is ignored" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{ "", "" }, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("check_no_legacy_make: clean (1 authored files)\n", streams.out.written());
}

test "a CRLF source keeps the predecessor's line numbers" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "one\r\ntwo\r\nmake ci\r\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"scripts/a.sh"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.written(), "scripts/a.sh:3:") != null);
}

test "every finding in a source is reported, not just the first" {
    var tree = try seededTree();
    defer tree.deinit();
    try tree.write("scripts/a.sh", "make a\nmake b\ncmd=(gmake c)\n");
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"scripts/a.sh"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expectEqual(@as(usize, 3), std.mem.count(u8, streams.err.written(), "legacy repository task: "));
}

test "the gate's own source is scanned, and stays quiet" {
    var tree = try seededTree();
    defer tree.deinit();
    var streams = Streams.init();
    defer streams.deinit();
    // The test runner is invoked from the tool directory by hand and from the
    // repository root by the build graph, so both spellings are tried.
    const cwd = std.Io.Dir.cwd();
    const limit: std.Io.Limit = .limited(4 * 1024 * 1024);
    const real = cwd.readFileAlloc(testing.io, "src/internal/root.zig", testing.allocator, limit) catch
        try cwd.readFileAlloc(testing.io, self_rel, testing.allocator, limit);
    defer testing.allocator.free(real);
    try tree.write(self_rel, real);
    const status = try runWith(&tree, &.{}, &.{}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 0), status);
}

test "a source far past any read ceiling is still scanned" {
    var tree = try seededTree();
    defer tree.deinit();
    // Written sparse, so the case costs no real disk: the finding sits past
    // any ceiling a read cap could impose, and the predecessor's read_text()
    // had no cap at all.
    try tree.dir.dir.createDirPath(testing.io, "scripts");
    {
        const file = try tree.dir.dir.createFile(testing.io, "scripts/big.sh", .{});
        defer file.close(testing.io);
        try file.writePositionalAll(testing.io, "\nmake ci\n", 17 * 1024 * 1024);
    }
    var streams = Streams.init();
    defer streams.deinit();
    const status = try runWith(&tree, &.{}, &.{"scripts/big.sh"}, .{ .floor = 1 }, &streams);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expect(std.mem.indexOf(u8, streams.err.written(), "scripts/big.sh:2: legacy repository task: make ci") != null);
    try testing.expect(std.mem.indexOf(u8, streams.err.written(), "cannot read") == null);
}
