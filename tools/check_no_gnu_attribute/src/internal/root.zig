//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The GNU-attribute gate's detector and scope rules (#858, #1178), free of
//! the file system and of argv: everything here is a pure function over text
//! and over repo-relative path strings, so the behaviour can be pinned by
//! tests without a tree on disk.
//!
//! First-party code uses the C23 attribute-specifier form `[[...]]`, e.g.
//! `[[gnu::weak]]`, not the GNU `__attribute__((...))` form. Three spellings
//! have no portable `[[...]]` equivalent the toolchain accepts (clang errors
//! on `[[gnu::interrupt]]` as an unknown attribute while it silently ignores
//! the GNU form) and stay exempt: `interrupt`, `cmse_nonsecure_entry` and
//! `cmse_nonsecure_call`, with or without `__leading_trailing__` underscores.
//!
//! Every quirk of the Python predecessor is reproduced deliberately rather
//! than tidied, and each one is pinned by a test. The load-bearing ones:
//!   * the gap in `__attribute__\s*\(\(` is Python's UNICODE `\s`, so a
//!     no-break space between the token and `((` still matches;
//!   * a name is normalised with `str.strip()`, whose whitespace set is a
//!     SEPARATE table in `char_classes.zig` because the two call sites are
//!     separate, not because the sets differ: measured over all 0x110000 code
//!     points on CPython 3.11.14, `re` `\s` and `str.isspace()` agree exactly
//!     (the same 29 code points, U+001C-U+001F included);
//!   * an unbalanced `((` yields no body, an empty name set, and therefore a
//!     FINDING, never a quiet pass;
//!   * a bare `____` is not longer than the dunder wrapper, so it does not
//!     normalise away and is a finding;
//!   * a waiver `ATTR-OK: <reason>` anywhere on the line silences every
//!     attribute on that line, including one that precedes it.

const std = @import("std");
const classes = @import("char_classes.zig");

/// First-party roots that carry hand-authored C/C++.
pub const roots = [_][]const u8{ "libs", "tests", "examples", "port", "tools", "apps" };

/// Scanned suffixes, matched with `str.endswith(EXTS)` semantics (a plain
/// byte suffix test, so `a.c.bak` is out of scope and `.c` is in it).
pub const exts = [_][]const u8{ ".c", ".h", ".cpp", ".hpp" };

/// Vendored SOUP and generated font data, matched as a SUBSTRING of the path
/// exactly as the predecessor did.
pub const exempt_dirs = [_][]const u8{ "/third_party/", "/ra8_fonts/" };

/// The three attributes that must stay `__attribute__`.
pub const allowed = [_][]const u8{ "interrupt", "cmse_nonsecure_entry", "cmse_nonsecure_call" };

pub const attr_token = "__attribute__";
pub const waiver_token = "ATTR-OK:";

/// Length of a bare `____` wrapper; a dunder name must EXCEED it to carry a
/// body worth stripping, which is why `____` itself is reported.
pub const min_dunder_len = 4;

/// A tree this size cannot legitimately collapse to a handful of files. Below
/// it the sweep is broken (a bad cwd, a renamed root) and reporting "clean"
/// would be a lie. Measured 2026-07-28: 2125 first-party C/C++ files.
pub const file_floor = 1700;

/// Top-level directories beneath which a build tree legitimately appears.
pub const build_tree_roots = [_][]const u8{ "docs", "examples", "local-poc", "port", "tests", "tools", "apps" };

/// Directory names owned by a tool, matched at ANY depth.
pub const tool_output_dir_names = [_][]const u8{ ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules" };

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------

fn inTable(cp: u21, table: []const [2]u21) bool {
    var low: usize = 0;
    var high: usize = table.len;
    while (low < high) {
        const mid = low + (high - low) / 2;
        const interval = table[mid];
        if (cp < interval[0]) {
            high = mid;
        } else if (cp > interval[1]) {
            low = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/// What regex `\s` matched in a `str` pattern.
pub fn isReSpace(cp: u21) bool {
    return inTable(cp, &classes.re_space);
}

/// What `str.isspace()` reported, i.e. what `strip()`/`lstrip()` removed.
pub fn isStrSpace(cp: u21) bool {
    return inTable(cp, &classes.str_space);
}

pub const Decoded = struct { cp: u21, len: usize };

/// Decode one code point. The caller has already established that the text
/// decoded as strict UTF-8 (a source that does not is skipped, as the
/// predecessor's `UnicodeDecodeError` branch did), so a malformed byte here
/// can only come from a test fixture; it is reported as itself, one byte
/// wide, which keeps every scan terminating.
pub fn decodeAt(text: []const u8, index: usize) Decoded {
    const length = std.unicode.utf8ByteSequenceLength(text[index]) catch return .{ .cp = text[index], .len = 1 };
    if (index + length > text.len) return .{ .cp = text[index], .len = 1 };
    const cp = std.unicode.utf8Decode(text[index .. index + length]) catch
        return .{ .cp = text[index], .len = 1 };
    return .{ .cp = cp, .len = length };
}

/// Length of the `\s*` run starting at `index`.
pub fn reSpaceRun(text: []const u8, index: usize) usize {
    var i = index;
    while (i < text.len) {
        const decoded = decodeAt(text, i);
        if (!isReSpace(decoded.cp)) break;
        i += decoded.len;
    }
    return i - index;
}

/// `str.strip()`: the widest slice with no leading or trailing `isspace()`.
pub fn strip(text: []const u8) []const u8 {
    var start: usize = 0;
    while (start < text.len) {
        const decoded = decodeAt(text, start);
        if (!isStrSpace(decoded.cp)) break;
        start += decoded.len;
    }
    var end = text.len;
    while (end > start) {
        var probe = end - 1;
        while (probe > start and (text[probe] & 0xC0) == 0x80) probe -= 1;
        const decoded = decodeAt(text, probe);
        if (probe + decoded.len != end or !isStrSpace(decoded.cp)) break;
        end = probe;
    }
    return text[start..end];
}

/// `str.lstrip()`.
pub fn lstrip(text: []const u8) []const u8 {
    var start: usize = 0;
    while (start < text.len) {
        const decoded = decodeAt(text, start);
        if (!isStrSpace(decoded.cp)) break;
        start += decoded.len;
    }
    return text[start..];
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

/// Text-mode read: `Path.read_text()` translates CRLF and a lone CR to LF
/// before any splitting, so the line breaks below never see a CR.
pub fn normalizeTerminators(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, text.len);
    errdefer out.deinit();
    var i: usize = 0;
    while (i < text.len) {
        if (text[i] == '\r') {
            try out.append('\n');
            i += if (i + 1 < text.len and text[i + 1] == '\n') 2 else 1;
        } else {
            try out.append(text[i]);
            i += 1;
        }
    }
    return out.toOwnedSlice();
}

/// Width of the `str.splitlines()` break at `index`, or 0. CR is absent by
/// the time this runs; 0x1F is whitespace but is NOT a break.
pub fn lineBreakLen(text: []const u8, index: usize) usize {
    const byte = text[index];
    if (byte == '\n' or byte == 0x0B or byte == 0x0C or byte == 0x1C or byte == 0x1D or byte == 0x1E) return 1;
    if (byte == 0xC2 and index + 1 < text.len and text[index + 1] == 0x85) return 2;
    if (byte == 0xE2 and index + 2 < text.len and text[index + 1] == 0x80 and
        (text[index + 2] == 0xA8 or text[index + 2] == 0xA9)) return 3;
    return 0;
}

pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn init(text: []const u8) LineIterator {
        return .{ .text = text };
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var i = start;
        while (i < self.text.len) {
            const width = lineBreakLen(self.text, i);
            if (width != 0) {
                self.index = i + width;
                return self.text[start..i];
            }
            i += 1;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

// ---------------------------------------------------------------------------
// The detector
// ---------------------------------------------------------------------------

/// `ATTR_RE = __attribute__\s*\(\(` anchored at `index`; answers the byte
/// index just past the `((`, or null.
///
/// The `\s*` needs no backtracking: whitespace is never `(`, so only the
/// longest run can be followed by the mandatory parens.
pub fn matchAttrAt(line: []const u8, index: usize) ?usize {
    if (!std.mem.startsWith(u8, line[index..], attr_token)) return null;
    const after_token = index + attr_token.len;
    const gap = after_token + reSpaceRun(line, after_token);
    if (!std.mem.startsWith(u8, line[gap..], "((")) return null;
    return gap + 2;
}

/// `ATTR_RE.finditer(line)`: non-overlapping matches, left to right.
///
/// The scan advances one BYTE on a miss where Python advanced one character;
/// the two agree because a match can only begin at `_`, an ASCII byte that
/// never appears inside a multi-byte sequence.
pub const AttrIterator = struct {
    line: []const u8,
    index: usize = 0,

    pub fn init(line: []const u8) AttrIterator {
        return .{ .line = line };
    }

    /// Answers the START of each match.
    pub fn next(self: *AttrIterator) ?usize {
        while (self.index < self.line.len) {
            if (matchAttrAt(self.line, self.index)) |end| {
                const start = self.index;
                self.index = end;
                return start;
            }
            self.index += 1;
        }
        return null;
    }
};

/// Balanced-paren body of the `__attribute__` at or after `pos`, or null when
/// the parens never close on this line.
pub fn attrBody(line: []const u8, pos: usize) ?[]const u8 {
    const found = std.mem.indexOfPos(u8, line, pos, "((") orelse return null;
    const open = found + 2;
    var depth: usize = 1;
    var q = open;
    while (q < line.len) : (q += 1) {
        if (line[q] == '(') {
            depth += 1;
        } else if (line[q] == ')') {
            depth -= 1;
            if (depth == 0) return line[open..q];
        }
    }
    return null;
}

/// True when byte column `pos` sits inside a `//` or `/*` comment on this
/// line. Deliberately crude, exactly as inherited: a line that opens AND
/// closes a block comment before the attribute is NOT treated as comment.
pub fn isCommentPos(line: []const u8, pos: usize) bool {
    const leading = lstrip(line);
    if (std.mem.startsWith(u8, leading, "*") or
        std.mem.startsWith(u8, leading, "//") or
        std.mem.startsWith(u8, leading, "/*")) return true;
    const before = line[0..pos];
    if (std.mem.indexOf(u8, before, "//") != null) return true;
    return std.mem.indexOf(u8, before, "/*") != null and std.mem.indexOf(u8, before, "*/") == null;
}

/// `WAIVER_RE = ATTR-OK:\s*\S` searched anywhere on the line.
///
/// `\s*` is greedy and `\S` is its complement, so after the maximal run the
/// next character (when there is one) always satisfies `\S`: the match
/// reduces to "some character follows the run". Each occurrence of the token
/// is tried in turn, as `re.search` does.
pub fn hasWaiver(line: []const u8) bool {
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, line, from, waiver_token)) |at| {
        const after = at + waiver_token.len;
        const rest = after + reSpaceRun(line, after);
        if (rest < line.len) return true;
        from = at + 1;
    }
    return false;
}

/// `_strip_us`: `__packed__` and `packed` are one attribute to GCC.
pub fn stripUnderscores(name: []const u8) []const u8 {
    if (name.len > min_dunder_len and
        std.mem.startsWith(u8, name, "__") and
        std.mem.endsWith(u8, name, "__")) return name[2 .. name.len - 2];
    return name;
}

/// One comma-separated body element reduced to its attribute name, i.e.
/// `_strip_us(t.strip().split("(")[0].strip())`.
pub fn attributeName(element: []const u8) []const u8 {
    const trimmed = strip(element);
    const head = if (std.mem.indexOfScalar(u8, trimmed, '(')) |at| trimmed[0..at] else trimmed;
    return stripUnderscores(strip(head));
}

fn isAllowedName(name: []const u8) bool {
    for (allowed) |candidate| {
        if (std.mem.eql(u8, name, candidate)) return true;
    }
    return false;
}

/// `names and names <= ALLOWED`, the test that silences an attribute.
///
/// A null body (unbalanced parens) gives the EMPTY set, which is falsy, so
/// the attribute is reported. A body of `" "` gives the one-element set
/// `{""}`, which is non-empty and not allowed, so it is reported too.
pub fn bodyIsAllowed(body: ?[]const u8) bool {
    const text = body orelse return false;
    var rest = text;
    while (true) {
        const cut = std.mem.indexOfScalar(u8, rest, ',');
        const element = if (cut) |at| rest[0..at] else rest;
        if (!isAllowedName(attributeName(element))) return false;
        if (cut) |at| rest = rest[at + 1 ..] else break;
    }
    return true;
}

/// `line.strip()[:100]`: the first 100 CODE POINTS of the trimmed line, not
/// the first 100 bytes.
pub fn snippet(line: []const u8) []const u8 {
    const trimmed = strip(line);
    var i: usize = 0;
    var seen: usize = 0;
    while (i < trimmed.len and seen < 100) : (seen += 1) {
        i += decodeAt(trimmed, i).len;
    }
    return trimmed[0..i];
}

/// True when this line carries at least one reportable attribute.
pub fn lineHasFinding(line: []const u8) bool {
    var iterator = AttrIterator.init(line);
    while (iterator.next()) |start| {
        if (isCommentPos(line, start)) continue;
        if (hasWaiver(line)) continue;
        if (bodyIsAllowed(attrBody(line, start))) continue;
        return true;
    }
    return false;
}

pub const Finding = struct { line: usize, snippet: []const u8 };

/// Every reportable attribute in one already-decoded, already-normalised
/// source text. One finding per MATCH, as inherited: the predecessor appended
/// `(i, line.strip()[:100])` per match, so two matches on one line produced
/// two IDENTICAL rows and both counted.
pub fn scanText(allocator: std.mem.Allocator, text: []const u8) !std.ArrayList(Finding) {
    var findings = std.ArrayList(Finding).init(allocator);
    errdefer findings.deinit();
    if (std.mem.indexOf(u8, text, attr_token) == null) return findings;
    var lines = LineIterator.init(text);
    var number: usize = 0;
    while (lines.next()) |line| {
        number += 1;
        var iterator = AttrIterator.init(line);
        while (iterator.next()) |start| {
            if (isCommentPos(line, start)) continue;
            if (hasWaiver(line)) continue;
            if (bodyIsAllowed(attrBody(line, start))) continue;
            try findings.append(.{ .line = number, .snippet = snippet(line) });
        }
    }
    return findings;
}

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

/// One path COMPONENT naming a build tree: exact `build`, or a `build-` /
/// `build_` / `cmake-build-` prefix. The separator is required, so
/// `builders` is source.
pub fn isBuildDirName(name: []const u8) bool {
    return std.mem.eql(u8, name, "build") or
        std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

fn isToolOutputDirName(name: []const u8) bool {
    for (tool_output_dir_names) |candidate| {
        if (std.mem.eql(u8, name, candidate)) return true;
    }
    return false;
}

fn isBuildTreeRoot(name: []const u8) bool {
    for (build_tree_roots) |candidate| {
        if (std.mem.eql(u8, name, candidate)) return true;
    }
    return false;
}

/// `lint_targets.is_build_output`: directory components only, so a FILE
/// called `build` is not a build tree.
pub fn isBuildOutput(rel: []const u8) bool {
    var parts = std.mem.splitScalar(u8, rel, '/');
    var first: ?[]const u8 = null;
    var index: usize = 0;
    var previous: ?[]const u8 = null;
    while (parts.next()) |part| {
        if (previous) |component| {
            if (isToolOutputDirName(component)) return true;
            const root_is_build_tree = if (index == 1) true else isBuildTreeRoot(first.?);
            if (isBuildDirName(component) and root_is_build_tree) return true;
        }
        if (first == null) first = part;
        previous = part;
        index += 1;
    }
    return false;
}

/// `lint_targets.is_build_output_path`: normalise a str or Path that may be
/// absolute, then apply the repo-relative predicate.
pub fn isBuildOutputPath(allocator: std.mem.Allocator, path: []const u8, repo_root: []const u8) !bool {
    const swapped = try allocator.alloc(u8, path.len);
    defer allocator.free(swapped);
    for (path, 0..) |byte, i| swapped[i] = if (byte == '\\') '/' else byte;
    var text = std.mem.trim(u8, swapped, "/");

    const root_swapped = try allocator.alloc(u8, repo_root.len);
    defer allocator.free(root_swapped);
    for (repo_root, 0..) |byte, i| root_swapped[i] = if (byte == '\\') '/' else byte;
    const root = std.mem.trim(u8, root_swapped, "/");

    const prefix = try std.fmt.allocPrint(allocator, "{s}/", .{root});
    defer allocator.free(prefix);
    if (root.len != 0 and std.mem.startsWith(u8, text, prefix)) {
        text = text[prefix.len..];
    } else if (std.mem.startsWith(u8, text, "./")) {
        text = text[2..];
    }
    return isBuildOutput(text);
}

/// `path.endswith(EXTS)`.
pub fn hasScannedExt(path: []const u8) bool {
    for (exts) |ext| {
        if (std.mem.endsWith(u8, path, ext)) return true;
    }
    return false;
}

/// `any(d in path for d in EXEMPT_DIRS)`.
pub fn isExemptPath(path: []const u8) bool {
    for (exempt_dirs) |fragment| {
        if (std.mem.indexOf(u8, path, fragment) != null) return true;
    }
    return false;
}

/// `sorted()` over `str`: UTF-8 orders bytes the way Python orders code
/// points, so a byte comparison is the code-point comparison.
pub fn pythonLessThan(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

pub fn renderFinding(writer: anytype, path: []const u8, line: usize, text: []const u8) !void {
    try writer.print(
        "{s}:{d}: GNU __attribute__ -- use the C23 [[...]] form (e.g. [[gnu::weak]]); {s}\n",
        .{ path, line, text },
    );
}

pub fn renderSummary(writer: anytype, total: usize) !void {
    try writer.print(
        "\ncheck_no_gnu_attribute: {d} violation(s). Migrate to [[...]] (only interrupt / " ++
            "cmse_nonsecure_entry / cmse_nonsecure_call may stay __attribute__; add " ++
            "`ATTR-OK: <reason>` for a justified exception).\n",
        .{total},
    );
}

pub fn renderClean(writer: anytype) !void {
    try writer.writeAll("check_no_gnu_attribute: clean -- all attributes use the C23 [[...]] form.\n");
}

/// The collapse line drops the `.py`, as the usage line and the selftest
/// summary do: every diagnostic names the tool that ran, and the module this
/// one used to name is deleted in the same change.
pub fn renderCollapsed(writer: anytype, count: usize) !void {
    try writer.print(
        "check_no_gnu_attribute: FATAL -- only {d} first-party source file(s) in scope, " ++
            "floor is {d}. A collapsed sweep reports a clean tree because it scanned nothing.\n",
        .{ count, file_floor },
    );
}

pub fn renderUsage(writer: anytype) !void {
    try writer.writeAll("usage: check_no_gnu_attribute [--selftest] [file ...]\n");
}

// ---------------------------------------------------------------------------
// Selftest
// ---------------------------------------------------------------------------

/// The predecessor's two fixtures, verbatim, minus the temporary directory:
/// the detector is a pure function here, so the cases run in memory.
pub const selftest_bad =
    "void f(void) __attribute__((weak));\n";

pub const selftest_good =
    "[[gnu::weak]] void f(void);\n" ++
    "void irq(void) __attribute__((interrupt));\n" ++
    "void g(void) __attribute__((packed)); /* ATTR-OK: wire ABI */\n" ++
    "// void prose(void) __attribute__((weak));\n";

pub const SelftestCase = struct { passed: bool, label: []const u8 };

/// Both directions, in the inherited order.
pub fn selftestCases(allocator: std.mem.Allocator) ![2]SelftestCase {
    var bad = try scanText(allocator, selftest_bad);
    defer bad.deinit();
    var good = try scanText(allocator, selftest_good);
    defer good.deinit();
    return .{
        .{ .passed = bad.items.len == 1, .label = "migratable GNU attribute fires" },
        .{ .passed = good.items.len == 0, .label = "C23, exact exception, waiver, and prose stay quiet" },
    };
}
