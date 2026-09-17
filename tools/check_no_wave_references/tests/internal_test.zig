//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Regression tests for the session-reference gate's pure algebra (#858).
//!
//! The predecessor's contract is pinned here case by case: the detector in
//! both directions, the boundary behaviour that keeps `wave_table` quiet, the
//! Unicode classes CPython's `re` applied to a str pattern, the opt-out, the
//! derived scope, and the snippet trimming.

const std = @import("std");
const implementation = @import("implementation");

test "the tool names itself as the gate does" {
    try std.testing.expectEqualStrings("check_no_wave_references", implementation.tool);
}

test "the floors are the predecessor's, not rounded" {
    try std.testing.expectEqual(@as(usize, 2500), implementation.file_floor);
    try std.testing.expectEqual(@as(usize, 1000), implementation.tracked_floor);
}

test "the report limits are the predecessor's" {
    try std.testing.expectEqual(@as(usize, 50), implementation.max_findings_shown);
    try std.testing.expectEqual(@as(usize, 120), implementation.snippet_max_len);
    try std.testing.expectEqual(@as(usize, 117), implementation.snippet_trim_len);
}

test "scan_exts carries every suffix the predecessor scanned" {
    for ([_][]const u8{
        ".c",  ".h",    ".cpp",   ".hpp",  ".cc",
        ".md", ".yml",  ".sh",    ".py",   ".txt",
        ".mk", ".just", ".cmake", ".yaml",
    }) |suffix| {
        var seen = false;
        for (implementation.scan_exts) |candidate| {
            if (std.mem.eql(u8, candidate, suffix)) seen = true;
        }
        try std.testing.expect(seen);
    }
    try std.testing.expectEqual(@as(usize, 14), implementation.scan_exts.len);
}

test "zig sources are deliberately out of scope" {
    try std.testing.expect(!implementation.hasScanSuffix("tools/check_no_wave_references/src/cli.zig"));
}

test "the self-exempt list names the launcher, not the deleted predecessor" {
    try std.testing.expect(implementation.isSelfExempt("scripts/builders/check_no_wave_references.sh"));
    try std.testing.expect(implementation.isSelfExempt("scripts/fix/fix_wave_references.py"));
    try std.testing.expect(implementation.isSelfExempt("docs/STYLE_GUIDE.md"));
    try std.testing.expect(implementation.isSelfExempt("CLAUDE.md"));
    // PATHREF-OK: the deleted predecessor, named here to pin that its path is
    // no longer exempt.
    try std.testing.expect(!implementation.isSelfExempt("scripts/checks/check_no_wave_references.py"));  // PATHREF-OK: deleted predecessor
    try std.testing.expect(!implementation.isSelfExempt("docs/ROADMAP.md"));
}

test "decodeAt reads one, two, three and four byte sequences" {
    try std.testing.expectEqual(@as(u21, 'a'), implementation.decodeAt("a", 0).code_point);
    try std.testing.expectEqual(@as(usize, 1), implementation.decodeAt("a", 0).width);
    try std.testing.expectEqual(@as(u21, 0x00A0), implementation.decodeAt("\u{00A0}", 0).code_point);
    try std.testing.expectEqual(@as(usize, 2), implementation.decodeAt("\u{00A0}", 0).width);
    try std.testing.expectEqual(@as(u21, 0x3000), implementation.decodeAt("\u{3000}", 0).code_point);
    try std.testing.expectEqual(@as(usize, 3), implementation.decodeAt("\u{3000}", 0).width);
    try std.testing.expectEqual(@as(u21, 0x1D7CE), implementation.decodeAt("\u{1D7CE}", 0).code_point);
    try std.testing.expectEqual(@as(usize, 4), implementation.decodeAt("\u{1D7CE}", 0).width);
}

test "decodeAt treats a malformed lead byte as one byte" {
    const bytes = [_]u8{ 0xFF, 'a' };
    try std.testing.expectEqual(@as(u21, 0xFF), implementation.decodeAt(&bytes, 0).code_point);
    try std.testing.expectEqual(@as(usize, 1), implementation.decodeAt(&bytes, 0).width);
}

test "decodeAt treats a truncated sequence as one byte" {
    const bytes = [_]u8{0xE3};
    try std.testing.expectEqual(@as(usize, 1), implementation.decodeAt(&bytes, 0).width);
}

test "isSpace accepts ASCII whitespace" {
    for ([_]u21{ ' ', '\t', '\n', '\r', 0x0B, 0x0C }) |code_point| {
        try std.testing.expect(implementation.isSpace(code_point));
    }
}

test "isSpace accepts the non-ASCII whitespace CPython does" {
    for ([_]u21{ 0x00A0, 0x1680, 0x2000, 0x2028, 0x3000 }) |code_point| {
        try std.testing.expect(implementation.isSpace(code_point));
    }
    try std.testing.expect(!implementation.isSpace('x'));
    try std.testing.expect(!implementation.isSpace(0x200B));
}

test "isWord is unicode alphanumeric plus underscore" {
    try std.testing.expect(implementation.isWord('a'));
    try std.testing.expect(implementation.isWord('Z'));
    try std.testing.expect(implementation.isWord('7'));
    try std.testing.expect(implementation.isWord('_'));
    try std.testing.expect(implementation.isWord(0x00E9));
    try std.testing.expect(!implementation.isWord('-'));
    try std.testing.expect(!implementation.isWord(' '));
    try std.testing.expect(!implementation.isWord('.'));
}

test "isDigit is the unicode decimal set, not [0-9]" {
    try std.testing.expect(implementation.isDigit('0'));
    try std.testing.expect(implementation.isDigit('9'));
    try std.testing.expect(implementation.isDigit(0x0664));
    try std.testing.expect(implementation.isDigit(0x1D7CE));
    try std.testing.expect(!implementation.isDigit('a'));
    try std.testing.expect(!implementation.isDigit(0x00BD));
}

test "isAsciiLetter stays ASCII, as the bracketed range is" {
    try std.testing.expect(implementation.isAsciiLetter('b'));
    try std.testing.expect(implementation.isAsciiLetter('B'));
    try std.testing.expect(!implementation.isAsciiLetter('7'));
    try std.testing.expect(!implementation.isAsciiLetter(0x00E9));
}

test "isWordBoundary answers at both ends of the text" {
    try std.testing.expect(implementation.isWordBoundary("wave", 0));
    try std.testing.expect(implementation.isWordBoundary("wave", 4));
    try std.testing.expect(!implementation.isWordBoundary("wave", 2));
    try std.testing.expect(!implementation.isWordBoundary("", 0));
}

test "isWordBoundary is false between two word characters across a multi-byte code point" {
    try std.testing.expect(!implementation.isWordBoundary("a\u{00E9}b", 1));
}

test "the detector fires on the predecessor's two positive cases" {
    try std.testing.expect(implementation.firesWave("fixed in Wave 70"));
    try std.testing.expect(implementation.firesWave("see wave-43b for context"));
}

test "the detector stays quiet on the predecessor's three negative cases" {
    try std.testing.expect(!implementation.firesWave("the sine wave is smooth"));
    try std.testing.expect(!implementation.firesWave("k_ra8_pdg_wave_saw selects the waveform"));
    try std.testing.expect(!implementation.firesWave("wave_table[0] holds the sample"));
}

test "the detector accepts every separator the class allows" {
    try std.testing.expect(implementation.firesWave("Wave 12"));
    try std.testing.expect(implementation.firesWave("Wave-12"));
    try std.testing.expect(implementation.firesWave("Wave_12"));
    try std.testing.expect(implementation.firesWave("Wave12"));
    try std.testing.expect(implementation.firesWave("Wave\t12"));
    try std.testing.expect(implementation.firesWave("Wave\u{00A0}12"));
}

test "the separator is at most one code point" {
    try std.testing.expect(!implementation.firesWave("Wave  12"));
    try std.testing.expect(!implementation.firesWave("Wave --12"));
}

test "the detector is case-insensitive only on the leading letter" {
    try std.testing.expect(implementation.firesWave("wave 7"));
    try std.testing.expect(implementation.firesWave("Wave 7"));
    try std.testing.expect(!implementation.firesWave("WAVE 7"));
    try std.testing.expect(!implementation.firesWave("wAve 7"));
}

test "the detector wraps in punctuation" {
    try std.testing.expect(implementation.firesWave("(Wave 12)"));
    try std.testing.expect(implementation.firesWave("[wave-7]"));
    try std.testing.expect(implementation.firesWave("see <wave 3>."));
}

test "a trailing underscore defeats the closing boundary" {
    try std.testing.expect(!implementation.firesWave("wave12_"));
    try std.testing.expect(!implementation.firesWave("wave 12_ok"));
}

test "a trailing letter is accepted once, not twice" {
    try std.testing.expect(implementation.firesWave("wave 43b"));
    try std.testing.expect(!implementation.firesWave("wave 43bc"));
}

test "a leading word character defeats the opening boundary" {
    try std.testing.expect(!implementation.firesWave("microwave 7"));
    try std.testing.expect(!implementation.firesWave("_wave 7"));
    try std.testing.expect(!implementation.firesWave("x_wave_43b"));
}

test "a hyphen before the token is not a word character, so the gate fires" {
    try std.testing.expect(implementation.firesWave("pre-wave 7"));
}

test "the detector needs at least one digit" {
    try std.testing.expect(!implementation.firesWave("wave"));
    try std.testing.expect(!implementation.firesWave("wave -"));
    try std.testing.expect(!implementation.firesWave("wave b"));
}

test "the detector fires on a non-ASCII decimal digit" {
    try std.testing.expect(implementation.firesWave("wave \u{0664}\u{0663}"));
}

test "the detector finds a reference anywhere in the line" {
    try std.testing.expect(implementation.firesWave("// the FRDY latch was fixed in Wave 70, see below"));
    try std.testing.expect(implementation.firesWave("wave 3"));
}

test "the detector accepts a long digit run" {
    try std.testing.expect(implementation.firesWave("wave 1234567890"));
}

test "the opt-out suppresses on the same line" {
    try std.testing.expect(implementation.hasOptOut("wave 12  WAVE-OK: upstream register name"));
    try std.testing.expect(implementation.hasOptOut("WAVE-OK:"));
    try std.testing.expect(implementation.hasOptOut("WAVE-OK   :"));
    try std.testing.expect(implementation.hasOptOut("WAVE-OK\t:"));
}

test "the opt-out needs its colon" {
    try std.testing.expect(!implementation.hasOptOut("WAVE-OK"));
    try std.testing.expect(!implementation.hasOptOut("WAVE-OK reason"));
    try std.testing.expect(!implementation.hasOptOut("wave-ok: lowercase is not the marker"));
}

test "a second opt-out candidate on the line still counts" {
    try std.testing.expect(implementation.hasOptOut("WAVE-OK no colon here, but WAVE-OK: this one has"));
}

test "pythonRstrip drops unicode trailing whitespace" {
    try std.testing.expectEqualStrings("x", implementation.pythonRstrip("x   "));
    try std.testing.expectEqualStrings("x", implementation.pythonRstrip("x\t\n"));
    try std.testing.expectEqualStrings("x", implementation.pythonRstrip("x\u{00A0}"));
    try std.testing.expectEqualStrings("  x", implementation.pythonRstrip("  x  "));
    try std.testing.expectEqualStrings("", implementation.pythonRstrip("   "));
    try std.testing.expectEqualStrings("", implementation.pythonRstrip(""));
}

test "splitLines breaks on every CPython terminator" {
    const cases = [_]struct { text: []const u8, count: usize }{
        .{ .text = "a\nb", .count = 2 },
        .{ .text = "a\r\nb", .count = 2 },
        .{ .text = "a\rb", .count = 2 },
        .{ .text = "a\u{000B}b", .count = 2 },
        .{ .text = "a\u{000C}b", .count = 2 },
        .{ .text = "a\u{001C}b", .count = 2 },
        .{ .text = "a\u{001D}b", .count = 2 },
        .{ .text = "a\u{001E}b", .count = 2 },
        .{ .text = "a\u{0085}b", .count = 2 },
        .{ .text = "a\u{2028}b", .count = 2 },
        .{ .text = "a\u{2029}b", .count = 2 },
        .{ .text = "a\u{001F}b", .count = 1 },
    };
    for (cases) |item| {
        var lines = implementation.splitLines(item.text);
        var seen: usize = 0;
        while (lines.next()) |_| seen += 1;
        try std.testing.expectEqual(item.count, seen);
    }
}

test "splitLines adds no trailing empty line and keeps interior blanks" {
    var lines = implementation.splitLines("a\n\nb\n");
    try std.testing.expectEqualStrings("a", lines.next().?);
    try std.testing.expectEqualStrings("", lines.next().?);
    try std.testing.expectEqualStrings("b", lines.next().?);
    try std.testing.expect(lines.next() == null);
}

test "splitLines on empty text yields nothing" {
    var lines = implementation.splitLines("");
    try std.testing.expect(lines.next() == null);
}

test "pathName is the final component" {
    try std.testing.expectEqualStrings("b.c", implementation.pathName("a/b.c"));
    try std.testing.expectEqualStrings("justfile", implementation.pathName("justfile"));
}

test "pathSuffix follows pathlib" {
    try std.testing.expectEqualStrings(".py", implementation.pathSuffix("a.py"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix(".bashrc"));
    try std.testing.expectEqualStrings("", implementation.pathSuffix("trailing."));
    try std.testing.expectEqualStrings(".txt", implementation.pathSuffix("CMakeLists.txt"));
}

test "isBuildDirName needs a real build directory name" {
    try std.testing.expect(implementation.isBuildDirName("build"));
    try std.testing.expect(implementation.isBuildDirName("build-arm"));
    try std.testing.expect(implementation.isBuildDirName("build_host"));
    try std.testing.expect(implementation.isBuildDirName("cmake-build-debug"));
    try std.testing.expect(!implementation.isBuildDirName("builders"));
}

test "isBuildOutput matches directory components only" {
    try std.testing.expect(implementation.isBuildOutput("build/x.c"));
    try std.testing.expect(implementation.isBuildOutput("tools/build/x.c"));
    try std.testing.expect(implementation.isBuildOutput("a/__pycache__/x.py"));
    try std.testing.expect(!implementation.isBuildOutput("scripts/build.sh"));  // PATHREF-OK: fixture path
    try std.testing.expect(!implementation.isBuildOutput("scripts/builders/x.sh"));  // PATHREF-OK: fixture path
    try std.testing.expect(!implementation.isBuildOutput("scripts/build/x.sh"));  // PATHREF-OK: fixture path
}

test "rawLanguage names the language a path implies" {
    try std.testing.expectEqualStrings("c", implementation.rawLanguage("a/b.c").?);
    try std.testing.expectEqualStrings("python", implementation.rawLanguage("a/b.py").?);
    try std.testing.expectEqualStrings("cmake", implementation.rawLanguage("a/CMakeLists.txt").?);
    try std.testing.expectEqualStrings("just", implementation.rawLanguage("justfile").?);
    try std.testing.expect(implementation.rawLanguage("a/README") == null);
}

test "isFirstParty drops SOUP, generated tables and build output" {
    try std.testing.expect(!implementation.isFirstParty("libs/third_party/x.c"));
    try std.testing.expect(!implementation.isFirstParty("apps/shared_libs/third_party/x.h"));
    try std.testing.expect(!implementation.isFirstParty("libs/ra8_fonts/x.c"));
    try std.testing.expect(!implementation.isFirstParty("tools/vela/generated/x.py"));
    try std.testing.expect(!implementation.isFirstParty("port/threadx/tx.c"));
    try std.testing.expect(!implementation.isFirstParty("build/x.c"));
    try std.testing.expect(implementation.isFirstParty("libs/ra8_ui/x.c"));
}

test "a threadx path that is not C survives the language exclusion" {
    try std.testing.expect(implementation.isFirstParty("port/threadx/README.md"));
}

test "isDocsVendored matches the three docs-side components at any depth" {
    try std.testing.expect(implementation.isDocsVendored("docs/reference/hum.md"));
    try std.testing.expect(implementation.isDocsVendored("docs/api/doxygen/index.md"));
    try std.testing.expect(implementation.isDocsVendored("docs/html/x.md"));
    try std.testing.expect(!implementation.isDocsVendored("docs/ROADMAP.md"));
    try std.testing.expect(!implementation.isDocsVendored("docs/references.md"));
}

test "hasScanSuffix and isScanBasename split the two halves of the scope" {
    try std.testing.expect(implementation.hasScanSuffix("a/b.md"));
    try std.testing.expect(!implementation.hasScanSuffix("a/justfile"));
    try std.testing.expect(implementation.isScanBasename("a/justfile"));
    try std.testing.expect(implementation.isScanBasename("Justfile"));
    try std.testing.expect(implementation.isScanBasename("infra/Dockerfile"));
    try std.testing.expect(!implementation.isScanBasename("a/my-justfile"));
}

test "derivedScope keeps the union of both halves, sorted and deduplicated" {
    const census = [_][]const u8{
        "src/b.c",
        "docs/a.md",
        "justfile",
        "src/b.c",
        "notes.rst",
        "libs/third_party/x.c",
        "docs/reference/hum.md",
        "build/gen.c",
    };
    const scope = try implementation.derivedScope(std.testing.allocator, &census);
    defer std.testing.allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 3), scope.len);
    try std.testing.expectEqualStrings("docs/a.md", scope[0]);
    try std.testing.expectEqualStrings("justfile", scope[1]);
    try std.testing.expectEqualStrings("src/b.c", scope[2]);
}

test "derivedScope on an empty census is empty, not an error" {
    const scope = try implementation.derivedScope(std.testing.allocator, &[_][]const u8{});
    defer std.testing.allocator.free(scope);
    try std.testing.expectEqual(@as(usize, 0), scope.len);
}

test "scopeReaches needs the separator, not just the prefix" {
    const scope = [_][]const u8{ "just/ci.just", "infrastructure/x.md" };
    try std.testing.expect(implementation.scopeReaches(&scope, "just"));
    try std.testing.expect(!implementation.scopeReaches(&scope, "infra"));
}

test "scanText numbers lines from one and reports the rstripped line" {
    const findings = try implementation.scanText(
        std.testing.allocator,
        "a.md",
        "clean\nfixed in Wave 70   \nalso clean\n",
    );
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 1), findings.len);
    try std.testing.expectEqualStrings("a.md", findings[0].path);
    try std.testing.expectEqual(@as(usize, 2), findings[0].line);
    try std.testing.expectEqualStrings("fixed in Wave 70", findings[0].snippet);
}

test "scanText honours the per-line opt-out" {
    const findings = try implementation.scanText(
        std.testing.allocator,
        "a.md",
        "see wave 12  WAVE-OK: upstream name\n",
    );
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 0), findings.len);
}

test "scanText reports every offending line, in file order" {
    const findings = try implementation.scanText(
        std.testing.allocator,
        "a.md",
        "wave 1\nquiet\nwave 2\n",
    );
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 2), findings.len);
    try std.testing.expectEqual(@as(usize, 1), findings[0].line);
    try std.testing.expectEqual(@as(usize, 3), findings[1].line);
}

test "scanText on a clean file reports nothing" {
    const findings = try implementation.scanText(
        std.testing.allocator,
        "a.md",
        "the sine wave is smooth\nwave_table[0]\n",
    );
    defer std.testing.allocator.free(findings);
    try std.testing.expectEqual(@as(usize, 0), findings.len);
}

test "renderSnippet leaves a short line alone" {
    const rendered = try implementation.renderSnippet(std.testing.allocator, "short");
    defer std.testing.allocator.free(rendered);
    try std.testing.expectEqualStrings("short", rendered);
}

test "renderSnippet leaves a line of exactly the ceiling alone" {
    const line = "x" ** 120;
    const rendered = try implementation.renderSnippet(std.testing.allocator, line);
    defer std.testing.allocator.free(rendered);
    try std.testing.expectEqual(@as(usize, 120), rendered.len);
}

test "renderSnippet trims one character past the ceiling" {
    const line = "x" ** 121;
    const rendered = try implementation.renderSnippet(std.testing.allocator, line);
    defer std.testing.allocator.free(rendered);
    try std.testing.expectEqual(@as(usize, 120), rendered.len);
    try std.testing.expectEqualStrings("...", rendered[117..]);
}

test "renderSnippet counts characters, not bytes" {
    const line = "\u{00E9}" ** 121;
    const rendered = try implementation.renderSnippet(std.testing.allocator, line);
    defer std.testing.allocator.free(rendered);
    // 117 two-byte characters plus the three-byte ellipsis marker.
    try std.testing.expectEqual(@as(usize, 117 * 2 + 3), rendered.len);
}

test "renderSnippet on an empty line is empty" {
    const rendered = try implementation.renderSnippet(std.testing.allocator, "");
    defer std.testing.allocator.free(rendered);
    try std.testing.expectEqualStrings("", rendered);
}
