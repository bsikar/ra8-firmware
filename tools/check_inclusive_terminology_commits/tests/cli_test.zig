//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status contract tests for the commit-message terminology gate (#858).
//!
//! `cli.run` takes the input text and both streams, so every status and every
//! printed line below is proved with no process, no pipe and no repository.

const std = @import("std");
const cli = @import("cli");

const controller = "mas" ++ "ter";
const copi = "MO" ++ "SI";

/// One run, capturing both streams and the status.
const Run = struct {
    status: u8,
    out: []const u8,
    err: []const u8,
};

/// Run the gate over `input` with `argv`, capturing everything it writes.
fn run(allocator: std.mem.Allocator, argv: []const []const u8, input: []const u8) !Run {
    var out = std.ArrayList(u8).init(allocator);
    var err = std.ArrayList(u8).init(allocator);
    const status = try cli.run(allocator, argv, input, out.writer(), err.writer());
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

test "clean text exits 0 and says so" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "fix(spi): rename the controller pin\n");
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings("[PASS] Commit message terminology clean.\n", result.out);
    try std.testing.expectEqualStrings("", result.err);
}

test "empty input is clean, never a vacuous failure" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "");
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings("[PASS] Commit message terminology clean.\n", result.out);
}

test "a banned term exits 1" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "fix(spi): swap " ++ copi ++ "\n");
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "the failure report matches the predecessor, line for line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "fix(spi): swap " ++ copi ++ "\n");
    try std.testing.expectEqualStrings(
        "[FAIL] Non-inclusive terminology in commit message(s):\n" ++
            "  line 1: " ++ copi ++ " -- use COPI\n" ++
            "    > fix(spi): swap " ++ copi ++ "\n",
        result.out,
    );
}

test "findings print on stdout, as bare print() did" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, copi ++ "\n");
    try std.testing.expectEqualStrings("", result.err);
    try std.testing.expect(result.out.len > 0);
}

test "several paragraphs report several findings, in line order" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(
        arena.allocator(),
        &.{},
        "uses " ++ copi ++ "\n\nand a " ++ controller ++ " clock\n",
    );
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "line 1:") != null);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "line 3:") != null);
}

test "an annotated paragraph exits 0" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(
        arena.allocator(),
        &.{},
        "uses " ++ copi ++ "\nLEGACY-OK: upstream datasheet name\n",
    );
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "the selftest passes and prints both OK lines" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{"--selftest"}, "");
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings(
        "[SELFTEST OK] fires on an un-annotated term, stays quiet on a wrapped\n" ++
            "              paragraph-scoped LEGACY-OK, and does not leak across paragraphs.\n",
        result.out,
    );
}

test "the selftest ignores stdin entirely" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{"--selftest"}, copi ++ " everywhere\n");
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "--selftest is honoured beside other arguments" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{ "extra", "--selftest" }, "");
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "[SELFTEST OK]") != null);
}

test "an unknown flag is not a usage error: there is no exit 2" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{"--nonsense"}, "clean subject\n");
    try std.testing.expectEqual(@as(u8, 0), result.status);
    try std.testing.expectEqualStrings("", result.err);
}

test "a positional argument is ignored and stdin still scanned" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{"some/path"}, copi ++ "\n");
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "text with no trailing newline is still scanned" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "uses " ++ copi);
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "line 1:") != null);
}

test "CRLF input reports the line the author sees" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "subject\r\n\r\nuses " ++ copi ++ "\r\n");
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "line 3:") != null);
}

test "an undecodable byte does not abort the scan" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, "subject \xff\nuses " ++ copi ++ "\n");
    try std.testing.expectEqual(@as(u8, 1), result.status);
    try std.testing.expect(std.mem.indexOf(u8, result.out, "line 2:") != null);
}

test "an undecodable byte survives into the echoed line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const result = try run(arena.allocator(), &.{}, copi ++ " \xff\n");
    try std.testing.expect(std.mem.indexOf(u8, result.out, "\xff") != null);
}

test "a whole clean history of many messages exits 0" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var text = std.ArrayList(u8).init(arena.allocator());
    var index: usize = 0;
    while (index < 200) : (index += 1) {
        try text.writer().print("fix(core): change {d}\n\nBody line for {d}.\n\n", .{ index, index });
    }
    const result = try run(arena.allocator(), &.{}, text.items);
    try std.testing.expectEqual(@as(u8, 0), result.status);
}

test "one bad message among many is still caught" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var text = std.ArrayList(u8).init(arena.allocator());
    var index: usize = 0;
    while (index < 50) : (index += 1) {
        try text.writer().print("fix(core): change {d}\n\n", .{index});
    }
    try text.writer().print("fix(spi): the {s} pin\n", .{copi});
    const result = try run(arena.allocator(), &.{}, text.items);
    try std.testing.expectEqual(@as(u8, 1), result.status);
}

test "the gate names itself without a file extension" {
    try std.testing.expectEqualStrings("check_inclusive_terminology_commits", cli.tool);
}

test "the selftest is callable on its own" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var out = std.ArrayList(u8).init(arena.allocator());
    var err = std.ArrayList(u8).init(arena.allocator());
    const status = try cli.selftest(arena.allocator(), out.writer(), err.writer());
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqualStrings("", err.items);
}
