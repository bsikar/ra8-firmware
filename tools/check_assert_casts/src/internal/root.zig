//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detection algebra for the redundant-cast gate on TEST_ASSERT_EQ (#858).
//!
//! TEST_ASSERT_EQ widens both arguments to int64_t internally, so an OUTER
//! cast is redundant, and an `(int)` applied to a uint32_t enum truncates to
//! 32-bit signed before that widening, which can pass a comparison that
//! should fail. Only a cast in LEADING position counts: deeper in the
//! expression a cast is usually load bearing.
//!
//! Every function here takes text and returns a decision. No file system, no
//! process state, no argv, so the contract is provable with no repo on disk.

const std = @import("std");

/// The macro whose arguments the gate inspects, including its open paren.
pub const macro = "TEST_ASSERT_EQ(";

/// Longest snippet echoed back per finding, counted in characters, not bytes.
pub const snippet_limit = 60;

/// Characters both Python `str.strip()` and its `\s` class remove, preserved
/// because the findings this replaces were produced with exactly that set.
pub const whitespace = " \t\n\x0b\x0c\r\x1c\x1d\x1e\x1f";

/// Every spelling `u?int(?:8|16|32|64)?_t|int|size_t|ssize_t` can produce.
/// Membership is only ever tested with a closing paren required next, so the
/// alternation order the regex engine used cannot change an outcome.
pub const cast_types = [_][]const u8{
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "uint_t",
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
    "int_t",
    "ssize_t",
    "size_t",
    "int",
};

/// Which of the macro's two arguments a finding is about.
pub const Argument = enum { first, second };

/// One redundant cast, located and quoted.
pub const Finding = struct {
    /// One-based line of the macro invocation.
    line: usize,
    /// The argument that opened with the cast.
    argument: Argument,
    /// Stripped, truncated argument text borrowed from the scanned text.
    snippet: []const u8,
};

/// Report whether `byte` is one of the whitespace characters the gate trims.
pub fn isSpace(byte: u8) bool {
    return std.mem.indexOfScalar(u8, whitespace, byte) != null;
}

/// Translate CRLF and lone CR to LF, the way Python text mode read sources.
///
/// The gate reported line numbers counted after that translation, so a CRLF
/// source has to collapse here or every finding below the first CR shifts.
pub fn normalizeTerminators(allocator: std.mem.Allocator, raw: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, raw.len);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < raw.len) : (index += 1) {
        if (raw[index] != '\r') {
            try out.append(raw[index]);
            continue;
        }
        try out.append('\n');
        if (index + 1 < raw.len and raw[index + 1] == '\n') index += 1;
    }
    return out.toOwnedSlice();
}

/// Replace every non-ASCII byte with U+FFFD, as `errors="replace"` did.
///
/// This matters for the echoed snippet: the old gate truncated at 60
/// CHARACTERS of a decoded string, so one stray byte counted once, not three.
pub fn decodeAsciiReplace(allocator: std.mem.Allocator, raw: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, raw.len);
    errdefer out.deinit();
    for (raw) |byte| {
        if (byte < 0x80) {
            try out.append(byte);
        } else {
            try out.appendSlice("\u{FFFD}");
        }
    }
    return out.toOwnedSlice();
}

/// Trim the gate's whitespace set from both ends.
pub fn stripEnds(text: []const u8) []const u8 {
    return std.mem.trim(u8, text, whitespace);
}

/// Return the first `limit` CHARACTERS of valid UTF-8 `text`.
pub fn sliceChars(text: []const u8, limit: usize) []const u8 {
    var count: usize = 0;
    var index: usize = 0;
    while (index < text.len and count < limit) : (count += 1) {
        const byte = text[index];
        const width: usize = if (byte < 0x80)
            1
        else if (byte & 0xE0 == 0xC0)
            2
        else if (byte & 0xF0 == 0xE0)
            3
        else
            4;
        index = @min(text.len, index + width);
    }
    return text[0..index];
}

/// Report whether `text` OPENS with an integer cast, whitespace allowed first.
pub fn hasLeadingCast(text: []const u8) bool {
    var index: usize = 0;
    while (index < text.len and isSpace(text[index])) : (index += 1) {}
    if (index >= text.len or text[index] != '(') return false;
    index += 1;
    for (cast_types) |name| {
        const end = index + name.len;
        if (end < text.len and std.mem.eql(u8, text[index..end], name) and text[end] == ')') return true;
    }
    return false;
}

/// Index of the paren closing the one already open at `start`.
///
/// An unbalanced invocation yields the last index rather than an error, which
/// is what the scan wants: the malformed call is skipped, the compiler names
/// it better than this gate could.
pub fn findCloseParen(text: []const u8, start: usize) usize {
    var depth: usize = 1;
    var index = start;
    while (index < text.len and depth != 0) : (index += 1) {
        switch (text[index]) {
            '(' => depth += 1,
            ')' => depth -= 1,
            else => {},
        }
    }
    return index -| 1;
}

/// Index of the first comma at bracket depth zero, or null when there is none.
pub fn splitAtTopLevelComma(inner: []const u8) ?usize {
    var depth: i64 = 0;
    for (inner, 0..) |char, index| {
        switch (char) {
            '(', '[', '{' => depth += 1,
            ')', ']', '}' => depth -= 1,
            ',' => if (depth == 0) return index,
            else => {},
        }
    }
    return null;
}

/// One-based line number of byte `index`.
pub fn lineOf(text: []const u8, index: usize) usize {
    return 1 + std.mem.count(u8, text[0..@min(index, text.len)], "\n");
}

/// Every leading-cast violation in `text`, in file order.
///
/// An invocation with no depth-zero comma is SKIPPED rather than reported: it
/// is a syntax error, not a cast, and reporting it here would bury the real
/// diagnostic. Snippets borrow from `text`, so the caller must keep it alive.
pub fn scanText(allocator: std.mem.Allocator, text: []const u8) ![]Finding {
    var found = std.ArrayList(Finding).init(allocator);
    errdefer found.deinit();
    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, text, cursor, macro)) |start| {
        const inner_start = start + macro.len;
        const close = findCloseParen(text, inner_start);
        const inner = if (close <= inner_start) text[0..0] else text[inner_start..close];
        if (splitAtTopLevelComma(inner)) |split| {
            const line = lineOf(text, start);
            const first = inner[0..split];
            const second = inner[split + 1 ..];
            if (hasLeadingCast(first)) try found.append(.{
                .line = line,
                .argument = .first,
                .snippet = sliceChars(stripEnds(first), snippet_limit),
            });
            if (hasLeadingCast(second)) try found.append(.{
                .line = line,
                .argument = .second,
                .snippet = sliceChars(stripEnds(second), snippet_limit),
            });
        }
        cursor = close + 1;
    }
    return found.toOwnedSlice();
}

/// Render one finding as the `path:line: message` row the gate has always
/// printed, so an editor or CI annotation parsing it keeps working.
pub fn renderFinding(allocator: std.mem.Allocator, path: []const u8, finding: Finding) ![]u8 {
    return switch (finding.argument) {
        .first => std.fmt.allocPrint(
            allocator,
            "{s}:{d}: cast in first arg of TEST_ASSERT_EQ: {s}{s}...",
            .{ path, finding.line, macro, finding.snippet },
        ),
        .second => std.fmt.allocPrint(
            allocator,
            "{s}:{d}: cast in second arg of TEST_ASSERT_EQ: ...{s}",
            .{ path, finding.line, finding.snippet },
        ),
    };
}

/// Normalise a path the way the old gate's `str(Path(raw))` did.
///
/// Findings quote the path as given on argv, so `./x.c` printed as `x.c`. A
/// compiled tool has no pathlib, and the rows are compared by tooling, so the
/// collapse of `.` components and repeated separators is reproduced here.
pub fn normalizePath(allocator: std.mem.Allocator, raw: []const u8) ![]u8 {
    var prefix: []const u8 = "";
    var rest = raw;
    if (std.mem.startsWith(u8, raw, "//") and !std.mem.startsWith(u8, raw, "///")) {
        prefix = "//";
        rest = raw[2..];
    } else if (std.mem.startsWith(u8, raw, "/")) {
        prefix = "/";
        rest = raw[1..];
    }
    var parts = std.ArrayList([]const u8).init(allocator);
    defer parts.deinit();
    var iterator = std.mem.splitScalar(u8, rest, '/');
    while (iterator.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
        try parts.append(part);
    }
    if (parts.items.len == 0) return allocator.dupe(u8, if (prefix.len == 0) "." else prefix);
    const joined = try std.mem.join(allocator, "/", parts.items);
    if (prefix.len == 0) return joined;
    defer allocator.free(joined);
    return std.mem.concat(allocator, u8, &.{ prefix, joined });
}
