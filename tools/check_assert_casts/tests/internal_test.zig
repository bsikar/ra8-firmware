//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the redundant-cast detector (#858).
//! These pin what the gate DECIDES: which casts fire, which stay quiet, how a
//! finding is rendered, and the text handling the reported rows depend on.

const std = @import("std");
const implementation = @import("implementation");

test "a leading (int) cast fires" {
    try std.testing.expect(implementation.hasLeadingCast("(int)value"));
}

test "a leading (int32_t) cast fires" {
    try std.testing.expect(implementation.hasLeadingCast("(int32_t)value"));
}

test "a leading (uint64_t) cast fires" {
    try std.testing.expect(implementation.hasLeadingCast("(uint64_t)value"));
}

test "a leading (size_t) cast fires" {
    try std.testing.expect(implementation.hasLeadingCast("(size_t)value"));
}

test "a leading (ssize_t) cast fires" {
    try std.testing.expect(implementation.hasLeadingCast("(ssize_t)value"));
}

test "whitespace before the cast is allowed" {
    try std.testing.expect(implementation.hasLeadingCast("   \t(int)value"));
}

test "an argument with no cast stays quiet" {
    try std.testing.expect(!implementation.hasLeadingCast(" value"));
}

test "a parenthesised non-type stays quiet" {
    try std.testing.expect(!implementation.hasLeadingCast("(intx)value"));
    try std.testing.expect(!implementation.hasLeadingCast("(other_t)value"));
}

test "a cast that is not in leading position stays quiet" {
    try std.testing.expect(!implementation.hasLeadingCast("load((int)value)"));
}

test "empty text has no leading cast" {
    try std.testing.expect(!implementation.hasLeadingCast(""));
    try std.testing.expect(!implementation.hasLeadingCast("   "));
}

test "findCloseParen returns the balanced close" {
    const text = "MACRO(a, b);";
    try std.testing.expectEqual(@as(usize, 10), implementation.findCloseParen(text, 6));
}

test "findCloseParen walks past nested parens" {
    const text = "MACRO(f(x), b);";
    try std.testing.expectEqual(@as(usize, 13), implementation.findCloseParen(text, 6));
}

test "findCloseParen on an unbalanced call yields the last index" {
    const text = "MACRO(a, b";
    try std.testing.expectEqual(@as(usize, 9), implementation.findCloseParen(text, 6));
}

test "splitAtTopLevelComma finds the argument boundary" {
    try std.testing.expectEqual(@as(?usize, 1), implementation.splitAtTopLevelComma("a, b"));
}

test "a comma inside a nested call does not split" {
    try std.testing.expectEqual(@as(?usize, 11), implementation.splitAtTopLevelComma("f(one, two), b"));
}

test "a comma inside braces or brackets does not split" {
    try std.testing.expectEqual(@as(?usize, 16), implementation.splitAtTopLevelComma("(t){ .a = 1 }[0], b"));
    try std.testing.expectEqual(@as(?usize, 9), implementation.splitAtTopLevelComma("arr[i, j], b"));
}

test "an invocation with no top-level comma has no split" {
    try std.testing.expectEqual(@as(?usize, null), implementation.splitAtTopLevelComma("f(a, b)"));
}

test "lineOf is one-based and counts newlines" {
    const text = "a\nb\nc";
    try std.testing.expectEqual(@as(usize, 1), implementation.lineOf(text, 0));
    try std.testing.expectEqual(@as(usize, 2), implementation.lineOf(text, 2));
    try std.testing.expectEqual(@as(usize, 3), implementation.lineOf(text, 4));
}

test "stripEnds trims the gate's whitespace set" {
    try std.testing.expectEqualStrings("x", implementation.stripEnds(" \t\r\n\x0b\x0c\x1cx \n"));
}

test "sliceChars truncates at the character limit" {
    try std.testing.expectEqualStrings("abc", implementation.sliceChars("abcdef", 3));
    try std.testing.expectEqualStrings("abcdef", implementation.sliceChars("abcdef", 60));
}

test "sliceChars counts a replacement character once" {
    const text = "a\u{FFFD}b";
    try std.testing.expectEqualStrings("a\u{FFFD}", implementation.sliceChars(text, 2));
}

test "decodeAsciiReplace swaps every non-ascii byte for U+FFFD" {
    const decoded = try implementation.decodeAsciiReplace(std.testing.allocator, "a\xC3\xA9b");
    defer std.testing.allocator.free(decoded);
    try std.testing.expectEqualStrings("a\u{FFFD}\u{FFFD}b", decoded);
}

test "normalizeTerminators collapses CRLF to LF" {
    const unified = try implementation.normalizeTerminators(std.testing.allocator, "a\r\nb\r\n");
    defer std.testing.allocator.free(unified);
    try std.testing.expectEqualStrings("a\nb\n", unified);
}

test "normalizeTerminators maps a lone CR to LF" {
    const unified = try implementation.normalizeTerminators(std.testing.allocator, "a\rb");
    defer std.testing.allocator.free(unified);
    try std.testing.expectEqualStrings("a\nb", unified);
}

test "both arguments of one invocation can fire" {
    const text = "TEST_ASSERT_EQ((int)value, (uint32_t)expected);\n";
    const found = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(found);
    try std.testing.expectEqual(@as(usize, 2), found.len);
    try std.testing.expectEqual(implementation.Argument.first, found[0].argument);
    try std.testing.expectEqual(implementation.Argument.second, found[1].argument);
    try std.testing.expectEqualStrings("(int)value", found[0].snippet);
    try std.testing.expectEqualStrings("(uint32_t)expected", found[1].snippet);
}

test "a nested cast stays quiet through a whole scan" {
    const text = "TEST_ASSERT_EQ(value, expected);\nTEST_ASSERT_EQ(load((int)value), expected);\n";
    const found = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(found);
    try std.testing.expectEqual(@as(usize, 0), found.len);
}

test "an invocation with no top-level comma is skipped, not reported" {
    const text = "TEST_ASSERT_EQ((int)value);\n";
    const found = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(found);
    try std.testing.expectEqual(@as(usize, 0), found.len);
}

test "the reported line is the line of the invocation" {
    const text = "one\ntwo\nTEST_ASSERT_EQ((int)a, b);\n";
    const found = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(found);
    try std.testing.expectEqual(@as(usize, 1), found.len);
    try std.testing.expectEqual(@as(usize, 3), found[0].line);
}

test "a long argument is quoted to sixty characters" {
    const long = "(int)" ++ "a" ** 100;
    const text = "TEST_ASSERT_EQ(" ++ long ++ ", b);\n";
    const found = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(found);
    try std.testing.expectEqual(@as(usize, 1), found.len);
    try std.testing.expectEqual(@as(usize, 60), found[0].snippet.len);
}

test "a scan of several invocations keeps file order" {
    const text = "TEST_ASSERT_EQ((int)a, b);\nTEST_ASSERT_EQ(c, (size_t)d);\n";
    const found = try implementation.scanText(std.testing.allocator, text);
    defer std.testing.allocator.free(found);
    try std.testing.expectEqual(@as(usize, 2), found.len);
    try std.testing.expectEqual(@as(usize, 1), found[0].line);
    try std.testing.expectEqual(@as(usize, 2), found[1].line);
}

test "a first-argument finding renders the macro back" {
    const row = try implementation.renderFinding(std.testing.allocator, "t.c", .{
        .line = 4,
        .argument = .first,
        .snippet = "(int)value",
    });
    defer std.testing.allocator.free(row);
    try std.testing.expectEqualStrings(
        "t.c:4: cast in first arg of TEST_ASSERT_EQ: TEST_ASSERT_EQ((int)value...",
        row,
    );
}

test "a second-argument finding renders with a leading ellipsis" {
    const row = try implementation.renderFinding(std.testing.allocator, "t.c", .{
        .line = 4,
        .argument = .second,
        .snippet = "(int)value",
    });
    defer std.testing.allocator.free(row);
    try std.testing.expectEqualStrings(
        "t.c:4: cast in second arg of TEST_ASSERT_EQ: ...(int)value",
        row,
    );
}

test "normalizePath drops a leading dot component" {
    const shown = try implementation.normalizePath(std.testing.allocator, "./bad.c");
    defer std.testing.allocator.free(shown);
    try std.testing.expectEqualStrings("bad.c", shown);
}

test "normalizePath collapses repeated separators" {
    const shown = try implementation.normalizePath(std.testing.allocator, "a//b/./c/");
    defer std.testing.allocator.free(shown);
    try std.testing.expectEqualStrings("a/b/c", shown);
}

test "normalizePath keeps an absolute root" {
    const shown = try implementation.normalizePath(std.testing.allocator, "/tmp//x.c");
    defer std.testing.allocator.free(shown);
    try std.testing.expectEqualStrings("/tmp/x.c", shown);
}

test "normalizePath turns an empty path into a dot" {
    const shown = try implementation.normalizePath(std.testing.allocator, "");
    defer std.testing.allocator.free(shown);
    try std.testing.expectEqualStrings(".", shown);
}
