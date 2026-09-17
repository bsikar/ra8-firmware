//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the PEP 668 override detector (#858).
//!
//! Every fixture that must carry the rejected option builds it from
//! `implementation.forbidden`, never as one literal: the gate scans its own
//! sources, so a spelled-out literal in this file would fail the live tree.

const std = @import("std");
const implementation = @import("implementation");

const forbidden = implementation.forbidden;

test "the forbidden option is the pip override, spelled in halves" {
    try std.testing.expect(std.mem.startsWith(u8, forbidden, "--break-"));
    try std.testing.expect(std.mem.endsWith(u8, forbidden, "-packages"));
    try std.testing.expectEqual(@as(usize, 23), forbidden.len);
}

test "the census floor and self source are the inherited scope policy" {
    try std.testing.expectEqual(@as(usize, 4000), implementation.min_scoped_files);
    try std.testing.expectEqualStrings(
        "tools/check_no_unsafe_python_install/src/internal/root.zig",
        implementation.self_source,
    );
}

test "vendored upstream trees are excluded" {
    try std.testing.expect(implementation.isExcluded("libs/third_party/lvgl/lv_conf.h"));
    try std.testing.expect(implementation.isExcluded("port/threadx/tx_api.h"));
    try std.testing.expect(implementation.isExcluded("apps/shared_libs/third_party/x.c"));
}

test "generated sbom copies and fixture corpora are excluded" {
    try std.testing.expect(implementation.isExcluded("docs/sbom/upstream/report.json"));
    try std.testing.expect(implementation.isExcluded("tests/fixtures/blob.bin"));
}

test "authored trees are in scope" {
    try std.testing.expect(!implementation.isExcluded("scripts/ci/gates/checks.sh"));
    try std.testing.expect(!implementation.isExcluded("docs/DOCS.md"));
    try std.testing.expect(!implementation.isExcluded("libs/ra8_ui/src/ui.c"));
}

test "an excluded prefix matches only on a directory boundary" {
    try std.testing.expect(!implementation.isExcluded("libs/third_partyx/file.c"));
    try std.testing.expect(!implementation.isExcluded("tests/fixtures.md"));
    try std.testing.expect(!implementation.isExcluded(""));
}

test "normalizeTerminators collapses CRLF to LF" {
    const out = try implementation.normalizeTerminators(std.testing.allocator, "a\r\nb\r\n");
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualStrings("a\nb\n", out);
}

test "normalizeTerminators translates a lone CR" {
    const out = try implementation.normalizeTerminators(std.testing.allocator, "a\rb");
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualStrings("a\nb", out);
}

test "normalizeTerminators leaves LF-only text alone" {
    const out = try implementation.normalizeTerminators(std.testing.allocator, "a\nb\n");
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualStrings("a\nb\n", out);
}

test "normalizeTerminators handles a trailing CR" {
    const out = try implementation.normalizeTerminators(std.testing.allocator, "a\r");
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualStrings("a\n", out);
}

test "boundaryLen reports the one-byte ASCII line breaks" {
    try std.testing.expectEqual(@as(usize, 1), implementation.boundaryLen("\n", 0));
    try std.testing.expectEqual(@as(usize, 1), implementation.boundaryLen("\x0b", 0));
    try std.testing.expectEqual(@as(usize, 1), implementation.boundaryLen("\x0c", 0));
    try std.testing.expectEqual(@as(usize, 1), implementation.boundaryLen("\x1c", 0));
    try std.testing.expectEqual(@as(usize, 1), implementation.boundaryLen("\x1d", 0));
    try std.testing.expectEqual(@as(usize, 1), implementation.boundaryLen("\x1e", 0));
}

test "boundaryLen reports the multi-byte line breaks" {
    try std.testing.expectEqual(@as(usize, 2), implementation.boundaryLen("\u{85}", 0));
    try std.testing.expectEqual(@as(usize, 3), implementation.boundaryLen("\u{2028}", 0));
    try std.testing.expectEqual(@as(usize, 3), implementation.boundaryLen("\u{2029}", 0));
}

test "boundaryLen rejects ordinary text and truncated sequences" {
    try std.testing.expectEqual(@as(usize, 0), implementation.boundaryLen("a", 0));
    try std.testing.expectEqual(@as(usize, 0), implementation.boundaryLen("\xc2", 0));
    try std.testing.expectEqual(@as(usize, 0), implementation.boundaryLen("\xc2\xa9", 0));
    try std.testing.expectEqual(@as(usize, 0), implementation.boundaryLen("\xe2\x80", 0));
    try std.testing.expectEqual(@as(usize, 0), implementation.boundaryLen("\xe2\x80\xa6", 0));
    try std.testing.expectEqual(@as(usize, 0), implementation.boundaryLen("a", 5));
}

fn scan(text: []const u8) ![]usize {
    return implementation.scanText(std.testing.allocator, text);
}

test "an override on the only line is line one" {
    const hits = try scan("pip install " ++ forbidden);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{1}, hits);
}

test "an override on the second line is line two" {
    const hits = try scan("clean\npip install " ++ forbidden ++ "\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{2}, hits);
}

test "every offending line is reported" {
    const hits = try scan(forbidden ++ "\nclean\n" ++ forbidden ++ "\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{ 1, 3 }, hits);
}

test "clean text reports nothing" {
    const hits = try scan("python3 -m venv .venv\n.venv/bin/pip install libclang\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqual(@as(usize, 0), hits.len);
}

test "empty text reports nothing" {
    const hits = try scan("");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqual(@as(usize, 0), hits.len);
}

test "a form feed ends a line, so later findings do not shift" {
    const hits = try scan("one\x0ctwo\n" ++ forbidden);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{3}, hits);
}

test "a unicode line separator ends a line too" {
    const hits = try scan("one\u{2028}" ++ forbidden);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{2}, hits);
}

test "a next-line character ends a line too" {
    const hits = try scan("one\u{85}" ++ forbidden);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{2}, hits);
}

test "a final line without a terminator is still scanned" {
    const hits = try scan("one\ntwo\n" ++ forbidden);
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{3}, hits);
}

test "a trailing terminator does not open an extra line" {
    const hits = try scan(forbidden ++ "\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{1}, hits);
}

test "the option split across two lines is not a finding" {
    const hits = try scan("--break-\nsystem-packages\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqual(@as(usize, 0), hits.len);
}

test "the option inside prose still fires" {
    const hits = try scan("hint: run pip install " ++ forbidden ++ " if it fails\n");
    defer std.testing.allocator.free(hits);
    try std.testing.expectEqualSlices(usize, &.{1}, hits);
}

/// Census resolver answering from a fixed list of existing paths.
const FakeTree = struct {
    present: []const []const u8,

    fn resolver(self: *const FakeTree) implementation.Resolver {
        return .{ .context = self, .is_file_fn = thunk };
    }

    fn thunk(context: *const anyopaque, rel: []const u8) bool {
        const self: *const FakeTree = @ptrCast(@alignCast(context));
        for (self.present) |path| {
            if (std.mem.eql(u8, path, rel)) return true;
        }
        return false;
    }
};

test "the scanned set drops census entries that are not files" {
    const tree = FakeTree{ .present = &.{ "a.c", "self.zig" } };
    const scoped = try implementation.selectScoped(
        std.testing.allocator,
        &.{ "a.c", "gone.c" },
        tree.resolver(),
        "self.zig",
    );
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 2), scoped.len);
    try std.testing.expectEqualStrings("a.c", scoped[0]);
    try std.testing.expectEqualStrings("self.zig", scoped[1]);
}

test "the scanned set drops excluded trees and empty entries" {
    const tree = FakeTree{ .present = &.{ "a.c", "libs/third_party/x.c", "self.zig" } };
    const scoped = try implementation.selectScoped(
        std.testing.allocator,
        &.{ "libs/third_party/x.c", "", "a.c" },
        tree.resolver(),
        "self.zig",
    );
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 2), scoped.len);
    try std.testing.expectEqualStrings("a.c", scoped[0]);
}

test "the gate's own source is force-added to the scanned set" {
    const tree = FakeTree{ .present = &.{"self.zig"} };
    const scoped = try implementation.selectScoped(
        std.testing.allocator,
        &.{},
        tree.resolver(),
        "self.zig",
    );
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 1), scoped.len);
    try std.testing.expectEqualStrings("self.zig", scoped[0]);
}

test "a census that already lists the gate's source scans it once" {
    const tree = FakeTree{ .present = &.{"self.zig"} };
    const scoped = try implementation.selectScoped(
        std.testing.allocator,
        &.{ "self.zig", "self.zig" },
        tree.resolver(),
        "self.zig",
    );
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 1), scoped.len);
}

test "a missing gate source cannot be force-added" {
    const tree = FakeTree{ .present = &.{"a.c"} };
    const scoped = try implementation.selectScoped(
        std.testing.allocator,
        &.{"a.c"},
        tree.resolver(),
        "self.zig",
    );
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 1), scoped.len);
    try std.testing.expect(!implementation.containsPath(scoped, "self.zig"));
}

test "the scanned set is sorted, whatever order the census arrived in" {
    const tree = FakeTree{ .present = &.{ "b.c", "a.c", "m/x.c", "self.zig" } };
    const scoped = try implementation.selectScoped(
        std.testing.allocator,
        &.{ "m/x.c", "b.c", "a.c" },
        tree.resolver(),
        "self.zig",
    );
    defer std.testing.allocator.free(scoped);
    try std.testing.expectEqual(@as(usize, 4), scoped.len);
    try std.testing.expectEqualStrings("a.c", scoped[0]);
    try std.testing.expectEqualStrings("b.c", scoped[1]);
    try std.testing.expectEqualStrings("m/x.c", scoped[2]);
    try std.testing.expectEqualStrings("self.zig", scoped[3]);
}

test "a census below the floor has collapsed" {
    try std.testing.expect(implementation.scopeCollapsed(3999, 4000, true));
    try std.testing.expect(!implementation.scopeCollapsed(4000, 4000, true));
    try std.testing.expect(!implementation.scopeCollapsed(9000, 4000, true));
}

test "a census without the gate's own source has collapsed" {
    try std.testing.expect(implementation.scopeCollapsed(9000, 4000, false));
}

test "containsPath finds an exact path only" {
    const paths = [_][]const u8{ "a.c", "b/c.c" };
    try std.testing.expect(implementation.containsPath(&paths, "b/c.c"));
    try std.testing.expect(!implementation.containsPath(&paths, "b/c"));
    try std.testing.expect(!implementation.containsPath(&paths, "c.c"));
}

test "a finding renders as path and line" {
    const row = try implementation.renderFinding(std.testing.allocator, "docs/X.md", 42);
    defer std.testing.allocator.free(row);
    try std.testing.expectEqualStrings("docs/X.md:42", row);
}

test "lessThanPath orders by bytes" {
    try std.testing.expect(implementation.lessThanPath({}, "a", "b"));
    try std.testing.expect(!implementation.lessThanPath({}, "b", "a"));
    try std.testing.expect(!implementation.lessThanPath({}, "a", "a"));
}

test "the inherited selftest cases all hold against the detector" {
    const failures = try implementation.selftestFailures(std.testing.allocator);
    defer std.testing.allocator.free(failures);
    try std.testing.expectEqual(@as(usize, 0), failures.len);
    try std.testing.expectEqual(@as(usize, 4), implementation.selftest_cases.len);
}

test "each selftest case expects exactly the lines the detector finds" {
    for (implementation.selftest_cases) |case| {
        const hits = try scan(case.text);
        defer std.testing.allocator.free(hits);
        try std.testing.expectEqualSlices(usize, case.expected, hits);
    }
}
