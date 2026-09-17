//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural tests for the legacy-task-runner detector (#858).
//!
//! Every expectation here was taken from the predecessor by running it, not
//! from reading its regular expressions: the command forms that fire, the
//! legitimate mentions that stay quiet, the exact text each pattern captures,
//! and the Unicode edges (`\b` awareness, the three case-insensitive folds,
//! the `str.splitlines` break set) where the two implementations could
//! plausibly disagree.

const std = @import("std");
const testing = std.testing;
const implementation = @import("implementation");

fn invocationOf(line: []const u8, active: bool) ?implementation.Invocation {
    return implementation.legacyInvocation(line, active);
}

fn rendered(allocator: std.mem.Allocator, line: []const u8, active: bool) !?[]u8 {
    const found = invocationOf(line, active) orelse return null;
    return try implementation.renderInvocation(allocator, found);
}

fn expectRender(line: []const u8, active: bool, want: []const u8) !void {
    const got = try rendered(testing.allocator, line, active) orelse {
        std.debug.print("expected a finding for {s}\n", .{line});
        return error.TestExpectedFinding;
    };
    defer testing.allocator.free(got);
    try testing.expectEqualStrings(want, got);
}

fn expectQuiet(line: []const u8, active: bool) !void {
    const got = try rendered(testing.allocator, line, active);
    if (got) |text| {
        defer testing.allocator.free(text);
        std.debug.print("expected silence for {s}, got {s}\n", .{ line, text });
        return error.TestExpectedSilence;
    }
}

test "a direct shell task fires and carries its first argument" {
    try expectRender("make ci", true, "make ci");
}

test "the legacy -C entry point fires" {
    try expectRender("make -C apps/board/stand_alone/blink build", true, "make -C");
}

test "the GNU spelling fires" {
    try expectRender("gmake ci", true, "gmake ci");
}

test "a bare command with no argument fires" {
    try expectRender("make", true, "make");
}

test "an executable with a trailing suffix does not fire" {
    try expectQuiet("makex ci", true);
    try expectQuiet("gmakex ci", true);
}

test "leading whitespace does not hide an active command" {
    try expectRender("   make ci", true, "make ci");
    try expectRender("\tmake ci", true, "make ci");
}

test "an argument opening a shell operator is not captured" {
    try expectRender("make #comment", true, "make");
    try expectRender("make ;next", true, "make");
    try expectRender("make &background", true, "make");
    try expectRender("make |pipe", true, "make");
}

test "an operator inside an argument truncates the capture" {
    try expectRender("make a#b", true, "make a");
}

test "a Dockerfile RUN and a one-line YAML run both fire" {
    try expectRender("RUN make coverage", true, "make coverage");
    try expectRender("run: make -C apps/blink", true, "make -C");
}

test "the active prefix spellings are exact" {
    try expectQuiet("Run: make ci", true);
    try expectQuiet("RUN: make ci", true);
}

test "a quoted executable fires with its quotes stripped" {
    try expectRender("\"make\" -C apps/blink", true, "make -C");
    try expectRender("'make' ci", true, "make ci");
    try expectRender("\"gmake\"", true, "gmake");
}

test "a command array fires, quoted or bare" {
    try expectRender("cmd=(make -C apps/blink)", true, "make -C");
    try expectRender("cmd=(\"make\" \"-C\" apps/blink)", true, "make \"-C\"");
    try expectRender("cmd=(make)", true, "make");
    try expectRender("_c9=(gmake target)", true, "gmake target");
}

test "an array assignment tolerates space before the parenthesis only" {
    try expectRender("cmd =(make x)", true, "make x");
    try expectQuiet("cmd= (make x)", true);
    try expectQuiet("9cmd=(make x)", true);
}

test "active command forms are invisible to documentation" {
    try expectQuiet("make ci", false);
    try expectQuiet("cmd=(make -C apps/blink)", false);
}

test "a Dockerfile RUN still fires in prose, as a guidance verb" {
    // Inherited overlap, not an accident: the guidance pattern is
    // case-insensitive, so `RUN` is also the verb `run`. The predecessor
    // reported this line on a documentation surface too.
    try expectRender("RUN make coverage", false, "make coverage");
}

test "a bare comment hint fires and keeps its trailing punctuation" {
    try expectRender("# make ci-native", false, "make ci-native");
    try expectRender("#make ci", false, "make ci");
    try expectRender("#   make ci  ", false, "make ci");
    try expectRender("# make ci.", false, "make ci.");
}

test "a comment with a second word stays quiet" {
    try expectQuiet("# make a b", false);
    try expectQuiet("# make", false);
}

test "a comment argument may be followed by separated punctuation" {
    try expectRender("# make ci .", false, "make ci");
}

test "a backticked hint fires" {
    try expectRender("# `make sbom` regenerates it", false, "make sbom");
}

test "a quoted task-runner reference fires without an argument" {
    // The prose noun is not captured: the pattern's only argument group
    // belongs to the other alternative, so the finding names the executable
    // alone.
    try expectRender("CI (or a local ``make`` target) catches drift", false, "make");
    try expectRender("'make' recipe", false, "make");
    try expectRender("\"make\" task", false, "make");
    try expectRender("`gmake` task", false, "gmake");
}

test "on an executing surface the same line is read as a quoted command" {
    try expectRender("'make' recipe", true, "make recipe");
    try expectRender("\"make\" task", true, "make task");
}

test "the prose noun after a quoted command needs a word boundary" {
    try expectQuiet("`make` targeted", false);
    try expectQuiet("`make` target_x", false);
    try expectQuiet("`make` target\u{e9}", false);
    try expectRender("`make` target.", false, "make");
}

test "the quoted form is case sensitive" {
    try expectQuiet("`MAKE sbom`", false);
}

test "mixed closing quotes still reach the prose noun" {
    try expectRender("`make'` target", false, "make");
}

test "an unquoted user hint fires for every verb" {
    try expectRender("Please run make misra", false, "make misra");
    try expectRender("use make ci", false, "make ci");
    try expectRender("invoke make ci", false, "make ci");
    try expectRender("try make ci", false, "make ci");
    try expectRender("rerun make ci", false, "make ci");
    try expectRender("execute make ci", false, "make ci");
}

test "guidance is case insensitive and keeps the source spelling" {
    try expectRender("RUN MAKE CI", false, "MAKE CI");
}

test "guidance folds the three code points re.IGNORECASE folds" {
    try expectRender("u\u{17f}e make ci", false, "make ci");
    try expectRender("invo\u{212a}e make ci", false, "make ci");
    try expectRender("run ma\u{212a}e ci", false, "ma\u{212a}e ci");
}

test "the verb boundary is Unicode aware" {
    try expectQuiet("\u{e9}run make ci", false);
    try expectQuiet("_run make ci", false);
}

test "guidance needs an argument" {
    try expectQuiet("run make", false);
    try expectQuiet("run make `quoted`", false);
}

test "legitimate mentions stay quiet" {
    try expectQuiet("command -v make || missing=build-essential", true);
    try expectQuiet("command -v gmake || missing=build-essential", true);
    try expectQuiet("for tool in curl cmake make tar cc; do", true);
    try expectQuiet("these controls make an empty scan fail", true);
    try expectQuiet("# make the detector fail", true);
    try expectQuiet("CMakeLists.txt and GNUmakefile", true);
    try expectQuiet("# Make is required by an upstream source build", true);
    try expectQuiet("x make ci", true);
    try expectQuiet("echo make ci", true);
}

test "the detector self-test agrees with its own contract" {
    const failures = try implementation.selftestFailures(testing.allocator);
    defer testing.allocator.free(failures);
    try testing.expectEqual(@as(usize, 0), failures.len);
    try testing.expectEqual(@as(usize, 19), implementation.selftest_cases.len);
}

test "the executable name is never spelled adjacent to a quote in this source" {
    try testing.expectEqualStrings("make", implementation.command_word);
    try testing.expectEqualStrings("gmake", implementation.gnu_command_word);
}

test "quote stripping matches str.strip" {
    try testing.expectEqualStrings("make", implementation.stripQuotes("\"make\""));
    try testing.expectEqualStrings("make", implementation.stripQuotes("'make'"));
    try testing.expectEqualStrings("make", implementation.stripQuotes("make"));
    try testing.expectEqualStrings("", implementation.stripQuotes("'\""));
}

test "path names and pathlib suffix semantics" {
    try testing.expectEqualStrings("b.md", implementation.pathName("a/b.md"));
    try testing.expectEqualStrings("justfile", implementation.pathName("justfile"));
    try testing.expectEqualStrings(".md", implementation.pathSuffix("docs/a.md"));
    try testing.expectEqualStrings("", implementation.pathSuffix("scripts/.bashrc"));
    try testing.expectEqualStrings("", implementation.pathSuffix("docs/trailing."));
    try testing.expectEqualStrings(".rst", implementation.pathSuffix("docs/a.b.rst"));
}

test "documentation suffixes are compared case folded" {
    try testing.expect(implementation.isDocSuffix(".md"));
    try testing.expect(implementation.isDocSuffix(".MD"));
    try testing.expect(implementation.isDocSuffix(".mdx"));
    try testing.expect(implementation.isDocSuffix(".rst"));
    try testing.expect(!implementation.isDocSuffix(".markdown"));
    try testing.expect(!implementation.isDocSuffix(""));
}

test "the baseline pattern is anchored to one .github component" {
    try testing.expect(implementation.matchesBaseline(".github/misra-baseline.txt"));
    try testing.expect(implementation.matchesBaseline(".github/baseline.txt"));
    try testing.expect(!implementation.matchesBaseline(".github/nested/misra-baseline.txt"));
    try testing.expect(!implementation.matchesBaseline(".github/baseline.md"));
    try testing.expect(!implementation.matchesBaseline("docs/baseline.txt"));
}

test "selection covers exact files, authored trees, docs and Dockerfiles" {
    try testing.expect(implementation.isSelected("justfile"));
    try testing.expect(implementation.isSelected(".clangd"));
    try testing.expect(implementation.isSelected("scripts/ci/gates/checks.sh"));
    try testing.expect(implementation.isSelected("tools/mcp/server.py"));
    try testing.expect(implementation.isSelected("docs/DOCS.md"));
    try testing.expect(implementation.isSelected(".devcontainer/Dockerfile"));
    try testing.expect(implementation.isSelected("ci/Dockerfile"));
    try testing.expect(!implementation.isSelected("libs/ra8_ui/src/ui.c"));
    try testing.expect(!implementation.isSelected("tools/vela/main.zig"));
}

test "vendored and fixture trees are excluded" {
    try testing.expect(implementation.isExcluded("libs/third_party/x/README.md"));
    try testing.expect(implementation.isExcluded("port/threadx/a.md"));
    try testing.expect(implementation.isExcluded("tests/fixtures/a.md"));
    try testing.expect(!implementation.isExcluded("tests/host/a.md"));
}

test "active surfaces are the ones that execute their lines" {
    try testing.expect(implementation.activeCommands("scripts/a.sh"));
    try testing.expect(implementation.activeCommands(".github/workflows/ci.yml"));
    try testing.expect(implementation.activeCommands("a/b.yaml"));
    try testing.expect(implementation.activeCommands(".devcontainer/Dockerfile"));
    try testing.expect(!implementation.activeCommands("docs/a.md"));
    try testing.expect(!implementation.activeCommands("justfile"));
}

test "line breaks follow str.splitlines, including the exotic ones" {
    const text = "a\nb\u{b}c\u{c}d\u{1c}e\u{1d}f\u{1e}g\u{85}h\u{2028}i\u{2029}j";
    var lines = implementation.LineIterator.init(text);
    var count: usize = 0;
    while (lines.next()) |_| count += 1;
    try testing.expectEqual(@as(usize, 10), count);
}

test "the unit separator is whitespace but not a line break" {
    try testing.expect(implementation.isPythonSpace(0x1f));
    try testing.expect(!implementation.isLineBreak(0x1f));
    var lines = implementation.LineIterator.init("a\u{1f}b");
    try testing.expectEqualStrings("a\u{1f}b", lines.next().?);
    try testing.expect(lines.next() == null);
}

test "CRLF and a lone CR both collapse to one break" {
    const normalised = try implementation.normalizeTerminators(testing.allocator, "a\r\nb\rc\n");
    defer testing.allocator.free(normalised);
    try testing.expectEqualStrings("a\nb\nc\n", normalised);
}

test "a trailing break yields no extra line" {
    var lines = implementation.LineIterator.init("a\nb\n");
    try testing.expectEqualStrings("a", lines.next().?);
    try testing.expectEqualStrings("b", lines.next().?);
    try testing.expect(lines.next() == null);
}

test "non-breaking whitespace still separates a command from its argument" {
    try expectRender("make\u{a0}ci", true, "make ci");
}

test "scanning a source numbers findings by line" {
    var findings = std.ArrayList([]const u8).init(testing.allocator);
    defer {
        for (findings.items) |item| testing.allocator.free(item);
        findings.deinit();
    }
    const text = "#!/usr/bin/env bash\nset -eu\nmake ci\necho done\ncmd=(gmake x)\n";
    try implementation.scanText(testing.allocator, testing.allocator, "scripts/a.sh", text, &findings);
    try testing.expectEqual(@as(usize, 2), findings.items.len);
    try testing.expectEqualStrings("scripts/a.sh:3: legacy repository task: make ci", findings.items[0]);
    try testing.expectEqualStrings("scripts/a.sh:5: legacy repository task: gmake x", findings.items[1]);
}

test "a documentation source is scanned without the active forms" {
    var findings = std.ArrayList([]const u8).init(testing.allocator);
    defer {
        for (findings.items) |item| testing.allocator.free(item);
        findings.deinit();
    }
    const text = "make ci\nPlease run make docs\n";
    try implementation.scanText(testing.allocator, testing.allocator, "docs/a.md", text, &findings);
    try testing.expectEqual(@as(usize, 1), findings.items.len);
    try testing.expectEqualStrings("docs/a.md:2: legacy repository task: make docs", findings.items[0]);
}

test "CRLF sources keep the predecessor's line numbers" {
    var findings = std.ArrayList([]const u8).init(testing.allocator);
    defer {
        for (findings.items) |item| testing.allocator.free(item);
        findings.deinit();
    }
    const text = "one\r\ntwo\r\nmake ci\r\n";
    try implementation.scanText(testing.allocator, testing.allocator, "scripts/a.sh", text, &findings);
    try testing.expectEqual(@as(usize, 1), findings.items.len);
    try testing.expectEqualStrings("scripts/a.sh:3: legacy repository task: make ci", findings.items[0]);
}

test "the floor and this source's own path are carried as constants" {
    try testing.expectEqual(@as(usize, 650), implementation.min_scoped_files);
    try testing.expectEqualStrings(
        "tools/check_no_legacy_make/src/internal/root.zig",
        implementation.self_source,
    );
}
