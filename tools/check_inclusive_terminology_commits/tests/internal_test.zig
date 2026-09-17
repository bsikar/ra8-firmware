//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural tests for the commit-message terminology detector (#858).
//!
//! Every rule the predecessor's five regular expressions and paragraph-scoped
//! opt-out carried is pinned here, including the ones that only show up in
//! Unicode text: the word boundary is Unicode-aware, the line split is wider
//! than `\n`, and case folding reaches three non-ASCII code points.

const std = @import("std");
const implementation = @import("implementation");

const controller = "mas" ++ "ter";
const peripheral = "sla" ++ "ve";
const copi = "MO" ++ "SI";
const cipo = "MI" ++ "SO";

/// Findings for `text`, as a count, under the testing allocator.
fn countFindings(text: []const u8) !usize {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(arena.allocator(), text);
    return violations.len;
}

/// The message of the single finding `text` produces.
fn onlyMessage(text: []const u8, buffer: []u8) ![]const u8 {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(arena.allocator(), text);
    try std.testing.expectEqual(@as(usize, 1), violations.len);
    @memcpy(buffer[0..violations[0].message.len], violations[0].message);
    return buffer[0..violations[0].message.len];
}

/// True when `id`'s pattern matches `text`, decoded as one line.
fn matchesLine(id: implementation.Term.Id, text: []const u8) !bool {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const chars = try implementation.decode(arena.allocator(), text);
    return implementation.lineMatches(id, chars);
}

/// True when `text`, decoded as one line, carries the opt-out.
fn hasOptOutIn(text: []const u8) !bool {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const chars = try implementation.decode(arena.allocator(), text);
    return implementation.hasOptOut(chars);
}

test "the bare term fires" {
    try std.testing.expect(try matchesLine(.controller, controller));
}

test "every documented suffix fires" {
    for ([_][]const u8{ "s", "ed", "ing", "ship" }) |suffix| {
        var buffer: [32]u8 = undefined;
        const word = try std.fmt.bufPrint(&buffer, "{s}{s}", .{ controller, suffix });
        try std.testing.expect(try matchesLine(.controller, word));
    }
}

test "an undocumented suffix stays quiet" {
    try std.testing.expect(!try matchesLine(.controller, controller ++ "ful"));
    try std.testing.expect(!try matchesLine(.controller, controller ++ "y"));
}

test "a prefix stays quiet: the boundary is leading too" {
    try std.testing.expect(!try matchesLine(.controller, "re" ++ controller));
}

test "an underscore is a word character on both sides" {
    try std.testing.expect(!try matchesLine(.controller, "_" ++ controller));
    try std.testing.expect(!try matchesLine(.controller, controller ++ "_key"));
}

test "a digit is a word character on both sides" {
    try std.testing.expect(!try matchesLine(.controller, "3" ++ controller));
    try std.testing.expect(!try matchesLine(.controller, controller ++ "3"));
}

test "the term is case-insensitive, suffix included" {
    try std.testing.expect(try matchesLine(.controller, "MAS" ++ "TER"));
    try std.testing.expect(try matchesLine(.controller, "mas" ++ "TERing"));
}

test "the long s folds onto s, as re.IGNORECASE folds it" {
    try std.testing.expect(try matchesLine(.controller, "ma\u{017F}ter"));
    try std.testing.expect(try matchesLine(.peripheral, "\u{017F}lave"));
}

test "an accented letter is a word character, so it suppresses the match" {
    try std.testing.expect(!try matchesLine(.controller, "\u{00E9}" ++ controller));
    try std.testing.expect(!try matchesLine(.controller, controller ++ "\u{00E9}"));
}

test "an em dash is not a word character, so the term still fires" {
    try std.testing.expect(try matchesLine(.controller, controller ++ "\u{2014}ok"));
}

test "an emoji is not a word character either" {
    try std.testing.expect(try matchesLine(.controller, "\u{1F680}" ++ controller));
}

test "a CJK ideograph is a word character" {
    try std.testing.expect(!try matchesLine(.controller, "\u{4E2D}" ++ controller));
}

test "the peripheral term takes only its two suffixes" {
    try std.testing.expect(try matchesLine(.peripheral, peripheral));
    try std.testing.expect(try matchesLine(.peripheral, peripheral ++ "s"));
    try std.testing.expect(try matchesLine(.peripheral, peripheral ++ "d"));
    try std.testing.expect(!try matchesLine(.peripheral, peripheral ++ "ry"));
}

test "the pin names are case-SENSITIVE" {
    try std.testing.expect(try matchesLine(.copi, copi));
    try std.testing.expect(!try matchesLine(.copi, "mosi"));
    try std.testing.expect(!try matchesLine(.copi, "Mosi"));
    try std.testing.expect(try matchesLine(.cipo, cipo));
    try std.testing.expect(!try matchesLine(.cipo, "miso"));
}

test "the pin names are bounded" {
    try std.testing.expect(!try matchesLine(.copi, copi ++ "X"));
    try std.testing.expect(!try matchesLine(.copi, "X" ++ copi));
    try std.testing.expect(try matchesLine(.copi, "the " ++ copi ++ "/" ++ cipo ++ " mux"));
}

test "the chip-select phrase takes exactly one of three separators" {
    try std.testing.expect(try matchesLine(.chip_select, peripheral ++ " select"));
    try std.testing.expect(try matchesLine(.chip_select, peripheral ++ "_Select"));
    try std.testing.expect(try matchesLine(.chip_select, peripheral ++ "-SELECT"));
    try std.testing.expect(!try matchesLine(.chip_select, peripheral ++ "  select"));
    try std.testing.expect(!try matchesLine(.chip_select, peripheral ++ "select"));
}

test "the chip-select phrase is bounded at both ends" {
    try std.testing.expect(!try matchesLine(.chip_select, "x" ++ peripheral ++ " select"));
    try std.testing.expect(!try matchesLine(.chip_select, peripheral ++ " selected"));
}

test "the first matching term wins the line" {
    var buffer: [64]u8 = undefined;
    const message = try onlyMessage(peripheral ++ " select\n", &buffer);
    try std.testing.expectEqualStrings(peripheral ++ " -- use Peripheral", message);
}

test "the term order is the predecessor's" {
    try std.testing.expectEqual(@as(usize, 5), implementation.banned.len);
    try std.testing.expectEqual(implementation.Term.Id.controller, implementation.banned[0].id);
    try std.testing.expectEqual(implementation.Term.Id.chip_select, implementation.banned[4].id);
}

test "one finding per line, never one per term" {
    try std.testing.expectEqual(@as(usize, 1), try countFindings(controller ++ " and " ++ copi ++ "\n"));
}

test "a clean message reports nothing" {
    try std.testing.expectEqual(@as(usize, 0), try countFindings("fix(spi): rename the controller pin mux\n"));
}

test "empty text reports nothing" {
    try std.testing.expectEqual(@as(usize, 0), try countFindings(""));
}

test "an opt-out silences its own line" {
    try std.testing.expectEqual(
        @as(usize, 0),
        try countFindings(copi ++ " stays LEGACY-OK: upstream name\n"),
    );
}

test "an opt-out covers the whole wrapped paragraph" {
    try std.testing.expectEqual(
        @as(usize, 0),
        try countFindings("uses " ++ copi ++ "\nLEGACY-OK: upstream\n"),
    );
}

test "an opt-out does not reach across a blank line" {
    try std.testing.expectEqual(
        @as(usize, 1),
        try countFindings("uses " ++ copi ++ "\n\nLEGACY-OK: next paragraph\n"),
    );
}

test "an opt-out reaches backwards inside its own paragraph" {
    try std.testing.expectEqual(
        @as(usize, 0),
        try countFindings("LEGACY-OK: upstream\nuses " ++ copi ++ "\n"),
    );
}

test "the opt-out tag is case-insensitive" {
    try std.testing.expect(try hasOptOutIn("legacy-ok:"));
    try std.testing.expect(try hasOptOutIn("Legacy-Ok :"));
}

test "the Kelvin sign folds onto k in the opt-out tag" {
    try std.testing.expect(try hasOptOutIn("LEGACY-O\u{212A}:"));
}

test "any run of Python whitespace may precede the opt-out colon" {
    try std.testing.expect(try hasOptOutIn("LEGACY-OK \t:"));
    try std.testing.expect(try hasOptOutIn("LEGACY-OK\u{00A0}:"));
    try std.testing.expect(try hasOptOutIn("LEGACY-OK\u{3000}:"));
}

test "an underscore instead of the hyphen is not the opt-out" {
    try std.testing.expect(!try hasOptOutIn("LEGACY_OK:"));
}

test "an opt-out with no colon is not an opt-out" {
    try std.testing.expect(!try hasOptOutIn("LEGACY-OK upstream"));
}

test "line numbers are one-based and count blank lines" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(
        arena.allocator(),
        "subject\n\nbody\n\nuses " ++ copi ++ "\n",
    );
    try std.testing.expectEqual(@as(usize, 1), violations.len);
    try std.testing.expectEqual(@as(usize, 5), violations[0].line);
}

test "a form feed splits a line, so later numbers do not shift" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(
        arena.allocator(),
        "subject\x0cmiddle\nuses " ++ copi ++ "\n",
    );
    try std.testing.expectEqual(@as(usize, 1), violations.len);
    try std.testing.expectEqual(@as(usize, 3), violations[0].line);
}

test "CRLF counts as one break" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(
        arena.allocator(),
        "subject\r\nuses " ++ copi ++ "\r\n",
    );
    try std.testing.expectEqual(@as(usize, 1), violations.len);
    try std.testing.expectEqual(@as(usize, 2), violations[0].line);
}

test "the three Unicode breaks split lines" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const chars = try implementation.decode(arena.allocator(), "a\u{0085}b\u{2028}c\u{2029}d");
    const lines = try implementation.splitLines(arena.allocator(), chars);
    try std.testing.expectEqual(@as(usize, 4), lines.len);
}

test "a trailing break does not invent an empty line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const chars = try implementation.decode(arena.allocator(), "a\nb\n");
    const lines = try implementation.splitLines(arena.allocator(), chars);
    try std.testing.expectEqual(@as(usize, 2), lines.len);
}

test "the echoed line is stripped of Python whitespace" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(
        arena.allocator(),
        "  \t" ++ copi ++ " pin \u{00A0}\n",
    );
    try std.testing.expectEqual(@as(usize, 1), violations.len);
    try std.testing.expectEqualStrings(copi ++ " pin", violations[0].text);
}

test "a line of only non-breaking space is a paragraph boundary" {
    try std.testing.expectEqual(
        @as(usize, 1),
        try countFindings("uses " ++ copi ++ "\n\u{00A0}\nLEGACY-OK: next\n"),
    );
}

test "a rendered finding matches the predecessor's two-line shape" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const violations = try implementation.findViolations(arena.allocator(), "uses " ++ copi ++ "\n");
    const rendered = try implementation.renderViolation(arena.allocator(), violations[0]);
    try std.testing.expectEqualStrings(
        "  line 1: " ++ copi ++ " -- use COPI\n    > uses " ++ copi,
        rendered,
    );
}

test "an undecodable byte is escaped, not fatal, and is not a word character" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const chars = try implementation.decode(arena.allocator(), "\xff" ++ controller);
    try std.testing.expectEqual(implementation.surrogate_base + 0xFF, chars[0].cp);
    try std.testing.expect(!implementation.isWordChar(chars[0].cp));
    try std.testing.expect(implementation.lineMatches(.controller, chars));
}

test "a truncated sequence escapes each stray byte" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const chars = try implementation.decode(arena.allocator(), "\xe2\x82");
    try std.testing.expectEqual(@as(usize, 2), chars.len);
}

test "word classification covers the ASCII set exactly" {
    var cp: u21 = 0;
    while (cp < 0x80) : (cp += 1) {
        const ascii_word = (cp >= 'a' and cp <= 'z') or (cp >= 'A' and cp <= 'Z') or
            (cp >= '0' and cp <= '9') or cp == '_';
        try std.testing.expectEqual(ascii_word, implementation.isWordChar(cp));
    }
}

test "the Unicode word table binary-searches its edges" {
    try std.testing.expect(implementation.isWordChar(0x00AA));
    try std.testing.expect(!implementation.isWordChar(0x00A9));
    try std.testing.expect(implementation.isWordChar(0x4E00));
    try std.testing.expect(!implementation.isWordChar(0x2014));
    try std.testing.expect(!implementation.isWordChar(0x10FFFF));
}

test "Python whitespace covers the separators and the wide spaces" {
    for ([_]u21{ 0x09, 0x0B, 0x1C, 0x1F, 0x20, 0x85, 0xA0, 0x1680, 0x2009, 0x205F, 0x3000 }) |cp| {
        try std.testing.expect(implementation.isPythonSpace(cp));
    }
    try std.testing.expect(!implementation.isPythonSpace(0x200B));
}

test "the information separators break lines but the unit separator does not" {
    try std.testing.expect(implementation.isLineBreak(0x1C));
    try std.testing.expect(implementation.isLineBreak(0x1E));
    try std.testing.expect(!implementation.isLineBreak(0x1F));
}

test "case folding reaches exactly the three non-ASCII code points" {
    try std.testing.expectEqual(@as(u21, 's'), implementation.foldCase(0x017F));
    try std.testing.expectEqual(@as(u21, 'k'), implementation.foldCase(0x212A));
    try std.testing.expectEqual(@as(u21, 'i'), implementation.foldCase(0x0130));
    try std.testing.expectEqual(@as(u21, 'i'), implementation.foldCase(0x0131));
    try std.testing.expectEqual(@as(u21, 0x00C9), implementation.foldCase(0x00C9));
}

test "the selftest fixtures carry what the selftest claims" {
    try std.testing.expect(try countFindings(implementation.selftest_fires) > 0);
    try std.testing.expectEqual(@as(usize, 0), try countFindings(implementation.selftest_quiet));
    try std.testing.expect(try countFindings(implementation.selftest_cross_paragraph) > 0);
}
