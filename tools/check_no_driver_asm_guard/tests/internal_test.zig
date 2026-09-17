//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detector algebra of the HAL driver asm-guard gate (#858): the comment
//! stripper, the directive matchers, the asm pattern and the conditional
//! walk. Every case here is pure text in and findings out, so the behaviour
//! the predecessor's four regular expressions encoded is pinned with no
//! repository on disk.

const std = @import("std");
const implementation = @import("implementation");

/// Scan one text and return just the line numbers it reports.
fn findingLines(allocator: std.mem.Allocator, text: []const u8) ![]usize {
    const findings = try implementation.scanText(allocator, text);
    var lines = try allocator.alloc(usize, findings.len);
    for (findings, 0..) |finding, index| lines[index] = finding.line_no;
    return lines;
}

test "a comment-free line survives the stripper unchanged" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "void f(void);\n");
    try std.testing.expectEqual(@as(usize, 1), lines.len);
    try std.testing.expectEqualStrings("void f(void);", lines[0]);
}

test "a line comment truncates the line but keeps it" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "int x; // __asm__\n");
    try std.testing.expectEqual(@as(usize, 1), lines.len);
    try std.testing.expectEqualStrings("int x; ", lines[0]);
}

test "a block comment on one line leaves the text either side" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "a /* __asm */ b\n");
    try std.testing.expectEqualStrings("a  b", lines[0]);
}

test "a block comment spanning lines blanks them and preserves the count" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(
        arena.allocator(),
        "head /* open\n__asm(\"nop\");\nclose */ tail\n",
    );
    try std.testing.expectEqual(@as(usize, 3), lines.len);
    try std.testing.expectEqualStrings("head ", lines[0]);
    try std.testing.expectEqualStrings("", lines[1]);
    try std.testing.expectEqualStrings(" tail", lines[2]);
}

test "an unterminated block comment swallows the rest of the file" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "/* open\n__asm(\"nop\");\n");
    try std.testing.expectEqualStrings("", lines[0]);
    try std.testing.expectEqualStrings("", lines[1]);
}

test "a slash-star-slash does not close the comment it opened" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "/*/ still open\n");
    try std.testing.expectEqualStrings("", lines[0]);
}

test "a line comment inside a block comment is just comment text" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "/* // */ kept\n");
    try std.testing.expectEqualStrings(" kept", lines[0]);
}

test "a string literal holding a slash pair is stripped, inherited as-is" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "puts(\"a//b\");\n");
    try std.testing.expectEqualStrings("puts(\"a", lines[0]);
}

test "the stripper is byte-faithful through multi-byte text" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "caf\u{00e9} /* x */ bar\n");
    try std.testing.expectEqualStrings("caf\u{00e9}  bar", lines[0]);
}

test "splitlines breaks on a form feed, shifting later line numbers" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "a\x0cb\nc\n");
    try std.testing.expectEqual(@as(usize, 3), lines.len);
    try std.testing.expectEqualStrings("a", lines[0]);
    try std.testing.expectEqualStrings("b", lines[1]);
}

test "splitlines breaks on the paragraph separator" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "a\u{2029}b");
    try std.testing.expectEqual(@as(usize, 2), lines.len);
}

test "a unit separator is whitespace but never a line break" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "a\x1fb\n");
    try std.testing.expectEqual(@as(usize, 1), lines.len);
}

test "a final line without a terminator is still a line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "a\nb");
    try std.testing.expectEqual(@as(usize, 2), lines.len);
}

test "a trailing newline adds no empty line" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try implementation.stripComments(arena.allocator(), "a\n");
    try std.testing.expectEqual(@as(usize, 1), lines.len);
}

test "CRLF folds to one break so line numbers match a text-mode read" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const text = try implementation.normalizeTerminators(arena.allocator(), "a\r\nb\rc\n");
    try std.testing.expectEqualStrings("a\nb\nc\n", text);
    const lines = try implementation.stripComments(arena.allocator(), text);
    try std.testing.expectEqual(@as(usize, 3), lines.len);
}

test "the three opening directives match with their rest captured" {
    try std.testing.expectEqualStrings(" defined(X)", implementation.matchIf("#if defined(X)").?);
    try std.testing.expectEqualStrings(" X", implementation.matchIf("#ifdef X").?);
    try std.testing.expectEqualStrings(" X", implementation.matchIf("#ifndef X").?);
}

test "leading and inner whitespace around the hash is allowed" {
    try std.testing.expectEqualStrings(" X", implementation.matchIf("   #  ifdef X").?);
    try std.testing.expectEqualStrings("", implementation.matchIf("\t#\tendif") orelse "");
}

test "a keyword that runs into a word is not a directive" {
    try std.testing.expect(implementation.matchIf("#ifdefx X") == null);
    try std.testing.expect(implementation.matchIf("#iffy") == null);
    try std.testing.expect(!implementation.matchEndif("#endifs"));
}

test "a non-ASCII letter after the keyword suppresses the match" {
    // Python's `\b` is Unicode-aware, so this line is ordinary text.
    try std.testing.expect(implementation.matchIf("#if\u{00e9} RA8_OFF_TARGET") == null);
    try std.testing.expect(!implementation.matchEndif("#endif\u{00e9}"));
}

test "a non-word non-ASCII character after the keyword still matches" {
    try std.testing.expect(implementation.matchEndif("#endif\u{2022}"));
}

test "a non-breaking space counts as whitespace before the hash" {
    try std.testing.expect(implementation.matchEndif("\u{00a0}#endif"));
}

test "elif and endif match only their own keyword" {
    try std.testing.expectEqualStrings(" defined(Y)", implementation.matchElif("#elif defined(Y)").?);
    try std.testing.expect(implementation.matchElif("#else") == null);
    try std.testing.expect(implementation.matchEndif("#endif /* X */"));
    try std.testing.expect(!implementation.matchEndif("#else"));
}

test "a directive needs the hash" {
    try std.testing.expect(implementation.matchIf("ifdef X") == null);
    try std.testing.expect(implementation.matchIf("") == null);
}

test "the asm pattern fires on both spellings" {
    try std.testing.expect(implementation.hasAsm("__asm(\"nop\");"));
    try std.testing.expect(implementation.hasAsm("__asm__(\"wfi\");"));
    try std.testing.expect(implementation.hasAsm("x = __asm;"));
}

test "the asm pattern needs clean ASCII boundaries" {
    try std.testing.expect(!implementation.hasAsm("ra8___asm"));
    try std.testing.expect(!implementation.hasAsm("__asmx"));
    try std.testing.expect(!implementation.hasAsm("__asm__x"));
    try std.testing.expect(!implementation.hasAsm("__asm_"));
    try std.testing.expect(!implementation.hasAsm("my__asm()"));
}

test "a non-ASCII letter beside the asm keyword does NOT suppress it" {
    // The asm pattern spells its boundaries as explicit ASCII classes, unlike
    // the Unicode-aware `\b` on the directive keywords.
    try std.testing.expect(implementation.hasAsm("\u{00e9}__asm(\"nop\")"));
}

test "asm appearing twice on a line is still one finding" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#ifdef RA8_OFF_TARGET\n__asm(\"nop\"); __asm(\"nop\");\n#endif\n",
    );
    try std.testing.expectEqualSlices(usize, &.{2}, lines);
}

test "guarded asm fires in both branches of the fixture" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(arena.allocator(), implementation.selftest_bad);
    try std.testing.expectEqualSlices(usize, &.{ 2, 4 }, lines);
}

test "seam calls and comment lookalikes stay quiet" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(arena.allocator(), implementation.selftest_good);
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "unguarded asm outside any conditional is not this gate's business" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(arena.allocator(), "void f(void) { __asm(\"nop\"); }\n");
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "asm under a conditional that names another flag stays quiet" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(arena.allocator(), "#ifdef RA8_DEBUG\n__asm(\"nop\");\n#endif\n");
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "an outer off-target frame taints a nested clean conditional" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#ifdef RA8_OFF_TARGET\n#ifdef RA8_DEBUG\n__asm(\"nop\");\n#endif\n#endif\n",
    );
    try std.testing.expectEqualSlices(usize, &.{3}, lines);
}

test "an elif naming the flag taints the whole frame" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#if defined(A)\n#elif defined(RA8_OFF_TARGET)\n__asm(\"nop\");\n#endif\n",
    );
    try std.testing.expectEqualSlices(usize, &.{3}, lines);
}

test "closing the tainted frame clears the taint" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#ifdef RA8_OFF_TARGET\n#endif\n__asm(\"nop\");\n",
    );
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "an unbalanced endif never pops past the bottom of the stack" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#endif\n#ifdef RA8_OFF_TARGET\n__asm(\"nop\");\n",
    );
    try std.testing.expectEqualSlices(usize, &.{3}, lines);
}

test "an elif with no open frame is ignored rather than fatal" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(arena.allocator(), "#elif defined(RA8_OFF_TARGET)\n__asm(\"nop\");\n");
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "asm sharing a line with a directive is never reported" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#ifdef RA8_OFF_TARGET\n#endif __asm(\"nop\")\n",
    );
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "the flag named anywhere in the rest of the line taints the frame" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#if !defined(RA8_OFF_TARGET) && defined(A)\n__asm(\"nop\");\n#endif\n",
    );
    try std.testing.expectEqualSlices(usize, &.{2}, lines);
}

test "asm hidden in a comment under a guard stays quiet" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const lines = try findingLines(
        arena.allocator(),
        "#ifdef RA8_OFF_TARGET\n/* __asm(\"nop\") is prose */\n#endif\n",
    );
    try std.testing.expectEqual(@as(usize, 0), lines.len);
}

test "a finding renders the stripped line at its repository-relative path" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const findings = try implementation.scanText(
        allocator,
        "#ifdef RA8_OFF_TARGET\n  __asm(\"nop\");  /* tail */\n#endif\n",
    );
    try std.testing.expectEqual(@as(usize, 1), findings.len);
    const rendered = try implementation.renderFinding(allocator, "libs/ra8_hal/src/x.c", findings[0]);
    try std.testing.expectEqualStrings(
        "libs/ra8_hal/src/x.c:2: inline asm '__asm(\"nop\");' sits inside a " ++
            "RA8_OFF_TARGET conditional -- route it through " ++
            "libs/ra8_hal/inc/ra8_hw_intrinsics.h instead",
        rendered,
    );
}

test "strip trims Unicode whitespace from both ends only" {
    try std.testing.expectEqualStrings("a b", implementation.strip("\u{00a0} a b \t"));
    try std.testing.expectEqualStrings("", implementation.strip("   "));
}

test "both selftest directions hold on the carried fixtures" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const cases = try implementation.selftestCases(arena.allocator());
    for (cases) |case| try std.testing.expect(case.passed);
}
