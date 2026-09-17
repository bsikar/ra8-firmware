//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and output contract for `list_tests` (#858).
//!
//! `run` is parameterised on the directory the search resolves against, the
//! `RA8_REPO_ROOT` value and both streams, so every case below builds a small
//! tree in a temporary directory and reads the exact bytes the tool would
//! print. Nothing here spawns a process or touches the real environment.

const std = @import("std");
const testing = std.testing;
const cli = @import("cli");

const Capture = struct {
    status: u8,
    out: []u8,
    err: []u8,

    fn deinit(self: *Capture) void {
        testing.allocator.free(self.out);
        testing.allocator.free(self.err);
    }
};

fn invoke(dir: std.fs.Dir, argv: []const []const u8, env: ?[]const u8) !Capture {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    var out = std.ArrayList(u8).init(testing.allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    errdefer err.deinit();

    const status = try cli.run(
        arena.allocator(),
        dir,
        argv,
        env,
        out.writer(),
        err.writer(),
    );
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

fn writeSource(dir: std.fs.Dir, path: []const u8, body: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = path, .data = body });
}

test "no category prints the usage line on stdout and exits 1" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var capture = try invoke(tmp.dir, &[_][]const u8{"list_tests"}, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_error, capture.status);
    try testing.expectEqualStrings("Usage: list_tests <category>\n", capture.out);
    try testing.expectEqualStrings("", capture.err);
}

test "an unknown category reports no tests on stdout and exits 1" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "nosuch" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_error, capture.status);
    try testing.expectEqualStrings(
        "Error: Category 'nosuch' not found or has no tests.\n",
        capture.out,
    );
}

test "a category directory that exists but holds no test file still exits 1" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("tests/hal");
    try writeSource(tmp.dir, "tests/hal/helper.c", "int helper(void);\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_error, capture.status);
    try testing.expectEqualStrings(
        "Error: Category 'hal' not found or has no tests.\n",
        capture.out,
    );
}

test "one test prints the banner, the row and a trailing blank line" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(
        tmp.dir,
        "tests/hal/test_gpio.c",
        "/**\n * @brief GPIO driver unit tests\n */\n",
    );
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expectEqualStrings(
        "== HAL TESTS (1) -- local: just tests::local hal | " ++
            "container: just tests::devcontainer hal\n\n" ++
            "  test_gpio                                GPIO driver unit tests\n\n",
        capture.out,
    );
    try testing.expectEqualStrings("", capture.err);
}

test "a source with no @brief falls back to '<name> unit tests'" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/net/test_lwip.c", "int main(void) { return 0; }\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "net" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "test_lwip unit tests") != null);
}

test "entries are sorted by name, not by discovery order" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/hal/test_zebra.c", "/** @brief last */\n");
    try writeSource(tmp.dir, "tests/hal/test_alpha.c", "/** @brief first */\n");
    try writeSource(tmp.dir, "tests/hal/test_mid.c", "/** @brief middle */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    const alpha = std.mem.indexOf(u8, capture.out, "test_alpha").?;
    const mid = std.mem.indexOf(u8, capture.out, "test_mid").?;
    const zebra = std.mem.indexOf(u8, capture.out, "test_zebra").?;
    try testing.expect(alpha < mid);
    try testing.expect(mid < zebra);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (3)") != null);
}

test "both .c and .cpp targets are discovered" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/hal/test_c_side.c", "/** @brief C side */\n");
    try writeSource(tmp.dir, "tests/hal/test_cxx_side.cpp", "/** @brief C++ side */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (2)") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "C side") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "C++ side") != null);
}

test "a file that is not named test_* is ignored" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/hal/test_real.c", "/** @brief real */\n");
    try writeSource(tmp.dir, "tests/hal/helper_test.c", "/** @brief not a target */\n");
    try writeSource(tmp.dir, "tests/hal/test_notes.md", "/** @brief wrong suffix */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (1)") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "not a target") == null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "wrong suffix") == null);
}

test "the category argument is lower-cased before the roots are chosen" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/hal/test_gpio.c", "/** @brief GPIO */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "HAL" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "== HAL TESTS (1)") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "just tests::local hal") != null);
}

test "shared resolves to apps/shared_libs/*/tests" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(
        tmp.dir,
        "apps/shared_libs/ra8_text/tests/test_shape.c",
        "/** @brief shaping */\n",
    );
    try writeSource(tmp.dir, "tests/shared/test_decoy.c", "/** @brief decoy */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "shared" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (1)") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "shaping") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "decoy") == null);
}

test "host resolves to apps/host/*/tests" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "apps/host/mdl/tests/test_mdl.c", "/** @brief mdl */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "host" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "mdl") != null);
}

test "tools resolves to tools/*/tests" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tools/epub_compile/tests/test_pack.c", "/** @brief packing */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "tools" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "packing") != null);
}

test "board collects from BOTH of its roots" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(
        tmp.dir,
        "apps/board/ek_ra8d2/ereader/tests/test_deep.c",
        "/** @brief deep root */\n",
    );
    try writeSource(
        tmp.dir,
        "apps/board/standalone/tests/test_shallow.c",
        "/** @brief shallow root */\n",
    );
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "board" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (2)") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "deep root") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "shallow root") != null);
}

test "a dot directory IS walked into, because pathlib.Path.glob walks it" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tools/real/tests/test_real.c", "/** @brief real */\n");
    try writeSource(tmp.dir, "tools/.hidden/tests/test_hidden.c", "/** @brief hidden */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "tools" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (2)") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "hidden") != null);
}

test "arguments after the category are ignored, as sys.argv[1] was" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/hal/test_gpio.c", "/** @brief GPIO */\n");
    var capture = try invoke(
        tmp.dir,
        &[_][]const u8{ "list_tests", "hal", "storage", "--whatever" },
        null,
    );
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "== HAL TESTS (1)") != null);
}

test "--repo-root points the search somewhere other than the cwd" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "elsewhere/tests/hal/test_gpio.c", "/** @brief GPIO */\n");
    var capture = try invoke(
        tmp.dir,
        &[_][]const u8{ "list_tests", "--repo-root", "elsewhere", "hal" },
        null,
    );
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "== HAL TESTS (1)") != null);
}

test "--repo-root=DIR is accepted in the joined spelling too" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "elsewhere/tests/hal/test_gpio.c", "/** @brief GPIO */\n");
    var capture = try invoke(
        tmp.dir,
        &[_][]const u8{ "list_tests", "--repo-root=elsewhere", "hal" },
        null,
    );
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "== HAL TESTS (1)") != null);
}

test "--repo-root with no value is a usage error on stderr" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "--repo-root" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_error, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.err, "--repo-root needs a directory") != null);
}

test "RA8_REPO_ROOT is used when no flag is given" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "from_env/tests/hal/test_gpio.c", "/** @brief GPIO */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, "from_env");
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "== HAL TESTS (1)") != null);
}

test "--repo-root outranks RA8_REPO_ROOT" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "flagged/tests/hal/test_flag.c", "/** @brief from flag */\n");
    try writeSource(tmp.dir, "from_env/tests/hal/test_env.c", "/** @brief from env */\n");
    var capture = try invoke(
        tmp.dir,
        &[_][]const u8{ "list_tests", "--repo-root", "flagged", "hal" },
        "from_env",
    );
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "from flag") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "from env") == null);
}

test "an unopenable repository root exits 1 with a message on stderr" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    var capture = try invoke(
        tmp.dir,
        &[_][]const u8{ "list_tests", "--repo-root", "no_such_dir", "hal" },
        null,
    );
    defer capture.deinit();
    try testing.expectEqual(cli.exit_error, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.err, "cannot open repository root") != null);
}

test "a CRLF source yields the same row as an LF one" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(
        tmp.dir,
        "tests/hal/test_crlf.c",
        "/**\r\n * @brief CRLF described\r\n */\r\n",
    );
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "CRLF described") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "\r") == null);
}

test "invalid UTF-8 in a source does not fail the listing" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try writeSource(tmp.dir, "tests/hal/test_bytes.c", "/** @brief ok\xff here */\n");
    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "ok here") != null);
}

test "a source past the read prefix keeps its row and its @brief" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    // The Python streamed the file line by line, so size never decided whether
    // a target was listed. A `readFileAlloc` capped at `max_source_bytes` used
    // to fail outright here and drop the row: a generated suite past the cap
    // vanished from `just tests::hal` with nothing printed to say so.
    try tmp.dir.makePath("tests/hal");
    var file = try tmp.dir.createFile("tests/hal/test_huge.c", .{});
    defer file.close();
    try file.writeAll("/** @brief huge generated suite */\n");
    const filler = try testing.allocator.alloc(u8, 1024 * 1024);
    defer testing.allocator.free(filler);
    @memset(filler, 'a');
    var written: usize = 0;
    while (written <= cli.max_source_bytes) : (written += filler.len) {
        try file.writeAll(filler);
    }

    var capture = try invoke(tmp.dir, &[_][]const u8{ "list_tests", "hal" }, null);
    defer capture.deinit();
    try testing.expectEqual(cli.exit_ok, capture.status);
    try testing.expect(std.mem.indexOf(u8, capture.out, "test_huge") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "huge generated suite") != null);
    try testing.expect(std.mem.indexOf(u8, capture.out, "TESTS (1)") != null);
}
