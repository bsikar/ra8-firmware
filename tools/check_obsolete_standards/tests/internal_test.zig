//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the obsolete-standards detector, scope
//! and report algebra (#858).
//!
//! These pin the PREDECESSOR's behaviour, not a tidier version of it: the
//! case-sensitive detector, Unicode word boundaries, `str.rstrip` and
//! `str.splitlines` semantics, the `.just` asymmetry between the tree-wide
//! suffix tuple and the per-file predicate, and both non-vacuity floors.

const std = @import("std");
const implementation = @import("implementation");

fn render(comptime shape: anytype, buffer: []u8) ![]const u8 {
    var stream = std.io.fixedBufferStream(buffer);
    try shape(stream.writer());
    return stream.getWritten();
}

// ---------------------------------------------------------------------------
// The contract literals
// ---------------------------------------------------------------------------

test "the tool names itself without the predecessor's .py suffix" {
    try std.testing.expectEqualStrings("check_obsolete_standards", implementation.tool);
}

test "both banned spellings are carried, in the predecessor's order" {
    try std.testing.expectEqual(@as(usize, 2), implementation.forbidden_patterns.len);
    try std.testing.expectEqualStrings("DO-178B", implementation.forbidden_patterns[0]);
    try std.testing.expectEqualStrings("DO178B", implementation.forbidden_patterns[1]);
}

test "the tree floor is 500 and the census floor is 1000" {
    try std.testing.expectEqual(@as(usize, 500), implementation.tree_floor);
    try std.testing.expectEqual(@as(usize, 1000), implementation.tracked_floor);
}

test "the whitelist still covers the files that document the ban" {
    for ([_][]const u8{ "CLAUDE.md", "PHILOSOPHIES.md", "docs/MCDC.md", "scripts/git/pre-commit" }) |rel| {
        try std.testing.expect(implementation.isWhitelisted(rel));
    }
}

test "the whitelist covers this gate's own sources, as it covered the predecessor's" {
    try std.testing.expect(implementation.isWhitelisted("tools/check_obsolete_standards/src/internal/root.zig"));
    try std.testing.expect(implementation.isWhitelisted("tools/check_obsolete_standards/src/cli.zig"));
}

test "an unrelated path is not whitelisted" {
    try std.testing.expect(!implementation.isWhitelisted("docs/ROADMAP.md"));
}

test "the tree-wide suffix tuple omits .just, exactly as the predecessor's did" {
    for (implementation.scan_suffixes) |suffix| {
        try std.testing.expect(!std.mem.eql(u8, suffix, ".just"));
    }
}

test "the per-file predicate's suffix set carries .just" {
    var found = false;
    for (implementation.scannable_suffixes) |suffix| {
        if (std.mem.eql(u8, suffix, ".just")) found = true;
    }
    try std.testing.expect(found);
}

test "the extensionless listfiles are named" {
    try std.testing.expectEqual(@as(usize, 3), implementation.scannable_names.len);
}

// ---------------------------------------------------------------------------
// Decoding, whitespace and line breaks
// ---------------------------------------------------------------------------

test "decodeAt reads a one-byte code point" {
    const decoded = implementation.decodeAt("x", 0);
    try std.testing.expectEqual(@as(u21, 'x'), decoded.code_point);
    try std.testing.expectEqual(@as(usize, 1), decoded.len);
}

test "decodeAt reads a multi-byte code point whole" {
    const decoded = implementation.decodeAt("\u{2028}", 0);
    try std.testing.expectEqual(@as(u21, 0x2028), decoded.code_point);
    try std.testing.expectEqual(@as(usize, 3), decoded.len);
}

test "decodeAt answers U+FFFD of width one for a malformed lead byte" {
    const decoded = implementation.decodeAt("\xff", 0);
    try std.testing.expectEqual(@as(u21, 0xFFFD), decoded.code_point);
    try std.testing.expectEqual(@as(usize, 1), decoded.len);
}

test "decodeAt answers U+FFFD for a truncated sequence rather than reading past the end" {
    const decoded = implementation.decodeAt("\xe2\x80", 0);
    try std.testing.expectEqual(@as(u21, 0xFFFD), decoded.code_point);
    try std.testing.expectEqual(@as(usize, 1), decoded.len);
}

test "isSpace holds for ASCII blank, tab and the Unicode spaces str.isspace accepts" {
    for ([_]u21{ ' ', '\t', '\n', '\r', 0x0B, 0x0C, 0x1C, 0x1D, 0x1E, 0x1F, 0x85, 0xA0, 0x2028, 0x3000 }) |code_point| {
        try std.testing.expect(implementation.isSpace(code_point));
    }
}

test "isSpace stays false for a letter and for U+200B, which str.isspace rejects" {
    try std.testing.expect(!implementation.isSpace('x'));
    try std.testing.expect(!implementation.isSpace(0x200B));
}

test "isWord holds for letters, digits and underscore" {
    for ([_]u21{ 'a', 'Z', '7', '_' }) |code_point| {
        try std.testing.expect(implementation.isWord(code_point));
    }
}

test "isWord holds for a non-ASCII letter and a non-ASCII digit" {
    try std.testing.expect(implementation.isWord(0xE9)); // e-acute
    try std.testing.expect(implementation.isWord(0x0661)); // Arabic-Indic one
}

test "isWord stays false for punctuation and for whitespace" {
    for ([_]u21{ '-', '.', ' ', 0x2014 }) |code_point| {
        try std.testing.expect(!implementation.isWord(code_point));
    }
}

test "pythonRstrip removes trailing blanks and leaves the indentation alone" {
    try std.testing.expectEqualStrings("  x", implementation.pythonRstrip("  x  \t"));
}

test "pythonRstrip removes a trailing non-ASCII space" {
    try std.testing.expectEqualStrings("x", implementation.pythonRstrip("x\u{00a0}\u{3000}"));
}

test "pythonRstrip leaves a zero-width space in place, as str.rstrip does" {
    try std.testing.expectEqualStrings("x\u{200b}", implementation.pythonRstrip("x\u{200b}"));
}

test "pythonRstrip answers empty for an all-whitespace line" {
    try std.testing.expectEqualStrings("", implementation.pythonRstrip("  \t "));
}

test "splitlines breaks on LF and drops no interior blank line" {
    var iterator = implementation.LineIterator{ .text = "a\n\nb" };
    try std.testing.expectEqualStrings("a", iterator.next().?);
    try std.testing.expectEqualStrings("", iterator.next().?);
    try std.testing.expectEqualStrings("b", iterator.next().?);
    try std.testing.expect(iterator.next() == null);
}

test "splitlines counts CRLF once" {
    var iterator = implementation.LineIterator{ .text = "a\r\nb" };
    try std.testing.expectEqualStrings("a", iterator.next().?);
    try std.testing.expectEqualStrings("b", iterator.next().?);
    try std.testing.expect(iterator.next() == null);
}

test "splitlines breaks on a lone CR" {
    var iterator = implementation.LineIterator{ .text = "a\rb" };
    try std.testing.expectEqualStrings("a", iterator.next().?);
    try std.testing.expectEqualStrings("b", iterator.next().?);
}

test "splitlines breaks on VT, FF and the file separators CPython honours" {
    var iterator = implementation.LineIterator{ .text = "a\x0bb\x0cc\x1cd\x1de\x1ef" };
    for ([_][]const u8{ "a", "b", "c", "d", "e", "f" }) |expected| {
        try std.testing.expectEqualStrings(expected, iterator.next().?);
    }
    try std.testing.expect(iterator.next() == null);
}

test "splitlines breaks on U+0085, U+2028 and U+2029" {
    var iterator = implementation.LineIterator{ .text = "a\u{0085}b\u{2028}c\u{2029}d" };
    for ([_][]const u8{ "a", "b", "c", "d" }) |expected| {
        try std.testing.expectEqualStrings(expected, iterator.next().?);
    }
}

test "splitlines does NOT break on U+001F, which is not a CPython line break" {
    var iterator = implementation.LineIterator{ .text = "a\x1fb" };
    try std.testing.expectEqualStrings("a\x1fb", iterator.next().?);
}

test "splitlines emits no trailing empty line for text ending in a break" {
    var iterator = implementation.LineIterator{ .text = "a\n" };
    try std.testing.expectEqualStrings("a", iterator.next().?);
    try std.testing.expect(iterator.next() == null);
}

// ---------------------------------------------------------------------------
// The detector and its word boundaries
// ---------------------------------------------------------------------------

test "the hyphenated spelling fires inside a comment" {
    try std.testing.expect(implementation.lineCitesObsolete("/* Written to DO-178B Level B. */"));
}

test "the hyphen-less spelling fires" {
    try std.testing.expect(implementation.lineCitesObsolete("# targets DO178B objectives"));
}

test "the current standard stays quiet" {
    try std.testing.expect(!implementation.lineCitesObsolete("/* Written to DO-178C Level B. */"));
}

test "an unrelated line naming no standard stays quiet" {
    try std.testing.expect(!implementation.lineCitesObsolete("int x = 178;"));
}

test "the detector is case-sensitive, as the predecessor's patterns were" {
    try std.testing.expect(!implementation.lineCitesObsolete("do-178b"));
    try std.testing.expect(!implementation.lineCitesObsolete("Do-178B"));
}

test "a trailing word character defeats the boundary" {
    try std.testing.expect(!implementation.lineCitesObsolete("DO-178Bx"));
    try std.testing.expect(!implementation.lineCitesObsolete("DO178B2"));
}

test "a trailing underscore defeats the boundary" {
    try std.testing.expect(!implementation.lineCitesObsolete("DO-178B_LEVEL"));
}

test "a leading word character defeats the boundary" {
    try std.testing.expect(!implementation.lineCitesObsolete("xDO-178B"));
    try std.testing.expect(!implementation.lineCitesObsolete("NOTDO178B"));
}

test "a hyphen before the token is not a word character, so it fires" {
    try std.testing.expect(implementation.lineCitesObsolete("pre-DO-178B"));
}

test "punctuation on both sides keeps the boundary" {
    try std.testing.expect(implementation.lineCitesObsolete("(DO-178B)"));
    try std.testing.expect(implementation.lineCitesObsolete("\"DO178B\","));
}

test "a non-ASCII letter neighbour defeats the boundary, because Python's \\b is Unicode-aware" {
    try std.testing.expect(!implementation.lineCitesObsolete("DO-178B\u{00e9}"));
    try std.testing.expect(!implementation.lineCitesObsolete("\u{00e9}DO178B"));
}

test "a non-ASCII punctuation neighbour keeps the boundary" {
    try std.testing.expect(implementation.lineCitesObsolete("\u{2014}DO-178B\u{2014}"));
}

test "the token at the very start and very end of a line fires" {
    try std.testing.expect(implementation.lineCitesObsolete("DO-178B"));
    try std.testing.expect(implementation.lineCitesObsolete("cites DO178B"));
}

test "patternFires answers per pattern, not for the pair" {
    try std.testing.expect(implementation.patternFires("DO178B", "DO178B"));
    try std.testing.expect(!implementation.patternFires("DO178B", "DO-178B"));
}

test "the detector survives a malformed UTF-8 neighbour without stalling" {
    try std.testing.expect(implementation.lineCitesObsolete("\xffDO-178B\xff"));
}

// ---------------------------------------------------------------------------
// scanText
// ---------------------------------------------------------------------------

test "scanText reports one finding per citing line with a 1-based line number" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "clean\ncites DO-178B\nclean\n");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 1), findings.len);
    try std.testing.expectEqual(@as(usize, 2), findings[0].lineno);
    try std.testing.expectEqualStrings("a.md", findings[0].path);
    try std.testing.expectEqualStrings("cites DO-178B", findings[0].line);
}

test "scanText records a line naming both spellings only once" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "DO-178B and DO178B");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 1), findings.len);
}

test "scanText right-strips the reported line" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "  DO-178B  \t\n");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqualStrings("  DO-178B", findings[0].line);
}

test "scanText reports every citing line, in order" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "DO178B\nx\nDO-178B\n");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 2), findings.len);
    try std.testing.expectEqual(@as(usize, 1), findings[0].lineno);
    try std.testing.expectEqual(@as(usize, 3), findings[1].lineno);
}

test "scanText counts a VT-separated citation on its own line" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "x\x0bDO-178B");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 1), findings.len);
    try std.testing.expectEqual(@as(usize, 2), findings[0].lineno);
}

test "scanText finds nothing in an empty file" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 0), findings.len);
}

test "scanText finds nothing in a clean file" {
    const findings = try implementation.scanText(std.testing.allocator, "a.md", "DO-178C\nIEC 61508 SIL 3\n");
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 0), findings.len);
}

// ---------------------------------------------------------------------------
// The per-file predicate
// ---------------------------------------------------------------------------

test "a source file with a scanned suffix is scannable" {
    for ([_][]const u8{ "a.c", "a.h", "a.cpp", "a.hpp", "a.md", "a.py", "a.sh", "a.cmake", "a.yml", "a.yaml", "a.txt" }) |rel| {
        try std.testing.expect(implementation.isScannable(rel));
    }
}

test "a .just file is scannable, though the tree-wide sweep never enumerates one" {
    try std.testing.expect(implementation.isScannable("just/ci.just"));
    try std.testing.expect(!implementation.hasScanSuffix("just/ci.just"));
}

test "the suffix match is case-insensitive" {
    try std.testing.expect(implementation.isScannable("README.MD"));
    try std.testing.expect(implementation.isScannable("a.YAML"));
}

test "the KELVIN SIGN folds to k, so a .cmaKe suffix cannot duck the scan" {
    try std.testing.expect(implementation.isScannable("a.cma\u{212a}e"));
}

test "the listfile names are scannable" {
    for ([_][]const u8{ "justfile", "Justfile", "CMakeLists.txt", "libs/x/CMakeLists.txt" }) |rel| {
        try std.testing.expect(implementation.isScannable(rel));
    }
}

test "a name merely ENDING in a listfile name is not one" {
    try std.testing.expect(!implementation.isScannable("my-justfile"));
}

test "a file with no suffix and no listfile name is not scannable" {
    try std.testing.expect(!implementation.isScannable("scripts/git/hook")); // PATHREF-OK: fixture path
}

test "a dotfile has no suffix, so it is not scannable" {
    try std.testing.expect(!implementation.isScannable(".bashrc"));
}

test "an unscanned suffix is not scannable" {
    for ([_][]const u8{ "a.zig", "a.rs", "a.png" }) |rel| {
        try std.testing.expect(!implementation.isScannable(rel));
    }
}

test "a whitelisted path is not scannable even though its suffix is scanned" {
    try std.testing.expect(!implementation.isScannable("CLAUDE.md"));
    try std.testing.expect(!implementation.isScannable("docs/MCDC.md"));
}

test "a third_party COMPONENT takes a path out of scope at any depth" {
    try std.testing.expect(!implementation.isScannable("libs/third_party/x.c"));
    try std.testing.expect(!implementation.isScannable("a/b/third_party/c/d.md"));
}

test "a directory merely CONTAINING the word third_party stays in scope" {
    try std.testing.expect(implementation.isScannable("docs/third_party_notes/x.md"));
    try std.testing.expect(!implementation.hasVendoredComponent("docs/third_party_notes/x.md"));
}

// ---------------------------------------------------------------------------
// Path helpers and the derived scope
// ---------------------------------------------------------------------------

test "pathName answers the final component" {
    try std.testing.expectEqualStrings("x.c", implementation.pathName("a/b/x.c"));
    try std.testing.expectEqualStrings("x.c", implementation.pathName("x.c"));
}

test "pathSuffix follows pathlib: no suffix for a dotfile or a trailing dot" {
    try std.testing.expectEqualStrings(".c", implementation.pathSuffix("x.c"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix(".bashrc"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("x."));
    try std.testing.expectEqualStrings(".gz", implementation.pathSuffix("x.tar.gz"));
}

test "isBuildDirName accepts the build tree spellings and rejects builders" {
    for ([_][]const u8{ "build", "build-arm", "build_host", "cmake-build-debug" }) |name| {
        try std.testing.expect(implementation.isBuildDirName(name));
    }
    try std.testing.expect(!implementation.isBuildDirName("builders"));
}

test "isBuildOutput fires on a top-level build tree and under a known root" {
    try std.testing.expect(implementation.isBuildOutput("build/x.c"));
    try std.testing.expect(implementation.isBuildOutput("tools/x/build/y.c"));
}

test "a build directory outside the known roots stays source" { // PATHREF-OK: fixture paths
    try std.testing.expect(!implementation.isBuildOutput("scripts/build/x.sh")); // PATHREF-OK: fixture path
}

test "isBuildOutput fires on a reserved tool directory at any depth" {
    try std.testing.expect(implementation.isBuildOutput("libs/x/__pycache__/y.py"));
    try std.testing.expect(implementation.isBuildOutput("a/b/CMakeFiles/c.cmake"));
}

test "a file merely NAMED build is not a build tree" {
    try std.testing.expect(!implementation.isBuildOutput("scripts/build")); // PATHREF-OK: fixture path
}

test "rawLanguage reads the listfiles by name and everything else by suffix" {
    try std.testing.expectEqualStrings("cmake", implementation.rawLanguage("a/CMakeLists.txt").?);
    try std.testing.expectEqualStrings("just", implementation.rawLanguage("justfile").?);
    try std.testing.expectEqualStrings("c", implementation.rawLanguage("a.c").?);
    try std.testing.expect(implementation.rawLanguage("a.md") == null);
}

test "the SOUP prefixes are excluded outright" {
    try std.testing.expect(implementation.isExcludedRel("libs/third_party/x.c", null));
    try std.testing.expect(implementation.isExcludedRel("tools/vela/generated/x.py", null));
}

test "port/threadx is excluded for C only" {
    try std.testing.expect(!implementation.isFirstParty("port/threadx/x.c"));
    try std.testing.expect(implementation.isFirstParty("port/threadx/x.md"));
}

test "hasScanSuffix is case-sensitive, as str.endswith is" {
    try std.testing.expect(implementation.hasScanSuffix("a.md"));
    try std.testing.expect(!implementation.hasScanSuffix("a.MD"));
}

test "isScanName matches only the exact listfile names" {
    try std.testing.expect(implementation.isScanName("a/justfile"));
    try std.testing.expect(!implementation.isScanName("a/my-justfile"));
}

test "derivedScope keeps first-party matches, sorted and deduplicated" {
    const census = [_][]const u8{ "b.md", "a.c", "a.c", "justfile", "x.zig" };
    const scope = try implementation.derivedScope(std.testing.allocator, &census);
    defer std.testing.allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 3), scope.len);
    try std.testing.expectEqualStrings("a.c", scope[0]);
    try std.testing.expectEqualStrings("b.md", scope[1]);
    try std.testing.expectEqualStrings("justfile", scope[2]);
}

test "derivedScope drops SOUP, generated tables and build output" {
    const census = [_][]const u8{ "libs/third_party/x.c", "tools/vela/generated/x.py", "build/x.c", "keep.md" };
    const scope = try implementation.derivedScope(std.testing.allocator, &census);
    defer std.testing.allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
    try std.testing.expectEqualStrings("keep.md", scope[0]);
}

test "derivedScope does NOT enumerate a .just file, preserving the predecessor's asymmetry" {
    const census = [_][]const u8{"just/ci.just"};
    const scope = try implementation.derivedScope(std.testing.allocator, &census);
    defer std.testing.allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 0), scope.len);
}

test "derivedScope keeps a whitelisted path: the whitelist is a per-file subtraction, not a scope one" {
    const census = [_][]const u8{"CLAUDE.md"};
    const scope = try implementation.derivedScope(std.testing.allocator, &census);
    defer std.testing.allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 1), scope.len);
}

test "scopeReaches sees a root it holds and not one it does not" {
    const scope = [_][]const u8{ "just/ci.just", "libs/a.c" };
    try std.testing.expect(implementation.scopeReaches(&scope, "just"));
    try std.testing.expect(!implementation.scopeReaches(&scope, "infra"));
}

test "scopeReaches needs the separator, so a prefix match is not a root" {
    const scope = [_][]const u8{"justfile"};
    try std.testing.expect(!implementation.scopeReaches(&scope, "just"));
}

test "sortPaths orders by byte value" {
    var paths = [_][]const u8{ "b", "A", "a" };
    implementation.sortPaths(&paths);
    try std.testing.expectEqualStrings("A", paths[0]);
    try std.testing.expectEqualStrings("a", paths[1]);
    try std.testing.expectEqualStrings("b", paths[2]);
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

test "the clean verdict names the tool and the file count" {
    var buffer: [128]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.renderClean(2601, stream.writer());
    try std.testing.expectEqualStrings(
        "check_obsolete_standards: 0 findings across 2601 file(s).\n",
        stream.getWritten(),
    );
}

test "the findings report opens with the four-line guidance and quotes each line" {
    var buffer: [512]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    const findings = [_]implementation.Finding{
        .{ .path = "docs/a.md", .lineno = 7, .line = "cites DO-178B" },
    };
    try implementation.renderFindings(&findings, stream.writer());
    const written = stream.getWritten();
    try std.testing.expect(std.mem.startsWith(u8, written, "[FAIL] Obsolete standard reference detected (DO-178B was\n"));
    try std.testing.expect(std.mem.endsWith(u8, written, "  docs/a.md:7: cites DO-178B\n"));
    try std.testing.expect(std.mem.indexOf(u8, written, "per CLAUDE.md. Offending lines:") != null);
}

test "the findings report keeps discovery order" {
    var buffer: [512]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    const findings = [_]implementation.Finding{
        .{ .path = "b.md", .lineno = 1, .line = "DO178B" },
        .{ .path = "a.md", .lineno = 9, .line = "DO-178B" },
    };
    try implementation.renderFindings(&findings, stream.writer());
    const written = stream.getWritten();
    const first = std.mem.indexOf(u8, written, "b.md:1").?;
    const second = std.mem.indexOf(u8, written, "a.md:9").?;
    try std.testing.expect(first < second);
}

test "the no-mode refusal explains why there is no default" {
    var buffer: [512]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.renderNoMode(stream.writer());
    const written = stream.getWritten();
    try std.testing.expect(std.mem.indexOf(u8, written, "pass --all") != null);
    try std.testing.expect(std.mem.indexOf(u8, written, "--staged (the pre-commit hook, the git index)") != null);
    try std.testing.expect(std.mem.indexOf(u8, written, "scanned nothing at") != null);
}

test "the collapsed-sweep refusal names the count and the floor" {
    var buffer: [512]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.renderFloorBreach(3, 500, stream.writer());
    const written = stream.getWritten();
    try std.testing.expect(std.mem.indexOf(u8, written, "enumerated only") != null);
    try std.testing.expect(std.mem.indexOf(u8, written, "3 file(s), below the floor of 500") != null);
}

test "the collapsed-census refusal names the census floor" {
    var buffer: [512]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buffer);
    try implementation.renderCensusCollapsed(12, 1000, stream.writer());
    const written = stream.getWritten();
    try std.testing.expect(std.mem.indexOf(u8, written, "only 12 tracked path(s), floor is 1000") != null);
}
