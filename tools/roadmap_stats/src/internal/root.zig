//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure half of the `roadmap_stats` host tool (#858): the ROADMAP.md parser,
//! the summary renderer and the marker substitution, reimplemented from the
//! Python `scripts/report/roadmap_stats.py`.  PATHREF-OK: the predecessor
//! this names is deleted in the same change.  No file system and no argv in
//! sight, so every quirk below is reachable from a test with a string.
//!
//! Zig has no regex, so the predecessor's three patterns are hand-written.
//! Their quirks are PINNED by `tests/internal_test.zig` rather than tidied,
//! because the committed summary block in `docs/ROADMAP.md` is only
//! reproducible by a parser that counts what the Python counted:
//!
//!   * `DRIVER_HEADING_RE = ^###\s+(.+?)\s*$`, applied with `.match`. The
//!     capture is never used, only the count. `\s+` backtracks, so `###`
//!     followed by four spaces still MATCHES: `\s+` gives one space back and
//!     `.` matches it. A lone `### ` does not, because `.+?` needs a
//!     character. `\s` is Unicode-aware, so U+00A0 is a valid separator.
//!   * `STATUS_LINE_RE = `\[(?P<mark>[ x~!])\]`\s*Status:`, SEARCHED (not
//!     matched) over the seven lines after the heading, i.e. `i+1 ..
//!     min(i+8, n)`. No status line in that window skips the heading
//!     entirely and advances `i` by one.
//!   * `CHECKBOX_RE = ^\s*\[([ x~!])\]`, applied with `.match`, so leading
//!     Unicode whitespace is allowed. `total_boxes` counts every match and
//!     `ticked_boxes` only `x`, so `~` and `!` are UNTICKED.
//!
//! Two structural quirks matter as much as the patterns. The checkbox walk
//! stops on a line starting `"### "` or `"## "`, and `"#### "` satisfies
//! neither, so a four-level heading does not stop it. And the walk breaks
//! after the first fenced block, so only the FIRST fence per section counts,
//! after which `i = max(k, i + 1)` can land back ON the next heading and the
//! outer loop re-examines it.

const std = @import("std");
const char_classes = @import("char_classes.zig");

/// Diagnostic prefix. The predecessor printed `roadmap_stats.py:`; every
/// migration in this epic drops the extension, and `docs/qualification/*`
/// name the tool rather than the script.
pub const tool = "roadmap_stats";

/// The markers stay byte-identical, `.py` spelling included: they are DATA
/// this tool matches on, and every committed copy of the block carries them.
/// Rewriting the marker would orphan the region it delimits.
pub const begin_mark = "<!-- BEGIN SUMMARY -- DO NOT EDIT BY HAND -- managed by roadmap_stats.py -->";
pub const end_mark = "<!-- END SUMMARY -->";

/// `[ x~!]`, the literal class both the status and the checkbox pattern use.
pub const status_marks = " x~!";

/// `range(i + 1, min(i + 8, n))`: the seven lines after the heading.
pub const status_window_lines: usize = 7;

/// Per-status driver census.
pub const Counts = struct {
    done: u32 = 0,
    wip: u32 = 0,
    blocked: u32 = 0,
    todo: u32 = 0,

    /// `sum(counts.values())`, the reported driver total.
    pub fn total(self: Counts) u32 {
        return self.done + self.wip + self.blocked + self.todo;
    }
};

/// Everything `parse_roadmap` returned.
pub const Stats = struct {
    counts: Counts = .{},
    total_boxes: u32 = 0,
    ticked_boxes: u32 = 0,
};

// -- text primitives ---------------------------------------------------------

/// One decoded code point and the byte length it occupied.
pub const Decoded = struct { code_point: u21, len: usize };

/// Decode the code point at `index`. A malformed or truncated sequence
/// decodes as its own lead byte over one byte, which can never be
/// whitespace, a break or a marker character, so a non-UTF-8 run degrades
/// into ordinary text instead of panicking. `cli.zig` refuses undecodable
/// input before this is reached; this is the backstop.
pub fn decodeAt(text: []const u8, index: usize) Decoded {
    const lead = text[index];
    const len = std.unicode.utf8ByteSequenceLength(lead) catch {
        return .{ .code_point = lead, .len = 1 };
    };
    if (index + len > text.len) return .{ .code_point = lead, .len = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + len]) catch {
        return .{ .code_point = lead, .len = 1 };
    };
    return .{ .code_point = code_point, .len = len };
}

/// `\s` for a str pattern, equal to `str.isspace`.
pub fn isSpace(code_point: u21) bool {
    return char_classes.inTable(&char_classes.space_intervals, code_point);
}

/// Whether the code point at `index` is whitespace.
pub fn isSpaceAt(text: []const u8, index: usize) bool {
    return isSpace(decodeAt(text, index).code_point);
}

/// Byte offset of the first non-whitespace code point, or `text.len`.
pub fn spaceRunEnd(text: []const u8) usize {
    var index: usize = 0;
    while (index < text.len) {
        const decoded = decodeAt(text, index);
        if (!isSpace(decoded.code_point)) break;
        index += decoded.len;
    }
    return index;
}

/// `str.lstrip()`.
pub fn lstrip(text: []const u8) []const u8 {
    return text[spaceRunEnd(text)..];
}

/// `str.rstrip()`.
pub fn rstrip(text: []const u8) []const u8 {
    var index: usize = 0;
    var end: usize = 0;
    while (index < text.len) {
        const decoded = decodeAt(text, index);
        index += decoded.len;
        if (!isSpace(decoded.code_point)) end = index;
    }
    return text[0..end];
}

/// `str.strip()`.
pub fn strip(text: []const u8) []const u8 {
    return rstrip(lstrip(text));
}

/// Whether every code point is whitespace (an empty string is, vacuously).
pub fn allSpace(text: []const u8) bool {
    return spaceRunEnd(text) == text.len;
}

/// The code points `str.splitlines` breaks on. U+001F is deliberately
/// absent: it is whitespace but NOT a line boundary, unlike U+001C..U+001E.
pub fn isLineBreak(code_point: u21) bool {
    return switch (code_point) {
        0x000A, 0x000B, 0x000C, 0x000D, 0x001C, 0x001D, 0x001E, 0x0085, 0x2028, 0x2029 => true,
        else => false,
    };
}

/// `str.splitlines()`: CRLF is one break, no trailing empty line, and a
/// break is never part of the line it ends.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        var cursor = self.index;
        while (cursor < self.text.len) {
            const decoded = decodeAt(self.text, cursor);
            if (isLineBreak(decoded.code_point)) {
                const line = self.text[self.index..cursor];
                var advance = decoded.len;
                if (decoded.code_point == '\r' and
                    cursor + 1 < self.text.len and
                    self.text[cursor + 1] == '\n') advance += 1;
                self.index = cursor + advance;
                return line;
            }
            cursor += decoded.len;
        }
        const line = self.text[self.index..];
        self.index = self.text.len;
        return line;
    }
};

/// `text.splitlines()` as a slice. The returned slice is owned; the lines
/// themselves point into `text`.
pub fn splitLines(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    var lines = std.ArrayList([]const u8).init(allocator);
    errdefer lines.deinit();
    var iterator = LineIterator{ .text = text };
    while (iterator.next()) |line| try lines.append(line);
    return lines.toOwnedSlice();
}

// -- the three patterns ------------------------------------------------------

/// `DRIVER_HEADING_RE.match(line) is not None`.
///
/// After `###` the line needs one whitespace code point for `\s+` and at
/// least one more code point for `.+?`; the trailing `\s*$` then matches
/// whatever is left, whitespace or not, because `.+?` backtracks outward.
/// So `###\u{a0}x` matches, `### ` does not, and `###    ` does.
pub fn matchDriverHeading(line: []const u8) bool {
    if (!std.mem.startsWith(u8, line, "###")) return false;
    const rest = line[3..];
    if (rest.len == 0) return false;
    const separator = decodeAt(rest, 0);
    if (!isSpace(separator.code_point)) return false;
    return rest.len > separator.len;
}

/// `STATUS_LINE_RE.search(line)`, returning the `mark` group.
pub fn findStatusMark(line: []const u8) ?u8 {
    if (line.len < 5) return null;
    var start: usize = 0;
    while (start + 5 <= line.len) : (start += 1) {
        if (line[start] != '`' or line[start + 1] != '[') continue;
        const mark = line[start + 2];
        if (std.mem.indexOfScalar(u8, status_marks, mark) == null) continue;
        if (line[start + 3] != ']' or line[start + 4] != '`') continue;
        const tail = line[start + 5 ..];
        if (std.mem.startsWith(u8, lstrip(tail), "Status:")) return mark;
    }
    return null;
}

/// `CHECKBOX_RE.match(line)`, returning the `mark` group.
pub fn matchCheckbox(line: []const u8) ?u8 {
    const body = lstrip(line);
    if (body.len < 3) return null;
    if (body[0] != '[') return null;
    const mark = body[1];
    if (std.mem.indexOfScalar(u8, status_marks, mark) == null) return null;
    if (body[2] != ']') return null;
    return mark;
}

/// Whether a line closes the checkbox walk: `"### "` or `"## "`, so a
/// `"#### "` heading does not.
pub fn stopsCheckboxWalk(line: []const u8) bool {
    return std.mem.startsWith(u8, line, "### ") or std.mem.startsWith(u8, line, "## ");
}

/// Whether a line opens or closes a fence: `line.strip().startswith("```")`.
pub fn isFence(line: []const u8) bool {
    return std.mem.startsWith(u8, strip(line), "```");
}

// -- the parser --------------------------------------------------------------

/// `parse_roadmap`, over already-split lines.
pub fn parseLines(lines: []const []const u8) Stats {
    var stats = Stats{};
    const n = lines.len;
    var i: usize = 0;

    while (i < n) {
        if (!matchDriverHeading(lines[i])) {
            i += 1;
            continue;
        }

        var status_mark: ?u8 = null;
        const window_end = @min(i + 1 + status_window_lines, n);
        var j = i + 1;
        while (j < window_end) : (j += 1) {
            if (findStatusMark(lines[j])) |mark| {
                status_mark = mark;
                break;
            }
        }
        const mark = status_mark orelse {
            i += 1;
            continue;
        };

        switch (mark) {
            'x' => stats.counts.done += 1,
            '~' => stats.counts.wip += 1,
            '!' => stats.counts.blocked += 1,
            else => stats.counts.todo += 1,
        }

        var k = i + 1;
        while (k < n) {
            if (stopsCheckboxWalk(lines[k])) break;
            if (isFence(lines[k])) {
                k += 1;
                while (k < n and !isFence(lines[k])) {
                    if (matchCheckbox(lines[k])) |box| {
                        stats.total_boxes += 1;
                        if (box == 'x') stats.ticked_boxes += 1;
                    }
                    k += 1;
                }
                break;
            }
            k += 1;
        }

        i = @max(k, i + 1);
    }

    return stats;
}

/// `parse_roadmap(text)`.
pub fn parseRoadmap(allocator: std.mem.Allocator, text: []const u8) !Stats {
    const lines = try splitLines(allocator, text);
    defer allocator.free(lines);
    return parseLines(lines);
}

// -- the renderer ------------------------------------------------------------

/// `ticked / total * 100.0`, and exactly `0.0` (never a division by zero and
/// never `nan`) when there are no boxes.
pub fn percentOf(ticked: u32, total: u32) f64 {
    if (total == 0) return 0.0;
    const ticked_f: f64 = @floatFromInt(ticked);
    const total_f: f64 = @floatFromInt(total);
    return ticked_f / total_f * 100.0;
}

/// `value * 10` rounded to an integer the way CPython's `{:.1f}` rounds:
/// on the EXACT binary value of the double, ties to even. Zig's own float
/// formatter is not assumed to agree, because a tie is reachable here (a
/// double that is exactly `n.n5`, e.g. 12.25 from 49/400 boxes, must render
/// `12.2` and not `12.3`).
pub fn tenthsHalfEven(value: f64) u64 {
    if (!(value > 0.0)) return 0;
    const bits: u64 = @bitCast(value);
    const mantissa_bits: u64 = bits & ((@as(u64, 1) << 52) - 1);
    const exponent_bits: u32 = @intCast((bits >> 52) & 0x7FF);

    var mantissa: u128 = mantissa_bits;
    var exponent: i32 = -1074;
    if (exponent_bits != 0) {
        mantissa |= @as(u128, 1) << 52;
        exponent = @as(i32, @intCast(exponent_bits)) - 1075;
    }
    if (mantissa == 0) return 0;

    const numerator: u128 = mantissa * 10;
    if (exponent >= 0) {
        if (exponent > 40) return std.math.maxInt(u64);
        return @intCast(numerator << @intCast(exponent));
    }

    const shift: u32 = @intCast(-exponent);
    if (shift > 126) return 0;
    const quotient: u128 = numerator >> @intCast(shift);
    const remainder: u128 = numerator - (quotient << @intCast(shift));
    const half: u128 = @as(u128, 1) << @intCast(shift - 1);
    const round_up = remainder > half or (remainder == half and (quotient & 1) == 1);
    return @intCast(quotient + @as(u128, if (round_up) 1 else 0));
}

/// `f"{value:.1f}"` for a non-negative value, into `buffer`.
pub fn formatOneDecimal(buffer: []u8, value: f64) ![]const u8 {
    const scaled = tenthsHalfEven(value);
    return std.fmt.bufPrint(buffer, "{d}.{d}", .{ scaled / 10, scaled % 10 });
}

/// `render_summary`: the block INCLUDING both markers, so a substitution can
/// never lose them. The column padding is the predecessor's, byte for byte:
/// four spaces after `DONE:`, five after `WIP:`, one after `BLOCKED:`, four
/// after `TODO:`.
pub fn renderSummary(allocator: std.mem.Allocator, stats: Stats) ![]u8 {
    var percent_buffer: [32]u8 = undefined;
    const percent = try formatOneDecimal(
        &percent_buffer,
        percentOf(stats.ticked_boxes, stats.total_boxes),
    );
    return std.fmt.allocPrint(allocator, "{s}\n" ++
        "- Total drivers tracked: {d}\n" ++
        "- DONE:    {d}\n" ++
        "- WIP:     {d}\n" ++
        "- BLOCKED: {d}\n" ++
        "- TODO:    {d}\n" ++
        "- Checklist coverage: {d}/{d} boxes ticked ({s}%)\n" ++
        "{s}", .{
        begin_mark,
        stats.counts.total(),
        stats.counts.done,
        stats.counts.wip,
        stats.counts.blocked,
        stats.counts.todo,
        stats.ticked_boxes,
        stats.total_boxes,
        percent,
        end_mark,
    });
}

/// The one-line status paragraph both terminal modes print, minus the verb.
pub fn renderCensus(allocator: std.mem.Allocator, stats: Stats) ![]u8 {
    return std.fmt.allocPrint(
        allocator,
        "(drivers={d} DONE={d} WIP={d} BLOCKED={d} TODO={d} boxes={d}/{d})",
        .{
            stats.counts.total(),
            stats.counts.done,
            stats.counts.wip,
            stats.counts.blocked,
            stats.counts.todo,
            stats.ticked_boxes,
            stats.total_boxes,
        },
    );
}

// -- the substitution --------------------------------------------------------

/// Why a document cannot carry a generated summary.
pub const RewriteError = error{
    /// Neither marker, or only one of them, appears anywhere in the text.
    MissingMarkers,
    /// Both markers appear, but no END marker follows the first BEGIN.
    ///
    /// DELIBERATE DIVERGENCE from the predecessor, and the only one in this
    /// migration. `rewrite()` in Python guarded on `BEGIN_MARK not in text or
    /// END_MARK not in text`, then partitioned on the first BEGIN and on the
    /// first END *after* it. A file whose END sits BEFORE its BEGIN passed
    /// that guard, found no END in the remainder, and `post` became empty, so
    /// the rewrite silently TRUNCATED everything after the summary. Since the
    /// document is tracked certification evidence, this tool refuses instead:
    /// fail closed, exit 2, write nothing.
    EndBeforeBegin,
};

/// `rewrite(text, summary)`. Everything outside the markers is preserved
/// byte for byte, line endings included.
pub fn rewrite(allocator: std.mem.Allocator, text: []const u8, summary: []const u8) ![]u8 {
    const begin = std.mem.indexOf(u8, text, begin_mark) orelse return RewriteError.MissingMarkers;
    if (std.mem.indexOf(u8, text, end_mark) == null) return RewriteError.MissingMarkers;

    const pre = text[0..begin];
    const rest = text[begin + begin_mark.len ..];
    const end = std.mem.indexOf(u8, rest, end_mark) orelse return RewriteError.EndBeforeBegin;
    const post = rest[end + end_mark.len ..];

    return std.mem.concat(allocator, u8, &.{ pre, summary, post });
}
