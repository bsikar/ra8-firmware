//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract tests for the PEP 668 override gate (#858).
//!
//! `cli.run` takes the directory, the census and both streams, so 0, 1 and 2
//! are all provable here with no process and no real repository. Fixtures
//! that must carry the rejected option build it from the halves, because the
//! gate scans its own sources.

const std = @import("std");
const cli = @import("cli");

const forbidden = "--break-" ++ "system-packages";
const unsafe_line = "python3 -m pip install " ++ forbidden ++ " libclang\n";

/// One gate invocation: the status it returned and what it wrote where.
const Outcome = struct {
    status: u8,
    out: []u8,
    err: []u8,

    fn deinit(self: Outcome) void {
        std.testing.allocator.free(self.out);
        std.testing.allocator.free(self.err);
    }
};

fn invoke(
    dir: std.fs.Dir,
    argv: []const []const u8,
    census: []const []const u8,
    policy: cli.Policy,
) !Outcome {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var out = std.ArrayList(u8).init(std.testing.allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(std.testing.allocator);
    errdefer err.deinit();
    const status = try cli.run(
        arena.allocator(),
        dir,
        ".",
        argv,
        .{ .provided = census },
        policy,
        out.writer(),
        err.writer(),
    );
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

/// Policy used by the fixtures: a one-file floor and a stand-in self source.
const test_policy = cli.Policy{ .floor = 1, .self_source = "self.zig" };

fn write(dir: std.fs.Dir, path: []const u8, data: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = path, .data = data });
}

test "the live policy carries the inherited floor and self source" {
    try std.testing.expectEqual(@as(usize, 4000), cli.Policy.default.floor);
    try std.testing.expectEqualStrings(
        "tools/check_no_unsafe_python_install/src/internal/root.zig",
        cli.Policy.default.self_source,
    );
}

test "the gate names itself without the retired .py suffix" {
    try std.testing.expectEqualStrings("check_no_unsafe_python_install", cli.tool);
}

test "a lone --selftest passes and reports its case count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(tmp.dir, &.{"--selftest"}, &.{}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_no_unsafe_python_install --selftest: PASS (4 cases)\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "--selftest beside another argument is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(tmp.dir, &.{ "--selftest", "extra" }, &.{}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings(
        "usage: check_no_unsafe_python_install [--selftest]\n",
        result.err,
    );
}

test "an unknown flag is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(tmp.dir, &.{"--all"}, &.{}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("", result.out);
}

test "a positional path is a usage error, the gate takes no targets" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try invoke(tmp.dir, &.{"docs/DOCS.md"}, &.{}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a clean census exits 0 and reports how many files were scanned" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "const forbidden = \"--break-\" ++ \"x\";\n");
    try write(tmp.dir, "docs/DOCS.md", "python3 -m venv .venv\n");
    const result = try invoke(tmp.dir, &.{}, &.{"docs/DOCS.md"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_no_unsafe_python_install: clean (2 first-party files)\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "one override exits 1 with the finding and the remedy on stderr" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, ".github/workflows/ci.yml", "steps:\n  run: " ++ unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{".github/workflows/ci.yml"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings("", result.out);
    try std.testing.expectEqualStrings(
        "unsafe system-Python package override found:\n" ++
            "  .github/workflows/ci.yml:2\n" ++
            "Create a venv and wire its interpreter/PATH explicitly.\n",
        result.err,
    );
}

test "findings are reported in path order, not census order" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "a.md", unsafe_line);
    try write(tmp.dir, "b.md", unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{ "b.md", "a.md" }, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "  a.md:1\n  b.md:1\n") != null);
}

test "several overrides in one file are all reported" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "hints.md", unsafe_line ++ "clean\n" ++ unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{"hints.md"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "  hints.md:1\n  hints.md:3\n") != null);
}

test "an override in the gate's own source is still a finding" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "  self.zig:1\n") != null);
}

test "a census below the floor exits 2, never a clean verdict" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "a.md", "clean\n");
    const policy = cli.Policy{ .floor = 5, .self_source = "self.zig" };
    const result = try invoke(tmp.dir, &.{}, &.{"a.md"}, policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("", result.out);
    try std.testing.expectEqualStrings(
        "scope collapsed to 2 files; expected at least 5 including self.zig\n",
        result.err,
    );
}

test "a census missing the gate's own source exits 2" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "a.md", "clean\n");
    const result = try invoke(tmp.dir, &.{}, &.{"a.md"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "including self.zig") != null);
}

test "an override inside a vendored tree is not the repository's problem" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "libs/third_party/setup.md", unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{"libs/third_party/setup.md"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_no_unsafe_python_install: clean (1 first-party files)\n",
        result.out,
    );
}

test "a census entry that no longer exists is skipped, not an error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    const result = try invoke(tmp.dir, &.{}, &.{ "gone.md", "" }, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_no_unsafe_python_install: clean (1 first-party files)\n",
        result.out,
    );
}

test "a directory in the census is not scanned as a file" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try tmp.dir.makePath("docs");
    const result = try invoke(tmp.dir, &.{}, &.{"docs"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a binary asset is skipped rather than reported" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "blob.bin", "\xff\xfe" ++ unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{"blob.bin"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a CRLF file reports the line the reader would see" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "guide.md", "intro\r\nsetup\r\nrun " ++ forbidden ++ "\r\n");
    const result = try invoke(tmp.dir, &.{}, &.{"guide.md"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "  guide.md:3\n") != null);
}

test "a duplicated census entry is scanned once" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "a.md", unsafe_line);
    const result = try invoke(tmp.dir, &.{}, &.{ "a.md", "a.md", "self.zig" }, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "  a.md:1\n") != null);
    try std.testing.expectEqual(@as(usize, 1), std.mem.count(u8, result.err, "a.md:1"));
}

test "an unreadable file exits 1 rather than passing the tree" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "self.zig", "clean\n");
    try write(tmp.dir, "secret.md", "clean\n");
    {
        const file = try tmp.dir.openFile("secret.md", .{});
        defer file.close();
        try file.chmod(0);
    }
    // A privileged test runner reads it anyway, and then there is no
    // unreadable file to assert about; skip rather than assert a falsehood.
    if (tmp.dir.openFile("secret.md", .{})) |probe| {
        probe.close();
        return error.SkipZigTest;
    } else |_| {}
    const result = try invoke(tmp.dir, &.{}, &.{"secret.md"}, test_policy);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "cannot read secret.md") != null);
}
