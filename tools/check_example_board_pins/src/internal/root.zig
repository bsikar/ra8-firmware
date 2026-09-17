//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Gate: examples shall not hand-encode board connector pins (#858, replacing
//! the Python `scripts/checks/check_example_board_pins.py`).  PATHREF-OK: the
//! predecessor this names is deleted in the same change.
//!
//! The EK-RA8D2 pinout is a board fact owned by `libs/ra8_board_ek_ra8d2`.
//! #251 showed the cost of duplicating it: the four USB-FS pins were
//! copy-pasted byte-identically across 29 apps under a dozen local names, so a
//! correction in one silently skipped the rest.  This module holds the decision
//! logic only: the encoding matcher, Python's line and whitespace semantics,
//! the build-output predicate that keeps generated trees out of scope, and the
//! renderers.  No file system, no argv, no process exit: `cli.zig` owns those.

const std = @import("std");
const classes = @import("char_classes.zig");

/// The `examples/` tree this gate sweeps, relative to the repository root.
pub const scan_root = "examples";

/// Suffixes the predecessor treated as scannable source.
pub const source_suffixes = [_][]const u8{ ".c", ".h", ".cpp", ".hpp" };

/// An `examples/` tree this size cannot legitimately collapse to a handful of
/// files.  Below this the sweep is broken, not clean: the idiom cannot be found
/// in a file nobody read.  Measured 2026-07-28 at 408 example sources.
pub const file_floor: usize = 320;

/// Top-level directories beneath which a build tree legitimately appears, at
/// any depth.  Deliberately not "any directory anywhere": a `build` directory
/// under `scripts/` is source and stays visible to the checkers (#377, #359).
pub const build_tree_roots = [_][]const u8{
    "docs", "examples", "local-poc", "port", "tests", "tools", "apps",
};

/// Directory names a tool reserves, matched at ANY depth because nobody can
/// legitimately author source in one.
pub const tool_output_dir_names = [_][]const u8{
    ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules",
};

/// True when one path COMPONENT names a build tree.  The separator is
/// required, so `builders` is not a build directory.
pub fn isBuildDirName(name: []const u8) bool {
    if (std.mem.eql(u8, name, "build")) return true;
    return std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

fn inList(list: []const []const u8, name: []const u8) bool {
    for (list) |item| {
        if (std.mem.eql(u8, item, name)) return true;
    }
    return false;
}

/// True when repo-relative `rel` lives inside a build tree.  Directory
/// components only: a FILE called `build` is not a build tree.
pub fn isBuildOutput(rel: []const u8) bool {
    var first: ?[]const u8 = null;
    var index: usize = 0;
    var parts = std.mem.splitScalar(u8, rel, '/');
    var pending: ?[]const u8 = parts.next();
    while (pending) |part| {
        const next = parts.next();
        if (first == null) first = part;
        // parts[:-1]: the last component is the file name, never a directory.
        if (next == null) break;
        if (inList(&tool_output_dir_names, part)) return true;
        if (isBuildDirName(part) and (index == 0 or inList(&build_tree_roots, first.?))) return true;
        index += 1;
        pending = next;
    }
    return false;
}

/// Normalise a path the way the predecessor's `is_build_output_path` did:
/// backslashes to slashes, outer slashes trimmed, then the repository root or
/// a leading `./` removed.  Writes into `buffer`, which must be at least
/// `path.len` bytes, and returns the repo-relative view.
pub fn normalizeRepoRelative(buffer: []u8, path: []const u8, repo_root: []const u8) []const u8 {
    std.debug.assert(buffer.len >= path.len);
    for (path, 0..) |byte, i| buffer[i] = if (byte == '\\') '/' else byte;
    var text = std.mem.trim(u8, buffer[0..path.len], "/");

    var root_buffer: [std.fs.max_path_bytes]u8 = undefined;
    var root: []const u8 = "";
    if (repo_root.len <= root_buffer.len) {
        for (repo_root, 0..) |byte, i| root_buffer[i] = if (byte == '\\') '/' else byte;
        root = std.mem.trim(u8, root_buffer[0..repo_root.len], "/");
    }

    if (root.len != 0 and text.len > root.len and
        std.mem.startsWith(u8, text, root) and text[root.len] == '/')
    {
        return text[root.len + 1 ..];
    }
    if (std.mem.startsWith(u8, text, "./")) return text[2..];
    return text;
}

/// `is_build_output_path`: the predecessor's single predicate over a path that
/// may be absolute, repo-relative or slash-wrapped.
pub fn isBuildOutputPath(path: []const u8, repo_root: []const u8) bool {
    var buffer: [std.fs.max_path_bytes * 2]u8 = undefined;
    if (path.len > buffer.len) return false;
    return isBuildOutput(normalizeRepoRelative(&buffer, path, repo_root));
}

/// `pathlib.PurePath.suffix`: the last dot segment, empty when the name is
/// itself dot-prefixed with nothing before the dot.  A file literally named
/// `.c` therefore has NO suffix and is not source on the argv path, even
/// though `rglob("*.c")` on the sweep path does list it.
pub fn pathlibSuffix(name: []const u8) []const u8 {
    if (name.len == 0) return "";
    var trimmed = name;
    while (trimmed.len > 1 and trimmed[trimmed.len - 1] == '.') trimmed = trimmed[0 .. trimmed.len - 1];
    const dot = std.mem.lastIndexOfScalar(u8, trimmed, '.') orelse return "";
    if (dot == 0) return "";
    var stem_end = dot;
    while (stem_end > 0 and trimmed[stem_end - 1] == '.') stem_end -= 1;
    if (stem_end == 0) return "";
    return trimmed[dot..];
}

/// True when the argv path branch treated this name as source.
pub fn isSourceName(name: []const u8) bool {
    return inList(&source_suffixes, pathlibSuffix(name));
}

/// True when `rglob("*" ++ suffix)` would have listed this name.  `fnmatch`
/// lets `*` match the empty string and does not hide a dot-prefixed name, so a
/// file called exactly `.c` IS listed here.
pub fn globMatchesSuffix(name: []const u8, suffix: []const u8) bool {
    return std.mem.endsWith(u8, name, suffix);
}

/// One decoded code point plus the byte length it occupied.
pub const DecodedCodePoint = struct { code_point: u21, length: usize };

/// Decode one code point at `index`, substituting U+FFFD for a byte that
/// cannot start or continue a valid sequence, the way `errors="replace"` did.
pub fn decodeAt(text: []const u8, index: usize) DecodedCodePoint {
    const length = std.unicode.utf8ByteSequenceLength(text[index]) catch
        return .{ .code_point = 0xFFFD, .length = 1 };
    if (index + length > text.len) return .{ .code_point = 0xFFFD, .length = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + length]) catch
        return .{ .code_point = 0xFFFD, .length = 1 };
    return .{ .code_point = code_point, .length = length };
}

/// True when CPython's `\s` matched the code point at `index`.
fn spaceAt(text: []const u8, index: usize) ?usize {
    const decoded = decodeAt(text, index);
    return if (classes.isReSpace(decoded.code_point)) decoded.length else null;
}

fn digitAt(text: []const u8, index: usize) ?usize {
    const decoded = decodeAt(text, index);
    return if (classes.isReDigit(decoded.code_point)) decoded.length else null;
}

fn spaceRun(text: []const u8, start: usize) usize {
    var index = start;
    while (index < text.len) {
        const length = spaceAt(text, index) orelse break;
        index += length;
    }
    return index;
}

fn digitRun(text: []const u8, start: usize) ?usize {
    var index = start;
    var seen = false;
    while (index < text.len) {
        const length = digitAt(text, index) orelse break;
        index += length;
        seen = true;
    }
    return if (seen) index else null;
}

fn literalAt(text: []const u8, index: usize, literal: []const u8) ?usize {
    if (index + literal.len > text.len) return null;
    if (!std.mem.eql(u8, text[index .. index + literal.len], literal)) return null;
    return index + literal.len;
}

/// The predecessor's ENCODING_RE, anchored at `start`:
/// `k_ra8_port_\d+\s*<<\s*8\s*\)\s*\|\s*\(\s*uint16_t\s*\)\s*k_ra8_pin_\d+`.
///
/// No backtracking is needed anywhere: every greedy run is followed by a
/// literal no member of that run can be, so the greedy choice is the only
/// choice CPython could have made either.
pub fn matchEncodingAt(text: []const u8, start: usize) bool {
    var index = literalAt(text, start, "k_ra8_port_") orelse return false;
    index = digitRun(text, index) orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, "<<") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, "8") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, ")") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, "|") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, "(") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, "uint16_t") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, ")") orelse return false;
    index = spaceRun(text, index);
    index = literalAt(text, index, "k_ra8_pin_") orelse return false;
    _ = digitRun(text, index) orelse return false;
    return true;
}

/// `ENCODING_RE.search(text)`: the encoding idiom anywhere in `text`.
pub fn matchEncoding(text: []const u8) bool {
    var index: usize = 0;
    while (index < text.len) : (index += 1) {
        if (text[index] != 'k') continue;
        if (matchEncodingAt(text, index)) return true;
    }
    return false;
}

/// `str.strip()`: trims every code point `str.isspace()` reported, which is
/// NOT the same set as `\s` (they part company on U+001C-U+001F).
pub fn strip(text: []const u8) []const u8 {
    var start: usize = 0;
    while (start < text.len) {
        const decoded = decodeAt(text, start);
        if (!classes.isStrSpace(decoded.code_point)) break;
        start += decoded.length;
    }
    var end = text.len;
    while (end > start) {
        var probe = end - 1;
        while (probe > start and (text[probe] & 0xC0) == 0x80) probe -= 1;
        const decoded = decodeAt(text, probe);
        if (probe + decoded.length != end) break;
        if (!classes.isStrSpace(decoded.code_point)) break;
        end = probe;
    }
    return text[start..end];
}

/// `str.splitlines()`: every break CPython recognised, CRLF counted once.
/// U+001F is deliberately absent, though `\s` and `str.strip()` both take it.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    fn breakLength(text: []const u8, index: usize) ?usize {
        const byte = text[index];
        if (byte == '\r') {
            if (index + 1 < text.len and text[index + 1] == '\n') return 2;
            return 1;
        }
        if (byte == '\n' or byte == 0x0B or byte == 0x0C or
            (byte >= 0x1C and byte <= 0x1E)) return 1;
        const decoded = decodeAt(text, index);
        if (decoded.code_point == 0x85 or decoded.code_point == 0x2028 or
            decoded.code_point == 0x2029) return decoded.length;
        return null;
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var cursor = start;
        while (cursor < self.text.len) {
            if (breakLength(self.text, cursor)) |length| {
                self.index = cursor + length;
                return self.text[start..cursor];
            }
            cursor += decodeAt(self.text, cursor).length;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

/// One hand-encoded board pin, as the report lists it.
pub const Finding = struct {
    path: []const u8,
    line_number: usize,
    snippet: []const u8,
};

/// Every finding in one decoded file body, in line order.
pub fn scanText(allocator: std.mem.Allocator, path: []const u8, text: []const u8) !std.ArrayList(Finding) {
    var findings = std.ArrayList(Finding).init(allocator);
    errdefer findings.deinit();
    var lines = LineIterator{ .text = text };
    var line_number: usize = 0;
    while (lines.next()) |line| {
        line_number += 1;
        if (matchEncoding(line)) {
            try findings.append(.{
                .path = path,
                .line_number = line_number,
                .snippet = strip(line),
            });
        }
    }
    return findings;
}

/// The tool's own name, as every message spells it.
pub const tool_name = "check_example_board_pins";

pub fn renderClean(writer: anytype, scanned: usize) !void {
    try writer.print(
        "{s}: {d} example file(s) scanned, none hand-encode a board pin.\n",
        .{ tool_name, scanned },
    );
}

pub fn renderFatalFloor(writer: anytype, scanned: usize) !void {
    try writer.print(
        "{s}: FATAL -- only {d} example file(s) in scope, floor is {d}. " ++
            "A collapsed sweep reports a clean tree because it scanned nothing.\n",
        .{ tool_name, scanned, file_floor },
    );
}

pub fn renderNoFiles(writer: anytype) !void {
    try writer.print("{s}: no files to scan\n", .{tool_name});
}

pub fn renderFindingHeader(writer: anytype, count: usize) !void {
    try writer.print("{s}: {d} hand-encoded board pin(s) in examples:\n\n", .{ tool_name, count });
}

pub fn renderFinding(writer: anytype, finding: Finding) !void {
    try writer.print("  {s}:{d}  {s}\n", .{ finding.path, finding.line_number, finding.snippet });
}

pub const guidance =
    "\nThe EK-RA8D2 pinout belongs to the board layer, not to each app.\n" ++
    "Reference the board symbol (k_ra8_board_*_pin_*, or an accessor like\n" ++
    "ra8_board_sw_pin) instead of re-encoding (port << 8 | pin). If the pin\n" ++
    "is a real board connector the board layer does not expose yet, add it\n" ++
    "to libs/ra8_board_ek_ra8d2 first, then reference it here.\n";

pub fn renderGuidance(writer: anytype) !void {
    try writer.writeAll(guidance);
}

/// The two fixtures the selftest fires the matcher at, in both directions.
pub const selftest_idiom = "  cfg.pin = ((uint16_t)k_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_11;";
pub const selftest_board_reference = "  cfg.pin = ra8_board_sw_pin(k_ra8_board_sw_user);";

/// The length of the maximal invalid subpart starting at `index`: how many
/// bytes CPython's UTF-8 decoder folded into ONE U+FFFD.  A lead byte plus
/// every continuation byte still valid for its position is one subpart, so a
/// truncated `E3 80` is a single replacement; a byte that can neither start
/// nor continue a sequence is a subpart on its own, so `FF FE` is two.
fn invalidSubpartLength(bytes: []const u8, index: usize) usize {
    const first = bytes[index];
    var expected: usize = 0;
    var second_min: u8 = 0x80;
    var second_max: u8 = 0xBF;
    switch (first) {
        0xC2...0xDF => expected = 2,
        0xE0 => {
            expected = 3;
            second_min = 0xA0;
        },
        0xE1...0xEC => expected = 3,
        0xED => {
            expected = 3;
            second_max = 0x9F;
        },
        0xEE...0xEF => expected = 3,
        0xF0 => {
            expected = 4;
            second_min = 0x90;
        },
        0xF1...0xF3 => expected = 4,
        0xF4 => {
            expected = 4;
            second_max = 0x8F;
        },
        // A continuation byte on its own, an overlong lead (C0, C1) or a lead
        // past U+10FFFF (F5-FF): nothing can continue it, so it stands alone.
        else => return 1,
    }
    var length: usize = 1;
    while (length < expected and index + length < bytes.len) {
        const byte = bytes[index + length];
        const min = if (length == 1) second_min else 0x80;
        const max = if (length == 1) second_max else 0xBF;
        if (byte < min or byte > max) break;
        length += 1;
    }
    return length;
}

/// `read_text(encoding="utf-8", errors="replace")`: a decoded copy in which
/// every byte sequence that cannot be decoded became U+FFFD, so one
/// undecodable file cannot abort the sweep.  The replacement is per maximal
/// subpart, not per byte, because that is what CPython emitted and the
/// snippet of a finding on such a line is printed verbatim.
pub fn decodeLossy(allocator: std.mem.Allocator, bytes: []const u8) ![]u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < bytes.len) {
        if (std.unicode.utf8ByteSequenceLength(bytes[index])) |length| {
            if (index + length <= bytes.len) {
                if (std.unicode.utf8Decode(bytes[index .. index + length])) |_| {
                    try out.appendSlice(bytes[index .. index + length]);
                    index += length;
                    continue;
                } else |_| {}
            }
        } else |_| {}
        try out.appendSlice("\u{FFFD}");
        index += invalidSubpartLength(bytes, index);
    }
    return out.toOwnedSlice();
}
