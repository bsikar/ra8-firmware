//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure host-build-entrypoint contract logic (#858), replacing
//! scripts/checks/check_host_build_entrypoints.py.
//!
//! Nothing here touches the file system, the environment, argv or a child
//! process: this module is the predecessor's parsing and matching, lifted out
//! so every quirk can be pinned by a regression test. Zig has no regex, so the
//! three Python patterns are hand-reimplemented, including the backtracking the
//! predecessor relied on. src/cli.zig owns discovery, the dispatcher run and
//! the exit contract.

const std = @import("std");
const char_classes = @import("char_classes.zig");

pub const tool = "check_host_build_entrypoints";

/// Path suffixes the predecessor counted as authored compiled implementation.
pub const compiled_suffixes = [_][]const u8{ ".c", ".cc", ".cpp", ".cxx", ".m", ".mm" };

pub const shared_just_delegation = "build_shared_libs.sh \"{{ lib }}\"";

pub const shared_dispatcher_contract = [_][]const u8{
    "is_standalone",
    "host_cmake.sh",
    "apps::shared::test",
};

pub const tools_just_contract = [_][]const u8{
    "build tool=\"all\":",
    "clean tool=\"all\":",
    "build_host_tools.sh build \"{{ tool }}\"",
    "build_host_tools.sh clean \"{{ tool }}\"",
};

pub const host_cmake_contract = [_][]const u8{
    "ra8_select_host_compiler",
    "ra8_select_emulator_compiler",
    "ra8_cmake_reset_if_incompatible",
    "-DCMAKE_C_COMPILER=",
    "-DCMAKE_CXX_COMPILER=",
};

pub const dispatcher_clean_contract = [_][]const u8{
    "clean_one",
    "\"$dir/cache_bench\"",
    "\"$dir/miniz_host.o\"",
    "\"$dir\"/*.trace",
};

// -- code points -------------------------------------------------------------

pub const Decoded = struct { cp: u21, len: usize };

/// Decode one code point, falling back to the raw byte on malformed input, the
/// way a Python `str` would already have been decoded before matching.
pub fn decodeAt(text: []const u8, i: usize) Decoded {
    const width = std.unicode.utf8ByteSequenceLength(text[i]) catch return .{ .cp = text[i], .len = 1 };
    if (i + width > text.len) return .{ .cp = text[i], .len = 1 };
    const cp = std.unicode.utf8Decode(text[i..][0..width]) catch return .{ .cp = text[i], .len = 1 };
    return .{ .cp = cp, .len = width };
}

pub fn isSpaceAt(text: []const u8, i: usize) bool {
    return char_classes.isSpace(decodeAt(text, i).cp);
}

pub fn allSpace(text: []const u8) bool {
    var i: usize = 0;
    while (i < text.len) {
        const d = decodeAt(text, i);
        if (!char_classes.isSpace(d.cp)) return false;
        i += d.len;
    }
    return true;
}

/// `str.lstrip()`.
pub fn lstrip(text: []const u8) []const u8 {
    var i: usize = 0;
    while (i < text.len) {
        const d = decodeAt(text, i);
        if (!char_classes.isSpace(d.cp)) break;
        i += d.len;
    }
    return text[i..];
}

/// `str.rstrip()`.
pub fn rstrip(text: []const u8) []const u8 {
    var i: usize = 0;
    var end: usize = 0;
    while (i < text.len) {
        const d = decodeAt(text, i);
        i += d.len;
        if (!char_classes.isSpace(d.cp)) end = i;
    }
    return text[0..end];
}

/// `str.strip()`.
pub fn strip(text: []const u8) []const u8 {
    return rstrip(lstrip(text));
}

// -- str.splitlines ----------------------------------------------------------

/// `str.splitlines()`: breaks on LF, VT, FF, CR, CRLF, 0x1C-0x1E, U+0085,
/// U+2028 and U+2029, and yields no trailing empty line.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    fn breakLen(text: []const u8, i: usize) ?usize {
        return switch (text[i]) {
            0x0A, 0x0B, 0x0C, 0x1C, 0x1D, 0x1E => @as(usize, 1),
            0x0D => if (i + 1 < text.len and text[i + 1] == 0x0A) @as(usize, 2) else @as(usize, 1),
            0xC2 => if (i + 1 < text.len and text[i + 1] == 0x85) @as(usize, 2) else null,
            0xE2 => if (i + 2 < text.len and text[i + 1] == 0x80 and
                (text[i + 2] == 0xA8 or text[i + 2] == 0xA9)) @as(usize, 3) else null,
            else => null,
        };
    }

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var i = start;
        while (i < self.text.len) {
            if (breakLen(self.text, i)) |n| {
                self.index = i + n;
                return self.text[start..i];
            }
            i += 1;
        }
        self.index = self.text.len;
        return self.text[start..];
    }
};

// -- RECIPE ------------------------------------------------------------------

fn isNameStart(b: u8) bool {
    return (b >= 'A' and b <= 'Z') or (b >= 'a' and b <= 'z') or b == '_';
}

fn isNameChar(b: u8) bool {
    return isNameStart(b) or (b >= '0' and b <= '9') or b == '-';
}

fn recipeTailMatches(line: []const u8, end: usize) bool {
    // The optional `(?:\s+[^:]*)?` group taken: at least one whitespace code
    // point, then any run of non-colons, then the colon and trailing space.
    if (end < line.len) {
        const d = decodeAt(line, end);
        if (char_classes.isSpace(d.cp)) {
            var p = end + d.len;
            while (p < line.len and line[p] != ':') p += 1;
            if (p < line.len and allSpace(line[p + 1 ..])) return true;
        }
    }
    // The group skipped: the colon has to sit straight after the name.
    if (end < line.len and line[end] == ':' and allSpace(line[end + 1 ..])) return true;
    return false;
}

/// `RECIPE.match(line)`: anchored, so an indented line is never a header.
/// Returns the recipe name, group 1.
pub fn matchRecipe(line: []const u8) ?[]const u8 {
    if (line.len == 0 or !isNameStart(line[0])) return null;
    var j: usize = 1;
    while (j < line.len and isNameChar(line[j])) j += 1;
    var end = j;
    while (end >= 1) : (end -= 1) {
        if (recipeTailMatches(line, end)) return line[0..end];
        if (end == 1) break;
    }
    return null;
}

// -- recipe bodies -----------------------------------------------------------

pub const Recipe = struct {
    name: []const u8,
    body: []const u8,
};

pub fn freeRecipes(allocator: std.mem.Allocator, recipes: []Recipe) void {
    for (recipes) |recipe| allocator.free(recipe.body);
    allocator.free(recipes);
}

pub fn freeStrings(allocator: std.mem.Allocator, items: []const []const u8) void {
    for (items) |item| allocator.free(item);
    allocator.free(items);
}

/// `_recipe_bodies`: recipe names and bodies from one Just module. A
/// non-indented, non-header line does not close the current recipe, and a
/// comment line inside a body is dropped.
pub fn recipeBodies(allocator: std.mem.Allocator, text: []const u8) ![]Recipe {
    var out = std.ArrayList(Recipe).init(allocator);
    errdefer {
        for (out.items) |recipe| allocator.free(recipe.body);
        out.deinit();
    }
    var lines = std.ArrayList([]const u8).init(allocator);
    defer lines.deinit();

    var name: ?[]const u8 = null;
    var it = LineIterator{ .text = text };
    while (it.next()) |line| {
        if (matchRecipe(line)) |matched| {
            if (name) |current| {
                const body = try std.mem.join(allocator, "\n", lines.items);
                errdefer allocator.free(body);
                try out.append(.{ .name = current, .body = body });
            }
            name = strip(matched);
            lines.clearRetainingCapacity();
        } else if (name != null and
            (std.mem.startsWith(u8, line, " ") or std.mem.startsWith(u8, line, "\t") or
                strip(line).len == 0))
        {
            if (!std.mem.startsWith(u8, lstrip(line), "#")) try lines.append(line);
        }
    }
    if (name) |current| {
        const body = try std.mem.join(allocator, "\n", lines.items);
        errdefer allocator.free(body);
        try out.append(.{ .name = current, .body = body });
    }
    return out.toOwnedSlice();
}

/// `_shell_commands`: join shell continuation lines into command units.
pub fn shellCommands(allocator: std.mem.Allocator, body: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    var current = std.ArrayList(u8).init(allocator);
    defer current.deinit();
    var scratch = std.ArrayList(u8).init(allocator);
    defer scratch.deinit();

    var it = LineIterator{ .text = body };
    while (it.next()) |raw| {
        const line = strip(raw);
        scratch.clearRetainingCapacity();
        try scratch.appendSlice(current.items);
        try scratch.append(' ');
        try scratch.appendSlice(line);
        const stripped = strip(scratch.items);
        const keep = stripped.len;
        std.mem.copyForwards(u8, scratch.items[0..keep], stripped);
        current.clearRetainingCapacity();
        try current.appendSlice(scratch.items[0..keep]);

        if (current.items.len > 0 and current.items[current.items.len - 1] == '\\') {
            current.shrinkRetainingCapacity(current.items.len - 1);
            const trimmed = rstrip(current.items);
            current.shrinkRetainingCapacity(trimmed.len);
        } else if (current.items.len > 0) {
            try out.append(try allocator.dupe(u8, current.items));
            current.clearRetainingCapacity();
        }
    }
    if (current.items.len > 0) try out.append(try allocator.dupe(u8, current.items));
    return out.toOwnedSlice();
}

// -- RAW_CMAKE_CONFIGURE -----------------------------------------------------

fn wordBoundaryAfter(text: []const u8, pos: usize) bool {
    if (pos >= text.len) return true;
    return !char_classes.isWord(decodeAt(text, pos).cp);
}

fn buildOrInstallAhead(text: []const u8, pos: usize) bool {
    if (pos > text.len) return false;
    for ([_][]const u8{ "--build", "--install" }) |literal| {
        if (std.mem.startsWith(u8, text[pos..], literal) and
            wordBoundaryAfter(text, pos + literal.len)) return true;
    }
    return false;
}

fn cmakeConfigureAt(text: []const u8, start: usize) bool {
    if (start > text.len or !std.mem.startsWith(u8, text[start..], "cmake")) return false;
    var i = start + "cmake".len;
    var seen: usize = 0;
    var after_first = i;
    while (i < text.len) {
        const d = decodeAt(text, i);
        if (!char_classes.isSpace(d.cp)) break;
        i += d.len;
        seen += 1;
        if (seen == 1) after_first = i;
    }
    if (seen == 0) return false;
    // `\s+` is greedy but gives ground back: with two or more whitespace code
    // points the lookahead lands on whitespace, never on `--`, so the pattern
    // fires even for `cmake  --build b`.
    if (seen >= 2) return true;
    return !buildOrInstallAhead(text, after_first);
}

/// `RAW_CMAKE_CONFIGURE.search(command)`.
pub fn rawCmakeConfigure(command: []const u8) bool {
    var p: usize = 0;
    while (true) : (p += 1) {
        if (p == 0 and cmakeConfigureAt(command, 0)) return true;
        if (p < command.len) {
            const d = decodeAt(command, p);
            if (char_classes.isSpace(d.cp) and cmakeConfigureAt(command, p + d.len)) return true;
        }
        if (p >= command.len) return false;
    }
}

// -- RAW_COMPILER ------------------------------------------------------------

fn digitSuffixLen(text: []const u8, pos: usize) usize {
    if (pos >= text.len or text[pos] != '-') return 0;
    var i = pos + 1;
    while (i < text.len and text[i] >= '0' and text[i] <= '9') i += 1;
    if (i == pos + 1) return 0;
    return i - pos;
}

fn flagAt(body: []const u8, pos: usize, flag: []const u8) bool {
    if (pos > body.len or !std.mem.startsWith(u8, body[pos..], flag)) return false;
    const q = pos + flag.len;
    if (q >= body.len) return true; // `$` at end of string
    return char_classes.isSpace(decodeAt(body, q).cp);
}

fn finalAlternation(body: []const u8, m: usize) bool {
    if (m <= body.len and std.mem.startsWith(u8, body[m..], "-std=")) return true;
    for ([_][]const u8{ "-c", "-o" }) |flag| {
        if (m == 0 and flagAt(body, 0, flag)) return true;
        if (m < body.len) {
            const d = decodeAt(body, m);
            if (char_classes.isSpace(d.cp) and flagAt(body, m + d.len, flag)) return true;
        }
    }
    return false;
}

fn compilerTailFrom(body: []const u8, token_end: usize) bool {
    var i = token_end;
    var seen: usize = 0;
    var after_first = token_end;
    while (i < body.len) {
        const d = decodeAt(body, i);
        if (!char_classes.isSpace(d.cp)) break;
        i += d.len;
        seen += 1;
        if (seen == 1) after_first = i;
    }
    if (seen == 0) return false;
    const run_end = i;
    var k = after_first;
    while (true) {
        // `[^\n]*` then the trailing alternation, anywhere up to the next LF.
        var limit = k;
        while (limit < body.len and body[limit] != '\n') limit += 1;
        var m = k;
        while (m <= limit) : (m += 1) {
            if (finalAlternation(body, m)) return true;
        }
        if (k >= run_end) return false;
        k += decodeAt(body, k).len;
    }
}

fn compilerFrom(body: []const u8, token_start: usize) bool {
    if (token_start > body.len) return false;
    const rest = body[token_start..];
    var ends: [8]usize = undefined;
    var n: usize = 0;
    if (std.mem.startsWith(u8, rest, "${cc}")) {
        ends[n] = token_start + 5;
        n += 1;
    }
    if (std.mem.startsWith(u8, rest, "${cc")) {
        ends[n] = token_start + 4;
        n += 1;
    }
    if (std.mem.startsWith(u8, rest, "$cc}")) {
        ends[n] = token_start + 4;
        n += 1;
    }
    if (std.mem.startsWith(u8, rest, "$cc")) {
        ends[n] = token_start + 3;
        n += 1;
    }
    if (std.mem.startsWith(u8, rest, "cc")) {
        ends[n] = token_start + 2;
        n += 1;
    }
    if (std.mem.startsWith(u8, rest, "gcc")) {
        const extra = digitSuffixLen(body, token_start + 3);
        if (extra > 0) {
            ends[n] = token_start + 3 + extra;
            n += 1;
        }
        ends[n] = token_start + 3;
        n += 1;
    }
    if (std.mem.startsWith(u8, rest, "clang")) {
        const extra = digitSuffixLen(body, token_start + 5);
        if (extra > 0) {
            ends[n] = token_start + 5 + extra;
            n += 1;
        }
        ends[n] = token_start + 5;
        n += 1;
    }
    for (ends[0..n]) |token_end| {
        if (compilerTailFrom(body, token_end)) return true;
    }
    return false;
}

/// `RAW_COMPILER.search(body)`. The separator after the compiler token is eaten
/// by `\s+`, so a single-space `gcc -c a.c` does NOT fire while `cc  -c ` does.
pub fn rawCompiler(body: []const u8) bool {
    var p: usize = 0;
    while (true) : (p += 1) {
        if (p == 0 and compilerFrom(body, 0)) return true;
        if (p < body.len) {
            const d = decodeAt(body, p);
            if (char_classes.isSpace(d.cp) and compilerFrom(body, p + d.len)) return true;
        }
        if (p >= body.len) return false;
    }
}

// -- findings ----------------------------------------------------------------

/// `_recipe_errors`: reject raw native CMake configure and compile-driver
/// bodies in one Just module.
pub fn recipeErrors(allocator: std.mem.Allocator, label: []const u8, text: []const u8) ![][]const u8 {
    var errors = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (errors.items) |item| allocator.free(item);
        errors.deinit();
    }
    const recipes = try recipeBodies(allocator, text);
    defer freeRecipes(allocator, recipes);

    for (recipes) |recipe| {
        const commands = try shellCommands(allocator, recipe.body);
        defer freeStrings(allocator, commands);
        for (commands) |command| {
            if (rawCmakeConfigure(command) and
                std.mem.indexOf(u8, command, "CMAKE_TOOLCHAIN_FILE") == null)
            {
                try errors.append(try std.fmt.allocPrint(
                    allocator,
                    "{s}: recipe {s}: raw native CMake bypasses host_cmake.sh",
                    .{ label, recipe.name },
                ));
            }
        }
        if (rawCompiler(recipe.body)) {
            try errors.append(try std.fmt.allocPrint(
                allocator,
                "{s}: recipe {s}: raw host compiler invocation bypasses CMake",
                .{ label, recipe.name },
            ));
        }
    }
    return errors.toOwnedSlice();
}

/// `_standalone_cmake`: whether a listfile declares a top-level CMake project.
pub fn standaloneCmake(text: []const u8) bool {
    var start: usize = 0;
    while (true) {
        var i = start;
        while (i < text.len) {
            const d = decodeAt(text, i);
            if (!char_classes.isSpace(d.cp)) break;
            i += d.len;
        }
        if (std.mem.startsWith(u8, text[i..], "project")) {
            var j = i + "project".len;
            while (j < text.len) {
                const d = decodeAt(text, j);
                if (!char_classes.isSpace(d.cp)) break;
                j += d.len;
            }
            if (j < text.len and text[j] == '(') return true;
        }
        const nl = std.mem.indexOfScalarPos(u8, text, start, '\n') orelse return false;
        start = nl + 1;
        if (start >= text.len) return false;
    }
}

/// `pathlib.PurePath(name).suffix`.
pub fn pathlibSuffix(name: []const u8) []const u8 {
    const idx = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    if (idx == 0 or idx == name.len - 1) return "";
    return name[idx..];
}

pub fn isCompiledSuffix(name: []const u8) bool {
    const suffix = pathlibSuffix(name);
    for (compiled_suffixes) |candidate| {
        if (std.mem.eql(u8, suffix, candidate)) return true;
    }
    return false;
}

pub fn lessThanString(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}

pub fn containsString(haystack: []const []const u8, needle: []const u8) bool {
    for (haystack) |item| {
        if (std.mem.eql(u8, item, needle)) return true;
    }
    return false;
}

/// `_dispatcher_errors_fixture`: the pure set comparison the selftest uses to
/// prove dispatcher coverage both ways.
pub fn dispatcherFixtureErrors(
    allocator: std.mem.Allocator,
    expected: []const []const u8,
    listed: []const []const u8,
) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    for (expected) |name| {
        if (!containsString(listed, name)) {
            try out.append(try std.fmt.allocPrint(allocator, "missing {s}", .{name}));
        }
    }
    for (listed) |name| {
        if (!containsString(expected, name)) {
            try out.append(try std.fmt.allocPrint(allocator, "extra {s}", .{name}));
        }
    }
    return out.toOwnedSlice();
}
