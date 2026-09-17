//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract of the NSC veneer gate (#858). `cli.run` is
//! parameterised on a directory handle, the repository root, the scanned
//! paths and both streams, so every status below is proved here with no
//! process and no real repository.

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
fn runGate(allocator: std.mem.Allocator, dir: std.fs.Dir, argv: []const []const u8) !Run {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(allocator);
    errdefer err.deinit();
    const status = try cli.run(
        allocator,
        dir,
        ".",
        argv,
        .{ .header = "inc/ra8_nsc.h", .src_dir = "src" },
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

const two_decls =
    "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void);\n" ++
    "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n";

test "a tree whose veneers are all defined exits 0 and reports the count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", two_decls);
    try writeFile(
        tmp.dir,
        "src/ra8_nsc_a.c",
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void) { return 0; }\n",
    );
    try writeFile(tmp.dir, "src/ra8_nsc_b.c", "RA8_NSC_VENEER void ra8_nsc_phantom(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: PASS -- all 2 RA8_NSC_VENEER declaration(s) defined.\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "a phantom veneer exits 1 and names it on stdout" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", two_decls);
    try writeFile(
        tmp.dir,
        "src/ra8_nsc_a.c",
        "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void) { return 0; }\n" ++
            "void caller(void) { ra8_nsc_phantom(); }\n",
    );

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: RA8_NSC_VENEER declared without a definition:\n" ++
            "  ra8_nsc_phantom: declared in inc/ra8_nsc.h, no definition in src/\n" ++
            "Fix each at the root -- implement the veneer, or delete the declaration.\n" ++
            "A phantom NS->S entry point in the public header is a trust hazard.\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "every phantom is listed in declaration order" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(
        tmp.dir,
        "inc/ra8_nsc.h",
        "RA8_NSC_VENEER void ra8_nsc_zulu(void);\nRA8_NSC_VENEER void ra8_nsc_alpha(void);\n",
    );
    try writeFile(tmp.dir, "src/ra8_nsc_a.c", "void unrelated(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "ra8_nsc_zulu").? <
        std.mem.indexOf(u8, result.out, "ra8_nsc_alpha").?);
}

test "a header with no declarations exits 0 with a zero count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "/* prose only, no veneers */\n");
    try writeFile(tmp.dir, "src/ra8_nsc_a.c", "void unrelated(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: PASS -- all 0 RA8_NSC_VENEER declaration(s) defined.\n",
        result.out,
    );
}

test "a missing header exits 1 rather than passing on nothing to parse" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/ra8_nsc_a.c", "void unrelated(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings("", result.out);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: header not found: inc/ra8_nsc.h\n",
        result.err,
    );
}

test "a directory where the header belongs exits 1" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("inc/ra8_nsc.h");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: header not found: inc/ra8_nsc.h\n",
        result.err,
    );
}

test "an undecodable header exits 1 instead of a traceback" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_x(void);\n\xff\xfe");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: cannot read header: inc/ra8_nsc.h\n",
        result.err,
    );
}

test "an undecodable source exits 1 instead of a traceback" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_x(void);\n");
    try writeFile(tmp.dir, "src/ra8_nsc_a.c", "\xff\xfe\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: cannot read source: src/ra8_nsc_a.c\n",
        result.err,
    );
}

test "a directory named like a source exits 1 when it is reached" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_x(void);\n");
    try tmp.dir.makePath("src/trap.c");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: cannot read source: src/trap.c\n",
        result.err,
    );
}

test "a missing source directory leaves every veneer missing" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_x(void);\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "ra8_nsc_x") != null);
}

test "a dot-prefixed source is in scope, as the glob left it" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_hidden(void);\n");
    try writeFile(tmp.dir, "src/.staged.c", "RA8_NSC_VENEER void ra8_nsc_hidden(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a header file in the source directory is not scanned as a source" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_x(void);\n");
    try writeFile(tmp.dir, "src/private.h", "RA8_NSC_VENEER void ra8_nsc_x(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "an uppercase suffix is out of scope, as the glob left it" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_x(void);\n");
    try writeFile(tmp.dir, "src/legacy.C", "RA8_NSC_VENEER void ra8_nsc_x(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "a definition in the last sorted source still counts" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_late(void);\n");
    try writeFile(tmp.dir, "src/a.c", "void unrelated(void) { }\n");
    try writeFile(tmp.dir, "src/z.c", "RA8_NSC_VENEER void ra8_nsc_late(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a CRLF header parses like its LF twin" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER\r\nvoid\r\nra8_nsc_crlf(void);\r\n");
    try writeFile(tmp.dir, "src/a.c", "RA8_NSC_VENEER void ra8_nsc_crlf(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "the selftest passes in both directions and exits 0" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--selftest"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "  [ok] matching veneer definition stays quiet\n" ++
            "  [ok] call-only phantom veneer fires\n" ++
            "check_nsc_veneer_defs --selftest: all cases pass (both directions).\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "the selftest needs no header on disk" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "inc/ra8_nsc.h", "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--selftest"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "an unknown flag is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--all"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("usage: check_nsc_veneer_defs [--selftest]\n", result.err);
    try std.testing.expectEqualStrings("", result.out);
}

test "a path argument is a usage error, the gate takes no file list" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"libs/ra8_nsc/src/ra8_nsc_eth.c"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "the selftest beside another argument is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{ "--selftest", "--selftest" });
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("usage: check_nsc_veneer_defs [--selftest]\n", result.err);
}

test "usage outranks a missing header" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"-x"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a repeated declaration is checked once" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(
        tmp.dir,
        "inc/ra8_nsc.h",
        "RA8_NSC_VENEER void ra8_nsc_dup(void);\nRA8_NSC_VENEER void ra8_nsc_dup(uint8_t a);\n",
    );
    try writeFile(tmp.dir, "src/a.c", "void unrelated(void) { }\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_nsc_veneer_defs: RA8_NSC_VENEER declared without a definition:\n" ++
            "  ra8_nsc_dup: declared in inc/ra8_nsc.h, no definition in src/\n" ++
            "Fix each at the root -- implement the veneer, or delete the declaration.\n" ++
            "A phantom NS->S entry point in the public header is a trust hazard.\n",
        result.out,
    );
}
