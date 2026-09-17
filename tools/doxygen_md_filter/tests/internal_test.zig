//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two Markdown transforms of `doxygen_md_filter` (#858). Every page on
//! the published docs site passes through them, so these cases pin what is
//! removed, what is rewritten, and above all what is left alone: a link this
//! filter mangles is a broken page nobody sees until the site is live.
//!
//! The resolver here is a fixed path set, so the transforms are proved with
//! no repository on disk.

const std = @import("std");
const implementation = @import("implementation");

/// Repository contents the tests resolve link targets against.
const present_files = [_][]const u8{
    "README.md",
    "docs/README.md",
    "libs/ra8_fonts/README.md",
    "apps/board/README.md",
};

fn fakeIsFile(context: *const anyopaque, path: []const u8) bool {
    _ = context;
    for (present_files) |candidate| {
        if (std.mem.eql(u8, candidate, path)) return true;
    }
    return false;
}

const fake_resolver = implementation.Resolver{
    .context = undefined,
    .isFileFn = fakeIsFile,
};

fn filter(text: []const u8, source_dir: []const u8) ![]u8 {
    return implementation.filterMarkdown(std.testing.allocator, text, source_dir, fake_resolver);
}

fn expectFiltered(expected: []const u8, text: []const u8, source_dir: []const u8) !void {
    const actual = try filter(text, source_dir);
    defer std.testing.allocator.free(actual);
    try std.testing.expectEqualStrings(expected, actual);
}

test "a line iterator keeps LF terminators" {
    var lines = implementation.LineIterator.init("a\nb\n");
    try std.testing.expectEqualStrings("a\n", lines.next().?);
    try std.testing.expectEqualStrings("b\n", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "a line iterator keeps CRLF and a lone CR" {
    var lines = implementation.LineIterator.init("a\r\nb\rc");
    try std.testing.expectEqualStrings("a\r\n", lines.next().?);
    try std.testing.expectEqualStrings("b\r", lines.next().?);
    try std.testing.expectEqualStrings("c", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "a final line without a terminator is still a line" {
    var lines = implementation.LineIterator.init("only");
    try std.testing.expectEqualStrings("only", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "a fence is recognised indented, with backticks or tildes" {
    try std.testing.expect(implementation.isFence("```\n"));
    try std.testing.expect(implementation.isFence("   ~~~zig\n"));
    try std.testing.expect(!implementation.isFence("``inline``\n"));
    try std.testing.expect(!implementation.isFence("text ```\n"));
}

test "a badge URL needs the workflow path before badge.svg" {
    try std.testing.expect(implementation.isBadgeUrl(
        "https://github.com/o/r/actions/workflows/ci.yml/badge.svg",
    ));
    try std.testing.expect(!implementation.isBadgeUrl("https://img.shields.io/badge.svg"));
    try std.testing.expect(!implementation.isBadgeUrl("https://github.com/o/r/actions/workflows/ci.yml"));
    try std.testing.expect(!implementation.isBadgeUrl("https://example.invalid/badge.svg/actions/workflows/"));
}

test "a plain badge image matches to its closing parenthesis" {
    const text = "![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg) tail";
    try std.testing.expectEqual(@as(?usize, text.len - 5), implementation.badgeMatchLen(text));
}

test "a link-wrapped badge match swallows the link target too" {
    const text = "[![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg)](https://github.com/o/r)";
    try std.testing.expectEqual(@as(?usize, text.len), implementation.badgeMatchLen(text));
}

test "an ordinary image is not a badge" {
    try std.testing.expectEqual(
        @as(?usize, null),
        implementation.badgeMatchLen("![diagram](docs/img/flow.svg)"),
    );
}

test "a badge is removed and its line dropped when nothing else remains" {
    try expectFiltered(
        "# Title\n",
        "# Title\n[![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg)](https://github.com/o/r)\n",
        "",
    );
}

test "a badge is removed but text on the same line survives" {
    try expectFiltered(
        "status:  today\n",
        "status: ![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg) today\n",
        "",
    );
}

test "an already blank line is kept" {
    try expectFiltered("a\n\nb\n", "a\n\nb\n", "");
}

test "a README link is rewritten to its directory page" {
    try expectFiltered(
        "see [fonts](@ref libs/ra8_fonts)\n",
        "see [fonts](libs/ra8_fonts/README.md)\n",
        "",
    );
}

test "a relative README link resolves against the page's own directory" {
    try expectFiltered(
        "see [fonts](@ref libs/ra8_fonts)\n",
        "see [fonts](../libs/ra8_fonts/README.md)\n",
        "docs",
    );
}

test "a README link that does not resolve is left alone" {
    try expectFiltered(
        "see [gone](libs/ra8_gone/README.md)\n",
        "see [gone](libs/ra8_gone/README.md)\n",
        "",
    );
}

test "the top-level README has no directory page to reference" {
    try expectFiltered("see [home](README.md)\n", "see [home](README.md)\n", "");
}

test "external, absolute and anchored targets pass through" {
    const text =
        "[a](https://example.invalid/README.md) [b](/README.md) [c](docs/README.md#top) [d](mailto:x@y/README.md)\n";
    try expectFiltered(text, text, "");
}

test "a target climbing above the repository root is left alone" {
    try expectFiltered(
        "[up](../../README.md)\n",
        "[up](../../README.md)\n",
        "docs",
    );
}

test "a link that is not to a README is untouched" {
    try expectFiltered("[guide](docs/DOCS.md)\n", "[guide](docs/DOCS.md)\n", "");
}

test "an image link to a README is not a link rewrite" {
    try expectFiltered("![alt](docs/README.md)\n", "![alt](docs/README.md)\n", "");
}

test "both transforms skip a fenced code block" {
    const text =
        "```md\n" ++
        "[![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg)](https://github.com/o/r)\n" ++
        "[docs](docs/README.md)\n" ++
        "```\n" ++
        "[docs](docs/README.md)\n";
    const expected =
        "```md\n" ++
        "[![CI](https://github.com/o/r/actions/workflows/ci.yml/badge.svg)](https://github.com/o/r)\n" ++
        "[docs](docs/README.md)\n" ++
        "```\n" ++
        "[docs](@ref docs)\n";
    try expectFiltered(expected, text, "");
}

test "two links on one line are both rewritten" {
    try expectFiltered(
        "[a](@ref docs) and [b](@ref apps/board)\n",
        "[a](docs/README.md) and [b](apps/board/README.md)\n",
        "",
    );
}

test "CRLF terminators survive a rewrite" {
    try expectFiltered("[a](@ref docs)\r\n", "[a](docs/README.md)\r\n", "");
}

test "a bracket that opens no link is copied through" {
    try expectFiltered("[unclosed and (text)\n", "[unclosed and (text)\n", "");
}

test "a link target holding whitespace is not a link" {
    try expectFiltered("[a](docs/READ ME.md)\n", "[a](docs/READ ME.md)\n", "");
}

test "an empty document filters to an empty document" {
    try expectFiltered("", "", "");
}

test "normalizeTarget collapses dot segments" {
    const normalized = try implementation.normalizeTarget(
        std.testing.allocator,
        "docs/guides",
        ".././../docs/README.md",
    );
    defer if (normalized) |value| std.testing.allocator.free(value);
    try std.testing.expectEqualStrings("docs/README.md", normalized.?);
}
