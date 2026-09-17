// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Exit-status contract tests for the stub-crypto gate (#858).
//
// `cli.run` takes a directory handle, a repository root, the governed TU list
// and both streams, so every status the gate can return is provable here with
// no process and no real repository.

const std = @import("std");
const cli = @import("cli");

const talloc = std.testing.allocator;

const guard_open = "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)";

const Run = struct {
    status: u8,
    out: []const u8,
    err: []const u8,

    fn deinit(self: Run) void {
        talloc.free(self.out);
        talloc.free(self.err);
    }
};

/// One stub TU written into a fresh tree, with its source text.
const Fixture = struct {
    rel: []const u8,
    token: []const u8,
    text: ?[]const u8,
};

fn write(dir: std.fs.Dir, rel: []const u8, text: []const u8) !void {
    if (std.fs.path.dirname(rel)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = rel, .data = text });
}

fn runIn(dir: std.fs.Dir, argv: []const []const u8, stubs: []const cli.Stub) !Run {
    var out = std.ArrayList(u8).init(talloc);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(talloc);
    errdefer err.deinit();
    const status = try cli.run(talloc, dir, ".", argv, stubs, out.writer(), err.writer());
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

fn runFixtures(argv: []const []const u8, fixtures: []const Fixture) !Run {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    var stubs = std.ArrayList(cli.Stub).init(talloc);
    defer stubs.deinit();
    for (fixtures) |fixture| {
        if (fixture.text) |text| try write(tmp.dir, fixture.rel, text);
        try stubs.append(.{ .rel = fixture.rel, .token = fixture.token });
    }
    return runIn(tmp.dir, argv, stubs.items);
}

const clean_source = guard_open ++ "\nstatic int tok;\n#else\nreturn k_ra8_err_unsupported;\n#endif\n";

test "a clean tree exits 0 and reports the TU count" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = clean_source },
        .{ .rel = "libs/b/src/two.c", .token = "tok", .text = clean_source },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_stub_crypto_guarded: PASS -- 2 stub crypto TU(s) guarded fail-closed.\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "an empty governed set is a vacuous pass, as the mapping drives the sweep" {
    const result = try runFixtures(&.{}, &.{});
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "0 stub crypto TU(s)") != null);
}

test "a missing stub TU exits 1 rather than reporting a clean sweep" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/gone.c", .token = "tok", .text = null },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "file not found") != null);
}

test "a directory standing where a stub TU belongs is a finding, not a read error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("libs/a/src/one.c");
    const stubs = [_]cli.Stub{.{ .rel = "libs/a/src/one.c", .token = "tok" }};
    const result = try runIn(tmp.dir, &.{}, &stubs);
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "file not found") != null);
    try std.testing.expectEqualStrings("", result.err);
}

test "an unguarded stub TU exits 1 and prints the remedy" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = "static int tok;\n" },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "missing the stub-crypto guard") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "Fix each at the root") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "a real crypto backend") != null);
}

test "the header names the gate and the failure" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = "static int tok;\n" },
    });
    defer result.deinit();
    try std.testing.expect(std.mem.startsWith(
        u8,
        result.out,
        "check_stub_crypto_guarded: insecure placeholder crypto not guarded fail-closed:\n",
    ));
}

test "an else branch returning ok exits 1" {
    const text = guard_open ++ "\nstatic int tok;\n#else\nreturn k_ra8_ok;\n#endif\n";
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = text },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "not fail-closed") != null);
}

test "an escaped insecure body exits 1 naming the line" {
    const text = clean_source ++ "static int tok;\n";
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = text },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "OUTSIDE the guard (line 6)") != null);
}

test "findings from several TUs are all reported in mapping order" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = "static int tok;\n" },
        .{ .rel = "libs/b/src/two.c", .token = "tok", .text = null },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    const first = std.mem.indexOf(u8, result.out, "libs/a/src/one.c").?;
    const second = std.mem.indexOf(u8, result.out, "libs/b/src/two.c").?;
    try std.testing.expect(first < second);
}

test "one broken TU does not stop the sweep of the rest" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = null },
        .{ .rel = "libs/b/src/two.c", .token = "tok", .text = clean_source },
        .{ .rel = "libs/c/src/three.c", .token = "tok", .text = null },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "libs/a/src/one.c") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "libs/c/src/three.c") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "libs/b/src/two.c") == null);
}

test "an undecodable stub TU exits 1 on stderr" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = "\xff\xfe not utf-8\n" },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "cannot decode stub TU") != null);
    try std.testing.expectEqualStrings("", result.out);
}

test "the selftest exits 0 printing both cases" {
    const result = try runFixtures(&.{"--selftest"}, &.{});
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "[ok] guarded token plus hard-error branch stays quiet") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "[ok] non-failing else and escaped insecure token both fire") != null);
    try std.testing.expect(std.mem.endsWith(
        u8,
        result.out,
        "check_stub_crypto_guarded --selftest: all cases pass (both directions).\n",
    ));
}

test "the selftest ignores the tree entirely" {
    const result = try runFixtures(&.{"--selftest"}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = "static int tok;\n" },
    });
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "no arguments beyond a lone selftest are accepted" {
    const result = try runFixtures(&.{ "--selftest", "extra" }, &.{});
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("usage: check_stub_crypto_guarded [--selftest]\n", result.err);
}

test "an unknown flag exits 2" {
    const result = try runFixtures(&.{"--all"}, &.{});
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a file list exits 2, because this gate takes no paths" {
    const result = try runFixtures(&.{"libs/a/src/one.c"}, &.{});
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("", result.out);
}

test "the usage line goes to stderr, never to stdout" {
    const result = try runFixtures(&.{"-h"}, &.{});
    defer result.deinit();
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "usage:") != null);
    try std.testing.expectEqualStrings("", result.out);
}

test "findings print on stdout, as the predecessor's bare print did" {
    const result = try runFixtures(&.{}, &.{
        .{ .rel = "libs/a/src/one.c", .token = "tok", .text = "static int tok;\n" },
    });
    defer result.deinit();
    try std.testing.expectEqualStrings("", result.err);
    try std.testing.expect(result.out.len != 0);
}

test "a repository root prefixes the read path but not the reported one" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "root/libs/a/src/one.c", clean_source);
    const stubs = [_]cli.Stub{.{ .rel = "libs/a/src/one.c", .token = "tok" }};
    var out = std.ArrayList(u8).init(talloc);
    defer out.deinit();
    var err = std.ArrayList(u8).init(talloc);
    defer err.deinit();
    const status = try cli.run(talloc, tmp.dir, "root", &.{}, &stubs, out.writer(), err.writer());
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "1 stub crypto TU(s)") != null);
}

test "a repository root that does not resolve makes every TU a finding" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const stubs = [_]cli.Stub{.{ .rel = "libs/a/src/one.c", .token = "tok" }};
    var out = std.ArrayList(u8).init(talloc);
    defer out.deinit();
    var err = std.ArrayList(u8).init(talloc);
    defer err.deinit();
    const status = try cli.run(talloc, tmp.dir, "absent", &.{}, &stubs, out.writer(), err.writer());
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "file not found") != null);
}

test "the reported path is the repository-relative one, not the read path" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try write(tmp.dir, "root/libs/a/src/one.c", "static int tok;\n");
    const stubs = [_]cli.Stub{.{ .rel = "libs/a/src/one.c", .token = "tok" }};
    var out = std.ArrayList(u8).init(talloc);
    defer out.deinit();
    var err = std.ArrayList(u8).init(talloc);
    defer err.deinit();
    const status = try cli.run(talloc, tmp.dir, "root", &.{}, &stubs, out.writer(), err.writer());
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "  libs/a/src/one.c:") != null);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "root/libs") == null);
}

test "the live governed set is the eight stub TUs" {
    try std.testing.expectEqual(@as(usize, 8), cli.default_stubs.len);
}

test "every live TU path is repository-relative" {
    for (cli.default_stubs) |stub| {
        try std.testing.expect(!std.fs.path.isAbsolute(stub.rel));
    }
}
