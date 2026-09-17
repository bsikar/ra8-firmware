//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure decisions behind the `check_since_version` host tool (#858),
//! replacing `scripts/checks/check-since-version.py`, which the migrating
//! commit deletes.
//!
//! Nothing here opens a file, spawns a process or reads the environment. The
//! two halves of the gate are functions of text: the PRESENCE half asks
//! whether a public `ra8_*` declaration has an `@since` tag within the
//! previous 30 lines, and the VALUE half asks whether every `@since` value in
//! a source file equals the one string in `VERSION`. Discovery and the file
//! system live in `cli.zig`.
//!
//! Zig has no regular expressions, so the three patterns the Python compiled
//! are hand-written matchers here. They are written to the engine's rules,
//! not to what the patterns look like, and the places where that is
//! observable are commented at the matcher.

const std = @import("std");
const char_classes = @import("char_classes.zig");

/// Name the tool reports itself as. The Python printed its own filename;
/// this is the same line with the new name, since the file it named is gone.
pub const tool_name = "check_since_version";

/// Suffixes whose `@since` VALUES are checked. `SOURCE_SUFFIXES` verbatim.
pub const source_suffixes = [_][]const u8{ ".c", ".h", ".cpp", ".hpp" };

/// How many lines above a declaration are searched for its `@since` tag.
pub const lookback_lines: usize = 30;

/// Vendored SOUP and generated tables, from `lint_targets.EXCLUDED_PREFIXES`.
pub const excluded_prefixes = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "tools/vela/generated/",
};

/// Vendored C that is SOUP for the only language these suffixes imply, from
/// `lint_targets.LANGUAGE_EXCLUDED_PREFIXES["c"]`.
pub const c_excluded_prefixes = [_][]const u8{"port/threadx/"};

/// Top-level roots under which a build tree legitimately appears.
pub const build_tree_roots = [_][]const u8{
    "docs", "examples", "local-poc", "port", "tests", "tools", "apps",
};

/// Directory names owned by a tool, matched at any depth.
pub const tool_output_dir_names = [_][]const u8{
    ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules",
};

/// `lint_targets.TRACKED_FLOOR`: a tree this size cannot legitimately
/// enumerate to a handful of files, and a collapsed enumeration must fail
/// rather than read as a clean tree.
pub const tracked_floor: usize = 1000;

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------

/// One decoded code point. A malformed byte decodes as U+FFFD of length 1;
/// callers validate the whole text as UTF-8 first (the Python decoded
/// strictly and skipped a file it could not decode), so that fallback is
/// unreachable for real inputs and exists only to keep this total.
pub const Decoded = struct { code_point: u21, len: usize };

pub fn decodeAt(text: []const u8, index: usize) Decoded {
    const len = std.unicode.utf8ByteSequenceLength(text[index]) catch
        return .{ .code_point = 0xFFFD, .len = 1 };
    if (index + len > text.len) return .{ .code_point = 0xFFFD, .len = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + len]) catch
        return .{ .code_point = 0xFFFD, .len = 1 };
    return .{ .code_point = code_point, .len = len };
}

/// `\s` for a CPython str pattern, which is also `str.isspace`.
pub fn isSpace(code_point: u21) bool {
    return char_classes.inTable(&char_classes.space_intervals, code_point);
}

/// `\w` for a CPython str pattern.
pub fn isWord(code_point: u21) bool {
    return char_classes.inTable(&char_classes.word_intervals, code_point);
}

/// Index just past the run of `\s` starting at `index` (a greedy `\s*`).
pub fn skipSpaces(text: []const u8, index: usize) usize {
    var at = index;
    while (at < text.len) {
        const decoded = decodeAt(text, at);
        if (!isSpace(decoded.code_point)) break;
        at += decoded.len;
    }
    return at;
}

/// Index just past the run of `\w` starting at `index` (a greedy `\w*`).
pub fn skipWord(text: []const u8, index: usize) usize {
    var at = index;
    while (at < text.len) {
        const decoded = decodeAt(text, at);
        if (!isWord(decoded.code_point)) break;
        at += decoded.len;
    }
    return at;
}

fn skipAsciiDigits(text: []const u8, index: usize) usize {
    var at = index;
    while (at < text.len and text[at] >= '0' and text[at] <= '9') at += 1;
    return at;
}

/// `str.strip()`: the Unicode whitespace set, not `re`'s ASCII subset.
pub fn pythonStrip(text: []const u8) []const u8 {
    var start: usize = 0;
    while (start < text.len) {
        const decoded = decodeAt(text, start);
        if (!isSpace(decoded.code_point)) break;
        start += decoded.len;
    }
    var end = text.len;
    while (end > start) {
        var probe = end - 1;
        while (probe > start and (text[probe] & 0xC0) == 0x80) probe -= 1;
        const decoded = decodeAt(text, probe);
        if (!isSpace(decoded.code_point)) break;
        end = probe;
    }
    return text[start..end];
}

/// `re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", text)`.
pub fn isSemver(text: []const u8) bool {
    var at = skipAsciiDigits(text, 0);
    if (at == 0) return false;
    var group: usize = 0;
    while (group < 2) : (group += 1) {
        if (at >= text.len or text[at] != '.') return false;
        const next = skipAsciiDigits(text, at + 1);
        if (next == at + 1) return false;
        at = next;
    }
    return at == text.len;
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

/// `str.splitlines()`, which breaks on far more than `\n`: CR, LF, CRLF, VT,
/// FF, FS, GS, RS, NEL, LINE SEPARATOR and PARAGRAPH SEPARATOR. Line NUMBERS
/// in this gate's output come from that split, so a header using any of them
/// numbers its problems the way the Python did.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        while (self.index < self.text.len) {
            const decoded = decodeAt(self.text, self.index);
            const break_len = lineBreakLength(self.text, self.index, decoded);
            if (break_len > 0) {
                const line = self.text[start..self.index];
                self.index += break_len;
                return line;
            }
            self.index += decoded.len;
        }
        return self.text[start..self.index];
    }
};

fn lineBreakLength(text: []const u8, index: usize, decoded: Decoded) usize {
    switch (decoded.code_point) {
        '\r' => {
            if (index + 1 < text.len and text[index + 1] == '\n') return 2;
            return 1;
        },
        '\n', 0x0B, 0x0C, 0x1C, 0x1D, 0x1E, 0x85, 0x2028, 0x2029 => return decoded.len,
        else => return 0,
    }
}

pub fn splitLines(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    var lines = std.ArrayList([]const u8).init(allocator);
    errdefer lines.deinit();
    var iterator = LineIterator{ .text = text };
    while (iterator.next()) |line| try lines.append(line);
    return lines.toOwnedSlice();
}

// ---------------------------------------------------------------------------
// The three patterns
// ---------------------------------------------------------------------------

/// `PUBLIC_DECL.match(line)`, returning the captured symbol.
///
/// The pattern is
/// `^(?:\[\[nodiscard\]\]\s+)?(?:static\s+inline\s+)?\s*ra8_\w+(?:\s*\*)?\s+(ra8_\w+)\s*\(`
/// and two of its behaviours are inherited deliberately.
///
/// `ra8_err_t *ra8_foo(` does NOT match. The `(?:\s*\*)` group can only end
/// on the `*`, and the `\s+` after it then has nothing to consume; dropping
/// the group leaves `\s+` matching the space and `ra8_` facing `*`. The
/// Python rejected that spelling and so does this, rather than quietly
/// widening the gate.
///
/// Backtracking inside `(?:\s*\*)` needs no search: every position strictly
/// inside the whitespace run holds a whitespace character, so only the run's
/// end can be the `*`. Trying the group there and then trying without it is
/// the whole of the engine's backtracking here.
pub fn matchPublicDecl(line: []const u8) ?[]const u8 {
    const nodiscard = "[[nodiscard]]";
    if (std.mem.startsWith(u8, line, nodiscard)) {
        const after = skipSpaces(line, nodiscard.len);
        if (after > nodiscard.len) {
            if (matchAfterAttribute(line, after)) |symbol| return symbol;
        }
    }
    return matchAfterAttribute(line, 0);
}

fn matchAfterAttribute(line: []const u8, start: usize) ?[]const u8 {
    if (std.mem.startsWith(u8, line[start..], "static")) {
        const after_static = skipSpaces(line, start + "static".len);
        if (after_static > start + "static".len and
            std.mem.startsWith(u8, line[after_static..], "inline"))
        {
            const after_inline = skipSpaces(line, after_static + "inline".len);
            if (after_inline > after_static + "inline".len) {
                if (matchDeclaration(line, after_inline)) |symbol| return symbol;
            }
        }
    }
    return matchDeclaration(line, start);
}

fn matchDeclaration(line: []const u8, start: usize) ?[]const u8 {
    var at = skipSpaces(line, start);
    if (!std.mem.startsWith(u8, line[at..], "ra8_")) return null;
    at += "ra8_".len;
    const type_end = skipWord(line, at);
    if (type_end == at) return null;

    const after_spaces = skipSpaces(line, type_end);
    if (after_spaces < line.len and line[after_spaces] == '*') {
        if (matchSymbol(line, after_spaces + 1)) |symbol| return symbol;
    }
    return matchSymbol(line, type_end);
}

fn matchSymbol(line: []const u8, start: usize) ?[]const u8 {
    const symbol_start = skipSpaces(line, start);
    if (symbol_start == start) return null; // `\s+` needs at least one space
    if (!std.mem.startsWith(u8, line[symbol_start..], "ra8_")) return null;
    const word_end = skipWord(line, symbol_start + "ra8_".len);
    if (word_end == symbol_start + "ra8_".len) return null;
    const before_paren = skipSpaces(line, word_end);
    if (before_paren >= line.len or line[before_paren] != '(') return null;
    return line[symbol_start..word_end];
}

/// `SINCE_TAG_PRESENT.search(...)`, which is a plain substring test.
pub fn hasSinceTag(text: []const u8) bool {
    return std.mem.indexOf(u8, text, "@since") != null;
}

/// `SINCE_VALUE.search(line)`, returning the captured version.
///
/// `@since\s+(?:Version\s+)?([0-9]+(?:\.[0-9]+){1,2}[a-z]?)`. The digits are
/// spelled `[0-9]`, not `\d`, so an Arabic-Indic digit never matched a
/// version here and must not start matching now.
pub fn findSinceValue(line: []const u8) ?[]const u8 {
    var search_from: usize = 0;
    while (std.mem.indexOfPos(u8, line, search_from, "@since")) |found| {
        search_from = found + 1;
        const after_tag = found + "@since".len;
        const after_spaces = skipSpaces(line, after_tag);
        if (after_spaces == after_tag) continue; // `\s+`
        if (std.mem.startsWith(u8, line[after_spaces..], "Version")) {
            const word_end = after_spaces + "Version".len;
            const after_word = skipSpaces(line, word_end);
            if (after_word > word_end) {
                if (matchVersionNumber(line, after_word)) |value| return value;
            }
        }
        if (matchVersionNumber(line, after_spaces)) |value| return value;
    }
    return null;
}

fn matchVersionNumber(line: []const u8, start: usize) ?[]const u8 {
    var at = skipAsciiDigits(line, start);
    if (at == start) return null;
    if (at >= line.len or line[at] != '.') return null;
    var next = skipAsciiDigits(line, at + 1);
    if (next == at + 1) return null;
    at = next;
    if (at < line.len and line[at] == '.') {
        next = skipAsciiDigits(line, at + 1);
        if (next > at + 1) at = next;
    }
    if (at < line.len and line[at] >= 'a' and line[at] <= 'z') at += 1;
    return line[start..at];
}

// ---------------------------------------------------------------------------
// The two halves of the gate
// ---------------------------------------------------------------------------

/// `is_under_lib_inc`: the public contract of an `ra8_*` library, where the
/// tag is mandatory. Substring tests on the whole path, as the Python wrote
/// them.
pub fn isUnderLibInc(path: []const u8) bool {
    return std.mem.indexOf(u8, path, "libs/ra8_") != null and
        std.mem.endsWith(u8, path, ".h") and
        std.mem.indexOf(u8, path, "/inc/") != null;
}

/// `path.suffix in SOURCE_SUFFIXES`, i.e. the LAST dotted component only.
pub fn hasSourceSuffix(path: []const u8) bool {
    const name = std.fs.path.basename(path);
    const dot = std.mem.lastIndexOfScalar(u8, name, '.') orelse return false;
    if (dot == 0) return false; // a dotfile has no suffix in pathlib
    const suffix = name[dot..];
    for (source_suffixes) |candidate| {
        if (std.mem.eql(u8, suffix, candidate)) return true;
    }
    return false;
}

/// Append one problem line per public declaration with no `@since` above it.
pub fn presenceProblems(
    allocator: std.mem.Allocator,
    display_path: []const u8,
    text: []const u8,
    out: *std.ArrayList([]const u8),
) !void {
    if (!std.unicode.utf8ValidateSlice(text)) return; // UnicodeDecodeError
    const lines = try splitLines(allocator, text);
    defer allocator.free(lines);

    for (lines, 0..) |line, index| {
        const symbol = matchPublicDecl(line) orelse continue;
        const first = if (index > lookback_lines) index - lookback_lines else 0;
        var tagged = false;
        for (lines[first..index]) |earlier| {
            if (hasSinceTag(earlier)) {
                tagged = true;
                break;
            }
        }
        if (tagged) continue;
        try out.append(try std.fmt.allocPrint(
            allocator,
            "{s}:{d}: {s} missing @since",
            .{ display_path, index + 1, symbol },
        ));
    }
}

/// Append one problem line per `@since` whose value is not the project's.
pub fn valueProblems(
    allocator: std.mem.Allocator,
    display_path: []const u8,
    text: []const u8,
    project_version: []const u8,
    out: *std.ArrayList([]const u8),
) !void {
    if (!std.unicode.utf8ValidateSlice(text)) return; // UnicodeDecodeError
    var iterator = LineIterator{ .text = text };
    var line_number: usize = 0;
    while (iterator.next()) |line| {
        line_number += 1;
        const value = findSinceValue(line) orelse continue;
        if (std.mem.eql(u8, value, project_version)) continue;
        try out.append(try std.fmt.allocPrint(
            allocator,
            "{s}:{d}: @since {s} != project {s}",
            .{ display_path, line_number, value, project_version },
        ));
    }
}

// ---------------------------------------------------------------------------
// Derived scope, from lint_targets
// ---------------------------------------------------------------------------

/// `lint_targets.is_build_dir_name`. The separator is required, so
/// `builders/` is source.
pub fn isBuildDirName(name: []const u8) bool {
    return std.mem.eql(u8, name, "build") or
        std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

/// `lint_targets.is_build_output`: DIRECTORY components only, so a file
/// called `build` is not a build tree.
pub fn isBuildOutput(rel: []const u8) bool {
    var iterator = std.mem.splitScalar(u8, rel, '/');
    const first = iterator.next() orelse return false;
    var component = first;
    var index: usize = 0;
    while (iterator.next()) |next_component| {
        for (tool_output_dir_names) |owned| {
            if (std.mem.eql(u8, component, owned)) return true;
        }
        if (isBuildDirName(component)) {
            if (index == 0) return true;
            for (build_tree_roots) |root_name| {
                if (std.mem.eql(u8, first, root_name)) return true;
            }
        }
        index += 1;
        component = next_component;
    }
    return false;
}

/// `first_party_paths((".c", ".h", ".cpp", ".hpp"))`'s filter, for one path.
pub fn inFirstPartyScope(rel: []const u8) bool {
    if (!hasSourceSuffix(rel)) return false;
    for (excluded_prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return false;
    }
    if (isBuildOutput(rel)) return false;
    // Every one of these suffixes resolves to language "c" in lint_targets,
    // so the per-language vendored tree is subtracted too.
    for (c_excluded_prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return false;
    }
    return true;
}

pub fn lessThanByBytes(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

pub fn writeProblems(
    stderr: anytype,
    project_version: []const u8,
    problems: []const []const u8,
) !void {
    try stderr.print("{s}: project version is {s}\n", .{ tool_name, project_version });
    for (problems) |line| try stderr.print("{s}\n", .{line});
    try stderr.print("\n{d} issue(s) found.\n", .{problems.len});
}

pub fn writeExpectation(stdout: anytype, held: bool, label: []const u8) !void {
    try stdout.print("  [{s}] {s}\n", .{ if (held) "ok" else "FAIL", label });
}

pub fn writeSelftestVerdict(
    stdout: anytype,
    stderr: anytype,
    failures: []const []const u8,
) !u8 {
    if (failures.len == 0) {
        try stdout.writeAll("selftest: all assertions held (both directions).\n");
        return 0;
    }
    try stderr.print("\nSELFTEST FAILED: {d} assertion(s)\n", .{failures.len});
    for (failures) |label| try stderr.print("  {s}\n", .{label});
    return 1;
}
