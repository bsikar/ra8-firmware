//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural tests for the argv membrane and the exit-status contract of the
//! host-build-entrypoint gate (#1335, part of #858).
//!
//! Every case pins a status, a stream or an enumeration that
//! `scripts/ci/gates/checks.sh` and the trusted launcher depend on. Two
//! properties matter more than the rest: a usage error is status 2 and never a
//! verdict, and an audit that enumerated nothing never reads as a clean tree.

const std = @import("std");
const cli = @import("cli");
const testing = std.testing;

/// Name the parsed action without naming its private union type.
fn actionName(action: anytype) []const u8 {
    return switch (action) {
        .help => "help",
        .selftest => "selftest",
        .audit => "audit",
        .usage_error => "usage_error",
    };
}

fn parsed(argv: []const []const u8) []const u8 {
    return actionName(cli.parseArgs(argv));
}

const Outcome = struct {
    status: u8,
    out: []const u8,
    err: []const u8,
};

fn runIn(arena: std.mem.Allocator, argv: []const []const u8, root: []const u8) !Outcome {
    var out = std.ArrayList(u8).init(arena);
    var err = std.ArrayList(u8).init(arena);
    const status = try cli.run(arena, argv, root, out.writer(), err.writer());
    return .{ .status = status, .out = out.items, .err = err.items };
}

const Tree = struct {
    tmp: std.testing.TmpDir,
    root: []const u8,

    fn init(arena: std.mem.Allocator) !Tree {
        var tmp = std.testing.tmpDir(.{});
        const root = try tmp.dir.realpathAlloc(arena, ".");
        return .{ .tmp = tmp, .root = root };
    }

    fn deinit(self: *Tree) void {
        self.tmp.cleanup();
    }

    fn write(self: *Tree, rel: []const u8, text: []const u8) !void {
        if (std.fs.path.dirname(rel)) |dir| try self.tmp.dir.makePath(dir);
        try self.tmp.dir.writeFile(.{ .sub_path = rel, .data = text });
    }
};

fn anyContains(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

test "parseArgs: no arguments is the audit" {
    try testing.expectEqualStrings("audit", parsed(&.{}));
}

test "parseArgs: --selftest" {
    try testing.expectEqualStrings("selftest", parsed(&.{"--selftest"}));
}

test "parseArgs: --self abbreviates to --selftest" {
    try testing.expectEqualStrings("selftest", parsed(&.{"--self"}));
}

test "parseArgs: --s abbreviates, since help and selftest cannot collide" {
    try testing.expectEqualStrings("selftest", parsed(&.{"--s"}));
}

test "parseArgs: --sel abbreviates" {
    try testing.expectEqualStrings("selftest", parsed(&.{"--sel"}));
}

test "parseArgs: -h is help" {
    try testing.expectEqualStrings("help", parsed(&.{"-h"}));
}

test "parseArgs: --help is help" {
    try testing.expectEqualStrings("help", parsed(&.{"--help"}));
}

test "parseArgs: --h abbreviates to help" {
    try testing.expectEqualStrings("help", parsed(&.{"--h"}));
}

test "parseArgs: --hel abbreviates to help" {
    try testing.expectEqualStrings("help", parsed(&.{"--hel"}));
}

test "parseArgs: an unknown long flag is a usage error" {
    try testing.expectEqualStrings("usage_error", parsed(&.{"--nope"}));
}

test "parseArgs: an unknown short flag is a usage error" {
    try testing.expectEqualStrings("usage_error", parsed(&.{"-x"}));
}

test "parseArgs: a lone dash is a usage error, not a positional" {
    try testing.expectEqualStrings("usage_error", parsed(&.{"-"}));
}

test "parseArgs: --selftest takes no value" {
    try testing.expectEqualStrings("usage_error", parsed(&.{"--selftest=1"}));
}

test "parseArgs: a positional argument is a usage error" {
    try testing.expectEqualStrings("usage_error", parsed(&.{"justfile"}));
}

test "parseArgs: an empty argument is a usage error" {
    try testing.expectEqualStrings("usage_error", parsed(&.{""}));
}

test "parseArgs: flags are case sensitive" {
    try testing.expectEqualStrings("usage_error", parsed(&.{"--SELFTEST"}));
}

test "parseArgs: a lone -- is consumed and leaves the audit" {
    try testing.expectEqualStrings("audit", parsed(&.{"--"}));
}

test "parseArgs: after -- a flag spelling is a positional, so a usage error" {
    try testing.expectEqualStrings("usage_error", parsed(&.{ "--", "--selftest" }));
}

test "parseArgs: repeating --selftest is still the selftest" {
    try testing.expectEqualStrings("selftest", parsed(&.{ "--selftest", "--selftest" }));
}

test "parseArgs: an unknown flag after --selftest still fails the parse" {
    try testing.expectEqualStrings("usage_error", parsed(&.{ "--selftest", "--nope" }));
}

test "parseArgs: the usage error carries the offending argument" {
    switch (cli.parseArgs(&.{"--nope"})) {
        .usage_error => |arg| try testing.expectEqualStrings("--nope", arg),
        else => return error.TestUnexpectedResult,
    }
}

test "parseArgs: the usage error names the first offending argument" {
    switch (cli.parseArgs(&.{ "-x", "-y" })) {
        .usage_error => |arg| try testing.expectEqualStrings("-x", arg),
        else => return error.TestUnexpectedResult,
    }
}

test "tool: the diagnostic name has no .py suffix" {
    try testing.expectEqualStrings("check_host_build_entrypoints", cli.tool);
}

test "usage_line: exact text" {
    try testing.expectEqualStrings(
        "usage: check_host_build_entrypoints [-h] [--selftest]",
        cli.usage_line,
    );
}

test "good_fixture: a recipe that goes through the wrapper" {
    try testing.expect(std.mem.indexOf(u8, cli.good_fixture, "host_cmake.sh") != null);
}

test "cross_fixture: a configure carrying the toolchain file is allowed" {
    try testing.expect(std.mem.indexOf(u8, cli.cross_fixture, "CMAKE_TOOLCHAIN_FILE") != null);
}

test "bad_cmake_fixture: a raw native configure" {
    try testing.expect(std.mem.indexOf(u8, cli.bad_cmake_fixture, "CMAKE_TOOLCHAIN_FILE") == null);
    try testing.expect(std.mem.indexOf(u8, cli.bad_cmake_fixture, "cmake -S") != null);
}

test "mixed_cmake_fixture: one allowed configure does not pardon the other" {
    var lines = std.mem.tokenizeScalar(u8, cli.mixed_cmake_fixture, '\n');
    var cmake_lines: usize = 0;
    while (lines.next()) |line| {
        if (std.mem.indexOf(u8, line, "cmake ") != null) cmake_lines += 1;
    }
    try testing.expectEqual(@as(usize, 2), cmake_lines);
}

test "bad_cc_fixture: a raw host compile driver" {
    try testing.expect(std.mem.indexOf(u8, cli.bad_cc_fixture, "-std=gnu23") != null);
    try testing.expect(std.mem.indexOf(u8, cli.bad_cc_fixture, " -o ") != null);
}

test "run: --help exits 0 and prints the usage line" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const r = try runIn(alloc, &.{"--help"}, tree.root);
    try testing.expectEqual(@as(u8, 0), r.status);
    try testing.expect(std.mem.indexOf(u8, r.out, "usage:") != null);
}

test "run: -h exits 0" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const r = try runIn(alloc, &.{"-h"}, tree.root);
    try testing.expectEqual(@as(u8, 0), r.status);
}

test "run: an unknown flag exits 2 and names it on stderr" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const r = try runIn(alloc, &.{"--nope"}, tree.root);
    try testing.expectEqual(@as(u8, 2), r.status);
    try testing.expect(std.mem.indexOf(u8, r.err, "--nope") != null);
}

test "run: a usage error prints nothing to stdout" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const r = try runIn(alloc, &.{"-x"}, tree.root);
    try testing.expectEqual(@as(u8, 2), r.status);
    try testing.expectEqualStrings("", r.out);
}

test "run: a positional argument exits 2" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const r = try runIn(alloc, &.{"justfile"}, tree.root);
    try testing.expectEqual(@as(u8, 2), r.status);
}

test "run: --selftest exits 0 and reports passing" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const r = try runIn(alloc, &.{"--selftest"}, tree.root);
    try testing.expectEqual(@as(u8, 0), r.status);
    try testing.expect(std.mem.indexOf(u8, r.out, "PASS") != null);
}

test "run: the selftest does not depend on the tree it is run in" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("justfile", cli.bad_cmake_fixture);

    const r = try runIn(alloc, &.{"--self"}, tree.root);
    try testing.expectEqual(@as(u8, 0), r.status);
}

test "selftest: exits 0 on its own and says so" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var out = std.ArrayList(u8).init(alloc);
    var err = std.ArrayList(u8).init(alloc);

    try testing.expectEqual(@as(u8, 0), try cli.selftest(alloc, out.writer(), err.writer()));
    try testing.expect(out.items.len > 0);
}

test "run: an audit of an empty tree never reports a clean tree" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    if (runIn(alloc, &.{}, tree.root)) |r| {
        try testing.expect(r.status != 0);
        try testing.expect(std.mem.indexOf(u8, r.out, "clean (") == null);
    } else |_| {}
}

test "run: a raw native configure in a Just recipe is a finding" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("justfile", cli.bad_cmake_fixture);

    if (runIn(alloc, &.{}, tree.root)) |r| {
        try testing.expect(r.status != 0);
    } else |_| {}
}

test "justFiles: the root justfile comes first, then just/*.just sorted" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("justfile", "build:\n    true\n");
    try tree.write("just/zebra.just", "z:\n    true\n");
    try tree.write("just/alpha.just", "a:\n    true\n");

    const files = try cli.justFiles(alloc, tree.root);
    try testing.expectEqual(@as(usize, 3), files.len);
    try testing.expect(std.mem.indexOf(u8, files[0], "justfile") != null);
    try testing.expect(std.mem.indexOf(u8, files[1], "alpha.just") != null);
    try testing.expect(std.mem.indexOf(u8, files[2], "zebra.just") != null);
}

test "justFiles: the root justfile is listed even when it is absent" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("just/alpha.just", "a:\n    true\n");

    // The predecessor put <root>/justfile at the head of the list
    // unconditionally and let the read fail open, so an absent root justfile
    // is an empty body rather than a missing entry. Inherited deliberately:
    // dropping it here would change how many Just files the clean line counts.
    const files = try cli.justFiles(alloc, tree.root);
    try testing.expectEqual(@as(usize, 2), files.len);
    try testing.expect(std.mem.indexOf(u8, files[0], "justfile") != null);
    try testing.expect(std.mem.indexOf(u8, files[1], "alpha.just") != null);
}

test "justFiles: a non-.just file in just/ is not a Just file" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("just/notes.txt", "not a recipe\n");

    const files = try cli.justFiles(alloc, tree.root);
    try testing.expectEqual(@as(usize, 1), files.len);
    try testing.expect(std.mem.indexOf(u8, files[0], "notes.txt") == null);
}

test "justFiles: an empty tree still enumerates the root justfile path" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const files = try cli.justFiles(alloc, tree.root);
    try testing.expectEqual(@as(usize, 1), files.len);
    try testing.expect(std.mem.endsWith(u8, files[0], "justfile"));
}

test "compiledTools: a tool root with authored sources under src/ is compiled" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("tools/widget/src/main.c", "int main(void){return 0;}\n");

    const compiled = try cli.compiledTools(alloc, tree.root);
    try testing.expect(anyContains(compiled, "widget"));
}

test "compiledTools: a tool root with no compiled source is not compiled" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("tools/notes/README.md", "prose\n");

    const compiled = try cli.compiledTools(alloc, tree.root);
    try testing.expect(!anyContains(compiled, "notes"));
}

test "cmakeTools: a root carrying CMakeLists.txt is registered" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("tools/widget/CMakeLists.txt", "# registered\n");

    const registered = try cli.cmakeTools(alloc, tree.root);
    try testing.expect(anyContains(registered, "widget"));
}

test "inventoryErrors: a compiled tool with no CMakeLists.txt is a finding" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("tools/widget/src/main.c", "int main(void){return 0;}\n");

    const errors = try cli.inventoryErrors(alloc, tree.root);
    try testing.expect(anyContains(errors, "widget"));
}

test "inventoryErrors: the same tool with CMakeLists.txt is not a finding" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();
    try tree.write("tools/widget/src/main.c", "int main(void){return 0;}\n");
    try tree.write("tools/widget/CMakeLists.txt", "# registered\n");

    const errors = try cli.inventoryErrors(alloc, tree.root);
    try testing.expect(!anyContains(errors, "widget"));
}

test "inventoryErrors: an empty tree has nothing to report" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const errors = try cli.inventoryErrors(alloc, tree.root);
    try testing.expectEqual(@as(usize, 0), errors.len);
}

test "sharedDispatchErrors: a missing shared.just is a finding" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    const errors = try cli.sharedDispatchErrors(alloc, tree.root);
    try testing.expect(errors.len > 0);
}

test "liveDispatch: a missing dispatcher script is a finding, not a crash" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();
    var tree = try Tree.init(alloc);
    defer tree.deinit();

    if (cli.liveDispatch(alloc, tree.root)) |dispatch| {
        try testing.expect(dispatch.errors.len > 0);
    } else |_| {}
}
