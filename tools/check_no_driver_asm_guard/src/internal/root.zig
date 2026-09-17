//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Comment stripper, conditional walk and inline-asm detector for the HAL
//! driver asm-guard gate (#858).
//!
//! Every function here is pure: text in, findings out, no file system and no
//! argv, so the detector is provable with no repository on disk. The four
//! regular expressions the predecessor carried
//! (`^\s*#\s*(if|ifdef|ifndef)\b(.*)$`, `^\s*#\s*elif\b(.*)$`,
//! `^\s*#\s*endif\b` and `(?<![A-Za-z0-9_])__asm(__)?(?![A-Za-z0-9_])`) are
//! reproduced here as hand-written matchers, because Zig's standard library
//! has no regular expressions and this gate's meaning lives in the exact
//! match.
//!
//! Two asymmetries in those patterns are inherited on purpose rather than
//! tidied: the directive keywords end in `\b`, which is Unicode-aware, so a
//! letter outside ASCII after `#endif` means the line is NOT a directive;
//! the asm pattern spells its boundaries as explicit ASCII classes, so the
//! same letter beside `__asm` does NOT suppress a finding.

const std = @import("std");
const word_chars = @import("word_chars.zig");

/// The build flag whose conditionals this gate governs.
pub const off_target = "RA8_OFF_TARGET";

/// The seam a guarded primitive belongs on, named in every finding.
pub const seam_header = "libs/ra8_hal/inc/ra8_hw_intrinsics.h";

/// One decoded code point and the bytes it occupies.
pub const Char = struct {
    code_point: u21,
    len: usize,
};

/// Decode the code point at `index`, or null at the end of the text.
///
/// A byte sequence that is not valid UTF-8 decodes as the single byte, so a
/// matcher cannot walk off the end of a malformed source; the caller rejects
/// such a source before scanning it, exactly as the predecessor's strict
/// `read_text(encoding="utf-8")` did.
pub fn charAt(text: []const u8, index: usize) ?Char {
    if (index >= text.len) return null;
    const first = text[index];
    if (first < 0x80) return .{ .code_point = first, .len = 1 };
    const len = std.unicode.utf8ByteSequenceLength(first) catch return .{ .code_point = first, .len = 1 };
    if (index + len > text.len) return .{ .code_point = first, .len = 1 };
    const decoded = std.unicode.utf8Decode(text[index .. index + len]) catch
        return .{ .code_point = first, .len = 1 };
    return .{ .code_point = decoded, .len = len };
}

/// Whether `cp` is a word character, as Python's `\w` and therefore `\b` see it.
pub fn isWordChar(cp: u21) bool {
    return word_chars.isWordChar(cp);
}

/// Whether `cp` is whitespace, as Python's `\s` and `str.strip` see it.
pub fn isPythonSpace(cp: u21) bool {
    return switch (cp) {
        0x09...0x0d, 0x1c...0x1f, 0x20, 0x85, 0xa0, 0x1680 => true,
        0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000 => true,
        else => false,
    };
}

/// Whether `cp` ends a line, as `str.splitlines` sees it.
///
/// Carriage returns are folded into `\n` before the split, so CRLF counts
/// once; the rest of the set is what makes a form feed mid-file shift every
/// later line number, which is why it is carried rather than approximated.
pub fn isLineBreak(cp: u21) bool {
    return switch (cp) {
        0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x85, 0x2028, 0x2029 => true,
        else => false,
    };
}

/// Fold CRLF and a lone CR into LF, the way Python's text mode reads a file.
pub fn normalizeTerminators(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, text.len);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < text.len) {
        const byte = text[index];
        if (byte == '\r') {
            try out.append('\n');
            index += if (index + 1 < text.len and text[index + 1] == '\n') 2 else 1;
            continue;
        }
        try out.append(byte);
        index += 1;
    }
    return out.toOwnedSlice();
}

/// Walk a text as `str.splitlines()` does: no terminators, no trailing empty
/// line after a final break.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn init(text: []const u8) LineIterator {
        return .{ .text = text };
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var cursor = start;
        while (charAt(self.text, cursor)) |ch| {
            if (isLineBreak(ch.code_point)) {
                self.index = cursor + ch.len;
                return self.text[start..cursor];
            }
            cursor += ch.len;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

/// Blank out C `/* */` and `//` comments, preserving the line count.
///
/// Preprocessor directives never live inside comments, so a line-preserving
/// strip lets the conditional walk and the asm scan share one clean view
/// without a false positive from a doc block that mentions `__asm__`. String
/// literals are NOT understood, inherited as-is: a `//` inside one truncates
/// the line here just as it did before.
pub fn stripComments(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer out.deinit();
    var in_block = false;
    var lines = LineIterator.init(text);
    while (lines.next()) |line| {
        var buffer = std.ArrayList(u8).init(allocator);
        errdefer buffer.deinit();
        var index: usize = 0;
        while (index < line.len) {
            const pair = line[index..@min(index + 2, line.len)];
            if (in_block) {
                if (std.mem.eql(u8, pair, "*/")) {
                    in_block = false;
                    index += 2;
                } else {
                    index += 1;
                }
                continue;
            }
            if (std.mem.eql(u8, pair, "/*")) {
                in_block = true;
                index += 2;
                continue;
            }
            if (std.mem.eql(u8, pair, "//")) break;
            try buffer.append(line[index]);
            index += 1;
        }
        try out.append(try buffer.toOwnedSlice());
    }
    return out.toOwnedSlice();
}

/// Index of the first character that is not Python whitespace, from `start`.
fn skipSpace(line: []const u8, start: usize) usize {
    var index = start;
    while (charAt(line, index)) |ch| {
        if (!isPythonSpace(ch.code_point)) break;
        index += ch.len;
    }
    return index;
}

/// Whether a `\b` sits at `index`, given the character before it is a word one.
fn boundaryAfter(line: []const u8, index: usize) bool {
    const ch = charAt(line, index) orelse return true;
    return !isWordChar(ch.code_point);
}

/// Match `^\s*#\s*<keyword>\b` for the first keyword that fits, returning the
/// rest of the line (the predecessor's captured group), or null.
///
/// The keywords are tried in the order the alternation listed them, which is
/// what makes `#ifdef` a directive and `#ifdefx` an ordinary line.
pub fn directiveRest(line: []const u8, keywords: []const []const u8) ?[]const u8 {
    var index = skipSpace(line, 0);
    if (index >= line.len or line[index] != '#') return null;
    index = skipSpace(line, index + 1);
    for (keywords) |keyword| {
        if (line.len - index < keyword.len) continue;
        if (!std.mem.eql(u8, line[index .. index + keyword.len], keyword)) continue;
        if (!boundaryAfter(line, index + keyword.len)) continue;
        return line[index + keyword.len ..];
    }
    return null;
}

/// Rest of an opening conditional line, or null when the line is not one.
pub fn matchIf(line: []const u8) ?[]const u8 {
    return directiveRest(line, &.{ "if", "ifdef", "ifndef" });
}

/// Rest of an `#elif` line, or null when the line is not one.
pub fn matchElif(line: []const u8) ?[]const u8 {
    return directiveRest(line, &.{"elif"});
}

/// Whether the line closes a conditional.
pub fn matchEndif(line: []const u8) bool {
    return directiveRest(line, &.{"endif"}) != null;
}

/// Whether `byte` is in the asm pattern's explicit ASCII boundary class.
fn isAsciiWord(byte: u8) bool {
    return (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
        (byte >= '0' and byte <= '9') or byte == '_';
}

/// Whether the line contains a bare `__asm` / `__asm__` statement keyword.
///
/// The optional `__` suffix is greedy and cannot be given back usefully: a
/// word character after `__asm` is either consumed by the suffix or lands on
/// the lookahead, so `__asm__x` and `__asmx` are both quiet.
pub fn hasAsm(line: []const u8) bool {
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, line, from, "__asm")) |position| {
        from = position + 1;
        if (position != 0 and isAsciiWord(line[position - 1])) continue;
        var after = position + "__asm".len;
        if (after + 2 <= line.len and std.mem.eql(u8, line[after .. after + 2], "__")) after += 2;
        if (after >= line.len or !isAsciiWord(line[after])) return true;
    }
    return false;
}

/// Trim Python whitespace from both ends, as `str.strip()` does.
pub fn strip(text: []const u8) []const u8 {
    const start = skipSpace(text, 0);
    var end = start;
    var cursor = start;
    while (charAt(text, cursor)) |ch| {
        cursor += ch.len;
        if (!isPythonSpace(ch.code_point)) end = cursor;
    }
    return text[start..end];
}

/// One guarded asm statement, at its line number in the comment-stripped view.
pub const Finding = struct {
    line_no: usize,
    /// The comment-stripped line, reported verbatim after stripping.
    line: []const u8,
};

/// Whether any open conditional region references the off-target flag.
fn anyGuarded(stack: []const bool) bool {
    for (stack) |guarded| {
        if (guarded) return true;
    }
    return false;
}

/// Findings for one translation unit's text, already newline-normalised.
///
/// `#else` deliberately leaves the frame's flag alone: both branches of a
/// `#ifdef RA8_OFF_TARGET` are off-target-conditioned regions. A directive
/// line is never asm-scanned, so asm sharing a line with `#if` stays quiet.
pub fn scanText(allocator: std.mem.Allocator, text: []const u8) ![]Finding {
    const lines = try stripComments(allocator, text);
    var stack = std.ArrayList(bool).init(allocator);
    defer stack.deinit();
    var findings = std.ArrayList(Finding).init(allocator);
    errdefer findings.deinit();

    for (lines, 1..) |line, line_no| {
        if (matchIf(line)) |rest| {
            try stack.append(std.mem.indexOf(u8, rest, off_target) != null);
            continue;
        }
        if (matchElif(line)) |rest| {
            if (stack.items.len != 0) {
                const top = stack.items.len - 1;
                stack.items[top] = stack.items[top] or std.mem.indexOf(u8, rest, off_target) != null;
            }
            continue;
        }
        if (matchEndif(line)) {
            if (stack.items.len != 0) _ = stack.pop();
            continue;
        }
        if (hasAsm(line) and anyGuarded(stack.items)) {
            try findings.append(.{ .line_no = line_no, .line = line });
        }
    }
    return findings.toOwnedSlice();
}

/// Render one finding exactly as the gate has always reported it.
pub fn renderFinding(
    allocator: std.mem.Allocator,
    rel: []const u8,
    finding: Finding,
) ![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "{s}:{d}: inline asm '{s}' sits inside a {s} conditional -- route it through {s} instead",
        .{ rel, finding.line_no, strip(finding.line), off_target, seam_header },
    );
}

/// The guarded fixture the detector must fire on, in both branches.
pub const selftest_bad =
    "#ifdef RA8_OFF_TARGET\n" ++
    "void f(void) { __asm(\"nop\"); }\n" ++
    "#else\n" ++
    "void g(void) { __asm__(\"wfi\"); }\n" ++
    "#endif\n";

/// The clean fixture: a seam call, and prose that merely names the keyword.
pub const selftest_good =
    "// __asm__(\"nop\") under RA8_OFF_TARGET is prose\n" ++
    "void f(void) { ra8_hw_wfi(); }\n";

/// How many findings the guarded fixture must produce.
pub const selftest_bad_findings = 2;

/// One selftest case: did it hold, and what does it prove.
pub const SelftestCase = struct {
    passed: bool,
    label: []const u8,
};

/// Run both detector directions over the in-memory fixtures.
pub fn selftestCases(allocator: std.mem.Allocator) ![2]SelftestCase {
    const bad = try scanText(allocator, selftest_bad);
    const good = try scanText(allocator, selftest_good);
    return .{
        .{
            .passed = bad.len == selftest_bad_findings,
            .label = "asm in both off-target branches fires",
        },
        .{
            .passed = good.len == 0,
            .label = "shared seam calls and comment lookalikes stay quiet",
        },
    };
}
