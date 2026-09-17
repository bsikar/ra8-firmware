//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression suite for the pure half of `roadmap_stats` (#858).
//! Every case here pins what `scripts/report/roadmap_stats.py` did.  PATHREF-OK:
//! the predecessor this names is deleted in the same change.  Quirks are
//! included, because the committed summary block in `docs/ROADMAP.md` is only
//! reproducible by a parser that counts exactly what the Python counted. A
//! test that looks wrong is a predecessor behaviour, and the comment beside
//! it says so.

const std = @import("std");
const testing = std.testing;
const imp = @import("implementation");

// -- contract literals -------------------------------------------------------

test "the diagnostic prefix drops the .py extension" {
    try testing.expectEqualStrings("roadmap_stats", imp.tool);
}

test "the BEGIN marker keeps the .py spelling, because it is data" {
    try testing.expectEqualStrings(
        "<!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->",
        imp.begin_mark,
    );
}

test "the END marker is byte-identical to the predecessor's" {
    try testing.expectEqualStrings("<!-- END SUMMARY -->", imp.end_mark);
}

test "the mark class is exactly [ x~!]" {
    try testing.expectEqualStrings(" x~!", imp.status_marks);
}

test "the status window is the seven lines after the heading" {
    try testing.expectEqual(@as(usize, 7), imp.status_window_lines);
}

test "Counts.total sums all four buckets" {
    const counts = imp.Counts{ .done = 3, .wip = 2, .blocked = 1, .todo = 4 };
    try testing.expectEqual(@as(u32, 10), counts.total());
}

test "a zeroed Counts totals zero" {
    try testing.expectEqual(@as(u32, 0), (imp.Counts{}).total());
}

test "Stats defaults to an empty census" {
    const stats = imp.Stats{};
    try testing.expectEqual(@as(u32, 0), stats.total_boxes);
    try testing.expectEqual(@as(u32, 0), stats.ticked_boxes);
    try testing.expectEqual(@as(u32, 0), stats.counts.total());
}

// -- decodeAt ----------------------------------------------------------------

test "decodeAt reads a one-byte code point" {
    const decoded = imp.decodeAt("a", 0);
    try testing.expectEqual(@as(u21, 'a'), decoded.code_point);
    try testing.expectEqual(@as(usize, 1), decoded.len);
}

test "decodeAt reads a two-byte code point" {
    const decoded = imp.decodeAt("\u{a0}", 0);
    try testing.expectEqual(@as(u21, 0xA0), decoded.code_point);
    try testing.expectEqual(@as(usize, 2), decoded.len);
}

test "decodeAt reads a three-byte code point" {
    const decoded = imp.decodeAt("\u{2028}", 0);
    try testing.expectEqual(@as(u21, 0x2028), decoded.code_point);
    try testing.expectEqual(@as(usize, 3), decoded.len);
}

test "decodeAt reads a four-byte code point" {
    const decoded = imp.decodeAt("\u{1F600}", 0);
    try testing.expectEqual(@as(u21, 0x1F600), decoded.code_point);
    try testing.expectEqual(@as(usize, 4), decoded.len);
}

test "decodeAt reads at an offset" {
    const decoded = imp.decodeAt("ab\u{3000}", 2);
    try testing.expectEqual(@as(u21, 0x3000), decoded.code_point);
    try testing.expectEqual(@as(usize, 3), decoded.len);
}

test "decodeAt degrades a malformed lead byte into one byte" {
    const decoded = imp.decodeAt("\xff", 0);
    try testing.expectEqual(@as(u21, 0xFF), decoded.code_point);
    try testing.expectEqual(@as(usize, 1), decoded.len);
}

test "decodeAt degrades a truncated sequence into one byte" {
    const decoded = imp.decodeAt("\xe2\x80", 0);
    try testing.expectEqual(@as(u21, 0xE2), decoded.code_point);
    try testing.expectEqual(@as(usize, 1), decoded.len);
}

test "decodeAt degrades an invalid continuation into one byte" {
    const decoded = imp.decodeAt("\xe2\x28\xa1", 0);
    try testing.expectEqual(@as(u21, 0xE2), decoded.code_point);
    try testing.expectEqual(@as(usize, 1), decoded.len);
}

// -- isSpace -----------------------------------------------------------------

test "ASCII space is whitespace" {
    try testing.expect(imp.isSpace(' '));
}

test "tab through carriage return are whitespace" {
    try testing.expect(imp.isSpace(0x09));
    try testing.expect(imp.isSpace(0x0A));
    try testing.expect(imp.isSpace(0x0B));
    try testing.expect(imp.isSpace(0x0C));
    try testing.expect(imp.isSpace(0x0D));
}

test "the file/group/record separators are whitespace" {
    try testing.expect(imp.isSpace(0x1C));
    try testing.expect(imp.isSpace(0x1D));
    try testing.expect(imp.isSpace(0x1E));
}

test "U+001F is whitespace even though it is not a line break" {
    try testing.expect(imp.isSpace(0x1F));
    try testing.expect(!imp.isLineBreak(0x1F));
}

test "NEL, NBSP and OGHAM space are whitespace" {
    try testing.expect(imp.isSpace(0x85));
    try testing.expect(imp.isSpace(0xA0));
    try testing.expect(imp.isSpace(0x1680));
}

test "the U+2000 block, the separators and the narrow spaces are whitespace" {
    try testing.expect(imp.isSpace(0x2000));
    try testing.expect(imp.isSpace(0x200A));
    try testing.expect(imp.isSpace(0x2028));
    try testing.expect(imp.isSpace(0x2029));
    try testing.expect(imp.isSpace(0x202F));
    try testing.expect(imp.isSpace(0x205F));
    try testing.expect(imp.isSpace(0x3000));
}

test "U+200B and U+FEFF are NOT whitespace to CPython" {
    try testing.expect(!imp.isSpace(0x200B));
    try testing.expect(!imp.isSpace(0xFEFF));
}

test "ordinary characters are not whitespace" {
    try testing.expect(!imp.isSpace('x'));
    try testing.expect(!imp.isSpace('['));
    try testing.expect(!imp.isSpace('#'));
    try testing.expect(!imp.isSpace(0x00B7));
}

test "isSpaceAt decodes before testing" {
    try testing.expect(imp.isSpaceAt("a\u{3000}", 1));
    try testing.expect(!imp.isSpaceAt("a\u{3000}", 0));
}

// -- strip family ------------------------------------------------------------

test "lstrip removes Unicode whitespace" {
    try testing.expectEqualStrings("x ", imp.lstrip(" \t\u{a0}\u{3000}x "));
}

test "rstrip removes Unicode whitespace" {
    try testing.expectEqualStrings(" x", imp.rstrip(" x \u{2000}\u{202f}"));
}

test "strip removes both ends" {
    try testing.expectEqualStrings("x y", imp.strip("\u{1f}  x y \u{85}"));
}

test "strip of an all-whitespace string is empty" {
    try testing.expectEqualStrings("", imp.strip(" \t\u{a0}"));
}

test "strip leaves an unpadded string alone" {
    try testing.expectEqualStrings("[x] a", imp.strip("[x] a"));
}

test "allSpace is vacuously true for an empty string" {
    try testing.expect(imp.allSpace(""));
}

test "allSpace accepts a mixed whitespace run and rejects any other character" {
    try testing.expect(imp.allSpace(" \t\u{a0}\u{3000}\u{1f}"));
    try testing.expect(!imp.allSpace("  x  "));
}

test "spaceRunEnd reports the byte offset of the first non-space" {
    try testing.expectEqual(@as(usize, 3), imp.spaceRunEnd(" \u{a0}x"));
    try testing.expectEqual(@as(usize, 0), imp.spaceRunEnd("x "));
}

// -- splitlines --------------------------------------------------------------

fn collect(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    return imp.splitLines(allocator, text);
}

test "splitLines breaks on LF and drops no interior blank" {
    const lines = try collect(testing.allocator, "a\n\nb\n");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 3), lines.len);
    try testing.expectEqualStrings("a", lines[0]);
    try testing.expectEqualStrings("", lines[1]);
    try testing.expectEqualStrings("b", lines[2]);
}

test "splitLines treats CRLF as one break" {
    const lines = try collect(testing.allocator, "a\r\nb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
    try testing.expectEqualStrings("a", lines[0]);
    try testing.expectEqualStrings("b", lines[1]);
}

test "splitLines treats a lone CR as a break" {
    const lines = try collect(testing.allocator, "a\rb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 2), lines.len);
    try testing.expectEqualStrings("b", lines[1]);
}

test "splitLines breaks on VT and FF" {
    const lines = try collect(testing.allocator, "a\x0bb\x0cc");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 3), lines.len);
}

test "splitLines breaks on the three separator controls" {
    const lines = try collect(testing.allocator, "a\x1cb\x1dc\x1ed");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 4), lines.len);
}

test "splitLines does NOT break on U+001F" {
    const lines = try collect(testing.allocator, "a\x1fb");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 1), lines.len);
    try testing.expectEqualStrings("a\x1fb", lines[0]);
}

test "splitLines breaks on NEL, LINE SEPARATOR and PARAGRAPH SEPARATOR" {
    const lines = try collect(testing.allocator, "a\u{85}b\u{2028}c\u{2029}d");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 4), lines.len);
    try testing.expectEqualStrings("c", lines[2]);
}

test "splitLines emits no trailing empty line for a trailing break" {
    const lines = try collect(testing.allocator, "a\n");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 1), lines.len);
}

test "splitLines of an empty string yields no lines" {
    const lines = try collect(testing.allocator, "");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 0), lines.len);
}

test "splitLines keeps a line without any break" {
    const lines = try collect(testing.allocator, "only");
    defer testing.allocator.free(lines);
    try testing.expectEqual(@as(usize, 1), lines.len);
    try testing.expectEqualStrings("only", lines[0]);
}

// -- DRIVER_HEADING_RE -------------------------------------------------------

test "a normal driver heading matches" {
    try testing.expect(imp.matchDriverHeading("### ra8_gpio"));
}

test "a tab separator matches" {
    try testing.expect(imp.matchDriverHeading("###\tra8_gpio"));
}

test "a non-breaking space separator matches, because \\s is Unicode-aware" {
    try testing.expect(imp.matchDriverHeading("###\u{a0}ra8_gpio"));
}

test "### followed by several spaces MATCHES, because \\s+ backtracks" {
    // Predecessor quirk: `\s+` gives one space back and `.` matches it, so a
    // whitespace-only heading still counts as a driver section.
    try testing.expect(imp.matchDriverHeading("###    "));
    try testing.expect(imp.matchDriverHeading("###  "));
}

test "### with exactly one trailing space does NOT match" {
    // `.+?` has no character left to take.
    try testing.expect(!imp.matchDriverHeading("### "));
}

test "bare ### does not match" {
    try testing.expect(!imp.matchDriverHeading("###"));
}

test "a four-level heading is not a driver heading" {
    try testing.expect(!imp.matchDriverHeading("#### ra8_gpio"));
    try testing.expect(!imp.matchDriverHeading("####"));
}

test "a two-level heading is not a driver heading" {
    try testing.expect(!imp.matchDriverHeading("## ra8_gpio"));
}

test "no separator after ### does not match" {
    try testing.expect(!imp.matchDriverHeading("###ra8_gpio"));
}

test "an indented heading does not match, because the pattern is anchored" {
    try testing.expect(!imp.matchDriverHeading(" ### ra8_gpio"));
    try testing.expect(!imp.matchDriverHeading("\t### ra8_gpio"));
}

test "an empty line does not match" {
    try testing.expect(!imp.matchDriverHeading(""));
}

test "trailing whitespace after the title is absorbed by \\s*$" {
    try testing.expect(imp.matchDriverHeading("### ra8_gpio   "));
}

// -- STATUS_LINE_RE ----------------------------------------------------------

test "a done status line reports x" {
    try testing.expectEqual(@as(?u8, 'x'), imp.findStatusMark("`[x]` Status: complete"));
}

test "a todo status line reports the space mark" {
    try testing.expectEqual(@as(?u8, ' '), imp.findStatusMark("`[ ]` Status: not started"));
}

test "a wip status line reports the tilde" {
    try testing.expectEqual(@as(?u8, '~'), imp.findStatusMark("`[~]` Status: partial"));
}

test "a blocked status line reports the bang" {
    try testing.expectEqual(@as(?u8, '!'), imp.findStatusMark("`[!]` Status: blocked"));
}

test "no whitespace between the tick and Status: still matches" {
    try testing.expectEqual(@as(?u8, 'x'), imp.findStatusMark("`[x]`Status:"));
}

test "Unicode whitespace between the tick and Status: still matches" {
    try testing.expectEqual(@as(?u8, 'x'), imp.findStatusMark("`[x]`\u{a0}\u{3000}Status:"));
}

test "the pattern is searched, so a prefix does not stop it" {
    try testing.expectEqual(@as(?u8, 'x'), imp.findStatusMark("| driver | `[x]` Status: done"));
}

test "an out-of-class mark does not match" {
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[y]` Status: done"));
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[X]` Status: done"));
}

test "the search skips a failing candidate and finds a later one" {
    try testing.expectEqual(
        @as(?u8, 'x'),
        imp.findStatusMark("`[y]` Status: a `[x]` Status: b"),
    );
}

test "the first of two valid candidates wins" {
    try testing.expectEqual(
        @as(?u8, '~'),
        imp.findStatusMark("`[~]` Status: a `[x]` Status: b"),
    );
}

test "Status: is case sensitive" {
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[x]` status: done"));
}

test "Status without its colon does not match" {
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[x]` Status done"));
}

test "a missing backtick on either side does not match" {
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("[x]` Status:"));
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[x] Status:"));
}

test "a space inside the brackets does not match" {
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[x ]` Status:"));
}

test "a short line cannot match" {
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark("`[x]"));
    try testing.expectEqual(@as(?u8, null), imp.findStatusMark(""));
}

// -- CHECKBOX_RE -------------------------------------------------------------

test "a ticked checkbox reports x" {
    try testing.expectEqual(@as(?u8, 'x'), imp.matchCheckbox("[x] init"));
}

test "an empty checkbox reports the space mark" {
    try testing.expectEqual(@as(?u8, ' '), imp.matchCheckbox("[ ] init"));
}

test "a wip checkbox is counted but NOT ticked" {
    try testing.expectEqual(@as(?u8, '~'), imp.matchCheckbox("[~] init"));
}

test "a blocked checkbox is counted but NOT ticked" {
    try testing.expectEqual(@as(?u8, '!'), imp.matchCheckbox("[!] init"));
}

test "leading ASCII whitespace is allowed" {
    try testing.expectEqual(@as(?u8, 'x'), imp.matchCheckbox("    [x] init"));
    try testing.expectEqual(@as(?u8, 'x'), imp.matchCheckbox("\t[x] init"));
}

test "leading Unicode whitespace is allowed, because \\s* is Unicode-aware" {
    try testing.expectEqual(@as(?u8, 'x'), imp.matchCheckbox("\u{3000}[x] init"));
}

test "a checkbox with nothing after it still matches" {
    try testing.expectEqual(@as(?u8, ' '), imp.matchCheckbox("[ ]"));
}

test "a checkbox is anchored, so text before it does not match" {
    try testing.expectEqual(@as(?u8, null), imp.matchCheckbox("- [x] init"));
    try testing.expectEqual(@as(?u8, null), imp.matchCheckbox("x [x] init"));
}

test "an out-of-class checkbox mark does not match" {
    try testing.expectEqual(@as(?u8, null), imp.matchCheckbox("[y] init"));
}

test "an unclosed checkbox does not match" {
    try testing.expectEqual(@as(?u8, null), imp.matchCheckbox("[x init"));
    try testing.expectEqual(@as(?u8, null), imp.matchCheckbox("[x"));
    try testing.expectEqual(@as(?u8, null), imp.matchCheckbox(""));
}

// -- walk boundaries ---------------------------------------------------------

test "the checkbox walk stops on a three- or two-level heading" {
    try testing.expect(imp.stopsCheckboxWalk("### next"));
    try testing.expect(imp.stopsCheckboxWalk("## section"));
}

test "a four-level heading does NOT stop the walk" {
    // Predecessor quirk: `"#### "` starts with neither `"### "` nor `"## "`.
    try testing.expect(!imp.stopsCheckboxWalk("#### deeper"));
}

test "a heading with no space after the hashes does not stop the walk" {
    try testing.expect(!imp.stopsCheckboxWalk("###next"));
    try testing.expect(!imp.stopsCheckboxWalk("##section"));
}

test "an indented heading does not stop the walk" {
    try testing.expect(!imp.stopsCheckboxWalk("  ### next"));
}

test "a fence is detected after stripping both ends" {
    try testing.expect(imp.isFence("```"));
    try testing.expect(imp.isFence("   ```text"));
    try testing.expect(imp.isFence("\u{a0}```"));
    try testing.expect(imp.isFence("```  "));
}

test "two backticks are not a fence, and neither is a fence with a prefix" {
    try testing.expect(!imp.isFence("``"));
    try testing.expect(!imp.isFence("x```"));
    try testing.expect(!imp.isFence(""));
}

// -- parseLines --------------------------------------------------------------

test "no lines is an empty census" {
    const stats = imp.parseLines(&[_][]const u8{});
    try testing.expectEqual(@as(u32, 0), stats.counts.total());
}

test "a heading with no status line is skipped entirely" {
    const lines = [_][]const u8{ "### ra8_gpio", "prose", "more prose" };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 0), stats.counts.total());
}

test "each mark lands in its own bucket" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done",
        "### b", "`[~]` Status: wip",
        "### c", "`[!]` Status: blocked",
        "### d", "`[ ]` Status: todo",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
    try testing.expectEqual(@as(u32, 1), stats.counts.wip);
    try testing.expectEqual(@as(u32, 1), stats.counts.blocked);
    try testing.expectEqual(@as(u32, 1), stats.counts.todo);
    try testing.expectEqual(@as(u32, 4), stats.counts.total());
}

test "a status line on the seventh line after the heading is still found" {
    const lines = [_][]const u8{
        "### a", "1", "2", "3", "4", "5", "6", "`[x]` Status: done",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
}

test "a status line on the eighth line after the heading is out of the window" {
    const lines = [_][]const u8{
        "### a", "1", "2", "3", "4", "5", "6", "7", "`[x]` Status: done",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 0), stats.counts.total());
}

test "checkboxes inside the first fence are counted" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "```", "[x] one", "[ ] two", "[~] three", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 3), stats.total_boxes);
    try testing.expectEqual(@as(u32, 1), stats.ticked_boxes);
}

test "tilde and bang checkboxes count as unticked" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "```", "[~] one", "[!] two", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 2), stats.total_boxes);
    try testing.expectEqual(@as(u32, 0), stats.ticked_boxes);
}

test "only the FIRST fence of a section is counted" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "```",     "[x] one",   "```",
        "prose", "```",                "[x] two", "[x] three", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
    try testing.expectEqual(@as(u32, 1), stats.ticked_boxes);
}

test "checkboxes outside any fence are not counted" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "[x] loose", "```", "[ ] inside", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
    try testing.expectEqual(@as(u32, 0), stats.ticked_boxes);
}

test "an unterminated fence counts to the end of the document" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "```", "[x] one", "[x] two",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 2), stats.total_boxes);
}

test "a two-level heading stops the walk before a later fence" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "## other", "```", "[x] one", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
    try testing.expectEqual(@as(u32, 0), stats.total_boxes);
}

test "a four-level heading does NOT stop the walk reaching the fence" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "#### detail", "```", "[x] one", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
}

test "a fenceless section leaves the next heading to be re-examined" {
    // `i = max(k, i + 1)` lands ON the stopping heading, so the outer loop
    // sees it again and counts its own status line.
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done",
        "### b", "`[~]` Status: wip",
        "```",   "[x] one",
        "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
    try testing.expectEqual(@as(u32, 1), stats.counts.wip);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
}

test "two fenced sections both count, and the second is not skipped" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "```", "[x] one", "```",
        "### b", "`[x]` Status: done", "```", "[ ] two", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 2), stats.counts.done);
    try testing.expectEqual(@as(u32, 2), stats.total_boxes);
    try testing.expectEqual(@as(u32, 1), stats.ticked_boxes);
}

test "a whitespace-only heading with a status line counts as a driver" {
    const lines = [_][]const u8{ "###   ", "`[x]` Status: done" };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
}

test "an indented fence opens and closes the block" {
    const lines = [_][]const u8{
        "### a", "`[x]` Status: done", "   ```zig", "  [x] one", "   ```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
}

test "a section whose status line sits after its fence still counts the fence" {
    const lines = [_][]const u8{
        "### a", "prose", "`[!]` Status: blocked", "```", "[ ] one", "```",
    };
    const stats = imp.parseLines(&lines);
    try testing.expectEqual(@as(u32, 1), stats.counts.blocked);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
}

test "parseRoadmap splits and parses one text" {
    const text = "intro\n### a\n`[x]` Status: done\n```\n[x] one\n[ ] two\n```\n";
    const stats = try imp.parseRoadmap(testing.allocator, text);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
    try testing.expectEqual(@as(u32, 2), stats.total_boxes);
    try testing.expectEqual(@as(u32, 1), stats.ticked_boxes);
}

test "parseRoadmap handles CRLF line endings" {
    const text = "### a\r\n`[x]` Status: done\r\n```\r\n[x] one\r\n```\r\n";
    const stats = try imp.parseRoadmap(testing.allocator, text);
    try testing.expectEqual(@as(u32, 1), stats.counts.done);
    try testing.expectEqual(@as(u32, 1), stats.total_boxes);
}

test "parseRoadmap of an empty document is an empty census" {
    const stats = try imp.parseRoadmap(testing.allocator, "");
    try testing.expectEqual(@as(u32, 0), stats.counts.total());
    try testing.expectEqual(@as(u32, 0), stats.total_boxes);
}

// -- percentages -------------------------------------------------------------

test "no boxes is exactly zero percent, never a division by zero" {
    try testing.expectEqual(@as(f64, 0.0), imp.percentOf(0, 0));
}

test "half the boxes is fifty percent" {
    try testing.expectEqual(@as(f64, 50.0), imp.percentOf(1, 2));
}

test "every box is a hundred percent" {
    try testing.expectEqual(@as(f64, 100.0), imp.percentOf(7, 7));
}

test "tenthsHalfEven rounds a plain value" {
    try testing.expectEqual(@as(u64, 375), imp.tenthsHalfEven(37.5));
    try testing.expectEqual(@as(u64, 1000), imp.tenthsHalfEven(100.0));
    try testing.expectEqual(@as(u64, 0), imp.tenthsHalfEven(0.0));
}

test "tenthsHalfEven breaks an exact tie towards the even digit" {
    // CPython: f"{12.25:.1f}" == "12.2", not "12.3".
    try testing.expectEqual(@as(u64, 122), imp.tenthsHalfEven(12.25));
    // and f"{12.75:.1f}" == "12.8", because 127 is odd.
    try testing.expectEqual(@as(u64, 128), imp.tenthsHalfEven(12.75));
    try testing.expectEqual(@as(u64, 2), imp.tenthsHalfEven(0.25));
}

test "tenthsHalfEven rounds a repeating value the way CPython prints it" {
    try testing.expectEqual(@as(u64, 333), imp.tenthsHalfEven(100.0 / 3.0));
    try testing.expectEqual(@as(u64, 667), imp.tenthsHalfEven(200.0 / 3.0));
}

test "tenthsHalfEven treats a negative value as zero" {
    try testing.expectEqual(@as(u64, 0), imp.tenthsHalfEven(-1.0));
}

test "formatOneDecimal renders one decimal place" {
    var buffer: [32]u8 = undefined;
    try testing.expectEqualStrings("0.0", try imp.formatOneDecimal(&buffer, 0.0));
    try testing.expectEqualStrings("100.0", try imp.formatOneDecimal(&buffer, 100.0));
    try testing.expectEqualStrings("37.5", try imp.formatOneDecimal(&buffer, 37.5));
    try testing.expectEqualStrings("12.2", try imp.formatOneDecimal(&buffer, 12.25));
    try testing.expectEqualStrings("12.8", try imp.formatOneDecimal(&buffer, 12.75));
}

test "formatOneDecimal renders the percentage of a real census" {
    var buffer: [32]u8 = undefined;
    try testing.expectEqualStrings(
        "33.3",
        try imp.formatOneDecimal(&buffer, imp.percentOf(1, 3)),
    );
    try testing.expectEqualStrings(
        "66.7",
        try imp.formatOneDecimal(&buffer, imp.percentOf(2, 3)),
    );
    try testing.expectEqualStrings(
        "12.5",
        try imp.formatOneDecimal(&buffer, imp.percentOf(1, 8)),
    );
}

// -- renderers ---------------------------------------------------------------

test "renderSummary reproduces the block byte for byte" {
    const stats = imp.Stats{
        .counts = .{ .done = 12, .wip = 3, .blocked = 1, .todo = 4 },
        .total_boxes = 8,
        .ticked_boxes = 1,
    };
    const rendered = try imp.renderSummary(testing.allocator, stats);
    defer testing.allocator.free(rendered);
    try testing.expectEqualStrings(
        imp.begin_mark ++ "\n" ++
            "- Total drivers tracked: 20\n" ++
            "- DONE:    12\n" ++
            "- WIP:     3\n" ++
            "- BLOCKED: 1\n" ++
            "- TODO:    4\n" ++
            "- Checklist coverage: 1/8 boxes ticked (12.5%)\n" ++
            imp.end_mark,
        rendered,
    );
}

test "renderSummary reports zero percent when there are no boxes" {
    const rendered = try imp.renderSummary(testing.allocator, imp.Stats{});
    defer testing.allocator.free(rendered);
    try testing.expect(std.mem.indexOf(u8, rendered, "0/0 boxes ticked (0.0%)") != null);
}

test "renderSummary carries both markers, so a rewrite cannot lose them" {
    const rendered = try imp.renderSummary(testing.allocator, imp.Stats{});
    defer testing.allocator.free(rendered);
    try testing.expect(std.mem.startsWith(u8, rendered, imp.begin_mark));
    try testing.expect(std.mem.endsWith(u8, rendered, imp.end_mark));
}

test "renderCensus renders the one-line status paragraph" {
    const stats = imp.Stats{
        .counts = .{ .done = 2, .wip = 1, .blocked = 0, .todo = 3 },
        .total_boxes = 10,
        .ticked_boxes = 4,
    };
    const census = try imp.renderCensus(testing.allocator, stats);
    defer testing.allocator.free(census);
    try testing.expectEqualStrings(
        "(drivers=6 DONE=2 WIP=1 BLOCKED=0 TODO=3 boxes=4/10)",
        census,
    );
}

// -- rewrite -----------------------------------------------------------------

const sample_document = "pre\n" ++ imp.begin_mark ++ "\nstale\n" ++ imp.end_mark ++ "\npost\n";

test "rewrite replaces the marked region and preserves everything outside it" {
    const summary = imp.begin_mark ++ "\nfresh\n" ++ imp.end_mark;
    const updated = try imp.rewrite(testing.allocator, sample_document, summary);
    defer testing.allocator.free(updated);
    try testing.expectEqualStrings("pre\n" ++ imp.begin_mark ++ "\nfresh\n" ++ imp.end_mark ++ "\npost\n", updated);
}

test "rewrite preserves CRLF outside the markers byte for byte" {
    const text = "pre\r\n" ++ imp.begin_mark ++ "\nold\n" ++ imp.end_mark ++ "\r\npost\r\n";
    const summary = imp.begin_mark ++ "\nnew\n" ++ imp.end_mark;
    const updated = try imp.rewrite(testing.allocator, text, summary);
    defer testing.allocator.free(updated);
    try testing.expect(std.mem.startsWith(u8, updated, "pre\r\n"));
    try testing.expect(std.mem.endsWith(u8, updated, "\r\npost\r\n"));
}

test "rewrite with an identical summary returns the same bytes" {
    const summary = imp.begin_mark ++ "\nstale\n" ++ imp.end_mark;
    const updated = try imp.rewrite(testing.allocator, sample_document, summary);
    defer testing.allocator.free(updated);
    try testing.expectEqualStrings(sample_document, updated);
}

test "rewrite refuses a document with neither marker" {
    try testing.expectError(
        imp.RewriteError.MissingMarkers,
        imp.rewrite(testing.allocator, "nothing here\n", "summary"),
    );
}

test "rewrite refuses a document missing the END marker" {
    try testing.expectError(
        imp.RewriteError.MissingMarkers,
        imp.rewrite(testing.allocator, "a\n" ++ imp.begin_mark ++ "\nb\n", "summary"),
    );
}

test "rewrite refuses a document missing the BEGIN marker" {
    try testing.expectError(
        imp.RewriteError.MissingMarkers,
        imp.rewrite(testing.allocator, "a\n" ++ imp.end_mark ++ "\nb\n", "summary"),
    );
}

test "rewrite refuses an END that sits before the BEGIN instead of truncating" {
    // DELIBERATE DIVERGENCE. The Python guard passed here (both markers are
    // present), then found no END after the BEGIN, so `post` became empty and
    // the rewrite silently dropped everything after the summary.
    const inverted = "a\n" ++ imp.end_mark ++ "\nb\n" ++ imp.begin_mark ++ "\ntail\n";
    try testing.expectError(
        imp.RewriteError.EndBeforeBegin,
        imp.rewrite(testing.allocator, inverted, "summary"),
    );
}

test "rewrite uses the first BEGIN and the first END after it" {
    const text = "a\n" ++ imp.begin_mark ++ "\nold\n" ++ imp.end_mark ++
        "\nmiddle\n" ++ imp.begin_mark ++ "\nsecond\n" ++ imp.end_mark ++ "\nz\n";
    const summary = imp.begin_mark ++ "\nnew\n" ++ imp.end_mark;
    const updated = try imp.rewrite(testing.allocator, text, summary);
    defer testing.allocator.free(updated);
    try testing.expect(std.mem.indexOf(u8, updated, "new") != null);
    // The second block is left exactly where it was.
    try testing.expect(std.mem.indexOf(u8, updated, "second") != null);
    try testing.expect(std.mem.endsWith(u8, updated, "\nz\n"));
}

test "rewrite tolerates an inverted END when a second END follows the BEGIN" {
    const text = imp.end_mark ++ "\n" ++ imp.begin_mark ++ "\nold\n" ++ imp.end_mark ++ "\ntail\n";
    const summary = imp.begin_mark ++ "\nnew\n" ++ imp.end_mark;
    const updated = try imp.rewrite(testing.allocator, text, summary);
    defer testing.allocator.free(updated);
    try testing.expect(std.mem.endsWith(u8, updated, "\ntail\n"));
    try testing.expect(std.mem.indexOf(u8, updated, "old") == null);
}

test "parse, render and rewrite is idempotent on the second pass" {
    const document = "### a\n`[x]` Status: done\n```\n[x] one\n[ ] two\n```\n\n" ++
        imp.begin_mark ++ "\nwrong\n" ++ imp.end_mark ++ "\ntail\n";

    const first_stats = try imp.parseRoadmap(testing.allocator, document);
    const first_summary = try imp.renderSummary(testing.allocator, first_stats);
    defer testing.allocator.free(first_summary);
    const once = try imp.rewrite(testing.allocator, document, first_summary);
    defer testing.allocator.free(once);
    try testing.expect(!std.mem.eql(u8, document, once));

    const second_stats = try imp.parseRoadmap(testing.allocator, once);
    const second_summary = try imp.renderSummary(testing.allocator, second_stats);
    defer testing.allocator.free(second_summary);
    const twice = try imp.rewrite(testing.allocator, once, second_summary);
    defer testing.allocator.free(twice);
    try testing.expectEqualStrings(once, twice);
}

test "the generated summary block never introduces a driver of its own" {
    // The rendered block carries `- DONE:` lines, not `###` headings, so
    // recomputing over a document that already holds one cannot drift.
    const stats = try imp.parseRoadmap(
        testing.allocator,
        imp.begin_mark ++ "\n- DONE:    3\n" ++ imp.end_mark ++ "\n",
    );
    try testing.expectEqual(@as(u32, 0), stats.counts.total());
    try testing.expectEqual(@as(u32, 0), stats.total_boxes);
}
