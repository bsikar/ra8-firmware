//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the pure half of the per-file MC/DC floor
//! gate (#858, #1205). Every case here pins a property of the predecessor,
//! scripts/checks/check_mcdc_floor.py, that was established by running it:
//! the path normalisation, the scope table, CPython's `int()` coercion, the
//! round-half-even percentage column, the offender ordering and the exact text
//! of every rendered line.

const std = @import("std");
const implementation = @import("implementation");

fn normalized(allocator: std.mem.Allocator, path: []const u8) ![]const u8 {
    return implementation.normalize(allocator, path, "ra8-firmware");
}

fn renderToString(allocator: std.mem.Allocator, comptime render: anytype, args: anytype) ![]const u8 {
    var buffer = std.ArrayList(u8).init(allocator);
    try @call(.auto, render, .{buffer.writer()} ++ args);
    return buffer.toOwnedSlice();
}

test "normalize strips an absolute checkout prefix" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings(
        "libs/ra8_core/src/core.c",
        try normalized(arena.allocator(), "/home/runner/ra8-firmware/libs/ra8_core/src/core.c"),
    );
}

test "normalize leaves an already relative path alone" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings(
        "libs/a.c",
        try normalized(arena.allocator(), "libs/a.c"),
    );
}

test "normalize folds backslashes to forward slashes" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings(
        "libs/a.c",
        try normalized(arena.allocator(), "C:\\ra8-firmware\\libs\\a.c"),
    );
}

test "normalize splits on the FIRST marker only" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings(
        "ra8-firmware/libs/a.c",
        try normalized(arena.allocator(), "/ra8-firmware/ra8-firmware/libs/a.c"),
    );
}

test "normalize splits on a marker anywhere, even mid-path" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings(
        "x.c",
        try normalized(arena.allocator(), "libs/ra8-firmware/x.c"),
    );
}

test "normalize lstrips a CHARACTER SET, not a prefix" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectEqualStrings("libs/a.c", try normalized(allocator, "./libs/a.c"));
    try std.testing.expectEqualStrings("libs/a.c", try normalized(allocator, ".//libs/a.c"));
    try std.testing.expectEqualStrings("libs/a.c", try normalized(allocator, "../libs/a.c"));
    try std.testing.expectEqualStrings("libs/a.c", try normalized(allocator, "...libs/a.c"));
    try std.testing.expectEqualStrings("libs/a.c", try normalized(allocator, "/libs/a.c"));
}

test "normalize of an empty or dot-only field is empty" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings("", try normalized(arena.allocator(), ""));
    try std.testing.expectEqualStrings("", try normalized(arena.allocator(), "./"));
}

test "normalize uses the checkout basename it is given" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqualStrings(
        "libs/a.c",
        try implementation.normalize(arena.allocator(), "/tmp/ra8-lane5/libs/a.c", "ra8-lane5"),
    );
    try std.testing.expectEqualStrings(
        "tmp/ra8-lane5/libs/a.c",
        try implementation.normalize(arena.allocator(), "/tmp/ra8-lane5/libs/a.c", "other"),
    );
}

test "every production root is in scope" {
    try std.testing.expect(implementation.inScope("libs/ra8_core/src/core.c"));
    try std.testing.expect(implementation.inScope("apps/shared_libs/book/src/book.c"));
    try std.testing.expect(implementation.inScope("examples/ek_ra8d2/demo/src/main.c"));
    try std.testing.expect(implementation.inScope("port/posix/src/io.c"));
    try std.testing.expect(implementation.inScope("tools/ra8_emulator/src/main.c"));
}

test "vendored SOUP and generated font tables are exempt" {
    try std.testing.expect(!implementation.inScope("libs/third_party/soup.c"));
    try std.testing.expect(!implementation.inScope("apps/shared_libs/third_party/soup/source.c"));
    try std.testing.expect(!implementation.inScope("libs/ra8_fonts/src/generated.c"));
}

test "nested test suites are exempt by path component" {
    try std.testing.expect(!implementation.inScope("apps/shared_libs/book/tests/src/test_book.c"));
    try std.testing.expect(!implementation.inScope("libs/ra8_core/test/x.c"));
    try std.testing.expect(!implementation.inScope("tools/demo/_deps/vendor.c"));
}

test "an exempt component matches whole, not by prefix" {
    try std.testing.expect(implementation.inScope("libs/testsuite/x.c"));
    try std.testing.expect(implementation.inScope("libs/_depsx/x.c"));
    try std.testing.expect(implementation.inScope("libs/tests.c"));
}

test "build output directories are exempt, including suffixed ones" {
    try std.testing.expect(!implementation.inScope("examples/ek_ra8d2/demo/build/generated.c"));
    try std.testing.expect(!implementation.inScope("examples/ek_ra8d2/demo/build-reflow-v2/generated.c"));
    try std.testing.expect(!implementation.inScope("port/esp-hosted/build-mcdc/shim.c"));
    try std.testing.expect(!implementation.inScope("libs/build-/x.c"));
}

test "a directory merely starting with build is not exempt" {
    try std.testing.expect(implementation.inScope("libs/buildx/x.c"));
    try std.testing.expect(implementation.inScope("libs/builder/x.c"));
}

test "anything outside the production roots is out of scope" {
    try std.testing.expect(!implementation.inScope("src/legacy.c"));
    try std.testing.expect(!implementation.inScope(""));
    try std.testing.expect(!implementation.inScope("/libs/a.c"));
    try std.testing.expect(!implementation.inScope("apps/host/x.c"));
}

test "the in-scope test is a string prefix, so a bare root name counts" {
    try std.testing.expect(implementation.inScope("libs/x.c"));
    try std.testing.expect(!implementation.inScope("libs"));
    try std.testing.expect(implementation.inScope("libsomething/x.c") == false);
}

test "scopePrefix returns the first matching root" {
    try std.testing.expectEqualStrings("libs/", implementation.scopePrefix("libs/a.c").?);
    try std.testing.expectEqualStrings(
        "apps/shared_libs/",
        implementation.scopePrefix("apps/shared_libs/book/src/book.c").?,
    );
    try std.testing.expectEqual(@as(?[]const u8, null), implementation.scopePrefix("src/a.c"));
}

test "parts iteration drops empty and dot components" {
    var parts = implementation.partsIterator("libs/./a.c");
    try std.testing.expectEqualStrings("libs", parts.next().?);
    try std.testing.expectEqualStrings("a.c", parts.next().?);
    try std.testing.expectEqual(@as(?[]const u8, null), parts.next());

    var doubled = implementation.partsIterator("libs//a.c");
    try std.testing.expectEqualStrings("libs", doubled.next().?);
    try std.testing.expectEqualStrings("a.c", doubled.next().?);
    try std.testing.expectEqual(@as(?[]const u8, null), doubled.next());
}

test "parts iteration keeps dot-dot verbatim" {
    var parts = implementation.partsIterator("libs/../a.c");
    try std.testing.expectEqualStrings("libs", parts.next().?);
    try std.testing.expectEqualStrings("..", parts.next().?);
    try std.testing.expectEqualStrings("a.c", parts.next().?);
}

test "parts iteration of an empty path yields nothing" {
    var parts = implementation.partsIterator("");
    try std.testing.expectEqual(@as(?[]const u8, null), parts.next());
}

test "isGeneratedPart matches build and build-prefixed names" {
    try std.testing.expect(implementation.isGeneratedPart("build"));
    try std.testing.expect(implementation.isGeneratedPart("build-mcdc"));
    try std.testing.expect(!implementation.isGeneratedPart("builds"));
    try std.testing.expect(!implementation.isGeneratedPart("rebuild"));
}

fn intOf(allocator: std.mem.Allocator, value: std.json.Value) !i128 {
    return implementation.pythonInt(allocator, value);
}

test "int() of an integer and a bool" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqual(@as(i128, 7), try intOf(arena.allocator(), .{ .integer = 7 }));
    try std.testing.expectEqual(@as(i128, 1), try intOf(arena.allocator(), .{ .bool = true }));
    try std.testing.expectEqual(@as(i128, 0), try intOf(arena.allocator(), .{ .bool = false }));
}

test "int() of a float truncates toward zero" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqual(@as(i128, 1), try intOf(arena.allocator(), .{ .float = 1.9 }));
    try std.testing.expectEqual(@as(i128, -1), try intOf(arena.allocator(), .{ .float = -1.9 }));
    try std.testing.expectEqual(@as(i128, 0), try intOf(arena.allocator(), .{ .float = -0.5 }));
}

test "int() of a decimal string parses" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqual(@as(i128, 5), try intOf(arena.allocator(), .{ .string = "5" }));
    try std.testing.expectEqual(@as(i128, 5), try intOf(arena.allocator(), .{ .string = "+5" }));
    try std.testing.expectEqual(@as(i128, -5), try intOf(arena.allocator(), .{ .string = "-5" }));
    try std.testing.expectEqual(@as(i128, 10), try intOf(arena.allocator(), .{ .string = "1_0" }));
}

test "int() of a string tolerates surrounding ASCII whitespace" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqual(@as(i128, 5), try intOf(arena.allocator(), .{ .string = "  5\t\n" }));
    try std.testing.expectEqual(@as(i128, 5), try intOf(arena.allocator(), .{ .string = "\x0b5\x0c" }));
}

test "int() of a string rejects the 0x1c-0x1f controls str.isspace accepts" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectError(error.ValueError, intOf(arena.allocator(), .{ .string = "\x1c5" }));
    try std.testing.expectError(error.ValueError, intOf(arena.allocator(), .{ .string = "5\x1f" }));
}

test "int() of a string folds non-ASCII spaces and digits to ASCII" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    try std.testing.expectEqual(
        @as(i128, 5),
        try intOf(arena.allocator(), .{ .string = "\u{3000}5" }),
    );
    try std.testing.expectEqual(
        @as(i128, 320),
        try intOf(arena.allocator(), .{ .string = "\u{663}20" }),
    );
}

test "int() of a malformed string is a ValueError" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "" }));
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "x" }));
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "5.0" }));
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "_5" }));
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "5_" }));
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "1__0" }));
    try std.testing.expectError(error.ValueError, intOf(allocator, .{ .string = "-" }));
}

test "int() of null, a list or an object is a TypeError" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectError(error.TypeError, intOf(allocator, .null));
    try std.testing.expectError(error.TypeError, intOf(allocator, .{
        .array = std.json.Array.init(allocator),
    }));
    try std.testing.expectError(error.TypeError, intOf(allocator, .{
        .object = std.json.ObjectMap.init(allocator),
    }));
}

test "int() beyond the documented bound is out of range, never wrapped" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const huge = "1" ++ ("0" ** 60);
    try std.testing.expectError(error.OutOfRange, intOf(arena.allocator(), .{ .string = huge }));
}

fn entryOf(allocator: std.mem.Allocator, json: []const u8) !std.json.Value {
    const parsed = try std.json.parseFromSlice(std.json.Value, allocator, json, .{});
    return parsed.value;
}

test "fileReachable subtracts deactivated decisions from the denominator" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const entry = try entryOf(allocator,
        \\{"file":"libs/a.c","total_decisions":10,"covered_decisions":8,"deactivated_decisions":2}
    );
    const reach = try implementation.fileReachable(allocator, entry);
    try std.testing.expectEqual(@as(i128, 8), reach.covered);
    try std.testing.expectEqual(@as(i128, 8), reach.reachable_total);
}

test "fileReachable defaults every missing count to zero" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const reach = try implementation.fileReachable(allocator, try entryOf(allocator,
        \\{"file":"libs/a.c"}
    ));
    try std.testing.expectEqual(@as(i128, 0), reach.covered);
    try std.testing.expectEqual(@as(i128, 0), reach.reachable_total);
}

test "fileReachable can go negative when deactivated exceeds total" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const reach = try implementation.fileReachable(allocator, try entryOf(allocator,
        \\{"total_decisions":1,"deactivated_decisions":4}
    ));
    try std.testing.expectEqual(@as(i128, -3), reach.reachable_total);
}

test "fileReachable on a non-object entry is an AttributeError" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectError(
        error.AttributeError,
        implementation.fileReachable(allocator, .{ .string = "libs/a.c" }),
    );
}

test "fileField reads the path, defaults to empty and rejects a non-string" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectEqualStrings("libs/a.c", try implementation.fileField(try entryOf(allocator,
        \\{"file":"libs/a.c"}
    )));
    try std.testing.expectEqualStrings("", try implementation.fileField(try entryOf(allocator,
        \\{"total_decisions":1}
    )));
    try std.testing.expectError(error.AttributeError, implementation.fileField(try entryOf(allocator,
        \\{"file":7}
    )));
    try std.testing.expectError(error.AttributeError, implementation.fileField(.{ .integer = 1 }));
}

test "reachablePct divides in the predecessor's operation order" {
    try std.testing.expectEqual(@as(f64, 100.0), implementation.reachablePct(2, 2));
    try std.testing.expectEqual(@as(f64, 50.0), implementation.reachablePct(1, 2));
    try std.testing.expectEqual(@as(f64, 150.0), implementation.reachablePct(3, 2));
    try std.testing.expectEqual(@as(f64, -50.0), implementation.reachablePct(-1, 2));
}

test "a third is below the floor, a whole is not" {
    try std.testing.expect(implementation.reachablePct(1, 3) < implementation.floor_pct);
    try std.testing.expect(!(implementation.reachablePct(3, 3) < implementation.floor_pct));
}

fn pct(buffer: []u8, value: f64) ![]const u8 {
    return implementation.formatPct(buffer, value);
}

test "the percentage column rounds half to EVEN, as CPython does" {
    var buffer: [64]u8 = undefined;
    try std.testing.expectEqualStrings("  6.2", try pct(&buffer, 6.25));
    try std.testing.expectEqualStrings("  6.8", try pct(&buffer, 6.75));
    try std.testing.expectEqualStrings(" 87.2", try pct(&buffer, 87.25));
    try std.testing.expectEqualStrings("  9.4", try pct(&buffer, 9.375));
}

test "the percentage column rounds a non-tie on the exact binary value" {
    var buffer: [64]u8 = undefined;
    try std.testing.expectEqualStrings("100.0", try pct(&buffer, 99.95));
    try std.testing.expectEqualStrings(" 33.3", try pct(&buffer, 33.333333333333336));
    try std.testing.expectEqualStrings(" 66.7", try pct(&buffer, 66.66666666666667));
}

test "the percentage column pads to width five" {
    var buffer: [64]u8 = undefined;
    try std.testing.expectEqualStrings("  0.0", try pct(&buffer, 0.0));
    try std.testing.expectEqualStrings("  9.5", try pct(&buffer, 9.5));
    try std.testing.expectEqualStrings(" 99.9", try pct(&buffer, 99.9));
    try std.testing.expectEqualStrings("150.0", try pct(&buffer, 150.0));
    try std.testing.expectEqualStrings("-50.0", try pct(&buffer, -50.0));
}

test "the percentage column carries a nine all the way up" {
    var buffer: [64]u8 = undefined;
    try std.testing.expectEqualStrings("100.0", try pct(&buffer, 99.99));
    try std.testing.expectEqualStrings(" 10.0", try pct(&buffer, 9.99));
}

fn offender(pct_value: f64, rel: []const u8) implementation.Offender {
    return .{ .pct = pct_value, .rel = rel, .covered = 1, .reachable_total = 2 };
}

test "offenders sort worst rate first" {
    var list = [_]implementation.Offender{
        offender(50.0, "b.c"),
        offender(0.0, "z.c"),
        offender(25.0, "a.c"),
    };
    std.mem.sort(implementation.Offender, &list, {}, implementation.offenderLessThan);
    try std.testing.expectEqualStrings("z.c", list[0].rel);
    try std.testing.expectEqualStrings("a.c", list[1].rel);
    try std.testing.expectEqualStrings("b.c", list[2].rel);
}

test "equal rates fall back to code-point order, so uppercase leads" {
    var list = [_]implementation.Offender{
        offender(50.0, "b/z.c"),
        offender(50.0, "a/z.c"),
        offender(50.0, "A.c"),
    };
    std.mem.sort(implementation.Offender, &list, {}, implementation.offenderLessThan);
    try std.testing.expectEqualStrings("A.c", list[0].rel);
    try std.testing.expectEqualStrings("a/z.c", list[1].rel);
    try std.testing.expectEqualStrings("b/z.c", list[2].rel);
}

fn documentFiles(allocator: std.mem.Allocator, json: []const u8) ![]const std.json.Value {
    const parsed = try std.json.parseFromSlice(std.json.Value, allocator, json, .{});
    return parsed.value.array.items;
}

test "collectOffenders counts one per production root and finds no offender" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"apps/shared_libs/b.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"examples/c.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"port/d.c","covered_decisions":1,"total_decisions":1},
        \\ {"file":"tools/e.c","covered_decisions":1,"total_decisions":1}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try std.testing.expectEqual(@as(usize, 5), collected.checked());
    try std.testing.expect(std.mem.allEqual(usize, &collected.census, 1));
}

test "collectOffenders skips a file with no reachable decision" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","covered_decisions":0,"total_decisions":0},
        \\ {"file":"libs/b.c","covered_decisions":0,"total_decisions":3,"deactivated_decisions":3}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try std.testing.expectEqual(@as(usize, 0), collected.checked());
}

test "collectOffenders keeps a deactivated-only file at full marks" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","covered_decisions":4,"total_decisions":9,"deactivated_decisions":5}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try std.testing.expectEqual(@as(usize, 1), collected.checked());
}

test "collectOffenders reports a below-floor file with its counts" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","covered_decisions":1,"total_decisions":4,"deactivated_decisions":1}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 1), collected.offenders.len);
    try std.testing.expectEqualStrings("libs/a.c", collected.offenders[0].rel);
    try std.testing.expectEqual(@as(i128, 1), collected.offenders[0].covered);
    try std.testing.expectEqual(@as(i128, 3), collected.offenders[0].reachable_total);
}

test "collectOffenders normalises absolute paths before scoping them" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"/w/ra8-firmware/libs/a.c","covered_decisions":0,"total_decisions":1}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 1), collected.offenders.len);
    try std.testing.expectEqualStrings("libs/a.c", collected.offenders[0].rel);
}

test "collectOffenders ignores an exempt entry even when its counts are junk" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/third_party/soup.c","total_decisions":null},
        \\ {"file":"src/legacy.c","covered_decisions":"nonsense"}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try std.testing.expectEqual(@as(usize, 0), collected.checked());
}

test "collectOffenders surfaces a malformed count on an in-scope entry" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","total_decisions":null}]
    );
    try std.testing.expectError(
        error.TypeError,
        implementation.collectOffenders(allocator, files, "ra8-firmware"),
    );
}

test "collectOffenders surfaces a non-string file field" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":7,"total_decisions":1}]
    );
    try std.testing.expectError(
        error.AttributeError,
        implementation.collectOffenders(allocator, files, "ra8-firmware"),
    );
}

test "collectOffenders surfaces a non-object entry" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\["libs/a.c"]
    );
    try std.testing.expectError(
        error.AttributeError,
        implementation.collectOffenders(allocator, files, "ra8-firmware"),
    );
}

test "collectOffenders coerces string and float counts" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","covered_decisions":"1","total_decisions":2.9}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 1), collected.offenders.len);
    try std.testing.expectEqual(@as(i128, 2), collected.offenders[0].reachable_total);
}

test "collectOffenders never treats an over-100% file as an offender" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/a.c","covered_decisions":5,"total_decisions":2}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 0), collected.offenders.len);
    try std.testing.expectEqual(@as(usize, 1), collected.checked());
}

test "collectOffenders sorts its result worst first" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const files = try documentFiles(allocator,
        \\[{"file":"libs/mid.c","covered_decisions":1,"total_decisions":2},
        \\ {"file":"libs/worst.c","covered_decisions":0,"total_decisions":2},
        \\ {"file":"libs/near.c","covered_decisions":3,"total_decisions":4}]
    );
    const collected = try implementation.collectOffenders(allocator, files, "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 3), collected.offenders.len);
    try std.testing.expectEqualStrings("libs/worst.c", collected.offenders[0].rel);
    try std.testing.expectEqualStrings("libs/mid.c", collected.offenders[1].rel);
    try std.testing.expectEqualStrings("libs/near.c", collected.offenders[2].rel);
}

test "missingScopes lists absent roots in declared order" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const missing = try implementation.missingScopes(arena.allocator(), .{ 0, 2, 0, 1, 0 });
    try std.testing.expectEqual(@as(usize, 3), missing.len);
    try std.testing.expectEqualStrings("libs/", missing[0]);
    try std.testing.expectEqualStrings("examples/", missing[1]);
    try std.testing.expectEqualStrings("tools/", missing[2]);
}

test "missingScopes is empty when every root contributed" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const missing = try implementation.missingScopes(arena.allocator(), .{ 1, 1, 1, 1, 1 });
    try std.testing.expectEqual(@as(usize, 0), missing.len);
}

test "the offender table is spelled exactly as the predecessor printed it" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const offenders = [_]implementation.Offender{
        .{ .pct = 0.0, .rel = "libs/ra8_core/src/core.c", .covered = 0, .reachable_total = 3 },
        .{ .pct = 50.0, .rel = "port/posix/src/io.c", .covered = 1, .reachable_total = 2 },
    };
    const text = try renderToString(
        arena.allocator(),
        implementation.renderOffenders,
        .{@as([]const implementation.Offender, &offenders)},
    );
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: 2 first-party file(s) below the 100% reachable-MC/DC floor (NO allowlist):\n" ++
            "  mc/dc  covered/reachable  file\n" ++
            "    0.0%      0/3           libs/ra8_core/src/core.c\n" ++
            "   50.0%      1/2           port/posix/src/io.c\n" ++
            "Fix each at the root -- add the missing MC/DC vector (N+1 vectors for N conditions; " ++
            "see docs/MCDC.md), or, if the gap is genuinely unreachable on any public-API path, " ++
            "catalogue it with a `// mcdc-deactivated:` rationale per DO-178C 6.4.4.3. " ++
            "Do NOT add an allowlist.\n",
        text,
    );
}

test "the pass line names the checked count" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const text = try renderToString(arena.allocator(), implementation.renderPass, .{@as(usize, 412)});
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: PASS -- all 412 first-party file(s) with a reachable decision are >= 100% MC/DC.\n",
        text,
    );
}

test "the non-vacuity line joins missing roots with a comma" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const missing = [_][]const u8{ "port/", "tools/" };
    const text = try renderToString(
        arena.allocator(),
        implementation.renderMissingScopes,
        .{@as([]const []const u8, &missing)},
    );
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- no reachable decisions matched required scope(s): " ++
            "port/, tools/; check the JSON path / scope.\n",
        text,
    );
}

test "the missing-report line names the absolute path and the remedy" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const text = try renderToString(
        arena.allocator(),
        implementation.renderMissingReport,
        .{@as([]const u8, "/w/ra8-firmware/build/mcdc-report/mcdc_per_file.json")},
    );
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- /w/ra8-firmware/build/mcdc-report/mcdc_per_file.json not found; " ++
            "run `bash scripts/report/mcdc_report.sh` first.\n",
        text,
    );
}

test "the no-files line is its own distinct error" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const text = try renderToString(arena.allocator(), implementation.renderNoFiles, .{});
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- MC/DC JSON has no files.\n",
        text,
    );
}

test "the unreadable-report line carries the reason" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const text = try renderToString(
        arena.allocator(),
        implementation.renderUnreadableReport,
        .{@as([]const u8, "SyntaxError")},
    );
    try std.testing.expectEqualStrings(
        "check_mcdc_floor: ERROR -- cannot read MC/DC JSON: SyntaxError\n",
        text,
    );
}

test "the selftest lines keep their shape" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    try std.testing.expectEqualStrings(
        "check_mcdc_floor selftest: PASS -- scope and non-vacuity checks hold.\n",
        try renderToString(allocator, implementation.renderSelftestPass, .{}),
    );
    try std.testing.expectEqualStrings(
        "check_mcdc_floor selftest: FAIL -- scope mismatch for libs/a.c: expected True\n",
        try renderToString(
            allocator,
            implementation.renderSelftestFailure,
            .{@as([]const u8, "scope mismatch for libs/a.c: expected True")},
        ),
    );
}

test "the embedded selftest holds against the real scope table" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const failures = try implementation.selftestFailures(arena.allocator(), "ra8-firmware");
    try std.testing.expectEqual(@as(usize, 0), failures.items.len);
}

test "the selftest carries every scope case the predecessor listed" {
    try std.testing.expectEqual(@as(usize, 14), implementation.scope_cases.len);
    var expected_true: usize = 0;
    for (implementation.scope_cases) |case| {
        if (case.expected) expected_true += 1;
    }
    try std.testing.expectEqual(@as(usize, 5), expected_true);
}

test "the floor and the report path are the predecessor's" {
    try std.testing.expectEqual(@as(f64, 100.0), implementation.floor_pct);
    try std.testing.expectEqualStrings(
        "build/mcdc-report/mcdc_per_file.json",
        implementation.mcdc_json_rel,
    );
    try std.testing.expectEqual(@as(usize, 5), implementation.in_scope_prefixes.len);
    try std.testing.expectEqual(@as(usize, 3), implementation.out_of_scope_prefixes.len);
    try std.testing.expectEqual(@as(usize, 3), implementation.out_of_scope_parts.len);
}

test "a count column never grows a plus sign" {
    var buffer: [64]u8 = undefined;
    try std.testing.expectEqualStrings("0", try implementation.formatCount(&buffer, 0));
    try std.testing.expectEqualStrings("12", try implementation.formatCount(&buffer, 12));
    try std.testing.expectEqualStrings("-3", try implementation.formatCount(&buffer, -3));
    try std.testing.expectEqualStrings(
        "170141183460469231731687303715884105727",
        try implementation.formatCount(&buffer, std.math.maxInt(i128)),
    );
}
