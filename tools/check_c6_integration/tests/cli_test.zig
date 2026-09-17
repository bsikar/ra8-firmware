//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract tests for the offline C6 integration-contract gate
//! (#858).
//!
//! `cli.run` is parameterised on a directory and both streams, so each case
//! builds a small tree in a temporary directory and asserts the status and
//! the exact stream the predecessor wrote to. The argv asymmetries, an
//! unrecognised flag being ignored and `--selftest` winning wherever it
//! appears, are inherited and pinned here.

const std = @import("std");
const cli = @import("cli");

const testing = std.testing;

const good_build =
    \\  COMPONENT_DIR="${PERIPHERAL_DIR}/components/mdl_service"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt" "${COMPONENT_DIR}/CMakeLists.txt"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/src/mdl_service.c" "${COMPONENT_DIR}/src/mdl_service.c"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/inc/ra8_mdl_service.h" "${COMPONENT_DIR}/include/ra8_mdl_service.h"
    \\cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_protocol.h" "${COMPONENT_DIR}/include/ra8_mdl_protocol.h"
    \\cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_http.h" "${COMPONENT_DIR}/include/ra8_mdl_http.h"
    \\grep -Eq 'T[[:space:]]+ra8_mdl_service_component_abi$'
    \\grep -Eq 'T[[:space:]]+esp_hosted_custom_rpc_sync_handler$'
    \\
;

const good_patch =
    \\+set(COMPONENTS esp_timer main mdl_service)
    \\+__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(
    \\
;

const good_header = "uint32_t ra8_mdl_service_component_abi(void);\n";

const good_source =
    \\[[gnu::noinline]] uint32_t ra8_mdl_service_component_abi(void) { return 1U; }
    \\esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t id) { return ESP_OK; }
    \\
;

const staged_sources = [_][]const u8{
    "port/esp32_c6/CMakeLists.txt",
    "port/esp32_c6/src/mdl_service.c",
    "port/esp32_c6/inc/ra8_mdl_service.h",
    "libs/ra8_c6link/inc/ra8_mdl_protocol.h",
    "libs/ra8_c6link/inc/ra8_mdl_http.h",
};

const Outcome = struct {
    status: u8,
    out: []const u8,
    err: []const u8,
};

fn writeFile(dir: std.fs.Dir, path: []const u8, body: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try dir.makePath(parent);
    try dir.writeFile(.{ .sub_path = path, .data = body });
}

/// Build the committed tree the gate reads, with each of the four inputs
/// overridable and the staged sources present unless `stage_sources` is off.
const Tree = struct {
    build: ?[]const u8 = good_build,
    patch: ?[]const u8 = good_patch,
    header: ?[]const u8 = good_header,
    source: ?[]const u8 = good_source,
    stage_sources: bool = true,
};

fn runIn(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    tree: Tree,
    argv: []const []const u8,
) !Outcome {
    const paths = cli.Paths{};
    // Two of the staged sources ARE two of the four contract inputs, so the
    // staged placeholders go down first and the contract inputs overwrite
    // them; an input the case omits is then removed again.
    if (tree.stage_sources) {
        for (staged_sources) |relative| try writeFile(dir, relative, "/* staged */\n");
    }
    if (tree.build) |body| try writeFile(dir, paths.build_script, body);
    if (tree.patch) |body| try writeFile(dir, paths.patch_file, body);
    if (tree.header) |body| try writeFile(dir, paths.service_header, body);
    if (tree.source) |body| try writeFile(dir, paths.service_source, body);
    if (tree.build == null) dir.deleteFile(paths.build_script) catch {};
    if (tree.patch == null) dir.deleteFile(paths.patch_file) catch {};
    if (tree.header == null) dir.deleteFile(paths.service_header) catch {};
    if (tree.source == null) dir.deleteFile(paths.service_source) catch {};

    var out: std.ArrayListUnmanaged(u8) = .{};
    var err: std.ArrayListUnmanaged(u8) = .{};
    const status = try cli.run(
        allocator,
        dir,
        ".",
        argv,
        paths,
        out.writer(allocator),
        err.writer(allocator),
    );
    return .{
        .status = status,
        .out = try out.toOwnedSlice(allocator),
        .err = try err.toOwnedSlice(allocator),
    };
}

fn expectRun(tree: Tree, argv: []const []const u8, body: anytype) !void {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const outcome = try runIn(arena.allocator(), tmp.dir, tree, argv);
    try body(outcome);
}

test "the agreeing committed contract exits 0 and says so on stdout" {
    try expectRun(.{}, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 0), outcome.status);
            try testing.expectEqualStrings(
                "check_c6_integration: C6 staging/component/ABI contract agrees.\n",
                outcome.out,
            );
            try testing.expectEqualStrings("", outcome.err);
        }
    }.check);
}

test "a staged source missing from the tree exits 1 and reports on stderr" {
    // Two of the five staged sources are themselves contract inputs, so they
    // are on disk even when nothing else is staged; the other three are the
    // ones the gate reports.
    try expectRun(.{ .stage_sources = false }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 1), outcome.status);
            try testing.expectEqualStrings("", outcome.out);
            try testing.expect(std.mem.startsWith(
                u8,
                outcome.err,
                "check_c6_integration: 3 C6 integration drift(s):\n",
            ));
            try testing.expect(std.mem.indexOf(
                u8,
                outcome.err,
                "staging: source does not exist: port/esp32_c6/CMakeLists.txt",
            ) != null);
        }
    }.check);
}

test "a missing build recipe exits 2 and names the path it wanted" {
    try expectRun(.{ .build = null }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 2), outcome.status);
            try testing.expectEqualStrings("", outcome.out);
            try testing.expect(std.mem.startsWith(u8, outcome.err, "check_c6_integration: FATAL -- missing "));
            try testing.expect(std.mem.indexOf(u8, outcome.err, "coprocessor/esp32c6/build.sh") != null);
        }
    }.check);
}

test "a missing patch exits 2" {
    try expectRun(.{ .patch = null }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 2), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.err, "0001-custom-rpc-sync-response-hook.patch") != null);
        }
    }.check);
}

test "a missing header exits 2" {
    try expectRun(.{ .header = null }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 2), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.err, "ra8_mdl_service.h") != null);
        }
    }.check);
}

test "a missing component source exits 2" {
    try expectRun(.{ .source = null }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 2), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.err, "mdl_service.c") != null);
        }
    }.check);
}

test "a missing input outranks a drift, the config status wins" {
    try expectRun(.{ .build = null, .stage_sources = false }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 2), outcome.status);
        }
    }.check);
}

test "the first missing input is the only one reported" {
    try expectRun(.{ .build = null, .patch = null }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 2), outcome.status);
            try testing.expectEqual(@as(usize, 1), std.mem.count(u8, outcome.err, "FATAL"));
            try testing.expect(std.mem.indexOf(u8, outcome.err, "build.sh") != null);
        }
    }.check);
}

test "an input that is not valid UTF-8 exits 1, where the decode traceback landed" {
    try expectRun(.{ .header = "uint32_t \xff\xfe(void);\n" }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 1), outcome.status);
            try testing.expect(std.mem.startsWith(u8, outcome.err, "check_c6_integration: FATAL -- cannot read "));
        }
    }.check);
}

test "a directory where an input file belongs exits 2" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const paths = cli.Paths{};
    try tmp.dir.makePath(paths.service_header);
    const outcome = try runIn(
        arena.allocator(),
        tmp.dir,
        .{ .header = null, .stage_sources = false },
        &.{},
    );
    try testing.expectEqual(@as(u8, 2), outcome.status);
}

test "a drifted component identity exits 1 with the component finding" {
    const patch = "+set(COMPONENTS esp_timer main other)\n" ++
        "+__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(\n";
    try expectRun(.{ .patch = patch }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 1), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.err, "component: build.sh stages 'mdl_service'") != null);
        }
    }.check);
}

test "a private ABI definition exits 1" {
    const source = "static uint32_t ra8_mdl_service_component_abi(void) { return 1U; }\n" ++
        "esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t id) { return ESP_OK; }\n";
    try expectRun(.{ .source = source }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 1), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.err, "ABI: source does not define externally visible") != null);
        }
    }.check);
}

test "the finding count in the header line matches the lines that follow" {
    try expectRun(.{ .build = "", .stage_sources = false }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 1), outcome.status);
            var lines = std.mem.splitScalar(u8, std.mem.trimRight(u8, outcome.err, "\n"), '\n');
            const head = lines.next().?;
            var counted: usize = 0;
            while (lines.next()) |line| {
                try testing.expect(std.mem.startsWith(u8, line, "  "));
                counted += 1;
            }
            const expected = try std.fmt.allocPrint(
                testing.allocator,
                "check_c6_integration: {d} C6 integration drift(s):",
                .{counted},
            );
            defer testing.allocator.free(expected);
            try testing.expectEqualStrings(expected, head);
        }
    }.check);
}

test "the selftest passes and prints its case count on stdout" {
    try expectRun(.{}, &.{"--selftest"}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 0), outcome.status);
            try testing.expectEqualStrings(
                "check_c6_integration --selftest: OK (10 cases: 9 fire, 1 stays quiet).\n",
                outcome.out,
            );
            try testing.expectEqualStrings("", outcome.err);
        }
    }.check);
}

test "the selftest never touches the tree, so it passes with no repository at all" {
    try expectRun(
        .{ .build = null, .patch = null, .header = null, .source = null, .stage_sources = false },
        &.{"--selftest"},
        struct {
            fn check(outcome: Outcome) !void {
                try testing.expectEqual(@as(u8, 0), outcome.status);
            }
        }.check,
    );
}

test "the selftest wins wherever it appears in argv" {
    try expectRun(.{}, &.{ "--verbose", "--selftest" }, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 0), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.out, "--selftest: OK") != null);
        }
    }.check);
}

test "an unrecognised flag is ignored, this gate has no usage status" {
    try expectRun(.{}, &.{"--not-a-flag"}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 0), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.out, "contract agrees") != null);
        }
    }.check);
}

test "a stray positional is ignored too" {
    try expectRun(.{}, &.{"some/path.c"}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 0), outcome.status);
        }
    }.check);
}

test "every selftest case meets its expectation" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const failures = try cli.selftestFailures(arena.allocator());
    try testing.expectEqual(@as(usize, 0), failures.len);
}

test "the selftest carries one quiet control and nine firing seams" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const cases = try cli.selftestCases(arena.allocator());
    try testing.expectEqual(@as(usize, 10), cases.len);
    var quiet: usize = 0;
    for (cases) |case| {
        if (!case.fires) quiet += 1;
    }
    try testing.expectEqual(@as(usize, 1), quiet);
}

test "each selftest case carries a distinct label" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const cases = try cli.selftestCases(arena.allocator());
    for (cases, 0..) |left, index| {
        for (cases[index + 1 ..]) |right| {
            try testing.expect(!std.mem.eql(u8, left.label, right.label));
        }
    }
}

test "the exit statuses are the documented three" {
    try testing.expectEqual(@as(u8, 0), cli.exit_ok);
    try testing.expectEqual(@as(u8, 1), cli.exit_fail);
    try testing.expectEqual(@as(u8, 2), cli.exit_config);
}

test "the repository root is honoured, the gate reads under it" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makePath("nested");
    var nested = try tmp.dir.openDir("nested", .{});
    defer nested.close();
    _ = try runIn(arena.allocator(), nested, .{}, &.{});

    var out: std.ArrayListUnmanaged(u8) = .{};
    var err: std.ArrayListUnmanaged(u8) = .{};
    const status = try cli.run(
        arena.allocator(),
        tmp.dir,
        "nested",
        &.{},
        .{},
        out.writer(arena.allocator()),
        err.writer(arena.allocator()),
    );
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expect(std.mem.indexOf(u8, out.items, "contract agrees") != null);
}

test "a recipe written with carriage returns still agrees" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const crlf_build = try std.mem.replaceOwned(u8, arena.allocator(), good_build, "\n", "\r\n");
    try expectRun(.{ .build = crlf_build }, &.{}, struct {
        fn check(outcome: Outcome) !void {
            try testing.expectEqual(@as(u8, 0), outcome.status);
            try testing.expect(std.mem.indexOf(u8, outcome.out, "contract agrees") != null);
        }
    }.check);
}
