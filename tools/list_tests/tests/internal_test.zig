//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the pure half of `list_tests` (#858).
//! Every case here pins a rule the deleted Python implementation had, so a
//! rewrite that changes one fails rather than silently producing a different
//! listing.

const std = @import("std");
const testing = std.testing;
const listing = @import("implementation");

test "componentMatches: a literal component matches only itself" {
    try testing.expect(listing.componentMatches("tests", "tests"));
    try testing.expect(!listing.componentMatches("tests", "test"));
    try testing.expect(!listing.componentMatches("tests", "tests2"));
}

test "componentMatches: a bare star matches any ordinary name" {
    try testing.expect(listing.componentMatches("*", "ra8_hal"));
    try testing.expect(listing.componentMatches("*", "a"));
}

test "componentMatches: a star DOES match a leading dot, as pathlib does" {
    // `glob.glob` hides dotted names; `pathlib.Path.glob` -- what the deleted
    // Python used -- does not, and a differential run proved it listed a target
    // under `tools/.hidden/tests/`.
    try testing.expect(listing.componentMatches("*", ".git"));
    try testing.expect(listing.componentMatches("*", ".zig-cache"));
    try testing.expect(listing.componentMatches(".git", ".git"));
    try testing.expect(!listing.componentMatches(".git", "git"));
}

test "componentMatches: a star matches an empty run" {
    try testing.expect(listing.componentMatches("test_*.c", "test_.c"));
    try testing.expect(listing.componentMatches("test_*.c", "test_gpio.c"));
    try testing.expect(!listing.componentMatches("test_*.c", "test_gpio.cpp"));
    try testing.expect(!listing.componentMatches("test_*.c", "gpio.c"));
}

test "componentMatches: a question mark matches exactly one byte" {
    try testing.expect(listing.componentMatches("test_?.c", "test_a.c"));
    try testing.expect(!listing.componentMatches("test_?.c", "test_ab.c"));
    try testing.expect(!listing.componentMatches("test_?.c", "test_.c"));
}

test "componentMatches: two stars still anchor the literal between them" {
    try testing.expect(listing.componentMatches("*_hal_*", "ra8_hal_gpio"));
    try testing.expect(!listing.componentMatches("*_hal_*", "ra8_halgpio"));
}

test "isWildcard: only star and question mark force a directory listing" {
    try testing.expect(listing.isWildcard("*"));
    try testing.expect(listing.isWildcard("test_*.c"));
    try testing.expect(listing.isWildcard("test_?.c"));
    try testing.expect(!listing.isWildcard("tests"));
    try testing.expect(!listing.isWildcard("apps"));
}

test "stem: the last suffix is removed and nothing else" {
    try testing.expectEqualStrings("test_gpio", listing.stem("test_gpio.c"));
    try testing.expectEqualStrings("test_gpio", listing.stem("test_gpio.cpp"));
    try testing.expectEqualStrings("test_a.b", listing.stem("test_a.b.c"));
    try testing.expectEqualStrings("test_gpio", listing.stem("test_gpio"));
    try testing.expectEqualStrings(".hidden", listing.stem(".hidden"));
}

test "briefInLine: the tag needs at least one whitespace byte after it" {
    try testing.expect(listing.briefInLine("@briefly no\n") == null);
    try testing.expect(listing.briefInLine(" * no tag here\n") == null);
    try testing.expectEqualStrings("yes", listing.briefInLine("@brief yes\n").?);
}

test "briefInLine: the description is the rest of the line, stripped" {
    try testing.expectEqualStrings(
        "GPIO driver unit tests",
        listing.briefInLine(" * @brief   GPIO driver unit tests   \n").?,
    );
    try testing.expectEqualStrings(
        "tab separated",
        listing.briefInLine("///@brief\ttab separated\n").?,
    );
}

test "briefInLine: internal spacing inside the description survives" {
    try testing.expectEqualStrings(
        "two  spaces  kept",
        listing.briefInLine(" * @brief two  spaces  kept\n").?,
    );
}

test "briefInLine: a bare tag yields an EMPTY description, not a miss" {
    // Inherited: `\s+` swallowed the newline and `(.*)` then captured nothing.
    try testing.expectEqualStrings("", listing.briefInLine("@brief\n").?);
    try testing.expectEqualStrings("", listing.briefInLine(" * @brief   \n").?);
}

test "briefInLine: a line with no terminator still works" {
    try testing.expectEqualStrings("last line", listing.briefInLine("@brief last line").?);
}

test "briefIn: the FIRST line carrying the tag wins" {
    const source =
        \\/**
        \\ * @file test_gpio.c
        \\ * @brief GPIO driver unit tests
        \\ * @brief a second tag that must be ignored
        \\ */
        \\
    ;
    try testing.expectEqualStrings("GPIO driver unit tests", listing.briefIn(source).?);
}

test "briefIn: a source with no tag has no description" {
    try testing.expect(listing.briefIn("int main(void) { return 0; }\n") == null);
    try testing.expect(listing.briefIn("") == null);
}

test "decodeIgnoringInvalid: a CRLF pair becomes one newline" {
    const decoded = try listing.decodeIgnoringInvalid(testing.allocator, "a\r\nb\r\n");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("a\nb\n", decoded);
}

test "decodeIgnoringInvalid: a lone carriage return becomes a newline" {
    const decoded = try listing.decodeIgnoringInvalid(testing.allocator, "a\rb\r");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("a\nb\n", decoded);
}

test "decodeIgnoringInvalid: invalid UTF-8 bytes are dropped, not fatal" {
    const decoded = try listing.decodeIgnoringInvalid(testing.allocator, "a\xffb\xfe\n");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("ab\n", decoded);
}

test "decodeIgnoringInvalid: valid multi-byte sequences survive intact" {
    const decoded = try listing.decodeIgnoringInvalid(testing.allocator, "caf\u{00e9} \u{2014}\n");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("caf\u{00e9} \u{2014}\n", decoded);
}

test "decodeIgnoringInvalid: a truncated sequence at EOF is dropped" {
    const decoded = try listing.decodeIgnoringInvalid(testing.allocator, "ok\xe2\x82");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("ok", decoded);
}

test "briefIn: a CRLF source yields the same description as an LF one" {
    const crlf = try listing.decodeIgnoringInvalid(
        testing.allocator,
        " * @brief CRLF page\r\n * @file x\r\n",
    );
    defer testing.allocator.free(crlf);
    try testing.expectEqualStrings("CRLF page", listing.briefIn(crlf).?);
}

test "searchPatterns: the four product-tier categories have fixed roots" {
    const shared = try listing.searchPatterns(testing.allocator, "shared");
    defer testing.allocator.free(shared);
    try testing.expectEqual(@as(usize, 1), shared.len);
    try testing.expectEqualStrings("apps/shared_libs/*/tests", shared[0]);

    const host = try listing.searchPatterns(testing.allocator, "host");
    defer testing.allocator.free(host);
    try testing.expectEqualStrings("apps/host/*/tests", host[0]);

    const tools = try listing.searchPatterns(testing.allocator, "tools");
    defer testing.allocator.free(tools);
    try testing.expectEqualStrings("tools/*/tests", tools[0]);
}

test "searchPatterns: board carries two roots, in order" {
    const board = try listing.searchPatterns(testing.allocator, "board");
    defer testing.allocator.free(board);
    try testing.expectEqual(@as(usize, 2), board.len);
    try testing.expectEqualStrings("apps/board/*/*/tests", board[0]);
    try testing.expectEqualStrings("apps/board/*/tests", board[1]);
}

test "searchPatterns: any other name is a directory under tests/" {
    const hal = try listing.searchPatterns(testing.allocator, "hal");
    defer {
        testing.allocator.free(hal[0]);
        testing.allocator.free(hal);
    }
    try testing.expectEqual(@as(usize, 1), hal.len);
    try testing.expectEqualStrings("tests/hal", hal[0]);
}

test "asciiLower and asciiUpper fold only ASCII" {
    const lower = try listing.asciiLower(testing.allocator, "HaL");
    defer testing.allocator.free(lower);
    try testing.expectEqualStrings("hal", lower);

    const upper = try listing.asciiUpper(testing.allocator, "storage");
    defer testing.allocator.free(upper);
    try testing.expectEqualStrings("STORAGE", upper);
}

test "defaultDescription: the stem plus 'unit tests'" {
    const description = try listing.defaultDescription(testing.allocator, "test_gpio");
    defer testing.allocator.free(description);
    try testing.expectEqualStrings("test_gpio unit tests", description);
}

test "writeRow: a short name is padded to the description column" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try listing.writeRow(buffer.writer(), .{ .name = "test_gpio", .description = "GPIO" });
    try testing.expectEqualStrings(
        "  test_gpio                                GPIO\n",
        buffer.items,
    );
    // Two leading spaces, then the name padded to exactly 40 columns.
    try testing.expectEqual(@as(usize, 2 + listing.name_column_width + 1), 43);
}

test "writeRow: a name past the column is never truncated" {
    const long = "test_a_very_long_target_name_that_exceeds_the_column";
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try listing.writeRow(buffer.writer(), .{ .name = long, .description = "desc" });
    try testing.expectEqualStrings("  " ++ long ++ " desc\n", buffer.items);
}

test "writeRow: an empty description still leaves the separating space" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try listing.writeRow(buffer.writer(), .{ .name = "test_x", .description = "" });
    try testing.expect(std.mem.endsWith(u8, buffer.items, " \n"));
}

test "writeHeader: the banner carries the upper-cased category and the count" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try listing.writeHeader(buffer.writer(), testing.allocator, "hal", 7);
    try testing.expectEqualStrings(
        "== HAL TESTS (7) -- local: just tests::local hal | " ++
            "container: just tests::devcontainer hal\n\n",
        buffer.items,
    );
}

test "writeHeader: the blank line print() added is part of the banner" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try listing.writeHeader(buffer.writer(), testing.allocator, "net", 0);
    try testing.expect(std.mem.endsWith(u8, buffer.items, "\n\n"));
}

test "lessThanByName: ordering is by name only" {
    try testing.expect(listing.lessThanByName(
        {},
        .{ .name = "test_a", .description = "z" },
        .{ .name = "test_b", .description = "a" },
    ));
    try testing.expect(!listing.lessThanByName(
        {},
        .{ .name = "test_b", .description = "a" },
        .{ .name = "test_a", .description = "z" },
    ));
    try testing.expect(!listing.lessThanByName(
        {},
        .{ .name = "test_a", .description = "a" },
        .{ .name = "test_a", .description = "b" },
    ));
}

test "test_suffixes: C before C++, the order discovery walks" {
    try testing.expectEqual(@as(usize, 2), listing.test_suffixes.len);
    try testing.expectEqualStrings("c", listing.test_suffixes[0]);
    try testing.expectEqualStrings("cpp", listing.test_suffixes[1]);
}
