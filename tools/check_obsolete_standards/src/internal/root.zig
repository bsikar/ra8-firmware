//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detector, scope and report algebra for the obsolete-standards gate (#858).
//!
//! Everything here is pure: it takes repository-relative path strings and
//! byte slices and answers questions about them. No file system, no process,
//! no argv. The `git ls-files` census, the `git diff --cached` index read and
//! the file reads live in `cli.zig`, so every rule below is provable in a
//! test with no repository on disk.
//!
//! The derived-scope rules reproduce `scripts/checks/lint_targets.py` -- the
//! shared primitive the Python gate called through `first_party_paths` -- and
//! the gate's own per-file subtractions on top. A hardcoded root list does
//! not fail when it goes stale; it reports success over a shrinking slice,
//! which is the defect the derived scope exists to prevent.

const std = @import("std");
const char_classes = @import("char_classes.zig");

/// Name the gate calls itself in diagnostics. The predecessor spelled this
/// with its `.py` suffix; every tool migrated under #858 drops it.
pub const tool = "check_obsolete_standards";

/// The banned spellings, in the predecessor's order. Both are wrapped in
/// `\b` and matched CASE-SENSITIVELY, so an unrelated identifier spelling
/// the token in another case is not a finding.
pub const forbidden_patterns = [_][]const u8{ "DO-178B", "DO178B" };

/// Files where the token may legitimately appear because they are *the*
/// files documenting that it is forbidden. The predecessor listed its own
/// path here; the entries below name this tool's sources instead, for the
/// same reason: the gate that documents the ban must not trip on itself.
pub const whitelist = [_][]const u8{
    "CLAUDE.md",
    "PHILOSOPHIES.md",
    "docs/MCDC.md",
    "scripts/builders/check_obsolete_standards.sh",
    "scripts/git/pre-commit",
    "tools/check_obsolete_standards/src/internal/root.zig",
    "tools/check_obsolete_standards/src/cli.zig",
    "tools/check_obsolete_standards/tests/internal_test.zig",
    "tools/check_obsolete_standards/tests/cli_test.zig",
};

/// Suffixes the TREE-WIDE sweep enumerates, matched with `str.endswith` and
/// therefore case-sensitively. `.just` is deliberately absent: the
/// predecessor's enumeration tuple omitted it while its per-file predicate
/// accepted it, so a `.just` file reached the scan only when a caller named
/// it (the commit hook) and never through `--all`. Preserved, not tidied: the
/// asymmetry is observable behaviour and widening it is a policy change.
pub const scan_suffixes = [_][]const u8{
    ".c",  ".h",   ".cpp",   ".hpp", ".md",   ".py",
    ".sh", ".txt", ".cmake", ".yml", ".yaml",
};

/// Suffixes the PER-FILE predicate accepts, matched on the lower-cased
/// suffix. This set carries `.just`; `scan_suffixes` does not.
pub const scannable_suffixes = [_][]const u8{
    ".c",  ".h",    ".cpp", ".hpp",   ".md",  ".py",
    ".sh", ".just", ".txt", ".cmake", ".yml", ".yaml",
};

/// Extensionless-by-convention listfiles. A suffix set alone cannot see them.
pub const scannable_names = [_][]const u8{ "justfile", "Justfile", "CMakeLists.txt" };

/// A path component that takes a file out of scope wholesale: vendored code
/// may cite whatever standard it was written against and is not ours to
/// correct.
pub const vendored_component = "third_party";

/// A tree-wide sweep that enumerated almost nothing has not found a clean
/// tree, it has lost its file list.
pub const tree_floor: usize = 500;

/// Smallest census the derived scope will trust, `lint_targets.TRACKED_FLOOR`.
pub const tracked_floor: usize = 1000;

/// Vendored SOUP and generated tables: `lint_targets.EXCLUDED_PREFIXES`.
pub const excluded_prefixes = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "tools/vela/generated/",
};

/// Excluded for C only. A vendored tree is SOUP for the language whose
/// sources it carries, while the build glue that compiles it is ours.
pub const c_excluded_prefixes = [_][]const u8{"port/threadx/"};

/// Top-level roots beneath which a build tree legitimately appears at any
/// depth. Deliberately not "any directory anywhere": a source directory
/// called `scripts/build/` PATHREF-OK: illustrative, not a tracked path
/// would otherwise be mistaken for build output.
pub const build_tree_roots = [_][]const u8{
    "docs", "examples", "local-poc", "port", "tests", "tools", "apps",
};

/// Directory names a tool reserves, matched at ANY depth because nobody can
/// legitimately author a source directory with one of these names.
pub const tool_output_dir_names = [_][]const u8{
    ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules",
};

/// One decoded code point and the byte width it occupied.
pub const Decoded = struct { code_point: u21, len: usize };

/// Decode the code point at `index`, treating malformed bytes as U+FFFD of
/// width one so a non-UTF-8 byte can never stall a scan.
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

/// `\w` for a CPython str pattern: what decides a `\b` boundary.
pub fn isWord(code_point: u21) bool {
    return char_classes.inTable(&char_classes.word_intervals, code_point);
}

/// Start index of the code point ending at `end`.
fn previousStart(text: []const u8, end: usize) usize {
    var probe = end - 1;
    while (probe > 0 and (text[probe] & 0xC0) == 0x80) probe -= 1;
    return probe;
}

/// `line.rstrip()`: trailing `str.isspace` code points removed. The leading
/// side is untouched, so a finding keeps its indentation exactly as the
/// predecessor printed it.
pub fn pythonRstrip(text: []const u8) []const u8 {
    var end = text.len;
    while (end > 0) {
        const start = previousStart(text, end);
        if (!isSpace(decodeAt(text, start).code_point)) break;
        end = start;
    }
    return text[0..end];
}

/// `str.splitlines()` semantics: every Unicode line break CPython honours,
/// with CRLF counted once and no trailing empty line.
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

/// True when the literal `needle` sits at `index` in `text` with a Python
/// word boundary on each side. Both patterns begin and end in a word
/// character, so `\b` there means "the neighbour is not a word character".
fn boundedAt(text: []const u8, index: usize, needle: []const u8) bool {
    if (!std.mem.startsWith(u8, text[index..], needle)) return false;
    if (index > 0) {
        const start = previousStart(text, index);
        if (isWord(decodeAt(text, start).code_point)) return false;
    }
    const after = index + needle.len;
    if (after < text.len and isWord(decodeAt(text, after).code_point)) return false;
    return true;
}

/// `pattern.search(line)` for one forbidden spelling.
pub fn patternFires(line: []const u8, needle: []const u8) bool {
    var at: usize = 0;
    while (at < line.len) {
        if (boundedAt(line, at, needle)) return true;
        at += decodeAt(line, at).len;
    }
    return false;
}

/// True when any forbidden pattern fires on this line.
pub fn lineCitesObsolete(line: []const u8) bool {
    for (forbidden_patterns) |needle| if (patternFires(line, needle)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Scope: which paths the scan is allowed to look at
// ---------------------------------------------------------------------------

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

/// `str.lower()` restricted to what a suffix comparison can observe.
///
/// ASCII folding plus U+212A (KELVIN SIGN), which CPython lowers to `k` and
/// which therefore lets `.CMAKE` spelled with it reach the scan. Every other
/// code point that lowers into ASCII produces a letter no target suffix
/// contains, so folding it would change no decision. Written out rather than
/// assumed, because a suffix predicate that silently disagrees with
/// `suffix.lower()` is how a file ducks a gate on spelling alone.
fn lowerSuffixEql(suffix: []const u8, target: []const u8) bool {
    var at: usize = 0;
    var index: usize = 0;
    while (at < suffix.len) {
        const decoded = decodeAt(suffix, at);
        const folded: u21 = switch (decoded.code_point) {
            'A'...'Z' => decoded.code_point + 32,
            0x212A => 'k',
            else => decoded.code_point,
        };
        if (folded > 0x7F) return false; // no target suffix holds a non-ASCII byte
        if (index >= target.len or target[index] != @as(u8, @intCast(folded))) return false;
        index += 1;
        at += decoded.len;
    }
    return index == target.len;
}

/// True when one path COMPONENT is exactly `third_party`, as
/// `any(part == "third_party" for part in path.parts)` tested. A directory
/// merely CONTAINING the word (`third_party_notes/`) stays in scope.
pub fn hasVendoredComponent(rel: []const u8) bool {
    var parts = std.mem.splitScalar(u8, rel, '/');
    while (parts.next()) |part| {
        if (std.mem.eql(u8, part, vendored_component)) return true;
    }
    return false;
}

/// True when this exact repo-relative path is whitelisted.
pub fn isWhitelisted(rel: []const u8) bool {
    return containsText(&whitelist, rel);
}

/// `check_obsolete_standards.scannable`, minus the `is_file()` probe the
/// caller owns: the whitelist, the vendored subtraction, then the
/// case-insensitive suffix set, then the listfile names.
pub fn isScannable(rel: []const u8) bool {
    if (isWhitelisted(rel)) return false;
    if (hasVendoredComponent(rel)) return false;
    const name = pathName(rel);
    const suffix = pathSuffix(name);
    if (suffix.len != 0) {
        for (scannable_suffixes) |candidate| {
            if (lowerSuffixEql(suffix, candidate)) return true;
        }
    }
    return containsText(&scannable_names, name);
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

/// True when `rel` ends in one of the TREE-WIDE suffixes. Case-sensitive,
/// because `str.endswith` is: a tracked `README.MD` was never enumerated by
/// `--all`, and only reached the scan when a caller named it.
pub fn hasScanSuffix(rel: []const u8) bool {
    for (scan_suffixes) |suffix| if (std.mem.endsWith(u8, rel, suffix)) return true;
    return false;
}

/// True when the path's own NAME is one of the extensionless listfiles. The
/// Python matched `endswith(name)` first and then filtered on the basename,
/// so `my-justfile` is not a listfile.
pub fn isScanName(rel: []const u8) bool {
    return containsText(&scannable_names, pathName(rel));
}

fn lessThanString(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// Sort a path list in place, byte order, as `sorted()` did.
pub fn sortPaths(paths: [][]const u8) void {
    std.mem.sort([]const u8, paths, {}, lessThanString);
}

/// `check_obsolete_standards.tracked_files`: the union of the suffix sweep
/// and the listfile sweep over `census`, first-party only, deduplicated and
/// sorted. Enumeration is the census the caller hands over, never a
/// hardcoded directory list.
pub fn derivedScope(allocator: std.mem.Allocator, census: []const []const u8) ![][]const u8 {
    var seen = std.StringHashMap(void).init(allocator);
    defer seen.deinit();
    var kept = std.ArrayList([]const u8).init(allocator);
    errdefer kept.deinit();

    for (census) |rel| {
        if (!hasScanSuffix(rel) and !isScanName(rel)) continue;
        if (!isFirstParty(rel)) continue;
        const gop = try seen.getOrPut(rel);
        if (gop.found_existing) continue;
        try kept.append(rel);
    }

    const out = try kept.toOwnedSlice();
    sortPaths(out);
    return out;
}

/// True when any path in `scope` starts with `root_name + "/"`: the probe
/// that keeps a clean verdict from resting on a scope that never looked.
pub fn scopeReaches(scope: []const []const u8, root_name: []const u8) bool {
    for (scope) |rel| {
        if (std.mem.startsWith(u8, rel, root_name) and
            rel.len > root_name.len and rel[root_name.len] == '/') return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Findings and the report
// ---------------------------------------------------------------------------

/// One offending line: the path scanned, the 1-based line number, and the
/// line itself, right-stripped.
pub const Finding = struct {
    path: []const u8,
    lineno: usize,
    line: []const u8,
};

/// `scan_text`: one finding per citing line, at most. A line naming two
/// obsolete standards is reported once, because the fix is to rewrite the
/// line and a second finding on it would only be noise.
pub fn scanText(allocator: std.mem.Allocator, path: []const u8, text: []const u8) ![]Finding {
    var hits = std.ArrayList(Finding).init(allocator);
    errdefer hits.deinit();

    var lineno: usize = 0;
    var iterator = LineIterator{ .text = text };
    while (iterator.next()) |line| {
        lineno += 1;
        if (!lineCitesObsolete(line)) continue;
        try hits.append(.{ .path = path, .lineno = lineno, .line = pythonRstrip(line) });
    }
    return hits.toOwnedSlice();
}

/// The four-line guidance the report opens with, on stdout as the
/// predecessor printed it.
pub const fail_header = [_][]const u8{
    "[FAIL] Obsolete standard reference detected (DO-178B was",
    "       superseded by DO-178C in December 2011). Use",
    "       DO-178C, IEC 61508 SIL 3, or ISO 26262 ASIL C/D",
    "       per CLAUDE.md. Offending lines:",
};

/// Print the findings report. Findings keep discovery order: the scan walks
/// an already-sorted path list and each file in line order.
pub fn renderFindings(findings: []const Finding, out: anytype) !void {
    for (fail_header) |line| try out.print("{s}\n", .{line});
    for (findings) |finding| {
        try out.print("  {s}:{d}: {s}\n", .{ finding.path, finding.lineno, finding.line });
    }
}

/// The clean verdict, which names the file count so a collapsed scan is
/// visible in the log even when it cleared the floor.
pub fn renderClean(scanned: usize, out: anytype) !void {
    try out.print("{s}: 0 findings across {d} file(s).\n", .{ tool, scanned });
}

/// The no-mode refusal. A bare invocation used to mean `--staged`, which is
/// how this gate scanned nothing at all in CI for its whole life.
pub fn renderNoMode(err: anytype) !void {
    try err.print(
        "{s}: pass --all (CI, the whole tracked tree) or\n" ++
            "  --staged (the pre-commit hook, the git index). There is no default:\n" ++
            "  this checker silently defaulted to --staged and so scanned nothing at\n" ++
            "  all in CI, where the index is always empty.\n",
        .{tool},
    );
}

/// The collapsed-sweep refusal.
pub fn renderFloorBreach(seen: usize, floor: usize, err: anytype) !void {
    try err.print(
        "{s}: FATAL -- the tree-wide sweep enumerated only\n" ++
            "  {d} file(s), below the floor of {d}. An empty or\n" ++
            "  collapsed file list reports success because it saw nothing.\n",
        .{ tool, seen, floor },
    );
}

/// The collapsed-census refusal, `lint_targets`' own floor.
pub fn renderCensusCollapsed(seen: usize, floor: usize, err: anytype) !void {
    try err.print(
        "{s}: FATAL -- only {d} tracked path(s), floor is {d}. A collapsed " ++
            "enumeration reports a clean tree because it enumerated nothing.\n",
        .{ tool, seen, floor },
    );
}
