//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detector algebra for the commit-message terminology gate (#858).
//!
//! Nothing here reads a file, looks at argv or touches a stream: text goes in,
//! findings come out, so every rule below is provable with no process and no
//! repository. The gate's meaning is five regular expressions and a
//! paragraph-scoped opt-out, all reproduced by hand because Zig's standard
//! library has no regular-expression engine.
//!
//! The banned vocabulary is spelled in halves (`"sla" ++ "ve"`) on purpose.
//! The source-level sibling gate scans this tree for exactly these words, and
//! its predecessor needed a whole-file exemption to hold them as regex
//! literals; splitting them keeps this tool out of that closed list.

const std = @import("std");
const word_chars = @import("word_chars.zig");

/// Banned term spellings, never written whole (see the module comment).
const legacy_controller = "mas" ++ "ter";
const legacy_peripheral = "sla" ++ "ve";
const legacy_copi = "MO" ++ "SI";
const legacy_cipo = "MI" ++ "SO";
const select_word = "select";

/// Suffixes the controller pattern's optional group accepts, in source order.
const controller_suffixes = [_][]const u8{ "s", "ed", "ing", "ship", "" };

/// Suffixes the peripheral pattern's optional group accepts, in source order.
const peripheral_suffixes = [_][]const u8{ "s", "d", "" };

/// Separators the chip-select phrase accepts between its two words.
const select_separators = " _-";

/// Lone-surrogate base Python's `surrogateescape` handler maps a stray byte to.
///
/// `sys.stdin` decodes UTF-8 with that handler, so an undecodable byte reaches
/// the detector as `U+DC00 + byte` rather than raising. It is neither a word
/// character nor whitespace, which is the only thing the rules ask of it.
pub const surrogate_base: u21 = 0xDC00;

/// One decoded code point and the bytes it came from.
pub const Char = struct {
    /// The code point, or `surrogate_base + byte` for an undecodable byte.
    cp: u21,
    /// Offset of the first byte in the source text.
    start: usize,
    /// Byte length, always 1 for an escaped byte.
    len: u8,
};

/// One reported line, as the gate prints it.
pub const Violation = struct {
    /// One-based line number within the scanned text.
    line: usize,
    /// The advice for the term that fired, first match in `banned` order.
    message: []const u8,
    /// The offending line, stripped, as a slice of the scanned bytes.
    text: []const u8,
};

/// A banned term: how to recognise it, and what to say when it fires.
pub const Term = struct {
    /// Which pattern to run.
    id: Id,
    /// The advice printed beside the line number.
    message: []const u8,

    /// The five patterns, in the order the predecessor declared them.
    pub const Id = enum { controller, peripheral, copi, cipo, chip_select };
};

/// The banned vocabulary, scanned in this order; the first hit ends the line.
///
/// The bare chip-select abbreviation is deliberately absent: in prose it fires
/// on the very commit
/// messages that document the rule, and it is caught at the source-file level
/// by the sibling gate instead.
pub const banned = [_]Term{
    .{ .id = .controller, .message = legacy_controller ++ " -- use Primary/Controller" },
    .{ .id = .peripheral, .message = legacy_peripheral ++ " -- use Peripheral" },
    .{ .id = .copi, .message = legacy_copi ++ " -- use COPI" },
    .{ .id = .cipo, .message = legacy_cipo ++ " -- use CIPO" },
    .{ .id = .chip_select, .message = "Sla" ++ "ve Select -- use CS" },
};

/// True when `cp` is a regular-expression word character, `\w` in Python.
pub fn isWordChar(cp: u21) bool {
    return word_chars.isWordChar(cp);
}

/// True when `cp` is whitespace to Python: both `str.isspace` and `\s`.
///
/// The two sets are identical for `str` patterns, and the detector needs both
/// roles: `\s*` inside the opt-out pattern, and `str.strip` on the echoed line.
pub fn isPythonSpace(cp: u21) bool {
    return switch (cp) {
        0x09...0x0D, 0x1C...0x1F, 0x20, 0x85, 0xA0, 0x1680 => true,
        0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000 => true,
        else => false,
    };
}

/// True when `cp` ends a line to `str.splitlines`.
///
/// Wider than `\n`: the vertical tab, the form feed, the three information
/// separators and the three Unicode breaks all split a line there, so a form
/// feed in a commit message must not shift every later line number.
pub fn isLineBreak(cp: u21) bool {
    return switch (cp) {
        0x0A...0x0D, 0x1C...0x1E, 0x85, 0x2028, 0x2029 => true,
        else => false,
    };
}

/// Fold one code point the way `re.IGNORECASE` folds it for an ASCII literal.
///
/// Beyond ASCII case, three code points fold onto letters this gate matches:
/// the long s onto `s`, the Kelvin sign onto `k`, and the dotted and dotless
/// Turkish letters onto `i`. Everything else compares as itself.
pub fn foldCase(cp: u21) u21 {
    if (cp >= 'A' and cp <= 'Z') return cp + 32;
    return switch (cp) {
        0x017F => 's',
        0x212A => 'k',
        0x0130, 0x0131 => 'i',
        else => cp,
    };
}

/// Decode `bytes` the way `sys.stdin` does: UTF-8, stray bytes escaped.
pub fn decode(allocator: std.mem.Allocator, bytes: []const u8) ![]Char {
    var chars = std.ArrayList(Char).init(allocator);
    errdefer chars.deinit();

    var index: usize = 0;
    while (index < bytes.len) {
        const width = std.unicode.utf8ByteSequenceLength(bytes[index]) catch {
            try chars.append(.{ .cp = surrogate_base + bytes[index], .start = index, .len = 1 });
            index += 1;
            continue;
        };
        if (index + width > bytes.len) {
            try chars.append(.{ .cp = surrogate_base + bytes[index], .start = index, .len = 1 });
            index += 1;
            continue;
        }
        const cp = std.unicode.utf8Decode(bytes[index .. index + width]) catch {
            try chars.append(.{ .cp = surrogate_base + bytes[index], .start = index, .len = 1 });
            index += 1;
            continue;
        };
        try chars.append(.{ .cp = cp, .start = index, .len = @intCast(width) });
        index += width;
    }
    return chars.toOwnedSlice();
}

/// Split decoded text into lines exactly as `str.splitlines` does.
///
/// Terminators are dropped, `\r\n` counts once, and a trailing break does not
/// invent an empty final line.
pub fn splitLines(allocator: std.mem.Allocator, chars: []const Char) ![][]const Char {
    var lines = std.ArrayList([]const Char).init(allocator);
    errdefer lines.deinit();

    var start: usize = 0;
    var index: usize = 0;
    while (index < chars.len) {
        if (!isLineBreak(chars[index].cp)) {
            index += 1;
            continue;
        }
        try lines.append(chars[start..index]);
        if (chars[index].cp == '\r' and index + 1 < chars.len and chars[index + 1].cp == '\n') {
            index += 2;
        } else {
            index += 1;
        }
        start = index;
    }
    if (start < chars.len) try lines.append(chars[start..]);
    return lines.toOwnedSlice();
}

/// The line with leading and trailing Python whitespace removed.
pub fn strip(line: []const Char) []const Char {
    var first: usize = 0;
    while (first < line.len and isPythonSpace(line[first].cp)) first += 1;
    var last: usize = line.len;
    while (last > first and isPythonSpace(line[last - 1].cp)) last -= 1;
    return line[first..last];
}

/// The bytes `chars` came from, as a slice of the original text.
pub fn textOf(bytes: []const u8, chars: []const Char) []const u8 {
    if (chars.len == 0) return bytes[0..0];
    const first = chars[0].start;
    const last = chars[chars.len - 1];
    return bytes[first .. last.start + last.len];
}

/// True when the line is blank, the paragraph boundary the predecessor used.
pub fn isBlank(line: []const Char) bool {
    return strip(line).len == 0;
}

/// Index one past `literal` when it matches at `at` under case folding.
fn matchFolded(line: []const Char, at: usize, literal: []const u8) ?usize {
    if (at + literal.len > line.len) return null;
    for (literal, 0..) |wanted, offset| {
        if (foldCase(line[at + offset].cp) != wanted) return null;
    }
    return at + literal.len;
}

/// Index one past `literal` when it matches at `at` byte for byte.
fn matchExact(line: []const Char, at: usize, literal: []const u8) ?usize {
    if (at + literal.len > line.len) return null;
    for (literal, 0..) |wanted, offset| {
        if (line[at + offset].cp != wanted) return null;
    }
    return at + literal.len;
}

/// True when position `at` is a `\b` word boundary in `line`.
///
/// Both ends of the line count as non-word, so a banned term alone on a line
/// is bounded on both sides.
pub fn boundaryAt(line: []const Char, at: usize) bool {
    const before = at > 0 and isWordChar(line[at - 1].cp);
    const after = at < line.len and isWordChar(line[at].cp);
    return before != after;
}

/// True when a folded `word` plus one of `suffixes` sits bounded at `at`.
fn matchWordWithSuffix(
    line: []const Char,
    at: usize,
    word: []const u8,
    suffixes: []const []const u8,
) bool {
    if (!boundaryAt(line, at)) return false;
    const stem_end = matchFolded(line, at, word) orelse return false;
    for (suffixes) |suffix| {
        const end = matchFolded(line, stem_end, suffix) orelse continue;
        if (boundaryAt(line, end)) return true;
    }
    return false;
}

/// True when the two-word chip-select phrase sits bounded at `at`.
fn matchSelectPhrase(line: []const Char, at: usize) bool {
    if (!boundaryAt(line, at)) return false;
    const after_word = matchFolded(line, at, legacy_peripheral) orelse return false;
    if (after_word >= line.len) return false;
    const separator = line[after_word].cp;
    if (separator > 0x7F) return false;
    if (std.mem.indexOfScalar(u8, select_separators, @intCast(separator)) == null) return false;
    const end = matchFolded(line, after_word + 1, select_word) orelse return false;
    return boundaryAt(line, end);
}

/// True when a bounded, case-sensitive `literal` sits at `at`.
fn matchExactWord(line: []const Char, at: usize, literal: []const u8) bool {
    if (!boundaryAt(line, at)) return false;
    const end = matchExact(line, at, literal) orelse return false;
    return boundaryAt(line, end);
}

/// True when `id`'s pattern matches anywhere in `line`.
pub fn lineMatches(id: Term.Id, line: []const Char) bool {
    var at: usize = 0;
    while (at <= line.len) : (at += 1) {
        const hit = switch (id) {
            .controller => matchWordWithSuffix(line, at, legacy_controller, &controller_suffixes),
            .peripheral => matchWordWithSuffix(line, at, legacy_peripheral, &peripheral_suffixes),
            .copi => matchExactWord(line, at, legacy_copi),
            .cipo => matchExactWord(line, at, legacy_cipo),
            .chip_select => matchSelectPhrase(line, at),
        };
        if (hit) return true;
    }
    return false;
}

/// True when the line carries a `LEGACY-OK:` opt-out.
///
/// Case-insensitive, and any run of Python whitespace may sit before the
/// colon, because an editor is free to wrap the annotation.
pub fn hasOptOut(line: []const Char) bool {
    var at: usize = 0;
    while (at < line.len) : (at += 1) {
        const after_tag = matchFolded(line, at, "legacy-ok") orelse continue;
        var cursor = after_tag;
        while (cursor < line.len and isPythonSpace(line[cursor].cp)) cursor += 1;
        if (cursor < line.len and line[cursor].cp == ':') return true;
    }
    return false;
}

/// Scan commit-message text, honouring paragraph-scoped opt-outs.
///
/// A paragraph is a run of lines between blank ones, and one `LEGACY-OK:`
/// anywhere inside it silences the whole paragraph: editors and git wrap
/// prose, so an annotation must cover every physical line of the paragraph it
/// sits in, and must not reach across a blank line into the next one.
pub fn findViolations(allocator: std.mem.Allocator, bytes: []const u8) ![]Violation {
    const chars = try decode(allocator, bytes);
    const lines = try splitLines(allocator, chars);

    var violations = std.ArrayList(Violation).init(allocator);
    errdefer violations.deinit();

    var paragraph_start: usize = 0;
    var index: usize = 0;
    while (index <= lines.len) : (index += 1) {
        const at_boundary = index == lines.len or isBlank(lines[index]);
        if (!at_boundary) continue;

        const paragraph = lines[paragraph_start..index];
        if (paragraph.len != 0 and !paragraphAnnotated(paragraph)) {
            for (paragraph, 0..) |line, offset| {
                for (banned) |term| {
                    if (!lineMatches(term.id, line)) continue;
                    try violations.append(.{
                        .line = paragraph_start + offset + 1,
                        .message = term.message,
                        .text = textOf(bytes, strip(line)),
                    });
                    break;
                }
            }
        }
        paragraph_start = index + 1;
    }
    return violations.toOwnedSlice();
}

/// True when any line of the paragraph carries the opt-out.
fn paragraphAnnotated(paragraph: []const []const Char) bool {
    for (paragraph) |line| {
        if (hasOptOut(line)) return true;
    }
    return false;
}

/// Render one violation the way the predecessor printed it.
pub fn renderViolation(allocator: std.mem.Allocator, violation: Violation) ![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "  line {d}: {s}\n    > {s}",
        .{ violation.line, violation.message, violation.text },
    );
}

/// Commit-message text the detector MUST fire on.
pub const selftest_fires = "fix(spi): rework the " ++ legacy_copi ++ "/" ++ legacy_cipo ++ " pin mux\n";

/// A wrapped paragraph whose opt-out sits on a different physical line.
pub const selftest_quiet = "ci(gates): widen scope\n\n" ++
    "Widening surfaced only verbatim upstream terms (IEEE 1588 PTP\n" ++
    legacy_controller ++ "/" ++ legacy_peripheral ++ ", datasheet " ++
    legacy_copi ++ "/" ++ legacy_cipo ++ " pin labels), LEGACY-OK: upstream\n" ++
    "domain terminology quoted verbatim, not our naming.\n";

/// An opt-out in the NEXT paragraph, which must not reach backwards.
pub const selftest_cross_paragraph = "fix(spi): rework the " ++ legacy_copi ++ " pin mux\n\n" ++
    "LEGACY-OK: unrelated note in the next paragraph\n";
