//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure transforms behind the `list_tests` host tool (#858), replacing the
//! Python implementation the migrating commit deletes.
//!
//! Nothing here opens a file or reads the process environment: glob matching,
//! `@brief` extraction, source decoding and row rendering are all functions of
//! their arguments, so the whole behaviour is provable with no repository on
//! disk. Directory walking is the one side effect, and it lives in `cli.zig`.

const std = @import("std");

/// Column the description starts in, matching the Python's `ljust(40)`.
pub const name_column_width: usize = 40;

/// Filename suffixes a test target may carry, in the order the Python
/// enumerated them (`TEST_SUFFIXES`).
pub const test_suffixes = [_][]const u8{ "c", "cpp" };

/// One discovered test target: the file stem and the description shown beside
/// it.
pub const Entry = struct {
    name: []const u8,
    description: []const u8,
};

/// Order entries by name only, so the sort stays the Python's
/// `tests.sort(key=lambda item: item[0])`: a stable sort on the name leaves
/// same-named entries in discovery order.
pub fn lessThanByName(_: void, left: Entry, right: Entry) bool {
    return std.mem.order(u8, left.name, right.name) == .lt;
}

/// The glob roots one category is searched under.
///
/// The four product-tier categories have fixed roots (`CATEGORY_PATTERNS`);
/// every other name is a directory under `tests/`, which is the only case that
/// has to build a string, hence the allocator.
pub fn searchPatterns(
    allocator: std.mem.Allocator,
    category: []const u8,
) ![]const []const u8 {
    if (std.mem.eql(u8, category, "shared")) {
        return try allocator.dupe([]const u8, &[_][]const u8{"apps/shared_libs/*/tests"});
    }
    if (std.mem.eql(u8, category, "host")) {
        return try allocator.dupe([]const u8, &[_][]const u8{"apps/host/*/tests"});
    }
    if (std.mem.eql(u8, category, "board")) {
        return try allocator.dupe([]const u8, &[_][]const u8{
            "apps/board/*/*/tests",
            "apps/board/*/tests",
        });
    }
    if (std.mem.eql(u8, category, "tools")) {
        return try allocator.dupe([]const u8, &[_][]const u8{"tools/*/tests"});
    }
    const joined = try std.fmt.allocPrint(allocator, "tests/{s}", .{category});
    return try allocator.dupe([]const u8, &[_][]const u8{joined});
}

/// True when one glob component matches one directory entry name.
///
/// `*` and `?` carry their `fnmatch` meanings and neither crosses a path
/// separator, because a component never contains one.
///
/// There is deliberately NO leading-dot rule. `glob.glob` hides names starting
/// with a dot; `pathlib.Path.glob`, which is what the deleted Python used, does
/// NOT, and a differential run over a tree holding `tools/.hidden/tests/` is
/// what settled it: the Python listed that target. Reinstating the `glob`
/// module's rule here would silently drop tests.
///
/// Character classes (`[abc]`) are not supported: no pattern this tool is
/// given has ever used one, and matching them literally is the safer failure.
pub fn componentMatches(pattern: []const u8, name: []const u8) bool {
    if (name.len == 0) return pattern.len == 0;
    return wildcardMatches(pattern, name);
}

fn wildcardMatches(pattern: []const u8, name: []const u8) bool {
    var pattern_index: usize = 0;
    var name_index: usize = 0;
    var star_index: ?usize = null;
    var star_name_index: usize = 0;
    while (name_index < name.len) {
        const literal = pattern_index < pattern.len and
            (pattern[pattern_index] == '?' or pattern[pattern_index] == name[name_index]);
        if (literal) {
            pattern_index += 1;
            name_index += 1;
        } else if (pattern_index < pattern.len and pattern[pattern_index] == '*') {
            star_index = pattern_index;
            star_name_index = name_index;
            pattern_index += 1;
        } else if (star_index) |star| {
            pattern_index = star + 1;
            star_name_index += 1;
            name_index = star_name_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.len and pattern[pattern_index] == '*') pattern_index += 1;
    return pattern_index == pattern.len;
}

/// True when a pattern component holds a wildcard and therefore has to be
/// matched against a directory listing rather than opened by name.
pub fn isWildcard(component: []const u8) bool {
    return std.mem.indexOfScalar(u8, component, '*') != null or
        std.mem.indexOfScalar(u8, component, '?') != null;
}

/// `pathlib.Path.stem`: the file name with its last suffix removed. A name
/// whose only dot is the first byte has no suffix and is returned whole.
pub fn stem(file_name: []const u8) []const u8 {
    const dot = std.mem.lastIndexOfScalar(u8, file_name, '.') orelse return file_name;
    if (dot == 0) return file_name;
    return file_name[0..dot];
}

/// The bytes CPython's `\s` class treats as whitespace, for ASCII input.
fn isSpace(byte: u8) bool {
    return switch (byte) {
        ' ', '\t', '\n', '\r', 0x0b, 0x0c => true,
        else => false,
    };
}

/// The `@brief` text carried by ONE line, or null.
///
/// Mirrors `re.search(r"@brief\s+(.*)", line)` applied to a line that still
/// carries its terminator, which is how the Python saw it: the tag has to be
/// followed by at least one whitespace byte, the capture stops at the line end
/// because `.` never matches a newline, and the result is stripped. A bare
/// `@brief` with nothing after it therefore yields an EMPTY description rather
/// than no match, and that is inherited behaviour, not an accident.
pub fn briefInLine(line: []const u8) ?[]const u8 {
    const tag = "@brief";
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, line, from, tag)) |at| {
        var cursor = at + tag.len;
        const start = cursor;
        while (cursor < line.len and isSpace(line[cursor])) cursor += 1;
        if (cursor > start) {
            var end = cursor;
            while (end < line.len and line[end] != '\n') end += 1;
            return std.mem.trim(u8, line[cursor..end], " \t\n\r\x0b\x0c");
        }
        from = at + 1;
    }
    return null;
}

/// The first `@brief` description in a decoded source file, or null.
///
/// Line by line, stopping at the first line that carries one, exactly as the
/// Python's `for source_line in handle: ... break` did.
pub fn briefIn(text: []const u8) ?[]const u8 {
    var offset: usize = 0;
    while (offset < text.len) {
        const newline = std.mem.indexOfScalarPos(u8, text, offset, '\n');
        const end = if (newline) |index| index + 1 else text.len;
        if (briefInLine(text[offset..end])) |brief| return brief;
        offset = end;
    }
    return null;
}

/// Decode a source file the way `open(encoding="utf-8", errors="ignore")` did.
///
/// Two behaviours matter and both are load bearing. Universal newlines mean a
/// `\r\n` pair and a lone `\r` both arrive as `\n`, so a CRLF source yields the
/// same description as an LF one. `errors="ignore"` drops bytes that are not
/// valid UTF-8 rather than failing, so a stray byte in a comment cannot make
/// the listing fail.
pub fn decodeIgnoringInvalid(allocator: std.mem.Allocator, bytes: []const u8) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < bytes.len) {
        const byte = bytes[index];
        if (byte == '\r') {
            try out.append('\n');
            index += 1;
            if (index < bytes.len and bytes[index] == '\n') index += 1;
            continue;
        }
        const length = std.unicode.utf8ByteSequenceLength(byte) catch {
            index += 1;
            continue;
        };
        if (index + length > bytes.len) {
            index += 1;
            continue;
        }
        _ = std.unicode.utf8Decode(bytes[index .. index + length]) catch {
            index += 1;
            continue;
        };
        try out.appendSlice(bytes[index .. index + length]);
        index += length;
    }
    return try out.toOwnedSlice();
}

/// `<stem> unit tests`, the description used when a source carries no
/// `@brief`.
pub fn defaultDescription(allocator: std.mem.Allocator, name: []const u8) ![]u8 {
    return try std.fmt.allocPrint(allocator, "{s} unit tests", .{name});
}

/// ASCII-only `str.lower`. Every category name this tool is given is ASCII,
/// and a non-ASCII one is passed through unchanged rather than folded by a
/// rule CPython's Unicode tables would disagree with.
pub fn asciiLower(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    const out = try allocator.alloc(u8, text.len);
    for (text, 0..) |byte, index| out[index] = std.ascii.toLower(byte);
    return out;
}

/// ASCII-only `str.upper`, the counterpart of `asciiLower`.
pub fn asciiUpper(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    const out = try allocator.alloc(u8, text.len);
    for (text, 0..) |byte, index| out[index] = std.ascii.toUpper(byte);
    return out;
}

/// The banner above the listing, followed by the blank line `print` added.
pub fn writeHeader(
    writer: anytype,
    allocator: std.mem.Allocator,
    category: []const u8,
    count: usize,
) !void {
    const upper = try asciiUpper(allocator, category);
    defer allocator.free(upper);
    try writer.print(
        "== {s} TESTS ({d}) -- local: just tests::local {s} | container: just tests::devcontainer {s}\n\n",
        .{ upper, count, category, category },
    );
}

/// One listing row: two leading spaces, the name padded to
/// `name_column_width`, one space, the description.
pub fn writeRow(writer: anytype, entry: Entry) !void {
    try writer.print("  {s}", .{entry.name});
    if (entry.name.len < name_column_width) {
        try writer.writeByteNTimes(' ', name_column_width - entry.name.len);
    }
    try writer.print(" {s}\n", .{entry.description});
}
