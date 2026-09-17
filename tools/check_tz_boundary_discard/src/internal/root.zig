//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Ban silent `ra8_err_t` discards at TrustZone boot boundaries (#191, T2-05).
//!
//! C23 makes an explicit `(void)` cast the sanctioned suppression for a
//! `[[nodiscard]]` diagnostic, so `-Wall -Wextra -Werror` can never flag
//!
//!   (void)ra8_cgc_init();   /* discarded right before BLXNS */
//!
//! even though the callee is `[[nodiscard]] ra8_err_t`. This module is the
//! backstop, and it is the whole of the decision: no file system, no argv, no
//! writers of its own beyond the renderers callers hand a stream to.
//!
//! Rules (first-party C/C++ only):
//!
//!   A. The world-switch family is must-handle EVERYWHERE: a `(void)`-cast
//!      discard of any `ra8_tz_secure_boot_*()` call is banned in every
//!      first-party file. A returned error there is a root-of-trust DENIAL or
//!      a rejected NS vector table.
//!   B. Inside a boot translation unit (any `.c` that defines `SystemInit` or
//!      `ra8_trustzone_init`) a `(void)`-cast discard of ANY `ra8_*()` call is
//!      banned; every fallible step in those TUs gates the S->NS switch.
//!
//! Ported from `scripts/checks/check_tz_boundary_discard.py`. Every regex the
//! predecessor used is reimplemented as a hand-rolled scan, which is only
//! sound because each `\s*` / `\s+` in those patterns is followed by a literal
//! that cannot itself be whitespace, so greedy matching needs no backtracking.
//! The one place the predecessor's Unicode semantics show through is
//! whitespace: `char_classes.zig` carries CPython's own tables.

const std = @import("std");
pub const char_classes = @import("char_classes.zig");

pub const tool_name = "check_tz_boundary_discard";

/// Roots walked by the whole-tree sweep, relative to the working directory,
/// exactly as the predecessor's `Path(root)` was.
pub const roots = [_][]const u8{ "libs", "tests", "examples", "port", "tools", "apps" };
pub const exts = [_][]const u8{ ".c", ".h", ".cpp", ".hpp" };
pub const exempt_dirs = [_][]const u8{ "/third_party/", "/ra8_fonts/" };

/// A tree this size cannot legitimately collapse to a handful of files. Below
/// this the sweep is broken (a bad working directory, a renamed root) and
/// reporting "clean" would be a lie: no boot TU would be read, so no discard
/// could ever be reported. Measured 2026-07-28: 2125 first-party C/C++ files.
pub const file_floor: usize = 1700;

/// `raw.strip()[:100]`, counted in code points as Python counts them.
pub const snippet_code_points: usize = 100;

pub const family_prefix = "ra8_tz_secure_boot_";
pub const any_prefix = "ra8_";
pub const waiver_marker = "TZ-DISCARD-OK:";
pub const boot_entry_points = [_][]const u8{ "SystemInit", "ra8_trustzone_init" };

/// `lint_targets.BUILD_TREE_ROOTS` and `TOOL_OUTPUT_DIR_NAMES`, reimplemented
/// because the predecessor imported `is_build_output_path` from that module.
pub const build_tree_roots = [_][]const u8{ "docs", "examples", "local-poc", "port", "tests", "tools", "apps" };
pub const tool_output_dir_names = [_][]const u8{ ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules" };

pub const Rule = enum {
    a,
    b,

    pub fn label(self: Rule) []const u8 {
        return switch (self) {
            .a => "A",
            .b => "B",
        };
    }

    /// The `what` clause the predecessor chose per rule.
    pub fn what(self: Rule) []const u8 {
        return switch (self) {
            .a => "world-switch result discarded",
            .b => "boot-TU ra8_* result discarded",
        };
    }
};

pub const Span = struct { start: usize, end: usize };

pub const Finding = struct {
    line: usize,
    rule: Rule,
    snippet: []const u8,
};

// ---------------------------------------------------------------------------
// Text primitives
// ---------------------------------------------------------------------------

pub const Decoded = struct { code_point: u21, len: usize };

/// Decode one code point. Callers only reach here with text that already
/// decoded as UTF-8 (the predecessor's `read_text(encoding="utf-8")` raised
/// `UnicodeDecodeError` otherwise and the file was skipped), so a malformed
/// sequence degrades to one opaque byte rather than an error.
pub fn decodeAt(text: []const u8, index: usize) Decoded {
    const first = text[index];
    if (first < 0x80) return .{ .code_point = first, .len = 1 };
    const len = std.unicode.utf8ByteSequenceLength(first) catch return .{ .code_point = first, .len = 1 };
    if (index + len > text.len) return .{ .code_point = first, .len = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + len]) catch {
        return .{ .code_point = first, .len = 1 };
    };
    return .{ .code_point = code_point, .len = len };
}

/// Decode the code point ending at `index`, for the `\s*$` scan that has to
/// walk backwards.
pub fn decodeBefore(text: []const u8, index: usize) Decoded {
    if (index == 0) return .{ .code_point = 0, .len = 0 };
    var start = index;
    var stepped: usize = 0;
    while (start > 0 and stepped < 4) {
        start -= 1;
        stepped += 1;
        if (text[start] & 0xC0 != 0x80) break;
    }
    const decoded = decodeAt(text, start);
    if (start + decoded.len == index) return decoded;
    return .{ .code_point = text[index - 1], .len = 1 };
}

pub fn isReSpace(code_point: u21) bool {
    return char_classes.inTable(&char_classes.re_space_intervals, code_point);
}

pub fn isStrSpace(code_point: u21) bool {
    return char_classes.inTable(&char_classes.str_space_intervals, code_point);
}

/// End of the `\s*` run starting at `start`.
pub fn spaceRun(text: []const u8, start: usize) usize {
    var index = start;
    while (index < text.len) {
        const decoded = decodeAt(text, index);
        if (!isReSpace(decoded.code_point)) break;
        index += decoded.len;
    }
    return index;
}

/// Start of the `\s*` run ending at `end`.
pub fn spaceRunBackward(text: []const u8, end: usize) usize {
    var index = end;
    while (index > 0) {
        const decoded = decodeBefore(text, index);
        if (decoded.len == 0 or !isReSpace(decoded.code_point)) break;
        index -= decoded.len;
    }
    return index;
}

/// Width of a `str.splitlines()` boundary at `index`, 0 when there is none.
pub fn lineBreakLen(text: []const u8, index: usize) usize {
    const decoded = decodeAt(text, index);
    return switch (decoded.code_point) {
        '\r' => if (index + 1 < text.len and text[index + 1] == '\n') 2 else 1,
        '\n', 0x000B, 0x000C, 0x001C, 0x001D, 0x001E, 0x0085, 0x2028, 0x2029 => decoded.len,
        else => 0,
    };
}

/// `str.splitlines()`: every boundary CPython honours, terminators dropped,
/// and no trailing empty line after a final break.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var index = start;
        while (index < self.text.len) {
            const brk = lineBreakLen(self.text, index);
            if (brk != 0) {
                self.index = index + brk;
                return self.text[start..index];
            }
            index += decodeAt(self.text, index).len;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

pub fn lstrip(text: []const u8) []const u8 {
    var index: usize = 0;
    while (index < text.len) {
        const decoded = decodeAt(text, index);
        if (!isStrSpace(decoded.code_point)) break;
        index += decoded.len;
    }
    return text[index..];
}

pub fn strip(text: []const u8) []const u8 {
    const front = lstrip(text);
    var end = front.len;
    while (end > 0) {
        const decoded = decodeBefore(front, end);
        if (decoded.len == 0 or !isStrSpace(decoded.code_point)) break;
        end -= decoded.len;
    }
    return front[0..end];
}

/// `raw.strip()[:100]`: a code-point slice, so a snippet of wide characters
/// keeps the predecessor's length rather than a byte-truncated prefix.
pub fn snippet(raw: []const u8) []const u8 {
    const trimmed = strip(raw);
    var index: usize = 0;
    var counted: usize = 0;
    while (index < trimmed.len and counted < snippet_code_points) {
        index += decodeAt(trimmed, index).len;
        counted += 1;
    }
    return trimmed[0..index];
}

// ---------------------------------------------------------------------------
// The four patterns
// ---------------------------------------------------------------------------

/// `"(void)" in text.replace(" ", "")`, the fast path that keeps the scan off
/// every file with no cast in it. Only the ASCII space is removed, exactly as
/// `str.replace(" ", "")` does.
pub fn hasVoidIgnoringSpaces(text: []const u8) bool {
    const needle = "(void)";
    var matched: usize = 0;
    for (text) |byte| {
        if (byte == ' ') continue;
        if (byte == needle[matched]) {
            matched += 1;
            if (matched == needle.len) return true;
        } else {
            matched = if (byte == needle[0]) 1 else 0;
        }
    }
    return false;
}

fn isNameByte(byte: u8) bool {
    return (byte >= 'a' and byte <= 'z') or (byte >= '0' and byte <= '9') or byte == '_';
}

/// `^\s*void\s+(?:SystemInit|ra8_trustzone_init)\s*\(\s*void\s*\)` anchored at
/// `start`.
pub fn matchBootEntryAt(text: []const u8, start: usize) bool {
    var index = spaceRun(text, start);
    if (!std.mem.startsWith(u8, text[index..], "void")) return false;
    index += "void".len;
    const after_void = spaceRun(text, index);
    if (after_void == index) return false; // `\s+` needs one
    index = after_void;
    var named = false;
    for (boot_entry_points) |name| {
        if (std.mem.startsWith(u8, text[index..], name)) {
            index += name.len;
            named = true;
            break;
        }
    }
    if (!named) return false;
    index = spaceRun(text, index);
    if (index >= text.len or text[index] != '(') return false;
    index = spaceRun(text, index + 1);
    if (!std.mem.startsWith(u8, text[index..], "void")) return false;
    index = spaceRun(text, index + "void".len);
    return index < text.len and text[index] == ')';
}

/// `BOOT_TU_RE.search(text)` with `re.MULTILINE`: `^` matches at the start of
/// the document and after a `\n`, and after a `\n` ONLY, which is why the
/// other `splitlines()` boundaries are not anchor points here.
pub fn bootTuMatch(text: []const u8) bool {
    var position: usize = 0;
    while (true) {
        if (matchBootEntryAt(text, position)) return true;
        const newline = std.mem.indexOfScalarPos(u8, text, position, '\n') orelse return false;
        position = newline + 1;
        if (position >= text.len) return matchBootEntryAt(text, position);
    }
}

/// One `\(\s*void\s*\)\s*(<prefix>[a-z0-9_]+)\s*\(` match anchored at `start`;
/// returns the end offset, as `m.end()` reported it.
pub fn matchDiscardAt(line: []const u8, start: usize, prefix: []const u8) ?usize {
    if (start >= line.len or line[start] != '(') return null;
    var index = spaceRun(line, start + 1);
    if (!std.mem.startsWith(u8, line[index..], "void")) return null;
    index = spaceRun(line, index + "void".len);
    if (index >= line.len or line[index] != ')') return null;
    index = spaceRun(line, index + 1);
    if (!std.mem.startsWith(u8, line[index..], prefix)) return null;
    index += prefix.len;
    var tail: usize = 0;
    while (index < line.len and isNameByte(line[index])) {
        index += 1;
        tail += 1;
    }
    if (tail == 0) return null; // `[a-z0-9_]+` needs one
    index = spaceRun(line, index);
    if (index >= line.len or line[index] != '(') return null;
    return index + 1;
}

/// `finditer`: the next non-overlapping match at or after `from`.
pub fn findDiscard(line: []const u8, from: usize, prefix: []const u8) ?Span {
    var index = from;
    while (index < line.len) {
        if (line[index] == '(') {
            if (matchDiscardAt(line, index, prefix)) |end| return .{ .start = index, .end = end };
        }
        index += decodeAt(line, index).len;
    }
    return null;
}

/// `WAIVER_RE.search(line)`: `TZ-DISCARD-OK:` followed by optional whitespace
/// and then at least one non-whitespace code point. The reason text is
/// required, so a bare marker at end of line waives nothing; a later
/// occurrence on the same line can still satisfy it, as `search` would.
pub fn hasWaiver(line: []const u8) bool {
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, line, from, waiver_marker)) |found| {
        const after = spaceRun(line, found + waiver_marker.len);
        if (after < line.len) return true;
        from = found + 1;
    }
    return false;
}

/// `re.search(r"\(\s*void\s*\)\s*$", raw)`: a cast left dangling at end of
/// line, which the predecessor joined with the following line before scanning.
pub fn endsWithVoidCast(raw: []const u8) bool {
    var end = raw.len;
    if (end > 0 and raw[end - 1] == '\n') end -= 1; // `$` before a single trailing newline
    var index = spaceRunBackward(raw, end);
    if (index == 0 or raw[index - 1] != ')') return false;
    index = spaceRunBackward(raw, index - 1);
    if (index < "void".len or !std.mem.eql(u8, raw[index - "void".len .. index], "void")) return false;
    index = spaceRunBackward(raw, index - "void".len);
    return index > 0 and raw[index - 1] == '(';
}

/// The predecessor's deliberately crude comment test: a line whose first
/// non-space characters open a comment, a `//` anywhere before the match, or
/// an unterminated `/*` before it.
pub fn isCommentPos(line: []const u8, position: usize) bool {
    const stripped = lstrip(line);
    if (std.mem.startsWith(u8, stripped, "*") or
        std.mem.startsWith(u8, stripped, "//") or
        std.mem.startsWith(u8, stripped, "/*")) return true;
    const before = line[0..@min(position, line.len)];
    if (std.mem.indexOf(u8, before, "//") != null) return true;
    return std.mem.indexOf(u8, before, "/*") != null and std.mem.indexOf(u8, before, "*/") == null;
}

// ---------------------------------------------------------------------------
// Path predicates (`lint_targets.is_build_output_path`, reimplemented)
// ---------------------------------------------------------------------------

pub fn isBuildDirName(name: []const u8) bool {
    return std.mem.eql(u8, name, "build") or
        std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

fn inList(list: []const []const u8, name: []const u8) bool {
    for (list) |entry| {
        if (std.mem.eql(u8, entry, name)) return true;
    }
    return false;
}

pub fn stripSlashes(text: []const u8) []const u8 {
    var start: usize = 0;
    var end = text.len;
    while (start < end and text[start] == '/') start += 1;
    while (end > start and text[end - 1] == '/') end -= 1;
    return text[start..end];
}

/// `is_build_output`: directory components only, so a FILE called `build` is
/// not a build tree.
pub fn isBuildOutput(rel: []const u8) bool {
    var first: ?[]const u8 = null;
    var index: usize = 0;
    var parts = std.mem.splitScalar(u8, rel, '/');
    while (parts.next()) |part| {
        if (parts.peek() == null) break; // `parts[:-1]`
        if (first == null) first = part;
        if (inList(&tool_output_dir_names, part)) return true;
        if (isBuildDirName(part) and (index == 0 or inList(&build_tree_roots, first.?))) return true;
        index += 1;
    }
    return false;
}

/// `is_build_output_path`: normalise an absolute, repo-relative or
/// slash-wrapped form, then apply `is_build_output`.
pub fn isBuildOutputPath(path: []const u8, repo_root: []const u8) bool {
    var text = stripSlashes(path);
    const root = stripSlashes(repo_root);
    if (root.len != 0 and text.len > root.len + 1 and
        std.mem.startsWith(u8, text, root) and text[root.len] == '/')
    {
        text = text[root.len + 1 ..];
    } else if (std.mem.startsWith(u8, text, "./")) {
        text = text[2..];
    }
    return isBuildOutput(text);
}

pub fn hasScannedExt(path: []const u8) bool {
    for (exts) |ext| {
        if (std.mem.endsWith(u8, path, ext)) return true;
    }
    return false;
}

pub fn isExempt(path: []const u8) bool {
    for (exempt_dirs) |fragment| {
        if (std.mem.indexOf(u8, path, fragment) != null) return true;
    }
    return false;
}

pub fn isCFile(path: []const u8) bool {
    return std.mem.endsWith(u8, path, ".c");
}

/// `sorted()` over `str`: code-point order, which for UTF-8 is byte order.
pub fn pythonLessThan(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.lessThan(u8, left, right);
}

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------

fn containsSpan(seen: []const Span, span: Span) bool {
    for (seen) |entry| {
        if (entry.start == span.start and entry.end == span.end) return true;
    }
    return false;
}

/// `check_file`, minus the read: every finding for one document, in the
/// predecessor's order (rule A across the line, then rule B), sharing one
/// `seen` set per line so a world-switch discard inside a boot TU is reported
/// once, under rule A.
///
/// Returned snippets borrow from `text`.
pub fn scanText(allocator: std.mem.Allocator, text: []const u8, is_c_file: bool) ![]Finding {
    var findings = std.ArrayList(Finding).init(allocator);
    errdefer findings.deinit();
    if (!hasVoidIgnoringSpaces(text)) return findings.toOwnedSlice();

    const boot_tu = is_c_file and bootTuMatch(text);

    var lines = std.ArrayList([]const u8).init(allocator);
    defer lines.deinit();
    var iterator = LineIterator{ .text = text };
    while (iterator.next()) |line| try lines.append(line);

    var joined = std.ArrayList(u8).init(allocator);
    defer joined.deinit();
    var seen = std.ArrayList(Span).init(allocator);
    defer seen.deinit();

    for (lines.items, 1..) |raw, number| {
        var line = raw;
        if (endsWithVoidCast(raw) and number < lines.items.len) {
            joined.clearRetainingCapacity();
            try joined.appendSlice(raw);
            try joined.appendSlice(lines.items[number]);
            line = joined.items;
        }
        if (hasWaiver(line)) continue;
        seen.clearRetainingCapacity();
        const rules = [_]Rule{ .a, .b };
        for (rules) |rule| {
            if (rule == .b and !boot_tu) break;
            const prefix = if (rule == .a) family_prefix else any_prefix;
            var from: usize = 0;
            while (findDiscard(line, from, prefix)) |span| {
                from = span.end;
                if (containsSpan(seen.items, span) or isCommentPos(line, span.start)) continue;
                try seen.append(span);
                try findings.append(.{ .line = number, .rule = rule, .snippet = snippet(raw) });
            }
        }
    }
    return findings.toOwnedSlice();
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

pub fn renderFinding(writer: anytype, path: []const u8, finding: Finding) !void {
    try writer.print(
        "{s}:{d}: [rule {s}] {s} -- handle the ra8_err_t " ++
            "(halt or a documented fallback; RA8_ERROR_CHECK[_NO_ABORT]); " ++
            "never (void)-cast it at a TrustZone boot boundary; {s}\n",
        .{ path, finding.line, finding.rule.label(), finding.rule.what(), finding.snippet },
    );
}

pub fn renderSummary(writer: anytype, total: usize) !void {
    try writer.print(
        "\n" ++ tool_name ++ ": {d} violation(s). A (void)-cast " ++
            "silences [[nodiscard]] by ISO C23 rule, so -Werror cannot catch " ++
            "these; check the result and fail safe instead (add " ++
            "`TZ-DISCARD-OK: <reason>` only for a justified exception).\n",
        .{total},
    );
}

pub fn renderClean(writer: anytype) !void {
    try writer.writeAll(tool_name ++ ": clean -- no silent ra8_err_t discards at " ++
        "TrustZone boot boundaries.\n");
}

pub fn renderFloor(writer: anytype, count: usize) !void {
    try writer.print(
        tool_name ++ ": FATAL -- only {d} first-party source " ++
            "file(s) in scope, floor is {d}. A collapsed sweep reports a " ++
            "clean tree because it scanned nothing.\n",
        .{ count, file_floor },
    );
}

pub fn renderUsage(writer: anytype) !void {
    try writer.writeAll("usage: " ++ tool_name ++ " [--selftest] [file ...]\n");
}

// ---------------------------------------------------------------------------
// Selftest (the predecessor's fixtures, as text rather than temporary files)
// ---------------------------------------------------------------------------

pub const family_fixture = "void f(void) { (void)ra8_tz_secure_boot_verify(); }\n";
pub const boot_fixture = "void SystemInit(void) { (void)ra8_cgc_init(); }\n";
pub const good_fixture =
    "void SystemInit(void) { if (ra8_cgc_init() != k_ra8_ok) { halt(); } }\n" ++
    "void f(void) { (void)ra8_tz_secure_boot_verify(); } " ++
    "/* TZ-DISCARD-OK: synthetic documented fallback */\n";

pub const selftest_labels = [_][]const u8{
    "world-switch and boot-translation-unit discards both fire",
    "handled results and exact reasoned waiver stay quiet",
};

pub const SelftestOutcome = struct { passed: [selftest_labels.len]bool };

/// Prove both boundary rules fire and that handled or waived calls stay quiet.
pub fn runSelftest(allocator: std.mem.Allocator) !SelftestOutcome {
    const family = try scanText(allocator, family_fixture, true);
    defer allocator.free(family);
    const boot = try scanText(allocator, boot_fixture, true);
    defer allocator.free(boot);
    const good = try scanText(allocator, good_fixture, true);
    defer allocator.free(good);

    var saw_a = false;
    var saw_b = false;
    var extra = false;
    for (family) |finding| switch (finding.rule) {
        .a => saw_a = true,
        .b => saw_b = true,
    };
    for (boot) |finding| switch (finding.rule) {
        .a => saw_a = true,
        .b => saw_b = true,
    };
    _ = &extra;
    return .{ .passed = .{ saw_a and saw_b, good.len == 0 } };
}

pub fn renderSelftest(writer: anytype, error_writer: anytype, outcome: SelftestOutcome) !u8 {
    var failures: usize = 0;
    for (selftest_labels, outcome.passed) |label, passed| {
        if (!passed) failures += 1;
        try writer.print("  [{s}] {s}\n", .{ if (passed) "ok" else "FAIL", label });
    }
    if (failures != 0) {
        try error_writer.print(tool_name ++ " --selftest: {d} failure(s)\n", .{failures});
        return 1;
    }
    try writer.writeAll(tool_name ++ " --selftest: all cases pass (both directions).\n");
    return 0;
}
