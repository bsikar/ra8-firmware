//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the obsolete-standards argv membrane and
//! exit-status contract (#858).
//!
//! The contract is the whole point of the gate: 0 clean or selftest held, 1 a
//! finding or a failed selftest, 2 a usage error, a missing mode, or an
//! enumeration that collapsed. Each status is pinned here with a provided
//! census and a scratch tree, so nothing below needs a repository or a
//! process.

const std = @import("std");
const cli = @import("cli");

fn actionName(action: cli.Action) []const u8 {
    return switch (action) {
        .help => "help",
        .selftest => "selftest",
        .all => "all",
        .staged => "staged",
        .none => "none",
        .usage_error => "usage_error",
    };
}

fn expectAction(argv: []const []const u8, expected: []const u8) !void {
    try std.testing.expectEqualStrings(expected, actionName(cli.parseArgs(argv)));
}

/// A scratch repository: a temporary directory plus the two output buffers
/// `run` writes into.
const Harness = struct {
    tmp: std.testing.TmpDir,
    out_buffer: [8192]u8 = undefined,
    err_buffer: [8192]u8 = undefined,

    fn init() Harness {
        return .{ .tmp = std.testing.tmpDir(.{}) };
    }

    fn deinit(self: *Harness) void {
        self.tmp.cleanup();
    }

    fn write(self: *Harness, rel: []const u8, body: []const u8) !void {
        if (std.fs.path.dirname(rel)) |parent| try self.tmp.dir.makePath(parent);
        try self.tmp.dir.writeFile(.{ .sub_path = rel, .data = body });
    }

    fn run(
        self: *Harness,
        argv: []const []const u8,
        census: cli.Census,
        staged: cli.Staged,
        policy: cli.Policy,
    ) !struct { status: u8, out: []const u8, err: []const u8 } {
        var out_stream = std.io.fixedBufferStream(&self.out_buffer);
        var err_stream = std.io.fixedBufferStream(&self.err_buffer);
        const status = try cli.run(
            std.testing.allocator,
            self.tmp.dir,
            ".",
            argv,
            census,
            staged,
            policy,
            out_stream.writer(),
            err_stream.writer(),
        );
        return .{
            .status = status,
            .out = out_stream.getWritten(),
            .err = err_stream.getWritten(),
        };
    }
};

/// A census padded to clear the census floor without touching the scan: the
/// padding paths carry an unscanned suffix, so only the named files matter.
fn paddedCensus(allocator: std.mem.Allocator, real: []const []const u8, total: usize) ![][]const u8 {
    var list = std.ArrayList([]const u8).init(allocator);
    try list.appendSlice(real);
    var index: usize = 0;
    while (list.items.len < total) : (index += 1) {
        try list.append(try std.fmt.allocPrint(allocator, "pad/{d}.bin", .{index}));
    }
    return list.toOwnedSlice();
}

// ---------------------------------------------------------------------------
// parseArgs
// ---------------------------------------------------------------------------

test "no arguments selects no mode, which is an error rather than a default" {
    try expectAction(&.{}, "none");
}

test "each long option selects its own action" {
    try expectAction(&.{"--all"}, "all");
    try expectAction(&.{"--staged"}, "staged");
    try expectAction(&.{"--selftest"}, "selftest");
    try expectAction(&.{"--help"}, "help");
}

test "-h selects help" {
    try expectAction(&.{"-h"}, "help");
}

test "an unambiguous abbreviation resolves, as argparse resolves it" {
    try expectAction(&.{"--a"}, "all");
    try expectAction(&.{"--st"}, "staged");
    try expectAction(&.{"--se"}, "selftest");
    try expectAction(&.{"--sel"}, "selftest");
    try expectAction(&.{"--h"}, "help");
}

test "--s is ambiguous between --staged and --selftest" {
    try expectAction(&.{"--s"}, "usage_error");
    switch (cli.parseArgs(&.{"--s"})) {
        .usage_error => |detail| try std.testing.expectEqualStrings("ambiguous option", detail),
        else => return error.TestUnexpectedResult,
    }
}

test "--all and --staged together are refused by the mutually exclusive group" {
    switch (cli.parseArgs(&.{ "--all", "--staged" })) {
        .usage_error => |detail| try std.testing.expect(std.mem.indexOf(u8, detail, "not allowed with argument --all") != null),
        else => return error.TestUnexpectedResult,
    }
}

test "--selftest wins over a mode, because the predecessor tested it first" {
    try expectAction(&.{ "--selftest", "--all" }, "selftest");
    try expectAction(&.{ "--staged", "--selftest" }, "selftest");
}

test "help wins over everything, as argparse acts on -h immediately" {
    try expectAction(&.{ "--all", "-h" }, "help");
}

test "an unknown option is a usage error" {
    try expectAction(&.{"--nope"}, "usage_error");
    try expectAction(&.{"-x"}, "usage_error");
}

test "a lone dash is a positional, so it is a usage error" {
    try expectAction(&.{"-"}, "usage_error");
}

test "a positional argument is a usage error: this gate takes no file list" {
    try expectAction(&.{"docs/a.md"}, "usage_error");
}

test "an explicit value on a store_true option is a usage error" {
    try expectAction(&.{"--all=1"}, "usage_error");
}

test "a lone -- is consumed and selects no mode" {
    try expectAction(&.{"--"}, "none");
}

test "an option after -- is a positional" {
    try expectAction(&.{ "--", "--all" }, "usage_error");
}

test "a repeated flag is harmless" {
    try expectAction(&.{ "--all", "--all" }, "all");
}

test "option matching is case-sensitive" {
    try expectAction(&.{"--ALL"}, "usage_error");
}

test "the usage line names both modes and the selftest" {
    try std.testing.expectEqualStrings(
        "usage: check_obsolete_standards [-h] [--all | --staged] [--selftest]",
        cli.usage_line,
    );
}

// ---------------------------------------------------------------------------
// The selftest fixtures
// ---------------------------------------------------------------------------

test "the selftest carries the predecessor's four detector cases, two each way" {
    try std.testing.expectEqual(@as(usize, 4), cli.cases.len);
    var firing: usize = 0;
    for (cli.cases) |case| {
        if (case.must_fire) firing += 1;
    }
    try std.testing.expectEqual(@as(usize, 2), firing);
}

test "every detector case agrees with its expectation" {
    try std.testing.expectEqual(@as(usize, 0), cli.caseFailures());
}

// ---------------------------------------------------------------------------
// run: help and usage
// ---------------------------------------------------------------------------

test "help exits 0, prints the usage on stdout and nothing on stderr" {
    var harness = Harness.init();
    defer harness.deinit();
    const result = try harness.run(&.{"-h"}, .{ .provided = &.{} }, .{ .provided = &.{} }, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.startsWith(u8, result.out, cli.usage_line));
    try std.testing.expectEqual(@as(usize, 0), result.err.len);
}

test "a usage error exits 2, names the offending spelling class on stderr and prints nothing on stdout" {
    var harness = Harness.init();
    defer harness.deinit();
    const result = try harness.run(&.{"--nope"}, .{ .provided = &.{} }, .{ .provided = &.{} }, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqual(@as(usize, 0), result.out.len);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "error:") != null);
}

test "a bare invocation exits 2 and explains that there is no default mode" {
    var harness = Harness.init();
    defer harness.deinit();
    const result = try harness.run(&.{}, .{ .provided = &.{} }, .{ .provided = &.{} }, cli.Policy.default);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "pass --all") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "where the index is always empty") != null);
}

// ---------------------------------------------------------------------------
// run: --all
// ---------------------------------------------------------------------------

test "a clean sweep exits 0 and reports the file count" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("docs/a.md", "Written to DO-178C Level B.\n");
    try harness.write("docs/b.md", "IEC 61508 SIL 3\n");
    const census = [_][]const u8{ "docs/a.md", "docs/b.md" };
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 2, .tracked_floor = 2 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 2 file(s).\n",
        result.out,
    );
}

test "a citing file exits 1 and quotes the line with its path and number" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("docs/a.md", "intro\nWritten to DO-178B Level B.\n");
    const census = [_][]const u8{"docs/a.md"};
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "[FAIL] Obsolete standard reference detected") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "  docs/a.md:2: Written to DO-178B Level B.\n") != null);
}

test "the hyphen-less spelling is caught by the sweep too" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("a.py", "# targets DO178B objectives\n");
    const census = [_][]const u8{"a.py"};
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "a.py:1:") != null);
}

test "a whitelisted file may cite the token without failing the gate" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("CLAUDE.md", "DO-178B is forbidden; use DO-178C.\n");
    const census = [_][]const u8{"CLAUDE.md"};
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a vendored file may cite whatever standard it was written against" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("libs/vendor/third_party/x.c", "/* DO-178B */\n");
    const census = [_][]const u8{"libs/vendor/third_party/x.c"};
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a file whose suffix is out of scope is neither enumerated nor read" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("a.zig", "// DO-178B\n");
    try harness.write("a.md", "clean\n");
    const census = [_][]const u8{ "a.zig", "a.md" };
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 1 file(s).\n",
        result.out,
    );
}

test "a non-UTF-8 file is skipped silently, as the UnicodeDecodeError branch did" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("a.md", "DO-178B \xff\xfe\n");
    const census = [_][]const u8{"a.md"};
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a census below its floor exits 2 rather than sweeping a collapsed list" {
    var harness = Harness.init();
    defer harness.deinit();
    const census = [_][]const u8{"a.md"};
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1000 },
    );
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "tracked path(s), floor is 1000") != null);
}

test "a sweep below the tree floor exits 2 rather than reporting a clean tree" {
    var harness = Harness.init();
    defer harness.deinit();
    const real = [_][]const u8{"a.md"};
    const census = try paddedCensus(std.testing.allocator, &real, 1000);
    defer {
        for (census[real.len..]) |pad| std.testing.allocator.free(pad);
        std.testing.allocator.free(census);
    }
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = census },
        .{ .provided = &.{} },
        .{ .tree_floor = 500, .tracked_floor = 1000 },
    );
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "below the floor of 500") != null);
}

test "an empty tree never reports clean" {
    var harness = Harness.init();
    defer harness.deinit();
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &.{} },
        .{ .provided = &.{} },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqual(@as(usize, 0), result.out.len);
}

test "a .just file is not enumerated by the sweep, so a citation in one is invisible to --all" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("just/ci.just", "# DO-178B\n");
    const census = [_][]const u8{ "just/ci.just", "a.md" };
    try harness.write("a.md", "clean\n");
    const result = try harness.run(
        &.{"--all"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 1 file(s).\n",
        result.out,
    );
}

// ---------------------------------------------------------------------------
// run: --staged
// ---------------------------------------------------------------------------

test "a staged citation exits 1" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("docs/a.md", "DO-178B\n");
    const staged = [_][]const u8{"docs/a.md"};
    const result = try harness.run(
        &.{"--staged"},
        .{ .provided = &.{} },
        .{ .provided = &staged },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "a staged .just file IS scanned, unlike the tree-wide sweep" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("just/ci.just", "# DO-178B\n");
    const staged = [_][]const u8{"just/ci.just"};
    const result = try harness.run(
        &.{"--staged"},
        .{ .provided = &.{} },
        .{ .provided = &staged },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "just/ci.just:1:") != null);
}

test "an empty index is clean and reports zero files, with no floor applied" {
    var harness = Harness.init();
    defer harness.deinit();
    const result = try harness.run(
        &.{"--staged"},
        .{ .provided = &.{} },
        .{ .provided = &.{} },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 0 file(s).\n",
        result.out,
    );
}

test "a staged path that no longer exists is skipped, as a staged deletion was" {
    var harness = Harness.init();
    defer harness.deinit();
    const staged = [_][]const u8{"gone.md"};
    const result = try harness.run(
        &.{"--staged"},
        .{ .provided = &.{} },
        .{ .provided = &staged },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 1 file(s).\n",
        result.out,
    );
}

test "a staged directory is skipped rather than read" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.tmp.dir.makePath("docs/a.md");
    const staged = [_][]const u8{"docs/a.md"};
    const result = try harness.run(
        &.{"--staged"},
        .{ .provided = &.{} },
        .{ .provided = &staged },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "the staged count reports paths OFFERED, not paths that survived the filter" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("a.zig", "// DO-178B\n");
    const staged = [_][]const u8{ "a.zig", "b.png" };
    const result = try harness.run(
        &.{"--staged"},
        .{ .provided = &.{} },
        .{ .provided = &staged },
        cli.Policy.default,
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 2 file(s).\n",
        result.out,
    );
}

// ---------------------------------------------------------------------------
// run: --selftest
// ---------------------------------------------------------------------------

test "the selftest exits 0 and prints one line per assertion plus the verdict" {
    var harness = Harness.init();
    defer harness.deinit();
    var scope = std.ArrayList([]const u8).init(std.testing.allocator);
    defer {
        for (scope.items) |name| std.testing.allocator.free(name);
        scope.deinit();
    }
    var index: usize = 0;
    while (index < 600) : (index += 1) {
        try scope.append(try std.fmt.allocPrint(std.testing.allocator, "docs/{d}.md", .{index}));
    }
    const result = try harness.run(
        &.{"--selftest"},
        .{ .provided = scope.items },
        .{ .provided = &.{} },
        .{ .tree_floor = 500, .tracked_floor = 500 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    // Four detector cases plus the enumeration-floor assertion.
    try std.testing.expectEqual(@as(usize, 5), std.mem.count(u8, result.out, "[ok] "));
    try std.testing.expect(std.mem.indexOf(u8, result.out, "tree-wide enumeration sees 600 file(s) (floor 500)") != null);
    try std.testing.expect(std.mem.endsWith(u8, result.out, "selftest: all assertions held (both directions).\n"));
}

test "the selftest names each detector case with its direction" {
    var harness = Harness.init();
    defer harness.deinit();
    var scope = std.ArrayList([]const u8).init(std.testing.allocator);
    defer {
        for (scope.items) |name| std.testing.allocator.free(name);
        scope.deinit();
    }
    var index: usize = 0;
    while (index < 10) : (index += 1) {
        try scope.append(try std.fmt.allocPrint(std.testing.allocator, "docs/{d}.md", .{index}));
    }
    const result = try harness.run(
        &.{"--selftest"},
        .{ .provided = scope.items },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "a DO-178B citation (must fire)") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "the hyphen-less DO178B spelling (must fire)") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "the current DO-178C citation (must stay quiet)") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "an unrelated line naming no standard (must stay quiet)") != null);
}

test "the selftest FAILS when the enumeration cannot clear the floor" {
    var harness = Harness.init();
    defer harness.deinit();
    const census = [_][]const u8{ "a.md", "b.md" };
    const result = try harness.run(
        &.{"--selftest"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 500, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "[FAIL] tree-wide enumeration sees 2 file(s) (floor 500)") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "SELFTEST FAILED: 1 assertion(s)") != null);
}

test "the selftest FAILS when the census itself collapsed" {
    var harness = Harness.init();
    defer harness.deinit();
    const census = [_][]const u8{"a.md"};
    const result = try harness.run(
        &.{"--selftest"},
        .{ .provided = &census },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1000 },
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "floor is 1000") != null);
}

test "the selftest is independent of the tree it runs in" {
    var harness = Harness.init();
    defer harness.deinit();
    try harness.write("docs/a.md", "DO-178B\n");
    var scope = std.ArrayList([]const u8).init(std.testing.allocator);
    defer {
        for (scope.items) |name| std.testing.allocator.free(name);
        scope.deinit();
    }
    var index: usize = 0;
    while (index < 10) : (index += 1) {
        try scope.append(try std.fmt.allocPrint(std.testing.allocator, "docs/{d}.md", .{index}));
    }
    const result = try harness.run(
        &.{"--selftest"},
        .{ .provided = scope.items },
        .{ .provided = &.{} },
        .{ .tree_floor = 1, .tracked_floor = 1 },
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
}
