//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Guard locator and escape detector for the stub-crypto gate (#858).
//!
//! Several secure-side translation units ship a deliberately INSECURE
//! placeholder body (a deterministic PRNG standing in for a TRNG, a plain
//! SRAM key store standing in for a hardware vault, fiction-opcode RSIP
//! bodies). Each one must sit inside
//!
//!     #if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)
//!
//! with a fail-closed `#else`, so a production image that sets neither flag
//! compiles the refusal rather than the placeholder.
//!
//! Every function here is pure: text in, findings out, no file system and no
//! argv, so the detector is provable with no repository on disk. The four
//! regular expressions the predecessor carried
//! (`^\s*#\s*if(n?def)?\b`, `^\s*#\s*else\b`, `^\s*#\s*endif\b` and
//! `^\s*#\s*error\b`, plus the opener's `^\s*#\s*if\b`) are reproduced as
//! hand-written matchers, because Zig's standard library has no regular
//! expressions and this gate's meaning lives in the exact match.

const std = @import("std");
const word_chars = @import("word_chars.zig");

/// The dev/eval flag that admits an insecure placeholder body.
pub const insecure_flag = "RA8_INSECURE_STUB_CRYPTO";

/// The off-target flag that admits the same body in a host build.
pub const off_target_flag = "RA8_OFF_TARGET";

/// The hard-error family a fail-closed `#else` returns.
pub const hard_error_token = "k_ra8_err_";

/// The guard as it is spelled in prose and in every diagnostic.
pub const guard_spelling = "#if defined(" ++ insecure_flag ++ ") || defined(" ++ off_target_flag ++ ")";

/// One stub translation unit and the token that appears ONLY in its insecure
/// body: never in the fail-closed `#else`, never in surrounding prose.
pub const Stub = struct {
    rel: []const u8,
    token: []const u8,
};

/// The governed set, in the order the predecessor's mapping listed it. There
/// is no allowlist: a TU either guards its insecure body fail-closed, or the
/// placeholder goes away in favour of a real backend.
pub const stub_tus = [_]Stub{
    .{ .rel = "libs/ra8_secure_app/src/secure_trng.c", .token = "internal_xorshift64" },
    .{ .rel = "libs/ra8_secure_app/src/key_vault.c", .token = "s_vault" },
    .{ .rel = "libs/ra8_hal/src/ra8_rsip_key_injection.c", .token = "ki_compute_mac" },
    .{ .rel = "libs/ra8_hal/src/ra8_rsip_ecc.c", .token = "k_ra8_rsip_asym_op_eddsa_sign" },
    .{ .rel = "libs/ra8_hal/src/ra8_rsip_cipher.c", .token = "internal_sym_run" },
    .{ .rel = "libs/ra8_hal/src/ra8_rsip_rsa.c", .token = "internal_rsa_dispatch" },
    .{ .rel = "libs/ra8_hal/src/ra8_rsip_asym.c", .token = "internal_hash_pull_digest" },
    .{ .rel = "libs/ra8_hal/src/ra8_rsip_devsec.c", .token = "k_ra8_rsip_off_life_state" },
};

/// One decoded code point and the bytes it occupies.
pub const Char = struct {
    code_point: u21,
    len: usize,
};

/// Decode the code point at `index`, or null at the end of the text.
///
/// A byte sequence that is not valid UTF-8 decodes as the single byte, so a
/// matcher cannot walk off the end of a malformed source; the caller rejects
/// such a source before scanning it, exactly as the predecessor's strict
/// `read_text(encoding="utf-8")` did.
pub fn charAt(text: []const u8, index: usize) ?Char {
    if (index >= text.len) return null;
    const first = text[index];
    if (first < 0x80) return .{ .code_point = first, .len = 1 };
    const len = std.unicode.utf8ByteSequenceLength(first) catch return .{ .code_point = first, .len = 1 };
    if (index + len > text.len) return .{ .code_point = first, .len = 1 };
    const decoded = std.unicode.utf8Decode(text[index .. index + len]) catch
        return .{ .code_point = first, .len = 1 };
    return .{ .code_point = decoded, .len = len };
}

/// Whether `cp` is a word character, as Python's `\w` and therefore `\b` see it.
///
/// The directive keywords all end in `\b`, and Python's `\b` is Unicode-aware,
/// so `#endif` followed by an accented letter is NOT a directive. An
/// ASCII-only boundary would close a guard the predecessor left open.
pub fn isWordChar(cp: u21) bool {
    return word_chars.isWordChar(cp);
}

/// Whether `cp` is whitespace, as Python's `\s` sees it.
pub fn isPythonSpace(cp: u21) bool {
    return switch (cp) {
        0x09...0x0d, 0x1c...0x1f, 0x20, 0x85, 0xa0, 0x1680 => true,
        0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000 => true,
        else => false,
    };
}

/// Whether `cp` ends a line, as `str.splitlines` sees it.
///
/// Carriage returns are folded into `\n` before the split, so CRLF counts
/// once; the rest of the set is what makes a form feed mid-file shift every
/// later line number, and line numbers are what this gate reports.
pub fn isLineBreak(cp: u21) bool {
    return switch (cp) {
        0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x85, 0x2028, 0x2029 => true,
        else => false,
    };
}

/// Fold CRLF and a lone CR into LF, the way Python's text mode reads a file.
pub fn normalizeTerminators(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, text.len);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < text.len) {
        const byte = text[index];
        if (byte == '\r') {
            try out.append('\n');
            index += if (index + 1 < text.len and text[index + 1] == '\n') 2 else 1;
            continue;
        }
        try out.append(byte);
        index += 1;
    }
    return out.toOwnedSlice();
}

/// Walk a text as `str.splitlines()` does: no terminators, no trailing empty
/// line after a final break.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn init(text: []const u8) LineIterator {
        return .{ .text = text };
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var cursor = start;
        while (charAt(self.text, cursor)) |ch| {
            if (isLineBreak(ch.code_point)) {
                self.index = cursor + ch.len;
                return self.text[start..cursor];
            }
            cursor += ch.len;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

/// `str.splitlines()` over an already-folded text.
pub fn splitLines(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer out.deinit();
    var lines = LineIterator.init(text);
    while (lines.next()) |line| try out.append(line);
    return out.toOwnedSlice();
}

/// Index of the first character that is not Python whitespace, from `start`.
fn skipSpace(line: []const u8, start: usize) usize {
    var index = start;
    while (charAt(line, index)) |ch| {
        if (!isPythonSpace(ch.code_point)) break;
        index += ch.len;
    }
    return index;
}

/// Whether a `\b` sits at `index`, given the character before it is a word one.
fn boundaryAfter(line: []const u8, index: usize) bool {
    const ch = charAt(line, index) orelse return true;
    return !isWordChar(ch.code_point);
}

/// Match `^\s*#\s*<keyword>\b`, trying the keywords in the order given.
///
/// Trying them in order is what makes `#ifdef` a directive and `#ifdefx` an
/// ordinary line: the longer spelling is offered first, and when its own `\b`
/// fails the shorter one fails too, because a word character still sits where
/// the boundary must be.
pub fn matchesDirective(line: []const u8, keywords: []const []const u8) bool {
    var index = skipSpace(line, 0);
    if (index >= line.len or line[index] != '#') return false;
    index = skipSpace(line, index + 1);
    for (keywords) |keyword| {
        if (line.len - index < keyword.len) continue;
        if (!std.mem.eql(u8, line[index .. index + keyword.len], keyword)) continue;
        if (!boundaryAfter(line, index + keyword.len)) continue;
        return true;
    }
    return false;
}

/// Whether the line opens a conditional: `#if`, `#ifdef` or `#ifndef`.
pub fn isIf(line: []const u8) bool {
    return matchesDirective(line, &.{ "ifndef", "ifdef", "if" });
}

/// Whether the line is an `#else`. `#elif` is NOT one, inherited as-is: the
/// predecessor's walk ignored `#elif` entirely.
pub fn isElse(line: []const u8) bool {
    return matchesDirective(line, &.{"else"});
}

/// Whether the line is an `#endif`.
pub fn isEndif(line: []const u8) bool {
    return matchesDirective(line, &.{"endif"});
}

/// Whether the line is a compile-time `#error`.
pub fn isError(line: []const u8) bool {
    return matchesDirective(line, &.{"error"});
}

/// Whether the line is the stub-crypto guard opener, in either flag order.
///
/// The opener is `#if` only: `#ifdef RA8_INSECURE_STUB_CRYPTO` is not the
/// guard, because a single-flag `#ifdef` cannot express the `||`. Both flags
/// are plain substring tests, so the two orders and any spacing are accepted.
pub fn isGuardOpen(line: []const u8) bool {
    if (!matchesDirective(line, &.{"if"})) return false;
    if (std.mem.indexOf(u8, line, insecure_flag) == null) return false;
    return std.mem.indexOf(u8, line, off_target_flag) != null;
}

/// The guard's three 0-based line indices.
pub const Region = struct {
    if_idx: usize,
    else_idx: usize,
    endif_idx: usize,
};

/// Locate the guard's `#if`, `#else` and `#endif`, or null when the opener is
/// absent or its matching `#else` / `#endif` cannot be resolved.
///
/// Inner conditionals are tracked by depth, so a nested `#if` does not steal
/// the match. Only an `#else` at depth 1 counts, and the LAST such `#else`
/// before the closing `#endif` wins: inherited, and pinned by a test.
pub fn findGuardRegion(lines: []const []const u8) ?Region {
    var if_idx: ?usize = null;
    for (lines, 0..) |line, index| {
        if (isGuardOpen(line)) {
            if_idx = index;
            break;
        }
    }
    const open = if_idx orelse return null;

    var depth: usize = 1;
    var else_idx: ?usize = null;
    var index = open + 1;
    while (index < lines.len) : (index += 1) {
        const line = lines[index];
        if (isIf(line)) {
            depth += 1;
        } else if (isEndif(line)) {
            depth -= 1;
            if (depth == 0) {
                const closed = else_idx orelse return null;
                return .{ .if_idx = open, .else_idx = closed, .endif_idx = index };
            }
        } else if (isElse(line) and depth == 1) {
            else_idx = index;
        }
    }
    return null;
}

/// Whether the `#else` body refuses to build or returns a hard error.
///
/// One `#error` or one `k_ra8_err_` mention anywhere in the branch is enough,
/// which is the predecessor's bar and deliberately a low one: the gate proves
/// the fail-closed half exists, the compiler and the unit tests prove it is
/// right.
pub fn isFailClosed(else_body: []const []const u8) bool {
    for (else_body) |line| {
        if (isError(line)) return true;
        if (std.mem.indexOf(u8, line, hard_error_token) != null) return true;
    }
    return false;
}

/// Render the finding for a stub TU that is not there.
pub fn renderMissingFile(allocator: std.mem.Allocator, rel: []const u8) ![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "{s}: file not found (expected an insecure stub TU here)",
        .{rel},
    );
}

/// Findings for one stub TU's already-split lines (empty when clean).
///
/// The order is inherited: the fail-closed complaint first, then the missing
/// insecure body, then the escape, so a diff of this gate's output against
/// the predecessor's is line for line.
pub fn checkLines(
    allocator: std.mem.Allocator,
    rel: []const u8,
    token: []const u8,
    lines: []const []const u8,
) ![][]const u8 {
    var problems = std.ArrayList([]const u8).init(allocator);
    errdefer problems.deinit();

    const region = findGuardRegion(lines) orelse {
        try problems.append(try std.fmt.allocPrint(
            allocator,
            "{s}: missing the stub-crypto guard '{s}' with a matching #else / #endif",
            .{ rel, guard_spelling },
        ));
        return problems.toOwnedSlice();
    };

    if (!isFailClosed(lines[region.else_idx + 1 .. region.endif_idx])) {
        try problems.append(try std.fmt.allocPrint(
            allocator,
            "{s}: the #else branch is not fail-closed " ++
                "(needs a #error or a k_ra8_err_* hard return, not k_ra8_ok)",
            .{rel},
        ));
    }

    // A hit ON the `#if` line itself is an escape, not an inside hit: the
    // bounds are strict on both sides, inherited and pinned by a test.
    var inside: usize = 0;
    var escaped = std.ArrayList(usize).init(allocator);
    errdefer escaped.deinit();
    for (lines, 0..) |line, index| {
        if (std.mem.indexOf(u8, line, token) == null) continue;
        if (region.if_idx < index and index < region.else_idx) {
            inside += 1;
        } else {
            try escaped.append(index);
        }
    }

    if (inside == 0) {
        try problems.append(try std.fmt.allocPrint(
            allocator,
            "{s}: insecure signature '{s}' not found inside the guarded " ++
                "#if region (is the insecure body still present and guarded?)",
            .{ rel, token },
        ));
    }
    if (escaped.items.len != 0) {
        var where = std.ArrayList(u8).init(allocator);
        errdefer where.deinit();
        for (escaped.items, 0..) |index, ordinal| {
            if (ordinal != 0) try where.appendSlice(", ");
            try where.writer().print("line {d}", .{index + 1});
        }
        try problems.append(try std.fmt.allocPrint(
            allocator,
            "{s}: insecure signature '{s}' appears OUTSIDE the guard ({s}) " ++
                "-- the insecure body must be fully inside the {s} block",
            .{ rel, token, where.items, guard_spelling },
        ));
    }

    return problems.toOwnedSlice();
}

/// Findings for one stub TU's source text, read as Python text mode reads it.
pub fn checkText(
    allocator: std.mem.Allocator,
    rel: []const u8,
    token: []const u8,
    text: []const u8,
) ![][]const u8 {
    const folded = try normalizeTerminators(allocator, text);
    const lines = try splitLines(allocator, folded);
    return checkLines(allocator, rel, token, lines);
}

/// The signature the selftest fixtures hide behind (and outside) the guard.
pub const selftest_signature = "insecure_fixture_signature";

/// The path the selftest fixtures claim, so its findings read like real ones.
pub const selftest_rel = "libs/fixture/stub.c";

/// The complete fixture: insecure body guarded, `#else` returning hard.
pub const selftest_good =
    "#if defined(" ++ insecure_flag ++ ") || defined(" ++ off_target_flag ++ ")\n" ++
    "static int " ++ selftest_signature ++ ";\n" ++
    "#else\nreturn k_ra8_err_unsupported;\n#endif\n";

/// The broken fixture: `#else` returns ok, and the insecure body escaped.
pub const selftest_bad =
    "#if defined(" ++ insecure_flag ++ ") || defined(" ++ off_target_flag ++ ")\n" ++
    "static int placeholder;\n#else\nreturn k_ra8_ok;\n#endif\n" ++
    "static int " ++ selftest_signature ++ ";\n";

/// How many findings the broken fixture must produce at least.
pub const minimum_bad_findings = 2;

/// One selftest case: did it hold, and what does it prove.
pub const SelftestCase = struct {
    passed: bool,
    label: []const u8,
};

/// Whether any finding in `problems` contains `needle`.
fn anyMentions(problems: []const []const u8, needle: []const u8) bool {
    for (problems) |problem| {
        if (std.mem.indexOf(u8, problem, needle) != null) return true;
    }
    return false;
}

/// Run both detector directions over the in-memory fixtures.
pub fn selftestCases(allocator: std.mem.Allocator) ![2]SelftestCase {
    const good = try checkText(allocator, selftest_rel, selftest_signature, selftest_good);
    const bad = try checkText(allocator, selftest_rel, selftest_signature, selftest_bad);
    return .{
        .{
            .passed = good.len == 0,
            .label = "guarded token plus hard-error branch stays quiet",
        },
        .{
            .passed = bad.len >= minimum_bad_findings and
                anyMentions(bad, "not fail-closed") and
                anyMentions(bad, "OUTSIDE"),
            .label = "non-failing else and escaped insecure token both fire",
        },
    };
}
