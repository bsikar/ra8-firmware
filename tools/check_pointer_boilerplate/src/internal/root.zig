//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detector and scope algebra for the pointer-only comment gate (#858).
//!
//! Application and example definitions inherit their contracts from their
//! declarations, so the generated sentence
//! `/* see header for the documented contract. */` carries no information.
//! It was emitted repeatedly during the source-layout migration; this gate is
//! the narrow regression guard that keeps it out of `apps/` and `examples/`.
//!
//! Nothing here touches the file system, argv or a process: the census is
//! handed in and existence is asked of an injected `Resolver`, so every rule
//! below is provable with no repository on disk.

const std = @import("std");

/// Path prefixes the gate scopes itself to. Legacy library wording is
/// deliberately outside this guard.
pub const scoped_prefixes = [_][]const u8{ "apps/", "examples/" };

/// Source suffixes the gate reads, compared case-folded.
pub const source_suffixes = [_][]const u8{
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm",
};

/// Smallest scoped file set the gate will trust. Below this the census read
/// the wrong tree, and a sweep that saw almost nothing must never report a
/// clean tree for exactly the wrong reason.
pub const min_scoped_files: usize = 850;

/// Asking whether a census path is a present regular file.
pub const Resolver = struct {
    context: *const anyopaque,
    is_file_fn: *const fn (context: *const anyopaque, rel: []const u8) bool,

    pub fn isFile(self: Resolver, rel: []const u8) bool {
        return self.is_file_fn(self.context, rel);
    }
};

/// Fold one code point the way the detector's case-insensitive match does.
///
/// ASCII upper case folds to lower. The two non-ASCII code points Python's
/// `re.IGNORECASE` folds onto letters this pattern contains are folded too, so
/// a long s reads as `s` exactly as it did before the migration.
pub fn fold(codepoint: u21) u21 {
    if (codepoint >= 'A' and codepoint <= 'Z') return codepoint + 32;
    return switch (codepoint) {
        0x017F => 's', // LATIN SMALL LETTER LONG S
        0x212A => 'k', // KELVIN SIGN
        else => codepoint,
    };
}

/// Whether a code point is whitespace to the detector's `\s`.
///
/// This is Python's Unicode `\s` set. The line separators in it cannot reach
/// a line (the splitter already consumed them), but they are listed so the
/// predicate is the same rule rather than a convenient subset.
pub fn isRegexSpace(codepoint: u21) bool {
    return switch (codepoint) {
        0x09...0x0D, 0x1C...0x1F, 0x20, 0x85, 0xA0 => true,
        0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000 => true,
        else => false,
    };
}

/// A line as a code point cursor, so the matcher below never indexes bytes.
const Cursor = struct {
    view: std.unicode.Utf8View,
    iterator: std.unicode.Utf8Iterator,

    fn init(line: []const u8) !Cursor {
        const view = try std.unicode.Utf8View.init(line);
        return .{ .view = view, .iterator = view.iterator() };
    }

    fn peek(self: *Cursor) ?u21 {
        const save = self.iterator.i;
        defer self.iterator.i = save;
        return self.iterator.nextCodepoint();
    }

    fn next(self: *Cursor) ?u21 {
        return self.iterator.nextCodepoint();
    }

    fn atEnd(self: *Cursor) bool {
        return self.peek() == null;
    }

    /// Consume `\s*`.
    fn skipSpace(self: *Cursor) void {
        while (self.peek()) |codepoint| {
            if (!isRegexSpace(codepoint)) return;
            _ = self.next();
        }
    }

    /// Consume an ASCII literal, case-folded. Restores position on a miss.
    fn eat(self: *Cursor, literal: []const u8) bool {
        const save = self.iterator.i;
        for (literal) |want| {
            const got = self.next() orelse {
                self.iterator.i = save;
                return false;
            };
            if (fold(got) != fold(want)) {
                self.iterator.i = save;
                return false;
            }
        }
        return true;
    }
};

/// Whether one source line is the generated pointer-only comment.
///
/// The shape, whole-line and case-insensitive:
///
///     <ws> "/*" <ws> "see " ["the "] ["internal "]
///     "header for the documented contract." <ws> "*/" <ws> EOL
///
/// The spaces between words are single literal spaces, never `\s`, so
/// `see  header` does not match. A line that is not valid UTF-8 cannot be one
/// of these comments and answers false.
pub fn isBanned(line: []const u8) bool {
    var cursor = Cursor.init(line) catch return false;
    cursor.skipSpace();
    if (!cursor.eat("/*")) return false;
    cursor.skipSpace();
    if (!cursor.eat("see ")) return false;
    _ = cursor.eat("the ");
    _ = cursor.eat("internal ");
    if (!cursor.eat("header for the documented contract.")) return false;
    cursor.skipSpace();
    if (!cursor.eat("*/")) return false;
    cursor.skipSpace();
    return cursor.atEnd();
}

/// Whether a code point ends a line to `str.splitlines`.
pub fn isLineBreak(codepoint: u21) bool {
    return switch (codepoint) {
        '\n', 0x0B, 0x0C, '\r', 0x1C, 0x1D, 0x1E, 0x85, 0x2028, 0x2029 => true,
        else => false,
    };
}

/// `str.splitlines` over UTF-8 text: every break above, `\r\n` as one break,
/// and no trailing empty line after a final break.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn init(text: []const u8) LineIterator {
        return .{ .text = text };
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var view = std.unicode.Utf8View.initUnchecked(self.text[start..]);
        var iterator = view.iterator();
        while (iterator.nextCodepoint()) |codepoint| {
            if (!isLineBreak(codepoint)) continue;
            const width = std.unicode.utf8CodepointSequenceLength(codepoint) catch 1;
            const end = start + iterator.i - width;
            self.index = start + iterator.i;
            if (codepoint == '\r' and self.index < self.text.len and self.text[self.index] == '\n') {
                self.index += 1;
            }
            return self.text[start..end];
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

/// One-based line numbers carrying the banned comment, in order.
pub fn scanText(allocator: std.mem.Allocator, text: []const u8) ![]usize {
    var hits = std.ArrayList(usize).init(allocator);
    errdefer hits.deinit();
    var lines = LineIterator.init(text);
    var number: usize = 0;
    while (lines.next()) |line| {
        number += 1;
        if (isBanned(line)) try hits.append(number);
    }
    return hits.toOwnedSlice();
}

/// Render one finding as the gate has always printed it: `path:line`.
pub fn renderFinding(allocator: std.mem.Allocator, rel: []const u8, line: usize) ![]const u8 {
    return std.fmt.allocPrint(allocator, "{s}:{d}", .{ rel, line });
}

/// The suffix of a path's final component, `pathlib.PurePath.suffix`.
///
/// A leading dot never opens a suffix (`.clang-format` has none), and a name
/// ending in a dot has none either (`trailing.`).
pub fn pathSuffix(rel: []const u8) []const u8 {
    const name = std.fs.path.basename(rel);
    const dot = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    if (dot == 0) return "";
    if (dot + 1 == name.len) return "";
    return name[dot..];
}

/// Whether a path's suffix, case-folded, is one the gate reads.
pub fn hasSourceSuffix(rel: []const u8) bool {
    const suffix = pathSuffix(rel);
    if (suffix.len == 0) return false;
    var buffer: [8]u8 = undefined;
    if (suffix.len > buffer.len) return false;
    const lowered = std.ascii.lowerString(buffer[0..suffix.len], suffix);
    for (source_suffixes) |candidate| {
        if (std.mem.eql(u8, lowered, candidate)) return true;
    }
    return false;
}

/// Whether a census path sits under a scoped prefix.
pub fn isScopedPrefix(rel: []const u8) bool {
    for (scoped_prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return true;
    }
    return false;
}

fn lessThanPath(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.lessThan(u8, left, right);
}

/// The scoped, de-duplicated, sorted file set the sweep reads.
pub fn selectScoped(
    allocator: std.mem.Allocator,
    census: []const []const u8,
    resolver: Resolver,
) ![][]const u8 {
    var seen = std.StringHashMap(void).init(allocator);
    defer seen.deinit();
    var selected = std.ArrayList([]const u8).init(allocator);
    errdefer selected.deinit();
    for (census) |rel| {
        if (!isScopedPrefix(rel)) continue;
        if (!hasSourceSuffix(rel)) continue;
        if (!resolver.isFile(rel)) continue;
        if (seen.contains(rel)) continue;
        try seen.put(rel, {});
        try selected.append(rel);
    }
    const paths = try selected.toOwnedSlice();
    std.mem.sort([]const u8, paths, {}, lessThanPath);
    return paths;
}

/// Whether the scoped set is too small to be this tree.
pub fn scopeCollapsed(scoped: usize, floor: usize) bool {
    return scoped < floor;
}

/// Detector cases, both directions, carried over unchanged.
pub const selftest_cases = [_]struct {
    line: []const u8,
    expected: bool,
    label: []const u8,
}{
    .{
        .line = "/* see header for the documented contract. */",
        .expected = true,
        .label = "plain generated form fires",
    },
    .{
        .line = "/* See the internal header for the documented contract. */",
        .expected = true,
        .label = "internal-header generated form fires",
    },
    .{
        .line = "/* see header for full description */",
        .expected = false,
        .label = "legacy wording stays quiet",
    },
    .{
        .line = "/* See header for the documented contract -- bounded scan. */",
        .expected = false,
        .label = "an implementation-specific note stays quiet",
    },
    .{
        .line = "const char* text = \"see header for the documented contract.\";",
        .expected = false,
        .label = "a string literal stays quiet",
    },
};

/// Labels of the detector cases that did not answer as documented.
pub fn selftestFailures(allocator: std.mem.Allocator) ![][]const u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    errdefer failures.deinit();
    for (selftest_cases) |case| {
        const hits = try scanText(allocator, case.line);
        defer allocator.free(hits);
        if ((hits.len != 0) != case.expected) try failures.append(case.label);
    }
    return failures.toOwnedSlice();
}
