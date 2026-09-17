//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure Markdown filtering for the `doxygen_md_filter` build tool (#858),
//! replacing the Python implementation this change deletes.
//!
//! Text in, text out: no file system and no process state, so both transforms
//! the published docs site depends on are provable without running a docs
//! build. Whether a link target exists is the one question this module cannot
//! answer on its own, so it asks a `Resolver` the caller supplies: the tool
//! backs that with the repository root, the tests back it with a fixed path
//! set.
//!
//! The two transforms, both skipped inside fenced code blocks where such text
//! is a literal example rather than a live link:
//!
//! 1. GitHub Actions status badges are removed, and a line left holding
//!    nothing but whitespace is dropped with them. The repository is private
//!    and the gh-pages site is not, so a badge URL answers 404 for every
//!    visitor and renders as a broken image.
//! 2. A link to `<dir>/README.md` becomes `@ref <dir>`. Doxygen folds each
//!    README into its directory page either way, but resolved from the raw
//!    `.md` target it emits the build machine's absolute path as the link
//!    tooltip, leaking a workspace path into public HTML. Only a target that
//!    provably exists in the repository is rewritten.

const std = @import("std");

/// Answers "is this repo-relative path an existing file?" for link rewriting.
///
/// A link is only rewritten when its target resolves to a file that is really
/// there, so the decision needs the repository; keeping it behind this
/// indirection is what keeps the transforms themselves pure.
pub const Resolver = struct {
    context: *const anyopaque,
    isFileFn: *const fn (context: *const anyopaque, path: []const u8) bool,

    pub fn isFile(self: Resolver, path: []const u8) bool {
        return self.isFileFn(self.context, path);
    }
};

/// Line boundaries honoured when splitting the input, terminators kept.
///
/// LF, CRLF and a lone CR, which is every terminator Markdown in this tree
/// uses. The wider set CPython's `str.splitlines` also splits on (form feed,
/// the C1 separators, U+2028) is deliberately NOT reproduced: those bytes
/// inside a Markdown page would be a defect of the page, and treating them as
/// line breaks silently reflows it.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn init(text: []const u8) LineIterator {
        return .{ .text = text };
    }

    /// The next line INCLUDING its terminator, or null at the end.
    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var cursor = start;
        while (cursor < self.text.len and self.text[cursor] != '\n' and self.text[cursor] != '\r') {
            cursor += 1;
        }
        if (cursor < self.text.len) {
            const is_crlf = self.text[cursor] == '\r' and
                cursor + 1 < self.text.len and
                self.text[cursor + 1] == '\n';
            cursor += if (is_crlf) 2 else 1;
        }
        self.index = cursor;
        return self.text[start..cursor];
    }
};

/// Whether `line` opens or closes a fenced code block.
///
/// Leading whitespace then three backticks or three tildes, matching the
/// filter this replaces. A fence toggles the block: the filter never parses
/// info strings, so an unbalanced fence leaves the rest of the page verbatim,
/// which is the safe direction for a link rewriter.
pub fn isFence(line: []const u8) bool {
    var index: usize = 0;
    while (index < line.len and std.ascii.isWhitespace(line[index])) index += 1;
    const rest = line[index..];
    return std.mem.startsWith(u8, rest, "```") or std.mem.startsWith(u8, rest, "~~~");
}

/// Whether `url` is a GitHub Actions workflow badge image.
///
/// The badge path comes first and `badge.svg` after it, so a link that merely
/// mentions one of the two is left alone.
pub fn isBadgeUrl(url: []const u8) bool {
    const marker = "/actions/workflows/";
    const start = std.mem.indexOf(u8, url, marker) orelse return false;
    return std.mem.indexOfPos(u8, url, start + marker.len, "badge.svg") != null;
}

/// Length of the Actions-badge image at the head of `text`, or null.
///
/// Recognises the plain image `![alt](url)` and the link-wrapped
/// `[![alt](url)](target)` form, which is how the badges are written in this
/// tree. Neither the alt text nor either URL may contain the delimiter that
/// ends it, so a match can never run past the end of its own construct.
pub fn badgeMatchLen(text: []const u8) ?usize {
    var cursor: usize = 0;
    if (text.len > 0 and text[0] == '[') cursor = 1;
    if (!std.mem.startsWith(u8, text[cursor..], "![")) return null;
    cursor += 2;

    const alt_len = std.mem.indexOfScalar(u8, text[cursor..], ']') orelse return null;
    cursor += alt_len + 1;
    if (cursor >= text.len or text[cursor] != '(') return null;
    cursor += 1;

    const url_len = std.mem.indexOfScalar(u8, text[cursor..], ')') orelse return null;
    if (!isBadgeUrl(text[cursor .. cursor + url_len])) return null;
    cursor += url_len + 1;

    if (std.mem.startsWith(u8, text[cursor..], "](")) {
        const target_start = cursor + 2;
        if (std.mem.indexOfScalar(u8, text[target_start..], ')')) |target_len| {
            cursor = target_start + target_len + 1;
        }
    }
    return cursor;
}

/// Append `line` to `out` with every Actions badge removed.
///
/// Returns whether anything was removed, which is what decides if a line left
/// blank is dropped: a line that was already blank stays.
pub fn stripBadges(out: *std.ArrayList(u8), line: []const u8) !bool {
    var index: usize = 0;
    var removed = false;
    while (index < line.len) {
        if (badgeMatchLen(line[index..])) |length| {
            index += length;
            removed = true;
            continue;
        }
        try out.append(line[index]);
        index += 1;
    }
    return removed;
}

/// Whether `text` is nothing but whitespace.
pub fn isBlank(text: []const u8) bool {
    for (text) |byte| {
        if (!std.ascii.isWhitespace(byte)) return false;
    }
    return true;
}

/// The repo-relative, `.`- and `..`-free form of a link target, or null.
///
/// Null means the target is not a rewrite candidate at all: it does not name a
/// README, it is external, absolute or anchored, or it climbs above the
/// repository root. `source_dir` is the repo-relative directory of the page
/// being filtered, empty at the root. The result is owned by the caller.
pub fn normalizeTarget(
    allocator: std.mem.Allocator,
    source_dir: []const u8,
    target: []const u8,
) !?[]u8 {
    if (!std.mem.endsWith(u8, target, "README.md")) return null;
    if (std.mem.startsWith(u8, target, "http://")) return null;
    if (std.mem.startsWith(u8, target, "https://")) return null;
    if (std.mem.startsWith(u8, target, "mailto:")) return null;
    if (std.mem.startsWith(u8, target, "/")) return null;
    if (std.mem.startsWith(u8, target, "#")) return null;

    var parts = std.ArrayList([]const u8).init(allocator);
    defer parts.deinit();

    for ([_][]const u8{ source_dir, target }) |segment_source| {
        var walk = std.mem.splitScalar(u8, segment_source, '/');
        while (walk.next()) |part| {
            if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
            if (std.mem.eql(u8, part, "..")) {
                if (parts.items.len == 0) return null; // escapes the repository root
                _ = parts.pop();
                continue;
            }
            try parts.append(part);
        }
    }

    return try std.mem.join(allocator, "/", parts.items);
}

/// The `@ref` replacement for a link target, or null to leave it untouched.
///
/// The target must normalise (above), name a file the resolver can see, and
/// live in a directory: the top-level README has no directory page of its own.
/// The result is owned by the caller.
pub fn refTarget(
    allocator: std.mem.Allocator,
    source_dir: []const u8,
    target: []const u8,
    resolver: Resolver,
) !?[]u8 {
    const normalized = try normalizeTarget(allocator, source_dir, target) orelse return null;
    defer allocator.free(normalized);

    if (!resolver.isFile(normalized)) return null;
    const separator = std.mem.lastIndexOfScalar(u8, normalized, '/') orelse return null;
    return try std.fmt.allocPrint(allocator, "@ref {s}", .{normalized[0..separator]});
}

/// Append `line` to `out`, rewriting every resolvable README link in it.
///
/// A Markdown link is `[text](target)` not preceded by `!`, where the target
/// holds neither whitespace nor a closing parenthesis. A link that is not
/// rewritten is copied through unchanged, and scanning resumes after it either
/// way, so a link can never be examined twice.
pub fn rewriteLinks(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(u8),
    line: []const u8,
    source_dir: []const u8,
    resolver: Resolver,
) !void {
    var index: usize = 0;
    while (index < line.len) {
        if (line[index] != '[' or (index > 0 and line[index - 1] == '!')) {
            try out.append(line[index]);
            index += 1;
            continue;
        }

        const text_len = std.mem.indexOfScalar(u8, line[index + 1 ..], ']') orelse {
            try out.append(line[index]);
            index += 1;
            continue;
        };
        const text_end = index + 1 + text_len; // the ']'
        if (text_end + 1 >= line.len or line[text_end + 1] != '(') {
            try out.append(line[index]);
            index += 1;
            continue;
        }

        const target_start = text_end + 2;
        var cursor = target_start;
        while (cursor < line.len and line[cursor] != ')' and
            !std.ascii.isWhitespace(line[cursor]))
        {
            cursor += 1;
        }
        if (cursor == target_start or cursor >= line.len or line[cursor] != ')') {
            try out.append(line[index]);
            index += 1;
            continue;
        }

        const link_text = line[index .. text_end + 1];
        const target = line[target_start..cursor];
        if (try refTarget(allocator, source_dir, target, resolver)) |ref| {
            defer allocator.free(ref);
            try out.appendSlice(link_text);
            try out.append('(');
            try out.appendSlice(ref);
            try out.append(')');
        } else {
            try out.appendSlice(line[index .. cursor + 1]);
        }
        index = cursor + 1;
    }
}

/// Filter a whole Markdown page: badges removed, README links rewritten.
///
/// `source_dir` is the repo-relative directory of the page, empty at the root.
/// The result is owned by the caller.
pub fn filterMarkdown(
    allocator: std.mem.Allocator,
    text: []const u8,
    source_dir: []const u8,
    resolver: Resolver,
) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();

    var stripped = std.ArrayList(u8).init(allocator);
    defer stripped.deinit();

    var in_fence = false;
    var lines = LineIterator.init(text);
    while (lines.next()) |line| {
        if (isFence(line)) {
            in_fence = !in_fence;
            try out.appendSlice(line);
            continue;
        }
        if (in_fence) {
            try out.appendSlice(line);
            continue;
        }

        stripped.clearRetainingCapacity();
        const removed = try stripBadges(&stripped, line);
        if (removed and isBlank(stripped.items)) continue; // the line held only a badge
        try rewriteLinks(allocator, &out, stripped.items, source_dir, resolver);
    }

    return try out.toOwnedSlice();
}
