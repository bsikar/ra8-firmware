//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Declaration and definition algebra for the NSC veneer gate (#858).
//!
//! Every function here is pure: text in, findings out, no file system and no
//! argv, so the detector is provable with no repository on disk. The two
//! patterns the Python gate carried as regular expressions
//! (`RA8_NSC_VENEER\s+\w[\w\s\*]*?\b(ra8_nsc_\w+)\s*\(` for a declaration,
//! and the same shape with a literal name for a definition) are reproduced
//! here as one hand-written matcher, because Zig's standard library has no
//! regular expressions and the gate's meaning lives in the exact match.

const std = @import("std");

/// The macro that marks a Secure-Gateway veneer; both patterns anchor on it.
pub const veneer_prefix = "RA8_NSC_VENEER";

/// Every veneer name starts here, so a declaration scan can capture one.
pub const name_prefix = "ra8_nsc_";

/// One decoded code point and the bytes it occupies.
pub const Char = struct {
    code_point: u21,
    len: usize,
};

/// Decode the code point at `index`, or null at the end of the text.
///
/// A byte sequence that is not valid UTF-8 decodes as the single byte, so the
/// matcher cannot walk off the end of a malformed source; the caller rejects
/// such a source before scanning it.
pub fn charAt(text: []const u8, index: usize) ?Char {
    if (index >= text.len) return null;
    const first = text[index];
    if (first < 0x80) return .{ .code_point = first, .len = 1 };
    const len = std.unicode.utf8ByteSequenceLength(first) catch
        return .{ .code_point = first, .len = 1 };
    if (index + len > text.len) return .{ .code_point = first, .len = 1 };
    const decoded = std.unicode.utf8Decode(text[index .. index + len]) catch
        return .{ .code_point = first, .len = 1 };
    return .{ .code_point = decoded, .len = len };
}

/// Whether one ASCII byte is a `\w` character.
///
/// Python's `\w` on a string also covers non-ASCII letters and digits, which
/// this predicate does not see; `boundaryBefore` handles the non-ASCII case
/// itself, because getting it wrong there is not symmetric. A wider `\b` finds
/// MORE declarations, which fails closed, but it also credits MORE
/// definitions, which fails OPEN: a phantom veneer would go unreported. So a
/// non-ASCII code point before a name counts as a word character unless it is
/// a space, which is what Python's class does for every letter and digit.
pub fn isWordByte(byte: u8) bool {
    return (byte >= 'a' and byte <= 'z') or
        (byte >= 'A' and byte <= 'Z') or
        (byte >= '0' and byte <= '9') or
        byte == '_';
}

/// Whether one code point is in Python's string `\s` class.
pub fn isPythonSpace(code_point: u21) bool {
    return switch (code_point) {
        // Python's string \s covers the ASCII control whitespace, including
        // the file/group/record separators, not just the C escapes.
        0x09...0x0d, 0x1c...0x1f, 0x20 => true,
        0x85, 0xa0, 0x1680 => true,
        0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000 => true,
        else => false,
    };
}

/// Length of the `\s` character at `index`, or null when there is none.
pub fn spaceLen(text: []const u8, index: usize) ?usize {
    const char = charAt(text, index) orelse return null;
    return if (isPythonSpace(char.code_point)) char.len else null;
}

/// Length of the mandatory `\w` after the macro's whitespace, or null.
///
/// Loose on purpose: a non-ASCII code point that is not whitespace is
/// accepted here, which again only widens the scan.
pub fn wordStartLen(text: []const u8, index: usize) ?usize {
    const char = charAt(text, index) orelse return null;
    if (char.len == 1) return if (isWordByte(text[index])) 1 else null;
    return if (isPythonSpace(char.code_point)) null else char.len;
}

/// Length of a `[\w\s\*]` character at `index`, or null when the class ends.
///
/// The class cannot cross `(`, `;`, `{` or `,`, which is what keeps one
/// veneer's macro from reaching over a statement into another's name.
pub fn classLen(text: []const u8, index: usize) ?usize {
    const char = charAt(text, index) orelse return null;
    if (char.len > 1) return char.len;
    const byte = text[index];
    if (isWordByte(byte) or byte == '*') return 1;
    return if (isPythonSpace(byte)) 1 else null;
}

/// Whether `\b` holds at `index` given that a word character follows.
///
/// A non-ASCII code point immediately before the name is treated as a word
/// character unless it is one of Python's spaces, so `\b` fails there exactly
/// as it does in Python for a letter or a digit. Treating it as a non-word
/// byte instead let a definition head like `<letter>ra8_nsc_foo(` satisfy a
/// declared `ra8_nsc_foo`, which is a phantom veneer reported as defined.
pub fn boundaryBefore(text: []const u8, index: usize) bool {
    if (index == 0) return true;
    const previous = text[index - 1];
    if (previous < 0x80) return !isWordByte(previous);

    // Walk back over the continuation bytes to the head of that code point.
    var start = index - 1;
    while (start > 0 and (text[start] & 0xc0) == 0x80) start -= 1;
    const char = charAt(text, start) orelse return true;
    // A malformed sequence is not a code point ending here; leave the
    // boundary alone rather than guess what Python's decoder would have seen.
    if (start + char.len != index) return true;
    return isPythonSpace(char.code_point);
}

/// What the matcher is looking for after the macro and the return type.
pub const Target = union(enum) {
    /// Capture any `ra8_nsc_\w+`, as the declaration scan does.
    any,
    /// Require this exact name, as the definition search does. No trailing
    /// word boundary: the Python pattern had none, so `ra8_nsc_target2(`
    /// never counts as a definition of `ra8_nsc_target`.
    literal: []const u8,
};

/// One matched declaration or definition head.
pub const Match = struct {
    /// Byte offset of the macro.
    start: usize,
    /// Byte offset just past the `(`.
    end: usize,
    /// The veneer name, borrowed from the scanned text.
    name: []const u8,
};

/// Match the pattern with the macro at `start`, or null.
pub fn matchAt(text: []const u8, start: usize, target: Target) ?Match {
    if (!std.mem.startsWith(u8, text[start..], veneer_prefix)) return null;
    var cursor = start + veneer_prefix.len;

    // `\s+` is greedy, and only its longest run can satisfy the `\w` that
    // follows, because a shorter run leaves a whitespace character there and
    // whitespace is never a word character. So the run needs no backtracking.
    const space_start = cursor;
    while (spaceLen(text, cursor)) |len| cursor += len;
    if (cursor == space_start) return null;

    cursor += wordStartLen(text, cursor) orelse return null;

    // `[\w\s\*]*?` is lazy: try the shortest expansion first and grow only
    // while the class still matches.
    var scan = cursor;
    while (true) {
        if (boundaryBefore(text, scan)) {
            if (matchNameAt(text, scan, target)) |match| {
                return .{ .start = start, .end = match.end, .name = match.name };
            }
        }
        scan += classLen(text, scan) orelse return null;
    }
}

/// Match the name and its `\s*\(` tail at `index`, or null.
const NameMatch = struct { end: usize, name: []const u8 };

fn matchNameAt(text: []const u8, index: usize, target: Target) ?NameMatch {
    var after: usize = undefined;
    var name: []const u8 = undefined;
    switch (target) {
        .any => {
            if (!std.mem.startsWith(u8, text[index..], name_prefix)) return null;
            var tail = index + name_prefix.len;
            // `\w+` is greedy, and a shortened run can only leave another word
            // character where the `(` must be, so the full run is the only one
            // that can succeed.
            while (tail < text.len and isWordByte(text[tail])) tail += 1;
            if (tail == index + name_prefix.len) return null;
            after = tail;
            name = text[index..tail];
        },
        .literal => |wanted| {
            if (!std.mem.startsWith(u8, text[index..], wanted)) return null;
            after = index + wanted.len;
            name = text[index..after];
        },
    }
    var tail = after;
    while (spaceLen(text, tail)) |len| tail += len;
    if (tail >= text.len or text[tail] != '(') return null;
    return .{ .end = tail + 1, .name = name };
}

/// First match at or after `from`, scanning left to right as the regular
/// expression engine did.
pub fn nextMatch(text: []const u8, from: usize, target: Target) ?Match {
    var cursor = from;
    while (std.mem.indexOfPos(u8, text, cursor, veneer_prefix)) |found| {
        if (matchAt(text, found, target)) |match| return match;
        cursor = found + 1;
    }
    return null;
}

/// Veneer names declared in the header, in first-seen order, de-duplicated.
pub fn declaredVeneers(allocator: std.mem.Allocator, text: []const u8) ![]const []const u8 {
    var seen = std.ArrayList([]const u8).init(allocator);
    errdefer seen.deinit();
    var cursor: usize = 0;
    while (nextMatch(text, cursor, .any)) |match| {
        cursor = match.end;
        var already = false;
        for (seen.items) |name| {
            if (std.mem.eql(u8, name, match.name)) already = true;
        }
        if (!already) try seen.append(match.name);
    }
    return seen.toOwnedSlice();
}

/// Whether one source text defines `name`.
pub fn definesVeneer(text: []const u8, name: []const u8) bool {
    return nextMatch(text, 0, .{ .literal = name }) != null;
}

/// Render the one-line report for a declared veneer with no definition.
pub fn renderMissing(
    allocator: std.mem.Allocator,
    name: []const u8,
    header: []const u8,
    src_dir: []const u8,
) ![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "  {s}: declared in {s}, no definition in {s}/",
        .{ name, header, src_dir },
    );
}

/// The header the detector selftest parses.
pub const selftest_header =
    "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void);\n" ++
    "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n";

/// The source the detector selftest scans: one veneer defined, one only
/// called, which is exactly the phantom shape the gate exists to catch.
pub const selftest_source =
    "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void) { return 0; }\n" ++
    "void caller(void) { ra8_nsc_phantom(); }\n";

/// One selftest case and whether it held.
pub const SelftestCase = struct {
    label: []const u8,
    passed: bool,
};

/// Run the detector selftest: a matching definition stays quiet, and a
/// call-only phantom fires. Both directions, because a detector that only
/// ever fires proves as little as one that never does.
pub fn selftestCases(allocator: std.mem.Allocator) ![]const SelftestCase {
    const declared = try declaredVeneers(allocator, selftest_header);
    defer allocator.free(declared);

    var defined = std.ArrayList([]const u8).init(allocator);
    defer defined.deinit();
    var missing = std.ArrayList([]const u8).init(allocator);
    defer missing.deinit();
    for (declared) |name| {
        if (definesVeneer(selftest_source, name)) {
            try defined.append(name);
        } else {
            try missing.append(name);
        }
    }

    const cases = try allocator.alloc(SelftestCase, 2);
    cases[0] = .{
        .label = "matching veneer definition stays quiet",
        .passed = defined.items.len == 1 and std.mem.eql(u8, defined.items[0], "ra8_nsc_defined"),
    };
    cases[1] = .{
        .label = "call-only phantom veneer fires",
        .passed = missing.items.len == 1 and std.mem.eql(u8, missing.items[0], "ra8_nsc_phantom"),
    };
    return cases;
}
