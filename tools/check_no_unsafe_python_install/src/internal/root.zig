//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Detection algebra for the PEP 668 override gate (#858).
//!
//! Python-managed repository tools belong in a virtual environment. The
//! system-pip override can mutate apt-owned files, and the user-site fallback
//! makes the interpreter and PATH depend on whichever account provisioned a
//! host, so the option may not appear in automation, images, provisioning,
//! documentation, or a copy-pasteable error hint.
//!
//! Every function here takes text (or a census of paths plus an injected
//! existence predicate) and returns a decision. No file system, no process
//! state, no argv, so the contract is provable with no repository on disk.

const std = @import("std");

/// The option no authored file may carry, spelled in two halves on purpose.
///
/// The gate scans its own implementation, so one whole literal anywhere in
/// these sources would make the live tree fail on the detector itself. The
/// Python this replaces split the string for exactly the same reason.
pub const forbidden = "--break-" ++ "system-packages";

/// Trees that are not authored here: vendored upstreams, generated SBOM
/// copies, and fixture corpora whose bytes are inputs rather than guidance.
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

/// Census floor. A gate that scanned a collapsed file set must never report a
/// clean tree, so a census below this is an error, not a pass.
pub const min_scoped_files: usize = 4000;

/// The gate's own implementation source, force-added to the census so the
/// detector always scans itself even if the census somehow misses it.
pub const self_source = "tools/check_no_unsafe_python_install/src/internal/root.zig";

/// Report whether a census-relative path lies under an excluded tree.
pub fn isExcluded(rel: []const u8) bool {
    for (excluded_prefixes) |prefix| {
        if (std.mem.startsWith(u8, rel, prefix)) return true;
    }
    return false;
}

/// Translate CRLF and lone CR to LF, the way Python text mode read sources.
///
/// The gate reports the line numbers it counted after that translation, so a
/// CRLF file has to collapse here or every finding below the first CR shifts.
pub fn normalizeTerminators(allocator: std.mem.Allocator, raw: []const u8) ![]u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, raw.len);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < raw.len) : (index += 1) {
        if (raw[index] != '\r') {
            try out.append(raw[index]);
            continue;
        }
        try out.append('\n');
        if (index + 1 < raw.len and raw[index + 1] == '\n') index += 1;
    }
    return out.toOwnedSlice();
}

/// Length in bytes of the line boundary at `index`, or 0 when there is none.
///
/// `str.splitlines` breaks on more than LF: the vertical tab, the form feed,
/// the three ASCII separators, and the U+0085, U+2028 and U+2029 code points
/// all end a line. A form feed inside a source file is common enough that
/// ignoring the wider set would shift every line number after it.
pub fn boundaryLen(text: []const u8, index: usize) usize {
    if (index >= text.len) return 0;
    switch (text[index]) {
        '\n', 0x0b, 0x0c, 0x1c, 0x1d, 0x1e => return 1,
        0xc2 => {
            if (index + 1 < text.len and text[index + 1] == 0x85) return 2;
            return 0;
        },
        0xe2 => {
            if (index + 2 < text.len and text[index + 1] == 0x80 and
                (text[index + 2] == 0xa8 or text[index + 2] == 0xa9)) return 3;
            return 0;
        },
        else => return 0,
    }
}

/// One-based line numbers whose line carries the forbidden option.
///
/// Lines are cut exactly where `str.splitlines` cut them, and a trailing
/// boundary does not open a further empty line.
pub fn scanText(allocator: std.mem.Allocator, text: []const u8) ![]usize {
    var hits = std.ArrayList(usize).init(allocator);
    errdefer hits.deinit();
    var number: usize = 1;
    var start: usize = 0;
    var index: usize = 0;
    while (index < text.len) {
        const width = boundaryLen(text, index);
        if (width == 0) {
            index += 1;
            continue;
        }
        if (std.mem.indexOf(u8, text[start..index], forbidden) != null) try hits.append(number);
        number += 1;
        index += width;
        start = index;
    }
    if (start < text.len and std.mem.indexOf(u8, text[start..], forbidden) != null) {
        try hits.append(number);
    }
    return hits.toOwnedSlice();
}

/// Injected answer to "is this census entry a readable file in the tree?".
///
/// The census filter is pure with this in hand, so the scope rules are
/// provable with no repository, exactly like the detector.
pub const Resolver = struct {
    context: *const anyopaque,
    is_file_fn: *const fn (context: *const anyopaque, rel: []const u8) bool,

    /// Ask the injected predicate about one census-relative path.
    pub fn isFile(self: Resolver, rel: []const u8) bool {
        return self.is_file_fn(self.context, rel);
    }
};

/// The scanned set: census entries that exist and are not excluded, plus the
/// gate's own source, deduplicated and sorted.
///
/// Sorted because the findings this replaces were emitted in that order, and
/// a gate whose output depends on Git's enumeration order cannot be diffed.
pub fn selectScoped(
    allocator: std.mem.Allocator,
    rels: []const []const u8,
    resolver: Resolver,
    self_rel: []const u8,
) ![][]const u8 {
    var kept = std.ArrayList([]const u8).init(allocator);
    errdefer kept.deinit();
    for (rels) |rel| {
        if (rel.len == 0) continue;
        if (isExcluded(rel)) continue;
        if (!resolver.isFile(rel)) continue;
        try kept.append(rel);
    }
    if (resolver.isFile(self_rel)) try kept.append(self_rel);

    const items = try kept.toOwnedSlice();
    std.mem.sort([]const u8, items, {}, lessThanPath);
    var unique: usize = 0;
    for (items) |item| {
        if (unique != 0 and std.mem.eql(u8, items[unique - 1], item)) continue;
        items[unique] = item;
        unique += 1;
    }
    // Shrink the allocation itself rather than returning a shorter view of
    // it: the caller owns and frees what it is handed, and a sub-slice of a
    // larger block is not a freeable allocation.
    return allocator.realloc(items, unique);
}

/// Order two paths by bytes, so the sweep is reproducible across hosts.
pub fn lessThanPath(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// Report whether a census is too small, or lost the gate's own source.
///
/// Either way nothing trustworthy was scanned, so the caller owes an error
/// rather than a verdict.
pub fn scopeCollapsed(count: usize, floor: usize, has_self: bool) bool {
    return count < floor or !has_self;
}

/// Report whether the scanned set contains the gate's own source.
pub fn containsPath(paths: []const []const u8, wanted: []const u8) bool {
    for (paths) |path| {
        if (std.mem.eql(u8, path, wanted)) return true;
    }
    return false;
}

/// Render one finding as `path:line`, the form the gate has always printed.
pub fn renderFinding(allocator: std.mem.Allocator, rel: []const u8, line: usize) ![]u8 {
    return std.fmt.allocPrint(allocator, "{s}:{d}", .{ rel, line });
}

/// One detector selftest: a fixture, the lines it must flag, and its label.
pub const SelftestCase = struct {
    /// Fixture text handed to `scanText`.
    text: []const u8,
    /// One-based lines the detector must report, in order.
    expected: []const usize,
    /// Human-facing description, printed on failure.
    label: []const u8,
};

/// The active install the gate exists to reject, built from the halves.
const unsafe_install = "python3 -m pip install " ++ forbidden ++ " libclang";

/// Both directions of the detector, inherited from the gate's own selftest.
pub const selftest_cases = [_]SelftestCase{
    .{ .text = unsafe_install, .expected = &.{1}, .label = "an unsafe active install fires" },
    .{
        .text = "hint: " ++ unsafe_install,
        .expected = &.{1},
        .label = "an unsafe documentation hint fires",
    },
    .{
        .text = "python3 -m venv .venv\n.venv/bin/pip install libclang",
        .expected = &.{},
        .label = "a venv passes",
    },
    .{
        .text = "python3 -m pip --version",
        .expected = &.{},
        .label = "a non-mutating pip probe passes",
    },
};

/// Labels of the selftest cases whose detector result is wrong.
pub fn selftestFailures(allocator: std.mem.Allocator) ![][]const u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    errdefer failures.deinit();
    for (selftest_cases) |case| {
        const hits = try scanText(allocator, case.text);
        defer allocator.free(hits);
        if (!std.mem.eql(usize, hits, case.expected)) try failures.append(case.label);
    }
    return failures.toOwnedSlice();
}
