//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract of the HAL driver asm-guard gate (#858). `cli.run` is
//! parameterised on a directory handle, the repository root, the scan root
//! and both streams, so every status below is proved here with no process and
//! no real repository.

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

/// Drive the gate over a temporary tree whose drivers live in `src/`.
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
        .{ .driver_dir = "src" },
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

const guarded_driver =
    "#ifdef RA8_OFF_TARGET\n" ++
    "void f(void) { __asm(\"nop\"); }\n" ++
    "#endif\n";

const clean_driver = "void f(void) { ra8_hw_wfi(); }\n";

test "a clean sweep exits 0 and reports the translation-unit count" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/a.c", clean_driver);
    try writeFile(tmp.dir, "src/b.c", clean_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "check_no_driver_asm_guard: PASS -- 2 HAL driver TU(s) carry no RA8_OFF_TARGET-guarded asm.\n",
        result.out,
    );
    try std.testing.expectEqualStrings("", result.err);
}

test "an empty driver directory is a clean sweep of zero drivers" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("src");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "0 HAL driver TU(s)") != null);
}

test "a guarded driver exits 1 and names the file, line and seam" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/bad.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(
        u8,
        result.out,
        "  src/bad.c:2: inline asm 'void f(void) { __asm(\"nop\"); }' sits inside a " ++
            "RA8_OFF_TARGET conditional",
    ) != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "ra8_hw_intrinsics.h") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "ra8_host_asm_stub.c") != null);
}

test "the failure report opens with the gate's own headline" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/bad.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expect(std.mem.startsWith(
        u8,
        result.out,
        "check_no_driver_asm_guard: a HAL driver guards bare asm on RA8_OFF_TARGET:\n",
    ));
}

test "findings print on stdout, leaving stderr empty" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/bad.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqualStrings("", result.err);
}

test "one clean and one guarded driver still fails the sweep" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/a_clean.c", clean_driver);
    try writeFile(tmp.dir, "src/b_bad.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "b_bad.c:2") != null);
}

test "findings are reported in sorted driver order" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/zeta.c", guarded_driver);
    try writeFile(tmp.dir, "src/alpha.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    const alpha = std.mem.indexOf(u8, result.out, "src/alpha.c").?;
    const zeta = std.mem.indexOf(u8, result.out, "src/zeta.c").?;
    try std.testing.expect(alpha < zeta);
}

test "only .c entries are swept, headers are not" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/a.c", clean_driver);
    try writeFile(tmp.dir, "src/guarded.h", guarded_driver);
    try writeFile(tmp.dir, "src/guarded.cpp", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "1 HAL driver TU(s)") != null);
}

test "the sweep does not descend into subdirectories" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/a.c", clean_driver);
    try writeFile(tmp.dir, "src/nested/bad.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "a dot-prefixed driver is NOT hidden from the sweep" {
    // pathlib's glob never hid these, so neither does the replacement.
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/.hidden.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "src/.hidden.c:2") != null);
}

test "a directory whose name ends in .c is a read failure, not a skip" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("src/trap.c");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "cannot read driver") != null);
}

test "a missing driver directory exits 1, never a clean 0" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expectEqualStrings(
        "check_no_driver_asm_guard: driver dir not found: src\n",
        result.out,
    );
}

test "an undecodable driver exits 1 rather than scanning rubbish" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/bad.c", "\xffvoid f(void);\n");

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.err, "cannot read driver") != null);
}

test "a CRLF driver reports the line the editor shows" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(
        tmp.dir,
        "src/crlf.c",
        "#ifdef RA8_OFF_TARGET\r\nvoid f(void) { __asm(\"nop\"); }\r\n#endif\r\n",
    );

    var result = try runGate(std.testing.allocator, tmp.dir, &.{});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "src/crlf.c:2") != null);
}

test "the selftest exits 0 and prints both directions" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--selftest"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "  [ok] asm in both off-target branches fires\n" ++
            "  [ok] shared seam calls and comment lookalikes stay quiet\n" ++
            "check_no_driver_asm_guard --selftest: all cases pass (both directions).\n",
        result.out,
    );
}

test "the selftest needs no driver directory at all" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--selftest"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings("", result.err);
}

test "an unknown flag is a usage error on stderr" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/a.c", clean_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"--all"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("usage: check_no_driver_asm_guard [--selftest]\n", result.err);
    try std.testing.expectEqualStrings("", result.out);
}

test "a file path argument is a usage error: this gate takes no file list" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/a.c", clean_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"src/a.c"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "--selftest beside anything else is a usage error" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{ "--selftest", "extra" });
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a repeated --selftest is a usage error too" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var result = try runGate(std.testing.allocator, tmp.dir, &.{ "--selftest", "--selftest" });
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
}

test "a usage error outranks a tree that would have failed" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "src/bad.c", guarded_driver);

    var result = try runGate(std.testing.allocator, tmp.dir, &.{"-x"});
    defer result.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(u8, 2), result.status);
    try std.testing.expectEqualStrings("", result.out);
}

test "the repository root is honoured when the gate runs from elsewhere" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeFile(tmp.dir, "repo/src/bad.c", guarded_driver);

    var out = std.ArrayList(u8).init(std.testing.allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(std.testing.allocator);
    defer err.deinit();
    const status = try cli.run(
        std.testing.allocator,
        tmp.dir,
        "repo",
        &.{},
        .{ .driver_dir = "src" },
        out.writer(),
        err.writer(),
    );
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.indexOf(u8, out.items, "src/bad.c:2") != null);
}
