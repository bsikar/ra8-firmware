//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Offline ESP32-C6 integration-contract detector (#858).
//!
//! The pinned ESP-hosted patch can apply cleanly while the eventual image is
//! still wrong: a renamed first-party header makes the build recipe stage a
//! nonexistent path, the patched explicit component set may omit the directory
//! the recipe stages, or a component ABI marker may become private while the
//! post-link assertion keeps its old name. Those failures otherwise appear
//! only in a full ESP-IDF build.
//!
//! Nothing here touches the file system, argv or a process: every entry point
//! takes text and answers findings, so the contract is provable with no
//! repository on disk. `cli.zig` owns the membrane; `main.zig` owns the
//! process.
//!
//! Every matcher below reproduces a CPython `re` pattern from the predecessor
//! gate. The behaviour is INHERITED, not redesigned: each asymmetry that a
//! reader would be tempted to tidy is spelled out at its call site and pinned
//! by a test, because a gate that silently changes what it accepts is a gate
//! nobody can trust.

const std = @import("std");

/// The public symbol the component exports and the post-link check asserts.
pub const component_abi = "ra8_mdl_service_component_abi";

/// The extension point the pinned patch declares weak and the component
/// source defines strong.
pub const custom_rpc_hook = "esp_hosted_custom_rpc_sync_handler";

/// Copies that must not silently disappear from the recipe. Existence checks
/// alone only prove the copies that remain, so they cannot catch a removal.
pub const required_staged_sources = [_][]const u8{
    "port/esp32_c6/CMakeLists.txt",
    "port/esp32_c6/src/mdl_service.c",
    "port/esp32_c6/inc/ra8_mdl_service.h",
    "libs/ra8_c6link/inc/ra8_mdl_protocol.h",
    "libs/ra8_c6link/inc/ra8_mdl_http.h",
};

/// A staged copy: the repository-relative source and the component-relative
/// destination, both as they are spelled in the recipe.
pub const StagedCopy = struct {
    source: []const u8,
    destination: []const u8,
};

/// `str.isspace` for a decoded code point, which is the same set CPython's
/// `re` matches with `\s` for `str` patterns. The recipe is authored ASCII,
/// but a stray non-breaking space between the quoted paths still separates
/// them for the predecessor, so it separates them here too.
pub fn isPythonSpace(code_point: u21) bool {
    return switch (code_point) {
        0x09...0x0d, 0x1c...0x1f, 0x20, 0x85, 0xa0, 0x1680 => true,
        0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000 => true,
        else => false,
    };
}

/// Decode one UTF-8 code point at `index`, answering the replacement
/// character for a byte that does not start a well-formed sequence. The
/// predecessor decoded its inputs strictly, so a malformed file never reached
/// the matchers at all; here a lone byte simply matches nothing.
pub fn decodeAt(text: []const u8, index: usize) struct { code_point: u21, len: usize } {
    if (index >= text.len) return .{ .code_point = 0, .len = 0 };
    const len = std.unicode.utf8ByteSequenceLength(text[index]) catch return .{ .code_point = 0xfffd, .len = 1 };
    if (index + len > text.len) return .{ .code_point = 0xfffd, .len = 1 };
    const code_point = std.unicode.utf8Decode(text[index .. index + len]) catch
        return .{ .code_point = 0xfffd, .len = 1 };
    return .{ .code_point = code_point, .len = len };
}

/// Translate line terminators the way CPython's text mode does: `\r\n` and a
/// lone `\r` both become `\n`.
///
/// The predecessor read all four inputs with `read_text`, so universal
/// newlines had already collapsed before any pattern ran. Without this a
/// carriage return before a line end hides the component declaration, which
/// was the one differential mismatch this gate produced.
pub fn normalizeTerminators(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out = try std.ArrayListUnmanaged(u8).initCapacity(allocator, text.len);
    var index: usize = 0;
    while (index < text.len) : (index += 1) {
        if (text[index] == '\r') {
            out.appendAssumeCapacity('\n');
            if (index + 1 < text.len and text[index + 1] == '\n') index += 1;
            continue;
        }
        out.appendAssumeCapacity(text[index]);
    }
    return out.toOwnedSlice(allocator);
}

/// Length of the whitespace run starting at `index`, in bytes. `\s*` in every
/// pattern here is greedy and can never usefully give characters back: the
/// byte that follows a whitespace run is by definition not whitespace, and no
/// pattern asks for whitespace immediately after one, so the longest run is
/// the only run worth trying.
pub fn spaceRun(text: []const u8, index: usize) usize {
    var cursor = index;
    while (cursor < text.len) {
        const decoded = decodeAt(text, cursor);
        if (decoded.len == 0 or !isPythonSpace(decoded.code_point)) break;
        cursor += decoded.len;
    }
    return cursor - index;
}

/// Length of the `[ \t]*` run starting at `index`. Deliberately ASCII-only:
/// the component-directory pattern spells its indent as a character class,
/// not as `\s`, so a non-breaking space there is NOT indentation.
pub fn blankRun(text: []const u8, index: usize) usize {
    var cursor = index;
    while (cursor < text.len and (text[cursor] == ' ' or text[cursor] == '\t')) cursor += 1;
    return cursor - index;
}

/// True when `index` is a `^` position in multiline mode: the start of the
/// text, or immediately after a line feed. Only `\n` starts a line for
/// CPython's `re`, whatever `str.splitlines` would do with the same text.
pub fn atLineStart(text: []const u8, index: usize) bool {
    if (index == 0) return true;
    return text[index - 1] == '\n';
}

/// True when `index` is a `$` position in multiline mode: the end of the
/// text, or immediately before a line feed. A vertical tab or form feed is
/// not a line end here, which is why a trailing carriage return stops the
/// component pattern matching at all.
pub fn atLineEnd(text: []const u8, index: usize) bool {
    if (index == text.len) return true;
    return text[index] == '\n';
}

fn literalAt(text: []const u8, index: usize, needle: []const u8) bool {
    return index + needle.len <= text.len and std.mem.eql(u8, text[index .. index + needle.len], needle);
}

/// The `[^"]+` capture that runs from `start` to the closing quote, or null.
/// The class excludes only the quote, so a path may legitimately contain a
/// newline; the predecessor read one that way and so does this.
fn capturedPath(text: []const u8, start: usize) ?struct { value: []const u8, end: usize } {
    var cursor = start;
    while (cursor < text.len and text[cursor] != '"') cursor += 1;
    if (cursor >= text.len) return null; // no closing quote
    if (cursor == start) return null; // `+`, not `*`: an empty path never matches.
    return .{ .value = text[start..cursor], .end = cursor + 1 };
}

const staged_prefix = "cp";
const staged_source_head = "\"${SCRIPT_DIR}/../../";
const staged_dest_head = "\"${COMPONENT_DIR}/";

/// Try the staged-copy pattern at `index`.
///
/// `cp\s+"\$\{SCRIPT_DIR\}/\.\./\.\./(?P<src>[^"]+)"\s*(?:\\\s*)?"\$\{COMPONENT_DIR\}/(?P<dest>[^"]+)"`
///
/// Two inherited asymmetries, both pinned by tests: the pattern has NO leading
/// word boundary, so `xcp "..."` is a staged copy as far as this gate is
/// concerned; and the continuation group is optional but appears ONCE, so a
/// recipe line broken across two backslashes is invisible.
pub fn matchStagedCopy(text: []const u8, index: usize) ?struct { copy: StagedCopy, end: usize } {
    if (!literalAt(text, index, staged_prefix)) return null;
    var cursor = index + staged_prefix.len;
    const gap = spaceRun(text, cursor);
    if (gap == 0) return null; // `\s+`
    cursor += gap;

    if (!literalAt(text, cursor, staged_source_head)) return null;
    const source = capturedPath(text, cursor + staged_source_head.len) orelse return null;
    cursor = source.end;

    cursor += spaceRun(text, cursor);
    if (cursor < text.len and text[cursor] == '\\') {
        cursor += 1;
        cursor += spaceRun(text, cursor);
    }

    if (!literalAt(text, cursor, staged_dest_head)) return null;
    const destination = capturedPath(text, cursor + staged_dest_head.len) orelse return null;

    return .{
        .copy = .{ .source = source.value, .destination = destination.value },
        .end = destination.end,
    };
}

/// Every staged copy in the recipe, in the order the recipe spells them.
pub fn parseStagedCopies(allocator: std.mem.Allocator, build_text: []const u8) ![]StagedCopy {
    var copies: std.ArrayListUnmanaged(StagedCopy) = .{};
    var index: usize = 0;
    while (index < build_text.len) {
        if (matchStagedCopy(build_text, index)) |found| {
            try copies.append(allocator, found.copy);
            index = found.end;
        } else index += 1;
    }
    return copies.toOwnedSlice(allocator);
}

const component_head = "COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/";

/// The literal staged component name, or null.
///
/// `^[ \t]*COMPONENT_DIR="\$\{PERIPHERAL_DIR\}/components/(?P<name>[^"/]+)"$`
///
/// The name class excludes the separator as well as the quote, so a nested
/// path is not a literal name, and `$` means the closing quote must end the
/// line: a trailing space, a carriage return or a form feed all make the
/// declaration invisible. The FIRST such declaration wins, matching `search`.
pub fn findComponentName(build_text: []const u8) ?[]const u8 {
    var index: usize = 0;
    while (index <= build_text.len) : (index += 1) {
        if (!atLineStart(build_text, index)) continue;
        const start = index + blankRun(build_text, index);
        if (!literalAt(build_text, start, component_head)) continue;
        var cursor = start + component_head.len;
        const name_start = cursor;
        while (cursor < build_text.len and build_text[cursor] != '"' and build_text[cursor] != '/') cursor += 1;
        if (cursor == name_start) continue; // `+`, not `*`.
        if (cursor >= build_text.len or build_text[cursor] != '"') continue;
        if (!atLineEnd(build_text, cursor + 1)) continue;
        return build_text[name_start..cursor];
    }
    return null;
}

const patch_components_head = "+set(COMPONENTS ";

/// Every `^\+set\(COMPONENTS (?P<names>[^)]+)\)$` capture in the patch.
///
/// The class excludes only the closing parenthesis, so the captured names may
/// span newlines, and exactly one literal space follows `COMPONENTS`, so a
/// second space lands inside the capture. `str.split()` swallows it either
/// way, but the capture is what the diagnostic would show, so it is preserved.
pub fn patchComponentSets(allocator: std.mem.Allocator, patch_text: []const u8) ![][]const u8 {
    var captures: std.ArrayListUnmanaged([]const u8) = .{};
    var index: usize = 0;
    while (index <= patch_text.len) : (index += 1) {
        if (!atLineStart(patch_text, index)) continue;
        if (!literalAt(patch_text, index, patch_components_head)) continue;
        const names_start = index + patch_components_head.len;
        var cursor = names_start;
        while (cursor < patch_text.len and patch_text[cursor] != ')') cursor += 1;
        if (cursor == names_start) continue; // `+`, not `*`.
        if (cursor >= patch_text.len) continue;
        if (!atLineEnd(patch_text, cursor + 1)) continue;
        try captures.append(allocator, patch_text[names_start..cursor]);
    }
    return captures.toOwnedSlice(allocator);
}

/// True when `names`, split the way `str.split()` splits on any run of
/// whitespace, contains `wanted`.
pub fn splitContains(names: []const u8, wanted: []const u8) bool {
    var index: usize = 0;
    while (index < names.len) {
        const gap = spaceRun(names, index);
        if (gap > 0) {
            index += gap;
            continue;
        }
        const start = index;
        while (index < names.len and spaceRun(names, index) == 0) {
            const decoded = decodeAt(names, index);
            index += if (decoded.len == 0) 1 else decoded.len;
        }
        if (std.mem.eql(u8, names[start..index], wanted)) return true;
    }
    return false;
}

/// `\s*\(\s*void\s*\)` starting at `index`, answering the end offset.
fn voidParameterList(text: []const u8, index: usize) ?usize {
    var cursor = index + spaceRun(text, index);
    if (cursor >= text.len or text[cursor] != '(') return null;
    cursor += 1;
    cursor += spaceRun(text, cursor);
    if (!literalAt(text, cursor, "void")) return null;
    cursor += "void".len;
    cursor += spaceRun(text, cursor);
    if (cursor >= text.len or text[cursor] != ')') return null;
    return cursor + 1;
}

/// `uint32_t\s+<name>` followed by a `(void)` parameter list, at `index`.
/// The name is matched as a literal with no trailing boundary, exactly as
/// `re.escape` spells it; the mandatory `(` after it is what stops a longer
/// symbol counting, so `<name>_x(void)` is not this function.
fn abiSignature(text: []const u8, index: usize, name: []const u8) ?usize {
    if (!literalAt(text, index, "uint32_t")) return null;
    var cursor = index + "uint32_t".len;
    const gap = spaceRun(text, cursor);
    if (gap == 0) return null;
    cursor += gap;
    if (!literalAt(text, cursor, name)) return null;
    cursor += name.len;
    return voidParameterList(text, cursor);
}

/// `^\s*uint32_t\s+<abi>\s*\(\s*void\s*\)\s*;` anywhere in the header.
///
/// `\s` crosses newlines, so the declaration may sit several blank lines
/// after a `^`, and a storage-class keyword on the same line suppresses it:
/// that is exactly the point, since the gate is asking whether the symbol is
/// declared PUBLIC.
pub fn declaresAbi(header_text: []const u8, name: []const u8) bool {
    var index: usize = 0;
    while (index <= header_text.len) : (index += 1) {
        if (!atLineStart(header_text, index)) continue;
        const start = index + spaceRun(header_text, index);
        const end = abiSignature(header_text, start, name) orelse continue;
        const semi = end + spaceRun(header_text, end);
        if (semi < header_text.len and header_text[semi] == ';') return true;
    }
    return false;
}

/// `\[\[[^\n]+\]\]\s*` at `index`, answering the end offset.
///
/// `[^\n]+` is greedy and cannot cross a line, so an attribute split over two
/// lines is invisible; giving the run back lands on the LAST `]]` on the line,
/// so `[[a]] junk uint32_t ...` is not a definition while `[[a]] [[b]]
/// uint32_t ...` is.
fn attributeRun(text: []const u8, index: usize) ?usize {
    if (!literalAt(text, index, "[[")) return null;
    var line_end = index;
    while (line_end < text.len and text[line_end] != '\n') line_end += 1;
    var cursor = line_end;
    while (cursor > index + 2) : (cursor -= 1) {
        if (cursor + 2 <= text.len and literalAt(text, cursor, "]]")) {
            return cursor + 2 + spaceRun(text, cursor + 2);
        }
    }
    return null;
}

/// `^\s*(?:\[\[[^\n]+\]\]\s*)?uint32_t\s+<abi>\s*\(\s*void\s*\)` in the source.
///
/// No semicolon, so this answers a DEFINITION head; the optional attribute
/// group is what lets an externally visible definition carry `[[gnu::...]]`
/// while `static` still reads as private.
pub fn definesAbi(source_text: []const u8, name: []const u8) bool {
    var index: usize = 0;
    while (index <= source_text.len) : (index += 1) {
        if (!atLineStart(source_text, index)) continue;
        const start = index + spaceRun(source_text, index);
        if (abiSignature(source_text, start, name) != null) return true;
        const after_attribute = attributeRun(source_text, start) orelse continue;
        if (abiSignature(source_text, after_attribute, name) != null) return true;
    }
    return false;
}

/// `^\+__attribute__\(\(weak\)\)\s+esp_err_t\s+<hook>\s*\(` in the patch.
///
/// The `+` must be the first character of the line, so an indented diff body
/// is not the weak declaration: the pattern reads added lines only.
pub fn patchDeclaresWeakHook(patch_text: []const u8, hook: []const u8) bool {
    const head = "+__attribute__((weak))";
    var index: usize = 0;
    while (index <= patch_text.len) : (index += 1) {
        if (!atLineStart(patch_text, index)) continue;
        if (!literalAt(patch_text, index, head)) continue;
        var cursor = index + head.len;
        const gap = spaceRun(patch_text, cursor);
        if (gap == 0) continue;
        cursor += gap;
        if (!literalAt(patch_text, cursor, "esp_err_t")) continue;
        cursor += "esp_err_t".len;
        const inner = spaceRun(patch_text, cursor);
        if (inner == 0) continue;
        cursor += inner;
        if (!literalAt(patch_text, cursor, hook)) continue;
        cursor += hook.len;
        cursor += spaceRun(patch_text, cursor);
        if (cursor < patch_text.len and patch_text[cursor] == '(') return true;
    }
    return false;
}

/// `^\s*esp_err_t\s+<hook>\s*\(` in the component source.
pub fn definesStrongHook(source_text: []const u8, hook: []const u8) bool {
    var index: usize = 0;
    while (index <= source_text.len) : (index += 1) {
        if (!atLineStart(source_text, index)) continue;
        var cursor = index + spaceRun(source_text, index);
        if (!literalAt(source_text, cursor, "esp_err_t")) continue;
        cursor += "esp_err_t".len;
        const gap = spaceRun(source_text, cursor);
        if (gap == 0) continue;
        cursor += gap;
        if (!literalAt(source_text, cursor, hook)) continue;
        cursor += hook.len;
        cursor += spaceRun(source_text, cursor);
        if (cursor < source_text.len and source_text[cursor] == '(') return true;
    }
    return false;
}

/// `pathlib.PurePosixPath(raw).name`: the last component after normalisation,
/// where a repeated separator collapses, a trailing separator is dropped and a
/// lone `.` component disappears while `..` survives as a name.
pub fn pathName(raw: []const u8) []const u8 {
    var last: []const u8 = "";
    var index: usize = 0;
    while (index < raw.len) {
        while (index < raw.len and raw[index] == '/') index += 1;
        const start = index;
        while (index < raw.len and raw[index] != '/') index += 1;
        if (index == start) break;
        const part = raw[start..index];
        if (std.mem.eql(u8, part, ".")) continue;
        last = part;
    }
    return last;
}

/// Which staged sources exist. The predecessor took the same injection: the
/// real run hands over the sources it found in the tree, the detector
/// selftest hands over a fixed set, and the contract is provable either way
/// with no repository on disk.
pub const Inventory = struct {
    sources: []const []const u8,

    pub fn has(self: Inventory, source: []const u8) bool {
        for (self.sources) |candidate| {
            if (std.mem.eql(u8, candidate, source)) return true;
        }
        return false;
    }
};

/// The four committed texts the contract is read from.
pub const Contract = struct {
    build_text: []const u8,
    patch_text: []const u8,
    header_text: []const u8,
    source_text: []const u8,
};

fn lessThan(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// Findings for missing, renamed or nonexistent staged files.
///
/// Order is inherited and load-bearing for the differential: the empty-recipe
/// line first, then the missing required sources in sorted order, then each
/// copy in recipe order with its existence finding before its rename finding.
pub fn checkStagedCopies(
    allocator: std.mem.Allocator,
    findings: *std.ArrayListUnmanaged([]const u8),
    build_text: []const u8,
    inventory: Inventory,
) !void {
    const copies = try parseStagedCopies(allocator, build_text);
    if (copies.len == 0) {
        try findings.append(allocator, try allocator.dupe(u8, "staging: build.sh contains no first-party component copies"));
    }

    var missing: std.ArrayListUnmanaged([]const u8) = .{};
    for (required_staged_sources) |required| {
        var staged = false;
        for (copies) |copy| {
            if (std.mem.eql(u8, copy.source, required)) {
                staged = true;
                break;
            }
        }
        if (!staged) try missing.append(allocator, required);
    }
    std.mem.sort([]const u8, missing.items, {}, lessThan);
    for (missing.items) |required| {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "staging: build.sh no longer stages required source {s}",
            .{required},
        ));
    }

    for (copies) |copy| {
        if (!inventory.has(copy.source)) {
            try findings.append(allocator, try std.fmt.allocPrint(
                allocator,
                "staging: source does not exist: {s}",
                .{copy.source},
            ));
        }
        if (!std.mem.eql(u8, pathName(copy.source), pathName(copy.destination))) {
            try findings.append(allocator, try std.fmt.allocPrint(
                allocator,
                "staging: {s} is renamed to {s}; copied component files must retain their source basename",
                .{ copy.source, copy.destination },
            ));
        }
    }
}

/// Findings when the staged and explicitly enabled component names differ.
pub fn checkComponent(
    allocator: std.mem.Allocator,
    findings: *std.ArrayListUnmanaged([]const u8),
    build_text: []const u8,
    patch_text: []const u8,
) !void {
    const component = findComponentName(build_text);
    const sets = try patchComponentSets(allocator, patch_text);
    if (component == null) {
        try findings.append(allocator, try allocator.dupe(
            u8,
            "component: build.sh does not declare a literal staged component name",
        ));
    }
    if (sets.len != 1) {
        try findings.append(allocator, try allocator.dupe(
            u8,
            "component: patch must add exactly one explicit set(COMPONENTS ...) declaration",
        ));
    }
    if (component != null and sets.len == 1 and !splitContains(sets[0], component.?)) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "component: build.sh stages '{s}', but the patch's explicit COMPONENTS set does not include it",
            .{component.?},
        ));
    }
}

/// Findings for the public, weak, strong and post-link symbol chain.
///
/// The two post-link checks are plain substring tests for the grep fragment
/// the recipe runs, `$` and all: the gate is asserting that the recipe still
/// asks for a strong text symbol, not running the grep itself.
pub fn checkSymbols(
    allocator: std.mem.Allocator,
    findings: *std.ArrayListUnmanaged([]const u8),
    contract: Contract,
) !void {
    if (!declaresAbi(contract.header_text, component_abi)) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "ABI: public header does not declare {s}(void)",
            .{component_abi},
        ));
    }
    if (!definesAbi(contract.source_text, component_abi)) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "ABI: source does not define externally visible {s}(void)",
            .{component_abi},
        ));
    }
    const abi_grep = "T[[:space:]]+" ++ component_abi ++ "$";
    if (std.mem.indexOf(u8, contract.build_text, abi_grep) == null) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "ABI: build.sh does not require strong text symbol {s}",
            .{component_abi},
        ));
    }

    if (!patchDeclaresWeakHook(contract.patch_text, custom_rpc_hook)) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "hook: patch does not provide weak extension point {s}",
            .{custom_rpc_hook},
        ));
    }
    if (!definesStrongHook(contract.source_text, custom_rpc_hook)) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "hook: component source does not define strong {s}",
            .{custom_rpc_hook},
        ));
    }
    const hook_grep = "T[[:space:]]+" ++ custom_rpc_hook ++ "$";
    if (std.mem.indexOf(u8, contract.build_text, hook_grep) == null) {
        try findings.append(allocator, try std.fmt.allocPrint(
            allocator,
            "hook: build.sh does not require strong text symbol {s}",
            .{custom_rpc_hook},
        ));
    }
}

/// Every offline-checkable C6 integration-contract finding, in the order the
/// predecessor emitted them.
pub fn checkContract(
    allocator: std.mem.Allocator,
    contract: Contract,
    inventory: Inventory,
) ![][]const u8 {
    var findings: std.ArrayListUnmanaged([]const u8) = .{};
    try checkStagedCopies(allocator, &findings, contract.build_text, inventory);
    try checkComponent(allocator, &findings, contract.build_text, contract.patch_text);
    try checkSymbols(allocator, &findings, contract);
    return findings.toOwnedSlice(allocator);
}
