//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Scope, detector and report algebra for the session-reference gate (#858).
//!
//! Everything here is pure: it takes a census of repository-relative paths and
//! byte slices, and answers questions about them. No file system, no process,
//! no argv. The `git ls-files` census and the reads live in `cli.zig`, so
//! every rule below is provable in a test with no repository on disk.
//!
//! The derived-scope rules reproduce `scripts/checks/lint_targets.py`, the
//! shared primitive the predecessor called through `first_party_paths`, plus
//! this gate's own docs-side subtraction. A hardcoded root list does not fail
//! when it goes stale, it reports success over a shrinking slice, which is the
//! defect the derived scope exists to prevent.

const std = @import("std");
const char_classes = @import("char_classes.zig");

/// Name the gate calls itself in diagnostics.
pub const tool = "check_no_wave_references";

/// Suffixes the gate scans, `SCAN_EXTS`. Matched with `endswith`, as the
/// predecessor's `first_party_paths(tuple(SCAN_EXTS))` did.
pub const scan_exts = [_][]const u8{
    ".c",  ".h",    ".cpp",   ".hpp",  ".cc",
    ".md", ".yml",  ".sh",    ".py",   ".txt",
    ".mk", ".just", ".cmake", ".yaml",
};

/// Extensionless-by-convention files, `SCAN_BASENAMES`. A suffix set alone
/// cannot see them, and the predecessor kept a path only when its BASENAME was
/// the listed name, so `my-justfile` is not one of these.
pub const scan_basenames = [_][]const u8{
    "justfile", "Justfile", "Dockerfile", "CMakeLists.txt", "GNUmakefile",
};

/// Committed datasheets and generated Doxygen output under `docs/`: content
/// that is not ours to police. Matched by path COMPONENT, exactly as the
/// predecessor's `set(Path(rel).parts) & DOCS_VENDOR_DIRS` did.
pub const docs_vendor_dirs = [_][]const u8{ "reference", "doxygen", "html" };

/// Files skipped whole, `SELF_EXEMPT_FILES`. Each one has to spell the banned
/// pattern to describe it, and tagging every such line individually would bury
/// them.
///
/// The predecessor listed its own implementation path first. That file no
/// longer exists, so the launcher takes its place; the Zig sources beside it
/// need no entry because `.zig` is not in `scan_exts` and never was.
pub const self_exempt_files = [_][]const u8{
    "scripts/builders/check_no_wave_references.sh",
    "scripts/fix/fix_wave_references.py",
    "docs/STYLE_GUIDE.md",
    "CLAUDE.md",
};

/// Vendored SOUP and generated tables: `lint_targets.EXCLUDED_PREFIXES`.
pub const excluded_prefixes = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "tools/vela/generated/",
};

/// Excluded for C only. A vendored tree is SOUP for the language whose sources
/// it carries, while the build glue that compiles it is ours.
pub const c_excluded_prefixes = [_][]const u8{"port/threadx/"};

/// Top-level roots beneath which a build tree legitimately appears at any
/// depth. Deliberately not "any directory anywhere": a `scripts/build/`  PATHREF-OK:
/// an illustrative path, not one this repository has to carry.
/// would be source and has to stay visible.
pub const build_tree_roots = [_][]const u8{
    "docs", "examples", "local-poc", "port", "tests", "tools", "apps",
};

/// Directory names a tool reserves, matched at ANY depth because nobody can
/// legitimately author a source directory with one of these names.
pub const tool_output_dir_names = [_][]const u8{
    ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules",
};

/// Smallest derived scope the gate will trust, `FILE_FLOOR`. Below it, "0
/// violations" is a clean verdict from a sweep that read almost nothing, which
/// is the exact failure the gate-honesty epic (#190) exists to prevent.
pub const file_floor: usize = 2500;

/// Smallest census the derived scope will trust, `lint_targets.TRACKED_FLOOR`.
pub const tracked_floor: usize = 1000;

/// Most findings printed before the report truncates, `MAX_FINDINGS_SHOWN`.
pub const max_findings_shown: usize = 50;

/// Longest snippet printed untrimmed, `SNIPPET_MAX_LEN`, in CHARACTERS: the
/// predecessor measured `len(line)` on a `str`, so a multi-byte character
/// counts once.
pub const snippet_max_len: usize = 120;

/// A trimmed snippet keeps this many characters and gains `...`,
/// `SNIPPET_TRIM_LEN`.
pub const snippet_trim_len: usize = snippet_max_len - 3;

/// One decoded code point and the byte width it occupied.
pub const Decoded = struct { code_point: u21, width: usize };

/// Decode the code point at `index`. A malformed or truncated sequence
/// decodes as the single byte, which keeps the scan advancing; the reader
/// rejects invalid UTF-8 before the detector ever sees it, exactly as
/// `read_text(encoding="utf-8")` did.
pub fn decodeAt(text: []const u8, index: usize) Decoded {
    const width = std.unicode.utf8ByteSequenceLength(text[index]) catch return .{
        .code_point = text[index],
        .width = 1,
    };
    if (index + width > text.len) return .{ .code_point = text[index], .width = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + width]) catch return .{
        .code_point = text[index],
        .width = 1,
    };
    return .{ .code_point = code_point, .width = width };
}

/// The code point ending at `index`, for the left half of a word boundary.
fn decodeBefore(text: []const u8, index: usize) Decoded {
    var start = index;
    var back: usize = 0;
    while (start > 0 and back < 4) {
        start -= 1;
        back += 1;
        if (text[start] & 0xC0 != 0x80) {
            const decoded = decodeAt(text, start);
            if (start + decoded.width == index) return decoded;
            return .{ .code_point = text[index - 1], .width = 1 };
        }
    }
    return .{ .code_point = text[index - 1], .width = 1 };
}

/// `\s` for a str pattern, which is also `str.isspace`.
pub fn isSpace(code_point: u21) bool {
    return char_classes.inTable(&char_classes.space_intervals, code_point);
}

/// `\w` for a str pattern: Unicode alphanumeric, or underscore.
pub fn isWord(code_point: u21) bool {
    return char_classes.inTable(&char_classes.word_intervals, code_point);
}

/// `\d` for a str pattern: the Unicode decimal-digit set, not `[0-9]`.
pub fn isDigit(code_point: u21) bool {
    if (code_point >= '0' and code_point <= '9') return true;
    for (char_classes.non_ascii_digit_run_starts) |start| {
        if (code_point >= start and code_point <= start + 9) return true;
    }
    return false;
}

/// `[A-Za-z]`, spelled as a range in the pattern and therefore ASCII-only.
pub fn isAsciiLetter(code_point: u21) bool {
    return (code_point >= 'a' and code_point <= 'z') or (code_point >= 'A' and code_point <= 'Z');
}

/// A `re` word boundary at byte offset `index`: the word-ness of the code
/// points either side differs, with off-the-end counting as non-word.
pub fn isWordBoundary(text: []const u8, index: usize) bool {
    const before = if (index == 0) false else isWord(decodeBefore(text, index).code_point);
    const after = if (index >= text.len) false else isWord(decodeAt(text, index).code_point);
    return before != after;
}

/// True when the optional separator class `[\s_\-]` accepts this code point.
fn isSeparator(code_point: u21) bool {
    return code_point == '_' or code_point == '-' or isSpace(code_point);
}

/// `WAVE_RE.search(line)`: `\b[Ww]ave[\s_\-]?\d+[A-Za-z]?\b`.
///
/// Hand-run rather than table-driven, because the pattern's two optional
/// pieces and the greedy `\d+` all backtrack, and the trailing `\b` is what
/// decides most real lines. `wave12_` is the case that proves it matters: the
/// greedy digit run leaves `2` against `_`, both word characters, so no
/// boundary; shortening the run leaves digit against digit, which is no
/// boundary either; the line stays quiet, exactly as it did under CPython.
pub fn firesWave(text: []const u8) bool {
    var index: usize = 0;
    while (index < text.len) : (index += decodeAt(text, index).width) {
        if (matchWaveAt(text, index)) return true;
    }
    return false;
}

/// One anchored attempt of the detector at `start`.
fn matchWaveAt(text: []const u8, start: usize) bool {
    if (!isWordBoundary(text, start)) return false;
    if (start + 4 > text.len) return false;
    if (text[start] != 'w' and text[start] != 'W') return false;
    if (!std.mem.eql(u8, text[start + 1 .. start + 4], "ave")) return false;

    const after_token = start + 4;
    // `[\s_\-]?` is greedy: the separator is tried first, then skipped.
    if (after_token < text.len) {
        const separator = decodeAt(text, after_token);
        if (isSeparator(separator.code_point) and
            matchDigitsAt(text, after_token + separator.width)) return true;
    }
    return matchDigitsAt(text, after_token);
}

/// `\d+[A-Za-z]?\b` anchored at `start`, with the greedy run backtracking.
fn matchDigitsAt(text: []const u8, start: usize) bool {
    var ends = std.BoundedArray(usize, 64){};
    var at = start;
    while (at < text.len) {
        const decoded = decodeAt(text, at);
        if (!isDigit(decoded.code_point)) break;
        at += decoded.width;
        ends.append(at) catch break;
    }
    if (ends.len == 0) return false;

    var taken = ends.len;
    while (taken > 0) : (taken -= 1) {
        const after_digits = ends.get(taken - 1);
        // `[A-Za-z]?` is greedy too: the letter is tried before the skip.
        if (after_digits < text.len) {
            const decoded = decodeAt(text, after_digits);
            if (isAsciiLetter(decoded.code_point) and
                isWordBoundary(text, after_digits + decoded.width)) return true;
        }
        if (isWordBoundary(text, after_digits)) return true;
    }
    return false;
}

/// `OPTOUT_RE.search(line)`: `WAVE-OK\s*:`, the per-line opt-out.
pub fn hasOptOut(text: []const u8) bool {
    const marker = "WAVE-OK";
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, text, from, marker)) |hit| {
        var at = hit + marker.len;
        while (at < text.len) {
            const decoded = decodeAt(text, at);
            if (!isSpace(decoded.code_point)) break;
            at += decoded.width;
        }
        if (at < text.len and text[at] == ':') return true;
        from = hit + 1;
    }
    return false;
}

/// `str.rstrip()`: drop trailing Unicode whitespace, the full set.
pub fn pythonRstrip(text: []const u8) []const u8 {
    var end = text.len;
    while (end > 0) {
        const decoded = decodeBefore(text, end);
        if (!isSpace(decoded.code_point)) break;
        end -= decoded.width;
    }
    return text[0..end];
}

/// Bytes consumed by a line terminator at `index`, or 0 when there is none.
/// `\r\n` counts once; `\x1f` does NOT break a line, though `\x1c`-`\x1e` do.
fn lineBreakWidth(text: []const u8, index: usize, decoded: Decoded) usize {
    return switch (decoded.code_point) {
        '\r' => if (index + 1 < text.len and text[index + 1] == '\n') 2 else 1,
        '\n', 0x0B, 0x0C, 0x1C, 0x1D, 0x1E, 0x85, 0x2028, 0x2029 => decoded.width,
        else => 0,
    };
}

/// `str.splitlines()`: every CPython line terminator, not just `\n`, and no
/// trailing empty line after a final terminator.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        while (self.index < self.text.len) {
            const decoded = decodeAt(self.text, self.index);
            const width = lineBreakWidth(self.text, self.index, decoded);
            if (width != 0) {
                const line = self.text[start..self.index];
                self.index += width;
                return line;
            }
            self.index += decoded.width;
        }
        return self.text[start..];
    }
};

/// Split `text` into lines the way `str.splitlines()` does.
pub fn splitLines(text: []const u8) LineIterator {
    return .{ .text = text };
}

fn containsText(haystack: []const []const u8, needle: []const u8) bool {
    for (haystack) |item| if (std.mem.eql(u8, item, needle)) return true;
    return false;
}

fn startsWithAny(rel: []const u8, prefixes: []const []const u8) bool {
    for (prefixes) |prefix| if (std.mem.startsWith(u8, rel, prefix)) return true;
    return false;
}

/// The final path component.
pub fn pathName(rel: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, rel, '/')) |cut| return rel[cut + 1 ..];
    return rel;
}

/// `pathlib.PurePath.suffix`: empty unless a dot sits strictly inside the
/// name, so `.bashrc` and `trailing.` both have no suffix.
pub fn pathSuffix(name: []const u8) []const u8 {
    const cut = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    if (cut == 0 or cut + 1 >= name.len) return "";
    return name[cut..];
}

/// True when one path COMPONENT names a build tree. The separator is
/// required, so `builders` is not a build directory.
pub fn isBuildDirName(name: []const u8) bool {
    if (std.mem.eql(u8, name, "build")) return true;
    return std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

/// True when repo-relative `rel` lives inside a build tree. DIRECTORY
/// components only: a file called `build` is not a build tree.
pub fn isBuildOutput(rel: []const u8) bool {
    const first_cut = std.mem.indexOfScalar(u8, rel, '/') orelse return false;
    const rooted = containsText(&build_tree_roots, rel[0..first_cut]);

    var index: usize = 0;
    var parts = std.mem.splitScalar(u8, rel, '/');
    var current = parts.next();
    while (current) |part| {
        const next = parts.next();
        if (next == null) break; // the last component is the file name
        if (containsText(&tool_output_dir_names, part)) return true;
        if (isBuildDirName(part) and (index == 0 or rooted)) return true;
        index += 1;
        current = next;
    }
    return false;
}

fn suffixLanguage(suffix: []const u8) ?[]const u8 {
    const pairs = [_]struct { []const u8, []const u8 }{
        .{ ".c", "c" },       .{ ".h", "c" },       .{ ".cpp", "c" },      .{ ".hpp", "c" },
        .{ ".cc", "c" },      .{ ".cxx", "c" },     .{ ".hh", "c" },       .{ ".hxx", "c" },
        .{ ".py", "python" }, .{ ".sh", "shell" },  .{ ".bash", "shell" }, .{ ".cmake", "cmake" },
        .{ ".yml", "yaml" },  .{ ".yaml", "yaml" }, .{ ".mk", "make" },    .{ ".just", "just" },
        .{ ".ld", "ld" },     .{ ".zig", "zig" },
    };
    for (pairs) |pair| if (std.mem.eql(u8, pair[0], suffix)) return pair[1];
    return null;
}

/// The language a path's name implies, before any exclusion is applied.
///
/// An extensionless name answers null rather than consulting a shebang. The
/// Python did read shebangs, but no shebang language is `c` and `c` is the
/// only language carrying an exclusion, so the two agree on every path this
/// gate can reach.
pub fn rawLanguage(rel: []const u8) ?[]const u8 {
    const name = pathName(rel);
    if (std.mem.eql(u8, name, "CMakeLists.txt")) return "cmake";
    if (std.mem.eql(u8, name, "justfile") or std.mem.eql(u8, name, "Justfile")) return "just";
    return suffixLanguage(pathSuffix(name));
}

/// `lint_targets._excluded`: SOUP, generated tables and build output always,
/// plus the per-language vendored trees when a language is supplied.
pub fn isExcludedRel(rel: []const u8, language: ?[]const u8) bool {
    if (startsWithAny(rel, &excluded_prefixes) or isBuildOutput(rel)) return true;
    const lang = language orelse return false;
    if (!std.mem.eql(u8, lang, "c")) return false;
    return startsWithAny(rel, &c_excluded_prefixes);
}

/// True when `rel` survives `first_party_paths`, language excludes included.
pub fn isFirstParty(rel: []const u8) bool {
    if (isExcludedRel(rel, null)) return false;
    const lang = rawLanguage(rel);
    if (lang != null and isExcludedRel(rel, lang)) return false;
    return true;
}

/// True when `rel` ends in one of `scan_exts`.
pub fn hasScanSuffix(rel: []const u8) bool {
    for (scan_exts) |suffix| if (std.mem.endsWith(u8, rel, suffix)) return true;
    return false;
}

/// True when the path's own NAME is one of `scan_basenames`.
pub fn isScanBasename(rel: []const u8) bool {
    return containsText(&scan_basenames, pathName(rel));
}

/// This gate's own subtraction: a committed datasheet or generated Doxygen
/// tree under `docs/`, matched by path component.
pub fn isDocsVendored(rel: []const u8) bool {
    var parts = std.mem.splitScalar(u8, rel, '/');
    while (parts.next()) |part| {
        if (containsText(&docs_vendor_dirs, part)) return true;
    }
    return false;
}

/// True when the whole file is skipped, `SELF_EXEMPT_FILES`.
pub fn isSelfExempt(rel: []const u8) bool {
    return containsText(&self_exempt_files, rel);
}

fn lessThanString(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// Sort a path list in place, byte order, as `sorted()` did.
pub fn sortPaths(paths: [][]const u8) void {
    std.mem.sort([]const u8, paths, {}, lessThanString);
}

/// The scan set: every in-scope first-party path in `census`, sorted and
/// deduplicated, with the docs-side vendored trees dropped.
pub fn derivedScope(allocator: std.mem.Allocator, census: []const []const u8) ![][]const u8 {
    var seen = std.StringHashMap(void).init(allocator);
    defer seen.deinit();
    var kept = std.ArrayList([]const u8).init(allocator);
    errdefer kept.deinit();

    for (census) |rel| {
        if (!hasScanSuffix(rel) and !isScanBasename(rel)) continue;
        if (!isFirstParty(rel)) continue;
        if (isDocsVendored(rel)) continue;
        const gop = try seen.getOrPut(rel);
        if (gop.found_existing) continue;
        try kept.append(rel);
    }

    const out = try kept.toOwnedSlice();
    sortPaths(out);
    return out;
}

/// True when any path in `scope` starts with `root_name + "/"`. The selftest's
/// reach probe: a clean run over a scope that never sees `infra/` or `just/`
/// proves nothing.
pub fn scopeReaches(scope: []const []const u8, root_name: []const u8) bool {
    for (scope) |rel| {
        if (std.mem.startsWith(u8, rel, root_name) and
            rel.len > root_name.len and rel[root_name.len] == '/') return true;
    }
    return false;
}

/// One reported reference.
pub const Finding = struct {
    path: []const u8,
    line: usize,
    snippet: []const u8,
};

/// Every session reference in one file's text. `path` is carried through
/// unchanged, so the caller decides what a finding is displayed as.
pub fn scanText(
    allocator: std.mem.Allocator,
    path: []const u8,
    text: []const u8,
) ![]Finding {
    var findings = std.ArrayList(Finding).init(allocator);
    errdefer findings.deinit();

    var number: usize = 0;
    var lines = splitLines(text);
    while (lines.next()) |line| {
        number += 1;
        if (hasOptOut(line)) continue;
        if (!firesWave(line)) continue;
        try findings.append(.{ .path = path, .line = number, .snippet = pythonRstrip(line) });
    }
    return findings.toOwnedSlice();
}

/// The printed snippet: the line itself, or its first `snippet_trim_len`
/// CHARACTERS plus `...`. Character-counted, because the predecessor measured
/// and sliced a `str`.
pub fn renderSnippet(allocator: std.mem.Allocator, line: []const u8) ![]const u8 {
    var characters: usize = 0;
    var cut: usize = line.len;
    var index: usize = 0;
    while (index < line.len) {
        if (characters == snippet_trim_len) cut = index;
        characters += 1;
        index += decodeAt(line, index).width;
    }
    if (characters <= snippet_max_len) return allocator.dupe(u8, line);
    return std.fmt.allocPrint(allocator, "{s}...", .{line[0..cut]});
}
