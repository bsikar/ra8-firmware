//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the GNU-attribute detector and its scope
//! rules (#858, #1178). Every expectation here was settled against the
//! Python predecessor under CPython before any Zig was written, including
//! the quirks that were deliberately NOT tidied.

const std = @import("std");
const implementation = @import("implementation");
const testing = std.testing;

fn findingCount(text: []const u8) !usize {
    var findings = try implementation.scanText(testing.allocator, text);
    defer findings.deinit();
    return findings.items.len;
}

fn firstSnippet(text: []const u8) ![]const u8 {
    var findings = try implementation.scanText(testing.allocator, text);
    defer findings.deinit();
    try testing.expect(findings.items.len >= 1);
    return findings.items[0].snippet;
}

// --- the detector, both directions ---------------------------------------

test "a migratable GNU attribute is a finding" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__((weak));\n"));
}

test "the C23 form is quiet" {
    try testing.expectEqual(@as(usize, 0), try findingCount("[[gnu::weak]] void f(void);\n"));
}

test "interrupt stays exempt" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void irq(void) __attribute__((interrupt));\n"));
}

test "the dunder spelling of an exempt attribute is exempt too" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void irq(void) __attribute__((__interrupt__));\n"));
}

test "both CMSE spellings are exempt" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void e(void) __attribute__((cmse_nonsecure_entry));\n"));
    try testing.expectEqual(@as(usize, 0), try findingCount("void c(void) __attribute__((cmse_nonsecure_call));\n"));
}

test "an exempt attribute with arguments is still exempt" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void f(void) __attribute__((__interrupt__(1)));\n"));
}

test "a list of exempt attributes is exempt" {
    try testing.expectEqual(
        @as(usize, 0),
        try findingCount("void f(void) __attribute__((interrupt,cmse_nonsecure_entry));\n"),
    );
}

test "one migratable attribute in a list of exempt ones is a finding" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__((interrupt, weak));\n"));
}

test "the dunder spelling of a migratable attribute is a finding" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__((__packed__));\n"));
}

test "a migratable attribute with arguments is a finding" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__((aligned(4)));\n"));
}

test "a bare dunder wrapper does not normalise away and is a finding" {
    // `____` is not LONGER than the wrapper, so _strip_us leaves it whole and
    // `____` is not an allowed name.
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__((____));\n"));
}

test "an empty body is a one-element set holding the empty name, so a finding" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__(( ));\n"));
}

test "unbalanced parens give no body, an empty set, and a finding" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__((weak\n"));
}

test "the token is case sensitive" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void f(void) __ATTRIBUTE__((weak));\n"));
}

test "a macro definition is scanned like any other line" {
    try testing.expectEqual(@as(usize, 1), try findingCount("#define X __attribute__((weak))\n"));
}

// --- the Unicode gap ------------------------------------------------------

test "an ASCII space before the parens still matches" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__ ((weak));\n"));
}

test "a tab before the parens still matches" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__\t((weak));\n"));
}

test "regex backslash-s is Unicode, so a no-break space still matches" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__\u{a0}((weak));\n"));
}

test "an ideographic space before the parens still matches" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void f(void) __attribute__\u{3000}((weak));\n"));
}

test "a name is trimmed with the str whitespace set, so padded exempt names stay quiet" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void f(void) __attribute__((\u{a0}interrupt\u{a0}));\n"));
}

test "a non-space character between token and parens breaks the match" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void f(void) __attribute__x((weak));\n"));
}

// --- comments and waivers -------------------------------------------------

test "a line-comment line is prose" {
    try testing.expectEqual(@as(usize, 0), try findingCount("// void prose(void) __attribute__((weak));\n"));
}

test "a doc-comment continuation line is prose" {
    try testing.expectEqual(@as(usize, 0), try findingCount(" * __attribute__((weak))\n"));
}

test "a block-comment line is prose" {
    try testing.expectEqual(@as(usize, 0), try findingCount("/* __attribute__((weak)) */\n"));
}

test "a trailing line comment before the attribute silences it" {
    try testing.expectEqual(@as(usize, 0), try findingCount("int x; // see __attribute__((weak))\n"));
}

test "a CLOSED block comment earlier on the line does NOT silence the attribute" {
    // The inherited check is crude on purpose: `*/` present in the prefix
    // means the column is no longer inside the comment.
    try testing.expectEqual(@as(usize, 1), try findingCount("x; /* c */ void f(void) __attribute__((weak));\n"));
}

test "an open block comment earlier on the line silences the attribute" {
    try testing.expectEqual(@as(usize, 0), try findingCount("x; /* c void f(void) __attribute__((weak));\n"));
}

test "a waiver with a reason silences the line" {
    try testing.expectEqual(
        @as(usize, 0),
        try findingCount("void g(void) __attribute__((packed)); /* ATTR-OK: wire ABI */\n"),
    );
}

test "a waiver whose reason is the comment terminator still counts" {
    // `\S` is satisfied by the `*` of `*/`, which is what the predecessor did.
    try testing.expectEqual(@as(usize, 0), try findingCount("void g(void) __attribute__((packed)); /* ATTR-OK: */\n"));
}

test "a waiver separated by a tab counts" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void g(void) __attribute__((packed)); // ATTR-OK:\tx\n"));
}

test "a waiver with no reason at end of line does not count" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void g(void) __attribute__((packed)); // ATTR-OK:\n"));
}

test "a waiver with only trailing whitespace does not count" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void g(void) __attribute__((packed)); // ATTR-OK:   \n"));
}

test "a second waiver occurrence can satisfy the search" {
    try testing.expectEqual(@as(usize, 0), try findingCount("void g(void) __attribute__((packed)); // ATTR-OK: ATTR-OK: x\n"));
}

test "a waiver before the attribute silences it too" {
    try testing.expectEqual(@as(usize, 0), try findingCount("/* ATTR-OK: legacy */ void g(void) __attribute__((packed));\n"));
}

test "the waiver token is case sensitive" {
    try testing.expectEqual(@as(usize, 1), try findingCount("void g(void) __attribute__((packed)); // attr-ok: no\n"));
}

// --- per-match counting ---------------------------------------------------

test "two reportable attributes on one line count twice" {
    try testing.expectEqual(
        @as(usize, 2),
        try findingCount("a __attribute__((weak)); b __attribute__((packed));\n"),
    );
}

test "a reportable attribute beside an exempt one counts once" {
    try testing.expectEqual(
        @as(usize, 1),
        try findingCount("void f(void) __attribute__((interrupt)) __attribute__((weak));\n"),
    );
}

test "matches are non-overlapping and left to right" {
    var iterator = implementation.AttrIterator.init("a __attribute__((x)) b __attribute__((y))");
    try testing.expectEqual(@as(?usize, 2), iterator.next());
    try testing.expectEqual(@as(?usize, 23), iterator.next());
    try testing.expectEqual(@as(?usize, null), iterator.next());
}

// --- lines ----------------------------------------------------------------

test "line numbers count from one" {
    var findings = try implementation.scanText(testing.allocator, "a\nb\nvoid f(void) __attribute__((weak));\n");
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 1), findings.items.len);
    try testing.expectEqual(@as(usize, 3), findings.items[0].line);
}

test "CRLF folds before splitting, so it does not shift line numbers" {
    const raw = "a\r\nb\r\nvoid f(void) __attribute__((weak));\r\n";
    const text = try implementation.normalizeTerminators(testing.allocator, raw);
    defer testing.allocator.free(text);
    var findings = try implementation.scanText(testing.allocator, text);
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 3), findings.items[0].line);
}

test "a lone CR folds to one break" {
    const text = try implementation.normalizeTerminators(testing.allocator, "a\rb\r");
    defer testing.allocator.free(text);
    try testing.expectEqualStrings("a\nb\n", text);
}

test "a form feed is a line break, as str.splitlines has it" {
    var findings = try implementation.scanText(testing.allocator, "a\x0cvoid f(void) __attribute__((weak));\n");
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 2), findings.items[0].line);
}

test "U+2028 and U+0085 are line breaks" {
    try testing.expectEqual(@as(usize, 3), implementation.lineBreakLen("\u{2028}", 0));
    try testing.expectEqual(@as(usize, 2), implementation.lineBreakLen("\u{85}", 0));
}

test "0x1f is whitespace but not a line break" {
    try testing.expectEqual(@as(usize, 0), implementation.lineBreakLen("\x1f", 0));
    try testing.expect(implementation.isReSpace(0x1f));
}

test "0x1c is a line break" {
    try testing.expectEqual(@as(usize, 1), implementation.lineBreakLen("\x1c", 0));
}

test "the two whitespace tables disagree only where CPython does" {
    // `\s` and `str.isspace()` agree across this pattern's inputs; both hold
    // 0x1c-0x1f, which is why 0x1f is space but not a break.
    try testing.expect(implementation.isStrSpace(0x1f));
    try testing.expect(implementation.isStrSpace(0xa0));
    try testing.expect(!implementation.isStrSpace('x'));
    try testing.expect(!implementation.isReSpace('x'));
}

// --- the fast path --------------------------------------------------------

test "a text with no attribute token at all is skipped" {
    try testing.expectEqual(@as(usize, 0), try findingCount("int x = 1;\n"));
}

// --- snippets -------------------------------------------------------------

test "the snippet is the trimmed line" {
    try testing.expectEqualStrings(
        "void f(void) __attribute__((weak));",
        try firstSnippet("    void f(void) __attribute__((weak));   \n"),
    );
}

test "the snippet truncates at 100 CODE POINTS, not bytes" {
    var text = std.ArrayList(u8).init(testing.allocator);
    defer text.deinit();
    for (0..120) |_| try text.appendSlice("\u{e9}");
    try text.appendSlice(" __attribute__((weak));\n");
    var findings = try implementation.scanText(testing.allocator, text.items);
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 1), findings.items.len);
    try testing.expectEqual(@as(usize, 100), try std.unicode.utf8CountCodepoints(findings.items[0].snippet));
    try testing.expectEqual(@as(usize, 200), findings.items[0].snippet.len);
}

// --- name normalisation ---------------------------------------------------

test "stripUnderscores normalises a dunder body" {
    try testing.expectEqualStrings("packed", implementation.stripUnderscores("__packed__"));
    try testing.expectEqualStrings("packed", implementation.stripUnderscores("packed"));
    try testing.expectEqualStrings("____", implementation.stripUnderscores("____"));
    try testing.expectEqualStrings("__x", implementation.stripUnderscores("__x"));
}

test "attributeName trims, cuts at the first paren, then trims again" {
    try testing.expectEqualStrings("aligned", implementation.attributeName("  aligned (4) "));
    try testing.expectEqualStrings("interrupt", implementation.attributeName("\u{a0}__interrupt__\u{a0}"));
    try testing.expectEqualStrings("", implementation.attributeName(" "));
}

test "bodyIsAllowed answers the subset test" {
    try testing.expect(implementation.bodyIsAllowed("interrupt"));
    try testing.expect(implementation.bodyIsAllowed("interrupt, cmse_nonsecure_call"));
    try testing.expect(!implementation.bodyIsAllowed("weak"));
    try testing.expect(!implementation.bodyIsAllowed(" "));
    try testing.expect(!implementation.bodyIsAllowed(null));
}

test "attrBody stops at the matching paren" {
    try testing.expectEqualStrings("aligned(4)", implementation.attrBody("f __attribute__((aligned(4)));", 2).?);
    try testing.expectEqual(@as(?[]const u8, null), implementation.attrBody("f __attribute__((weak", 2));
}

// --- scope ----------------------------------------------------------------

test "the scanned suffixes are a plain endswith test" {
    try testing.expect(implementation.hasScannedExt("a.c"));
    try testing.expect(implementation.hasScannedExt("a.hpp"));
    try testing.expect(implementation.hasScannedExt(".c"));
    try testing.expect(!implementation.hasScannedExt("a.C"));
    try testing.expect(!implementation.hasScannedExt("a.c.bak"));
    try testing.expect(!implementation.hasScannedExt("noext"));
}

test "vendored SOUP and generated fonts are exempt by substring" {
    try testing.expect(implementation.isExemptPath("libs/third_party/x/a.c"));
    try testing.expect(implementation.isExemptPath("libs/ra8_fonts/a.c"));
    try testing.expect(!implementation.isExemptPath("libs/ra8_net/third_party.c"));
    try testing.expect(!implementation.isExemptPath("libs/ra8_net/a.c"));
}

test "a build directory is build output only where a build tree is produced" {
    try testing.expect(implementation.isBuildOutput("tests/build/a.c"));
    try testing.expect(implementation.isBuildOutput("build/a.c"));
    try testing.expect(implementation.isBuildOutput("tools/x/cmake-build-debug/a.c"));
    try testing.expect(!implementation.isBuildOutput("scripts/build/a.c"));
    try testing.expect(!implementation.isBuildOutput("libs/builders/a.c"));
}

test "a FILE named build is not a build tree" {
    try testing.expect(!implementation.isBuildOutput("tests/build"));
}

test "tool output directory names match at any depth" {
    try testing.expect(implementation.isBuildOutput("scripts/x/__pycache__/a.c"));
    try testing.expect(implementation.isBuildOutput("libs/x/.zig-cache/a.c"));
    try testing.expect(implementation.isBuildOutput("libs/x/CMakeFiles/a.c"));
}

test "isBuildDirName requires the separator" {
    try testing.expect(implementation.isBuildDirName("build"));
    try testing.expect(implementation.isBuildDirName("build-cov"));
    try testing.expect(implementation.isBuildDirName("build_x"));
    try testing.expect(!implementation.isBuildDirName("builders"));
    try testing.expect(!implementation.isBuildDirName("rebuild"));
}

test "isBuildOutputPath normalises an absolute path against the repo root" {
    try testing.expect(try implementation.isBuildOutputPath(testing.allocator, "/repo/tests/build/a.c", "/repo"));
    try testing.expect(!try implementation.isBuildOutputPath(testing.allocator, "/repo/tests/a.c", "/repo"));
    try testing.expect(try implementation.isBuildOutputPath(testing.allocator, "./tests/build/a.c", "/repo"));
    try testing.expect(try implementation.isBuildOutputPath(testing.allocator, "tests\\build\\a.c", "/repo"));
}

test "sorting is by code point, which for UTF-8 is byte order" {
    var paths = [_][]const u8{ "libs/b.c", "libs/A.c", "libs/a.c" };
    std.mem.sort([]const u8, &paths, {}, implementation.pythonLessThan);
    try testing.expectEqualStrings("libs/A.c", paths[0]);
    try testing.expectEqualStrings("libs/a.c", paths[1]);
    try testing.expectEqualStrings("libs/b.c", paths[2]);
}

// --- constants ------------------------------------------------------------

test "the floor and the dunder length are the inherited values" {
    try testing.expectEqual(@as(usize, 1700), implementation.file_floor);
    try testing.expectEqual(@as(usize, 4), implementation.min_dunder_len);
}

test "the exempt attribute set is exactly three names" {
    try testing.expectEqual(@as(usize, 3), implementation.allowed.len);
}

test "the scanned roots are the inherited six" {
    try testing.expectEqual(@as(usize, 6), implementation.roots.len);
    try testing.expectEqualStrings("libs", implementation.roots[0]);
    try testing.expectEqualStrings("apps", implementation.roots[5]);
}

// --- diagnostics ----------------------------------------------------------

test "a finding renders the inherited one-line form" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    try implementation.renderFinding(out.writer(), "libs/a.c", 7, "void f(void) __attribute__((weak));");
    try testing.expectEqualStrings(
        "libs/a.c:7: GNU __attribute__ -- use the C23 [[...]] form (e.g. [[gnu::weak]]); " ++
            "void f(void) __attribute__((weak));\n",
        out.items,
    );
}

test "the summary names the count and the three exemptions" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    try implementation.renderSummary(out.writer(), 3);
    try testing.expect(std.mem.startsWith(u8, out.items, "\ncheck_no_gnu_attribute: 3 violation(s)."));
    try testing.expect(std.mem.indexOf(u8, out.items, "cmse_nonsecure_call") != null);
    try testing.expect(std.mem.indexOf(u8, out.items, "ATTR-OK: <reason>") != null);
}

test "the collapse line names the count and the floor" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    try implementation.renderCollapsed(out.writer(), 12);
    try testing.expect(std.mem.indexOf(u8, out.items, "only 12 first-party source file(s)") != null);
    try testing.expect(std.mem.indexOf(u8, out.items, "floor is 1700") != null);
}

test "the clean line is the inherited sentence" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    try implementation.renderClean(out.writer());
    try testing.expectEqualStrings(
        "check_no_gnu_attribute: clean -- all attributes use the C23 [[...]] form.\n",
        out.items,
    );
}

// --- selftest -------------------------------------------------------------

test "both selftest cases hold" {
    const cases = try implementation.selftestCases(testing.allocator);
    for (cases) |case| try testing.expect(case.passed);
}

test "the selftest fixtures are the predecessor's" {
    try testing.expectEqual(@as(usize, 1), try findingCount(implementation.selftest_bad));
    try testing.expectEqual(@as(usize, 0), try findingCount(implementation.selftest_good));
}

test "lineHasFinding agrees with the scan on one line" {
    try testing.expect(implementation.lineHasFinding("void f(void) __attribute__((weak));"));
    try testing.expect(!implementation.lineHasFinding("void f(void) __attribute__((interrupt));"));
}

test "strip and lstrip use the str whitespace set" {
    try testing.expectEqualStrings("x", implementation.strip("\u{a0} x \u{3000}"));
    try testing.expectEqualStrings("x ", implementation.lstrip("\u{a0} x "));
    try testing.expectEqualStrings("", implementation.strip("  "));
}

test "matchAttrAt answers the end of the match" {
    try testing.expectEqual(@as(?usize, 15), implementation.matchAttrAt("__attribute__((weak))", 0));
    try testing.expectEqual(@as(?usize, null), implementation.matchAttrAt("__attribute__(weak)", 0));
}
