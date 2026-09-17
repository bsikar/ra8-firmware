//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detector for the legacy-task-runner gate (#858).
//!
//! The repository task runner is Just. GNU Make can still be a real
//! dependency of CMake or an upstream source build, so this detector
//! deliberately matches only command-shaped task invocations: an executable
//! shell/YAML/Docker line, a shell command array, or a command presented in
//! quotes/backticks or after a user-guidance verb. Natural English,
//! CMake/Makefile names, tool lists and dependency probes stay outside that
//! shape.
//!
//! Every function here is pure: text in, findings out, no file system and no
//! argv, so the detector is provable with no repository on disk. The five
//! regular expressions the predecessor carried are reproduced as hand-written
//! matchers, because Zig's standard library has no regular expressions and
//! this gate's meaning lives in the exact match. Each matcher's pattern is
//! quoted above it, and the backtracking the engine would do is reproduced
//! where it can change the captured text.
//!
//! Like its predecessor, this source is inside the scanned scope and builds
//! the executable name from fragments, so the detector never reports itself.

const std = @import("std");
const word_chars = @import("word_chars.zig");

/// The legacy runner's executable name, assembled rather than spelled, so
/// this source stays outside its own detector.
pub const command_tail = "ake";

/// The legacy runner, and its GNU-prefixed spelling.
pub const command_word = "m" ++ command_tail;
pub const gnu_command_word = "g" ++ command_word;

/// This detector's own source, force-added to the scanned scope so the gate
/// always scans itself. A compiled tool has no `__file__`, so the path is a
/// constant rather than derived.
pub const self_source = "tools/check_no_legacy_make/src/internal/root.zig";

/// Authored surfaces named exactly.
pub const exact_files = [_][]const u8{
    ".clangd",
    ".cppcheck-suppressions",
    ".env.example",
    "CMakePresets.json",
    "justfile",
};

/// Authored automation trees.
pub const prefixes = [_][]const u8{
    ".devcontainer/",
    ".github/workflows/",
    ".vscode/",
    "just/",
    "scripts/",
    "tools/mcp/",
};

/// Documentation suffixes, compared after case folding.
pub const doc_suffixes = [_][]const u8{ ".md", ".mdx", ".rst" };

/// Vendored and fixture trees, outside the migration contract.
pub const excluded_prefixes = [_][]const u8{
    "docs/sbom/upstream/",
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "port/netxduo/",
    "port/nimble/",
    "port/threadx/",
    "port/usbx/",
    "tests/fixtures/",
};

/// Floor below which the scope is treated as collapsed rather than clean.
pub const min_scoped_files: usize = 650;

/// One decoded code point and the bytes it occupied.
pub const Char = struct { cp: u21, len: usize };

/// Decode the code point at `i`, or null at or past the end. Input is
/// validated UTF-8 before it reaches here, matching the predecessor's strict
/// decode: a source that does not decode is skipped, never scanned.
pub fn charAt(text: []const u8, i: usize) ?Char {
    if (i >= text.len) return null;
    const len = std.unicode.utf8ByteSequenceLength(text[i]) catch return Char{ .cp = text[i], .len = 1 };
    if (i + len > text.len) return Char{ .cp = text[i], .len = 1 };
    const cp = std.unicode.utf8Decode(text[i .. i + len]) catch return Char{ .cp = text[i], .len = 1 };
    return Char{ .cp = cp, .len = len };
}

/// Python's `str.isspace`, which is also the set regular-expression `\s`
/// matches for `str`. The separators 0x1c-0x1f are whitespace here even
/// though only 0x1c-0x1e break a line.
pub fn isPythonSpace(cp: u21) bool {
    return switch (cp) {
        0x09...0x0d, 0x1c...0x20, 0x85, 0xa0, 0x1680 => true,
        0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000 => true,
        else => false,
    };
}

/// Every code point Python's `str.splitlines` breaks on. 0x1f is whitespace
/// but is not a break.
pub fn isLineBreak(cp: u21) bool {
    return switch (cp) {
        0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x85, 0x2028, 0x2029 => true,
        else => false,
    };
}

/// Whether `cp` is a word character for `\b`, Unicode-aware as Python's is.
pub fn isWordChar(cp: u21) bool {
    return word_chars.isWordChar(cp);
}

/// Text-mode universal newlines: CRLF and a lone CR both become LF. The
/// predecessor read its sources in text mode, so line numbers only agree
/// once this runs first.
pub fn normalizeTerminators(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, text.len);
    errdefer out.deinit();
    var i: usize = 0;
    while (i < text.len) : (i += 1) {
        if (text[i] == '\r') {
            try out.append('\n');
            if (i + 1 < text.len and text[i + 1] == '\n') i += 1;
        } else {
            try out.append(text[i]);
        }
    }
    return out.toOwnedSlice();
}

/// `str.splitlines` over already-normalised text: the terminator is dropped
/// and a trailing break yields no extra empty line.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn init(text: []const u8) LineIterator {
        return .{ .text = text };
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var i = self.index;
        while (charAt(self.text, i)) |ch| {
            if (isLineBreak(ch.cp)) {
                const line = self.text[start..i];
                var step = ch.len;
                if (ch.cp == '\r' and i + 1 < self.text.len and self.text[i + 1] == '\n') step += 1;
                self.index = i + step;
                return line;
            }
            i += ch.len;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

/// One command-shaped invocation: the executable as the predecessor rendered
/// it (quotes stripped) and the first argument when the pattern captured one.
pub const Invocation = struct {
    executable: []const u8,
    argument: ?[]const u8 = null,
};

/// `str.strip("\"'")`: remove any run of the two quote characters from both
/// ends.
pub fn stripQuotes(text: []const u8) []const u8 {
    var start: usize = 0;
    var end: usize = text.len;
    while (start < end and (text[start] == '"' or text[start] == '\'')) start += 1;
    while (end > start and (text[end - 1] == '"' or text[end - 1] == '\'')) end -= 1;
    return text[start..end];
}

/// Fold one ASCII pattern letter against one text code point the way
/// `re.IGNORECASE` does. Beyond ASCII case, CPython folds exactly three code
/// points onto letters these patterns contain: U+017F onto `s`, U+212A onto
/// `k`, and U+0130/U+0131 onto `i`.
fn foldEq(pattern: u8, cp: u21) bool {
    if (cp < 0x80) {
        return std.ascii.toLower(@as(u8, @intCast(cp))) == std.ascii.toLower(pattern);
    }
    return switch (std.ascii.toLower(pattern)) {
        's' => cp == 0x17f,
        'k' => cp == 0x212a,
        'i' => cp == 0x130 or cp == 0x131,
        else => false,
    };
}

/// Match a literal at `i`, optionally case-insensitively. Returns the byte
/// length consumed, which is not the literal's length once a folded
/// non-ASCII code point stands in for a letter.
fn matchLiteral(text: []const u8, i: usize, literal: []const u8, fold: bool) ?usize {
    var at = i;
    for (literal) |want| {
        const ch = charAt(text, at) orelse return null;
        if (fold) {
            if (!foldEq(want, ch.cp)) return null;
        } else {
            if (ch.cp != want) return null;
        }
        at += ch.len;
    }
    return at - i;
}

/// Byte length of the whitespace run at `i`, zero when there is none.
pub fn spaceLen(text: []const u8, i: usize) usize {
    var at = i;
    while (charAt(text, at)) |ch| {
        if (!isPythonSpace(ch.cp)) break;
        at += ch.len;
    }
    return at - i;
}

/// One executable match: the captured text and where it ends.
pub const ExecMatch = struct { text: []const u8, end: usize };

/// `(?:g?make|"g?make"|'g?make')` at `i`.
///
/// No alternative can match where another does, and `g?` is decided by the
/// next byte, so this needs no backtracking: at most one candidate exists.
pub fn matchExecutableAt(text: []const u8, i: usize, fold: bool) ?ExecMatch {
    const quote: ?u8 = blk: {
        const ch = charAt(text, i) orelse return null;
        if (ch.cp == '"') break :blk '"';
        if (ch.cp == '\'') break :blk '\'';
        break :blk null;
    };
    const start = if (quote == null) i else i + 1;
    var at = start;
    if (matchLiteral(text, at, gnu_command_word, fold)) |len| {
        at += len;
    } else if (matchLiteral(text, at, command_word, fold)) |len| {
        at += len;
    } else return null;
    if (quote) |q| {
        const ch = charAt(text, at) orelse return null;
        if (ch.cp != q) return null;
        at += ch.len;
    }
    return ExecMatch{ .text = text[i..at], .end = at };
}

/// Byte length of the run at `i` of characters outside `excluded`, treating
/// whitespace as excluded throughout.
fn runLen(text: []const u8, i: usize, excluded: []const u8) usize {
    var at = i;
    while (charAt(text, at)) |ch| {
        if (isPythonSpace(ch.cp)) break;
        if (ch.cp < 0x80 and std.mem.indexOfScalar(u8, excluded, @as(u8, @intCast(ch.cp))) != null) break;
        at += ch.len;
    }
    return at - i;
}

/// `^\s*(?:(?:RUN|run:)\s+)?(EXE)(?=\s|$)(?:\s+([^\s#;&|]+))?`
///
/// The optional Docker/YAML prefix is tried first and dropped when the
/// executable does not follow it, exactly as the engine backtracks.
pub fn matchActive(line: []const u8) ?Invocation {
    const base = spaceLen(line, 0);
    var starts: [2]usize = undefined;
    var count: usize = 0;
    inline for (.{ "RUN", "run:" }) |prefix| {
        if (matchLiteral(line, base, prefix, false)) |len| {
            const spaces = spaceLen(line, base + len);
            if (spaces > 0) {
                starts[count] = base + len + spaces;
                count += 1;
            }
        }
    }
    starts[count] = base;
    count += 1;
    for (starts[0..count]) |start| {
        const exe = matchExecutableAt(line, start, false) orelse continue;
        const after = charAt(line, exe.end);
        if (after != null and !isPythonSpace(after.?.cp)) continue;
        var invocation = Invocation{ .executable = stripQuotes(exe.text) };
        const spaces = spaceLen(line, exe.end);
        if (spaces > 0) {
            const arg_start = exe.end + spaces;
            const len = runLen(line, arg_start, "#;&|");
            if (len > 0) invocation.argument = line[arg_start .. arg_start + len];
        }
        return invocation;
    }
    return null;
}

/// `^\s*[A-Za-z_][A-Za-z0-9_]*\s*=\(\s*(EXE)(?=\s|\))(?:\s+([^\s)]+))?`
pub fn matchArray(line: []const u8) ?Invocation {
    var at = spaceLen(line, 0);
    const first = charAt(line, at) orelse return null;
    if (!(first.cp < 0x80 and (std.ascii.isAlphabetic(@as(u8, @intCast(first.cp))) or first.cp == '_'))) return null;
    at += first.len;
    while (charAt(line, at)) |ch| {
        if (!(ch.cp < 0x80 and (std.ascii.isAlphanumeric(@as(u8, @intCast(ch.cp))) or ch.cp == '_'))) break;
        at += ch.len;
    }
    at += spaceLen(line, at);
    if (matchLiteral(line, at, "=(", false)) |len| {
        at += len;
    } else return null;
    at += spaceLen(line, at);
    const exe = matchExecutableAt(line, at, false) orelse return null;
    const after = charAt(line, exe.end);
    if (after == null) return null;
    if (!(isPythonSpace(after.?.cp) or after.?.cp == ')')) return null;
    var invocation = Invocation{ .executable = stripQuotes(exe.text) };
    const spaces = spaceLen(line, exe.end);
    if (spaces > 0) {
        const arg_start = exe.end + spaces;
        const len = runLen(line, arg_start, ")");
        if (len > 0) invocation.argument = line[arg_start .. arg_start + len];
    }
    return invocation;
}

/// Whether the tail `\s*[.`'"]?\s*$` matches at `i`.
fn commentTailMatches(line: []const u8, i: usize) bool {
    var at = i + spaceLen(line, i);
    if (charAt(line, at)) |ch| {
        if (ch.cp == '.' or ch.cp == '`' or ch.cp == '\'' or ch.cp == '"') at += ch.len;
    }
    at += spaceLen(line, at);
    return at >= line.len;
}

/// ``^\s*#\s*(EXE)\s+([^\s]+)\s*[.`'"]?\s*$``
///
/// The argument run is greedy, so a trailing full stop lands inside the
/// captured argument. Shorter runs are then tried in turn, which is what
/// lets a space-separated closing quote still match.
pub fn matchComment(line: []const u8) ?Invocation {
    var at = spaceLen(line, 0);
    const hash = charAt(line, at) orelse return null;
    if (hash.cp != '#') return null;
    at += hash.len;
    at += spaceLen(line, at);
    const exe = matchExecutableAt(line, at, false) orelse return null;
    const spaces = spaceLen(line, exe.end);
    if (spaces == 0) return null;
    const arg_start = exe.end + spaces;
    const greedy = runLen(line, arg_start, "");
    if (greedy == 0) return null;
    var take = greedy;
    while (take > 0) : (take -= 1) {
        if (commentTailMatches(line, arg_start + take)) {
            return Invocation{
                .executable = stripQuotes(exe.text),
                .argument = line[arg_start .. arg_start + take],
            };
        }
    }
    return null;
}

/// ``(?:`|'|")(g?make)(?:\s+([^\s`'"]+)|(?:`|'|")+\s+(?:target|recipe|task)\b)``
///
/// Case-sensitive, and the captured executable carries no quotes. Leftmost
/// match wins, and at each position the argument form is tried before the
/// prose form.
pub fn matchQuoted(line: []const u8) ?Invocation {
    var i: usize = 0;
    while (charAt(line, i)) |ch| : (i += ch.len) {
        if (!(ch.cp == '`' or ch.cp == '\'' or ch.cp == '"')) continue;
        const name_start = i + ch.len;
        var at = name_start;
        if (matchLiteral(line, at, gnu_command_word, false)) |len| {
            at += len;
        } else if (matchLiteral(line, at, command_word, false)) |len| {
            at += len;
        } else continue;
        const name = line[name_start..at];
        const spaces = spaceLen(line, at);
        if (spaces > 0) {
            const arg_start = at + spaces;
            const len = runLen(line, arg_start, "`'\"");
            if (len > 0) {
                return Invocation{ .executable = name, .argument = line[arg_start .. arg_start + len] };
            }
        }
        var closers = at;
        while (charAt(line, closers)) |q| {
            if (!(q.cp == '`' or q.cp == '\'' or q.cp == '"')) break;
            closers += q.len;
        }
        if (closers == at) continue;
        const gap = spaceLen(line, closers);
        if (gap == 0) continue;
        const word_start = closers + gap;
        inline for (.{ "target", "recipe", "task" }) |noun| {
            if (matchLiteral(line, word_start, noun, false)) |len| {
                const next = charAt(line, word_start + len);
                if (next == null or !isWordChar(next.?.cp)) {
                    return Invocation{ .executable = name };
                }
            }
        }
    }
    return null;
}

/// ``\b(?:run|use|invoke|try|rerun|execute)\s+(EXE)\s+([^\s`'"]+)``, case-insensitive.
///
/// The leading `\b` is Unicode-aware, so an accented letter before the verb
/// suppresses the match. No two verbs can start at one position, so the
/// alternation needs no ordering beyond the first that matches.
pub fn matchGuidance(line: []const u8) ?Invocation {
    var i: usize = 0;
    var previous_word = false;
    while (charAt(line, i)) |ch| : ({
        previous_word = isWordChar(ch.cp);
        i += ch.len;
    }) {
        if (previous_word) continue;
        var verb_len: usize = 0;
        inline for (.{ "run", "use", "invoke", "try", "rerun", "execute" }) |verb| {
            if (verb_len == 0) {
                if (matchLiteral(line, i, verb, true)) |len| verb_len = len;
            }
        }
        if (verb_len == 0) continue;
        const spaces = spaceLen(line, i + verb_len);
        if (spaces == 0) continue;
        const exe = matchExecutableAt(line, i + verb_len + spaces, true) orelse continue;
        const gap = spaceLen(line, exe.end);
        if (gap == 0) continue;
        const arg_start = exe.end + gap;
        const len = runLen(line, arg_start, "`'\"");
        if (len == 0) continue;
        return Invocation{
            .executable = stripQuotes(exe.text),
            .argument = line[arg_start .. arg_start + len],
        };
    }
    return null;
}

/// The command-shaped legacy invocation on `line`, if any.
///
/// Active command forms are only consulted for surfaces that execute their
/// lines; documentation is matched by the three presentation forms alone.
pub fn legacyInvocation(line: []const u8, active_commands: bool) ?Invocation {
    if (active_commands) {
        if (matchActive(line)) |found| return found;
        if (matchArray(line)) |found| return found;
    }
    if (matchComment(line)) |found| return found;
    if (matchQuoted(line)) |found| return found;
    if (matchGuidance(line)) |found| return found;
    return null;
}

/// Render an invocation as the predecessor printed it.
pub fn renderInvocation(allocator: std.mem.Allocator, invocation: Invocation) ![]u8 {
    if (invocation.argument) |argument| {
        return std.fmt.allocPrint(allocator, "{s} {s}", .{ invocation.executable, argument });
    }
    return allocator.dupe(u8, invocation.executable);
}

/// Render one finding line.
pub fn renderFinding(
    allocator: std.mem.Allocator,
    rel: []const u8,
    number: usize,
    invocation: Invocation,
) ![]u8 {
    const rendered = try renderInvocation(allocator, invocation);
    defer allocator.free(rendered);
    return std.fmt.allocPrint(allocator, "{s}:{d}: legacy repository task: {s}", .{ rel, number, rendered });
}

/// Whether a line-executing surface, where the active command forms apply.
pub fn activeCommands(rel: []const u8) bool {
    if (std.mem.endsWith(u8, rel, ".sh")) return true;
    if (std.mem.endsWith(u8, rel, ".yml")) return true;
    if (std.mem.endsWith(u8, rel, ".yaml")) return true;
    return std.mem.eql(u8, pathName(rel), "Dockerfile");
}

/// The final component of a repository-relative path.
pub fn pathName(rel: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, rel, '/')) |cut| return rel[cut + 1 ..];
    return rel;
}

/// `pathlib.PurePath.suffix`: empty for a dot-prefixed name and for a name
/// that ends in its only dot.
pub fn pathSuffix(rel: []const u8) []const u8 {
    const name = pathName(rel);
    const cut = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    if (cut == 0 or cut + 1 >= name.len) return "";
    return name[cut..];
}

/// Whether the suffix is a documentation suffix once lowered.
pub fn isDocSuffix(suffix: []const u8) bool {
    var buffer: [16]u8 = undefined;
    if (suffix.len > buffer.len) return false;
    const lowered = std.ascii.lowerString(buffer[0..suffix.len], suffix);
    for (doc_suffixes) |candidate| {
        if (std.mem.eql(u8, lowered, candidate)) return true;
    }
    return false;
}

/// `^\.github/[^/]*baseline[^/]*\.txt$`
pub fn matchesBaseline(rel: []const u8) bool {
    const head = ".github/";
    if (!std.mem.startsWith(u8, rel, head)) return false;
    const name = rel[head.len..];
    if (std.mem.indexOfScalar(u8, name, '/') != null) return false;
    if (!std.mem.endsWith(u8, name, ".txt")) return false;
    const body = name[0 .. name.len - ".txt".len];
    return std.mem.indexOf(u8, body, "baseline") != null;
}

/// Whether a vendored or fixture path, outside the migration contract.
pub fn isExcluded(rel: []const u8) bool {
    for (excluded_prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return true;
    }
    return false;
}

/// Whether the path is an authored surface the contract covers.
pub fn isSelected(rel: []const u8) bool {
    for (exact_files) |candidate| {
        if (std.mem.eql(u8, rel, candidate)) return true;
    }
    for (prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return true;
    }
    if (matchesBaseline(rel)) return true;
    if (isDocSuffix(pathSuffix(rel))) return true;
    return std.mem.eql(u8, pathName(rel), "Dockerfile");
}

/// Scan one already-decoded source, appending a finding line per hit.
///
/// `scratch` holds the normalised copy and is released by the caller between
/// sources; findings outlive the scan and come from `allocator`.
pub fn scanText(
    allocator: std.mem.Allocator,
    scratch: std.mem.Allocator,
    rel: []const u8,
    text: []const u8,
    findings: *std.ArrayList([]const u8),
) !void {
    const normalised = try normalizeTerminators(scratch, text);
    defer scratch.free(normalised);
    const active = activeCommands(rel);
    var lines = LineIterator.init(normalised);
    var number: usize = 0;
    while (lines.next()) |line| {
        number += 1;
        if (legacyInvocation(line, active)) |invocation| {
            try findings.append(try renderFinding(allocator, rel, number, invocation));
        }
    }
}

/// One both-direction detector case.
pub const SelftestCase = struct {
    line: []const u8,
    expected: bool,
    label: []const u8,
};

/// The cases the predecessor proved, carried across unchanged: every command
/// form fires, and every legitimate mention stays quiet.
pub const selftest_cases = [_]SelftestCase{
    .{ .line = command_word ++ " ci", .expected = true, .label = "a direct shell task fires" },
    .{ .line = command_word ++ " -C apps/board/stand_alone/blink build", .expected = true, .label = "a -C task fires" },
    .{ .line = gnu_command_word ++ " ci", .expected = true, .label = "a gmake task fires" },
    .{ .line = "cmd=(" ++ command_word ++ " -C apps/blink)", .expected = true, .label = "a command array fires" },
    .{ .line = "cmd=(\"" ++ command_word ++ "\" \"-C\" apps/blink)", .expected = true, .label = "a quoted array command fires" },
    .{ .line = "\"" ++ command_word ++ "\" -C apps/blink", .expected = true, .label = "a quoted executable fires" },
    .{ .line = "run: " ++ command_word ++ " -C apps/blink", .expected = true, .label = "a one-line YAML command fires" },
    .{ .line = "RUN " ++ command_word ++ " coverage", .expected = true, .label = "a Dockerfile task fires" },
    .{ .line = "# " ++ command_word ++ " ci-native", .expected = true, .label = "a bare comment hint fires" },
    .{ .line = "# `" ++ command_word ++ " sbom` regenerates it", .expected = true, .label = "a backticked hint fires" },
    .{ .line = "CI (or a local ``" ++ command_word ++ "`` target) catches drift", .expected = true, .label = "a quoted legacy task-runner reference fires" },
    .{ .line = "Please run " ++ command_word ++ " misra", .expected = true, .label = "an unquoted user hint fires" },
    .{ .line = "command -v " ++ command_word ++ " || missing=build-essential", .expected = false, .label = "a dependency probe stays quiet" },
    .{ .line = "command -v " ++ gnu_command_word ++ " || missing=build-essential", .expected = false, .label = "a gmake probe stays quiet" },
    .{ .line = "for tool in curl cmake " ++ command_word ++ " tar cc; do", .expected = false, .label = "an upstream tool list stays quiet" },
    .{ .line = "these controls " ++ command_word ++ " an empty scan fail", .expected = false, .label = "natural English stays quiet" },
    .{ .line = "# " ++ command_word ++ " the detector fail", .expected = false, .label = "natural comment prose stays quiet" },
    .{ .line = "CMakeLists.txt and GNU" ++ command_word ++ "file", .expected = false, .label = "build-system filenames stay quiet" },
    .{ .line = "# M" ++ command_tail ++ " is required by an upstream source build", .expected = false, .label = "an explanatory mention stays quiet" },
};

/// Labels of the cases whose detector answer disagrees with the contract.
pub fn selftestFailures(allocator: std.mem.Allocator) ![][]const u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    errdefer failures.deinit();
    for (selftest_cases) |case| {
        const fired = legacyInvocation(case.line, true) != null;
        if (fired != case.expected) try failures.append(case.label);
    }
    return failures.toOwnedSlice();
}
