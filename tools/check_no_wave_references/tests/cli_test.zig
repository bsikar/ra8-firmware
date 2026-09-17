//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and report tests for the session-reference gate (#858).
//!
//! `cli.run` is parameterised on a directory handle, a repository root, a
//! census and both streams, so the whole contract the launcher passes through
//! is provable here with no process and no real repository.

const std = @import("std");
const cli = @import("cli");

/// A scratch repository: a temporary directory with files written into it and
/// a census the gate is handed instead of shelling out to git.
const Fixture = struct {
    tmp: std.testing.TmpDir,
    root: []const u8,
    allocator: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) !Fixture {
        var tmp = std.testing.tmpDir(.{});
        const root = try tmp.dir.realpathAlloc(allocator, ".");
        return .{ .tmp = tmp, .root = root, .allocator = allocator };
    }

    fn write(self: *Fixture, rel: []const u8, body: []const u8) !void {
        if (std.fs.path.dirname(rel)) |parent| try self.tmp.dir.makePath(parent);
        try self.tmp.dir.writeFile(.{ .sub_path = rel, .data = body });
    }

    fn deinit(self: *Fixture) void {
        self.allocator.free(self.root);
        self.tmp.cleanup();
    }
};

/// A census padded to clear the tracked floor, so a test can say what it means
/// to test without also having to invent a thousand paths.
fn paddedCensus(allocator: std.mem.Allocator, real: []const []const u8, total: usize) ![][]const u8 {
    var rels = std.ArrayList([]const u8).init(allocator);
    for (real) |rel| try rels.append(rel);
    var index: usize = 0;
    while (rels.items.len < total) : (index += 1) {
        try rels.append(try std.fmt.allocPrint(allocator, "pad/f{d}.md", .{index}));
    }
    return rels.toOwnedSlice();
}

fn freeCensus(allocator: std.mem.Allocator, census: [][]const u8, real: usize) void {
    for (census[real..]) |rel| allocator.free(rel);
    allocator.free(census);
}

/// The live policy is too large for a fixture; these floors keep the SHAPE of
/// the contract (a scope below the floor is fatal) at a testable size.
const test_policy = cli.Policy{ .file_floor = 3, .tracked_floor = 2 };

test "the tool names itself as the gate does" {
    try std.testing.expectEqualStrings("check_no_wave_references", cli.tool);
}

test "the live policy carries the predecessor's floors" {
    try std.testing.expectEqual(@as(usize, 2500), cli.Policy.default.file_floor);
    try std.testing.expectEqual(@as(usize, 1000), cli.Policy.default.tracked_floor);
}

test "the selftest carries exactly the predecessor's five detector cases" {
    try std.testing.expectEqual(@as(usize, 5), cli.detector_cases.len);
    var must_fire: usize = 0;
    for (cli.detector_cases) |item| {
        if (item.must_fire) must_fire += 1;
    }
    try std.testing.expectEqual(@as(usize, 2), must_fire);
}

test "a clean tree exits 0 with the clean line on stdout" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "the sine wave is smooth\n");
    try fixture.write("b.md", "wave_table[0] holds the sample\n");
    try fixture.write("c.md", "nothing here\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqualStrings("no-wave-refs: 0 violations -- gate clean.\n", out.items);
    try std.testing.expectEqualStrings("", err.items);
}

test "one reference exits 1 and is reported with its path, line and snippet" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "clean\nfixed in Wave 70\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.startsWith(u8, out.items, "no-wave-refs: 1 violations found.\n"));
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  a.md:2 fixed in Wave 70\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "Per-line opt-out:") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "scripts/fix/fix_wave_references.py --apply") != null);
    try std.testing.expectEqualStrings("", err.items);
}

test "the whole report goes to stdout, as the predecessor printed it" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "wave 3\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    _ = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqualStrings("", err.items);
    try std.testing.expect(std.mem.endsWith(
        u8,
        out.items,
        "Auto-fix helper: scripts/fix/fix_wave_references.py --apply\n",
    ));
}

test "findings past the ceiling are truncated with a count" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var body = std.ArrayList(u8).init(allocator);
    defer body.deinit();
    var index: usize = 0;
    while (index < 53) : (index += 1) try body.writer().print("wave {d}\n", .{index + 1});
    try fixture.write("a.md", body.items);
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "no-wave-refs: 53 violations found.") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  ... 3 more (truncated)\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  a.md:50 wave 50\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  a.md:51 wave 51\n") == null);
}

test "exactly the ceiling is not truncated" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var body = std.ArrayList(u8).init(allocator);
    defer body.deinit();
    var index: usize = 0;
    while (index < 50) : (index += 1) try body.writer().print("wave {d}\n", .{index + 1});
    try fixture.write("a.md", body.items);
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    _ = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expect(std.mem.indexOf(u8, out.items, "(truncated)") == null);
}

test "a self-exempt file is skipped whole" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("docs/STYLE_GUIDE.md", "never write Wave 70 in source\n");
    try fixture.write("CLAUDE.md", "the Wave 12 wording is banned\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "docs/STYLE_GUIDE.md", "CLAUDE.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
}

test "an opt-out line keeps the tree clean" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "see wave 12  WAVE-OK: upstream register name\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
}

test "an out-of-scope suffix is never read" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("notes.rst", "fixed in Wave 70\n");
    try fixture.write("a.md", "clean\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "notes.rst", "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
}

test "an unreadable path contributes nothing and is not an error" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "clean\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md", "absent.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
}

test "a file that is not UTF-8 is skipped, exactly as the decode error was" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "fixed in Wave 70\xff\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
}

test "a collapsed scope is FATAL with exit 2, never a clean verdict" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "notes.rst" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expectEqualStrings("", out.items);
    try std.testing.expect(std.mem.indexOf(u8, err.items, "FATAL -- only 1 file(s) in scope") != null);
    try std.testing.expect(std.mem.indexOf(u8, err.items, "floor is 3") != null);
}

test "a collapsed census is FATAL with exit 2 before any scope is taken" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{"a.md"};
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expect(std.mem.indexOf(u8, err.items, "FATAL -- only 1 tracked path(s)") != null);
}

test "an empty census is FATAL, not vacuously clean" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &[_][]const u8{} },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expectEqualStrings("", out.items);
}

test "--selftest exits 0 and prints one ok line per assertion" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const real = [_][]const u8{ "infra/a.md", "just/ci.just" };
    const census = try paddedCensus(allocator, &real, 8);
    defer freeCensus(allocator, census, real.len);

    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{"--selftest"},
        .{ .provided = census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqual(@as(usize, 8), std.mem.count(u8, out.items, "  [ok] "));
    try std.testing.expect(std.mem.indexOf(u8, out.items, "[FAIL]") == null);
    try std.testing.expect(std.mem.endsWith(
        u8,
        out.items,
        "selftest: all assertions held (both directions).\n",
    ));
    try std.testing.expectEqualStrings("", err.items);
}

test "--selftest fails when the scope cannot reach the roots it must" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = try paddedCensus(allocator, &[_][]const u8{}, 8);
    defer freeCensus(allocator, census, 0);

    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{"--selftest"},
        .{ .provided = census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expectEqual(@as(usize, 2), std.mem.count(u8, out.items, "  [FAIL] "));
    try std.testing.expect(std.mem.indexOf(u8, err.items, "SELFTEST FAILED: 2 assertion(s)") != null);
}

test "--selftest fails when the scope collapses below the floor" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "infra/a.md", "just/ci.just" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{"--selftest"},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "[FAIL] derived scope sees 2 file(s)") != null);
}

test "--selftest wins wherever it sits in argv" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "fixed in Wave 70\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const real = [_][]const u8{ "infra/a.md", "just/ci.just" };
    const census = try paddedCensus(allocator, &real, 8);
    defer freeCensus(allocator, census, real.len);

    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{ "ignored", "--selftest" },
        .{ .provided = census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "selftest:") != null);
}

test "an unknown argument does not change the sweep, as it never did" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "clean\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{"--nope"},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqualStrings("no-wave-refs: 0 violations -- gate clean.\n", out.items);
}

test "a docs-side vendored path is out of scope even when it offends" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("docs/reference/hum.md", "fixed in Wave 70\n");
    try fixture.write("a.md", "clean\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "docs/reference/hum.md", "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 0), status);
}

test "a long offending line is trimmed in the report" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "wave 7 " ++ ("x" ** 200) ++ "\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "x...\n") != null);
}

test "findings from several files all appear" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("a.md", "wave 1\n");
    try fixture.write("b.md", "clean\n");
    try fixture.write("c.md", "see wave-43b\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "a.md", "b.md", "c.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "no-wave-refs: 2 violations found.") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  a.md:1 wave 1\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  c.md:1 see wave-43b\n") != null);
}

test "a listfile with no suffix is in scope" {
    const allocator = std.testing.allocator;
    var fixture = try Fixture.init(allocator);
    defer fixture.deinit();
    try fixture.write("justfile", "# fixed in Wave 70\n");
    try fixture.write("a.md", "clean\n");
    try fixture.write("b.md", "clean\n");

    var out = std.ArrayList(u8).init(allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    defer err.deinit();

    const census = [_][]const u8{ "justfile", "a.md", "b.md" };
    const status = try cli.run(
        allocator,
        fixture.tmp.dir,
        fixture.root,
        &.{},
        .{ .provided = &census },
        test_policy,
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  justfile:1 # fixed in Wave 70\n") != null);
}
