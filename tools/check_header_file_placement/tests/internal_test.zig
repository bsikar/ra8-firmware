//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the placement rules (#858, #1219). Every
//! expectation here was established by probing the predecessor's CPython
//! before the Zig was written, so the asymmetries are PINNED, not tidied.

const std = @import("std");
const implementation = @import("implementation");

const testing = std.testing;

fn render(comptime call: anytype, args: anytype) ![]u8 {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    errdefer buffer.deinit();
    const writer = buffer.writer();
    try @call(.auto, call, .{writer} ++ args);
    return buffer.toOwnedSlice();
}

// ---------------------------------------------------------------------------
// pathlib suffix / stem semantics
// ---------------------------------------------------------------------------

test "suffix of an ordinary header" {
    try testing.expectEqualStrings(".h", implementation.pathSuffix("libs/m/src/a.h"));
}

test "suffix of each accepted spelling" {
    try testing.expectEqualStrings(".hpp", implementation.pathSuffix("a.hpp"));
    try testing.expectEqualStrings(".hh", implementation.pathSuffix("a.hh"));
    try testing.expectEqualStrings(".hxx", implementation.pathSuffix("a.hxx"));
}

test "a name that is only a dotted suffix has NO suffix" {
    // Path(".h").suffix == "": a file literally called .h is not a header.
    try testing.expectEqualStrings("", implementation.pathSuffix(".h"));
    try testing.expectEqualStrings("", implementation.pathSuffix("x/.h"));
    try testing.expect(!implementation.isHeader("x/src/.h"));
}

test "a trailing dot is not a suffix" {
    try testing.expectEqualStrings("", implementation.pathSuffix("a."));
    try testing.expectEqualStrings("a.", implementation.pathStem("a."));
}

test "a doubled dot still yields the last suffix" {
    try testing.expectEqualStrings(".h", implementation.pathSuffix("a..h"));
    try testing.expectEqualStrings("a.", implementation.pathStem("a..h"));
}

test "only the LAST suffix counts" {
    try testing.expectEqualStrings(".txt", implementation.pathSuffix("a.h.txt"));
    try testing.expect(!implementation.isHeader("a.h.txt"));
}

test "a leading-dot name with a further dot keeps its suffix" {
    try testing.expectEqualStrings(".h", implementation.pathSuffix(".h.h"));
    try testing.expectEqualStrings(".h", implementation.pathStem(".h.h"));
}

test "header matching is case sensitive" {
    try testing.expect(!implementation.isHeader("a.H"));
    try testing.expect(!implementation.isHeader("a.HPP"));
}

test "a suffixless file is not a header" {
    try testing.expect(!implementation.isHeader("libs/m/src/README"));
}

test "a C source is not a header" {
    try testing.expect(!implementation.isHeader("libs/m/src/a.c"));
}

test "stem of a header" {
    try testing.expectEqualStrings("widget", implementation.pathStem("libs/m/src/widget.h"));
}

test "pathName takes the final component" {
    try testing.expectEqualStrings("a.h", implementation.pathName("libs/m/src/a.h"));
    try testing.expectEqualStrings("a.h", implementation.pathName("a.h"));
}

test "pathName ignores a trailing slash" {
    try testing.expectEqualStrings("src", implementation.pathName("libs/m/src/"));
}

// ---------------------------------------------------------------------------
// the _internal marker
// ---------------------------------------------------------------------------

test "an _internal header is module-private" {
    try testing.expect(implementation.isInternal("libs/m/src/widget_internal.h"));
}

test "a plain header is not" {
    try testing.expect(!implementation.isInternal("libs/m/src/widget.h"));
}

test "the marker must END the stem, not merely appear in it" {
    try testing.expect(!implementation.isInternal("libs/m/src/widget_internal_extra.h"));
}

test "a stem that is exactly the marker counts" {
    try testing.expect(implementation.isInternal("libs/m/src/_internal.h"));
}

test "the marker is case sensitive" {
    try testing.expect(!implementation.isInternal("libs/m/src/widget_INTERNAL.h"));
}

test "the marker is read from the stem, not the suffix" {
    // ".h" has no suffix, so the whole name is the stem and does not end in
    // the marker.
    try testing.expect(!implementation.isInternal("libs/m/src/.h"));
}

// ---------------------------------------------------------------------------
// nearest inc/src ancestor
// ---------------------------------------------------------------------------

test "a header directly under src/ is governed by src" {
    try testing.expectEqualStrings("src", implementation.governingDir("libs/m/src/a.h").?);
    try testing.expect(implementation.underSrc("libs/m/src/a.h"));
}

test "a header under inc/ is public" {
    try testing.expectEqualStrings("inc", implementation.governingDir("libs/m/inc/a.h").?);
    try testing.expect(!implementation.underSrc("libs/m/inc/a.h"));
}

test "the NEAREST ancestor decides, so a deeper inc rescues" {
    try testing.expectEqualStrings("inc", implementation.governingDir("libs/m/src/sub/inc/a.h").?);
    try testing.expect(!implementation.underSrc("libs/m/src/sub/inc/a.h"));
}

test "a deeper src condemns a header below a higher inc" {
    try testing.expect(implementation.underSrc("libs/m/inc/sub/src/a.h"));
}

test "a src component deep in the path still governs" {
    try testing.expect(implementation.underSrc("libs/m/src/a/b/c/a.h"));
}

test "no inc or src ancestor means the header is out of the rule" {
    try testing.expect(implementation.governingDir("libs/m/a.h") == null);
    try testing.expect(!implementation.underSrc("libs/m/a.h"));
}

test "a bare filename has no parent components" {
    try testing.expect(implementation.governingDir("a.h") == null);
}

test "the FILE's own name never governs" {
    // A file called `src` is not a directory component of its own parent.
    try testing.expect(implementation.governingDir("libs/m/src") == null);
}

test "component matching is exact, not a prefix" {
    try testing.expect(!implementation.underSrc("libs/m/srcs/a.h"));
    try testing.expect(!implementation.underSrc("libs/m/source/a.h"));
}

test "component matching is case sensitive" {
    try testing.expect(!implementation.underSrc("libs/m/SRC/a.h"));
}

test "an absolute path walks the same way" {
    try testing.expect(implementation.underSrc("/home/x/repo/libs/m/src/a.h"));
}

// ---------------------------------------------------------------------------
// build-output and vendored exclusions
// ---------------------------------------------------------------------------

test "an exact build component is a build tree at the root" {
    try testing.expect(implementation.isBuildDirName("build"));
    try testing.expect(implementation.isBuildOutput("build/src/a.h"));
}

test "the separator is required, so builders is source" {
    try testing.expect(!implementation.isBuildDirName("builders"));
    try testing.expect(!implementation.isBuildOutput("scripts/builders/src/a.h"));
}

test "the three build prefixes are recognised" {
    try testing.expect(implementation.isBuildDirName("build-cov"));
    try testing.expect(implementation.isBuildDirName("build_host"));
    try testing.expect(implementation.isBuildDirName("cmake-build-debug"));
}

test "a deeper build directory only counts under a build-tree root" {
    try testing.expect(implementation.isBuildOutput("tests/module/build/src/a.h"));
    try testing.expect(!implementation.isBuildOutput("scripts/build/src/a.h"));
}

test "each build-tree root is honoured" {
    for ([_][]const u8{ "docs", "examples", "local-poc", "port", "tests", "tools", "apps" }) |root| {
        var buffer: [64]u8 = undefined;
        const path = try std.fmt.bufPrint(&buffer, "{s}/m/build/src/a.h", .{root});
        try testing.expect(implementation.isBuildOutput(path));
    }
}

test "a non-build-tree root keeps a deeper build directory visible" {
    try testing.expect(!implementation.isBuildOutput("libs/m/build/src/a.h"));
}

test "tool output directories match at any depth" {
    try testing.expect(implementation.isBuildOutput("libs/m/CMakeFiles/src/a.h"));
    try testing.expect(implementation.isBuildOutput("libs/m/__pycache__/a.h"));
    try testing.expect(implementation.isBuildOutput("libs/m/.zig-cache/a.h"));
    try testing.expect(implementation.isBuildOutput("libs/m/_deps/a.h"));
    try testing.expect(implementation.isBuildOutput("libs/m/node_modules/a.h"));
}

test "a FILE named build is not a build tree" {
    // The last component is never examined.
    try testing.expect(!implementation.isBuildOutput("libs/m/src/build"));
    try testing.expect(!implementation.isBuildOutput("CMakeFiles"));
}

test "stripSlashes trims both ends" {
    try testing.expectEqualStrings("a/b", implementation.stripSlashes("///a/b//"));
    try testing.expectEqualStrings("", implementation.stripSlashes("///"));
}

test "isBuildOutputPath folds backslashes" {
    try testing.expect(try implementation.isBuildOutputPath(
        testing.allocator,
        "tests\\module\\build\\src\\a.h",
        "repo",
    ));
}

test "isBuildOutputPath strips the repo root" {
    try testing.expect(try implementation.isBuildOutputPath(
        testing.allocator,
        "/repo/tests/m/build/src/a.h",
        "/repo",
    ));
    try testing.expect(!try implementation.isBuildOutputPath(
        testing.allocator,
        "/repo/libs/m/build/src/a.h",
        "/repo",
    ));
}

test "isBuildOutputPath strips a leading ./" {
    try testing.expect(try implementation.isBuildOutputPath(
        testing.allocator,
        "./tests/m/build/src/a.h",
        "/other",
    ));
}

test "a repo root that is a bare prefix is not stripped" {
    // "/repository/..." must not lose "/repo" as a prefix.
    try testing.expect(!try implementation.isBuildOutputPath(
        testing.allocator,
        "/repository/libs/m/src/a.h",
        "/repo",
    ));
}

test "each vendored fragment takes a header out of scope" {
    for ([_][]const u8{
        "/repo/libs/third_party/v/src/a.h",
        "/repo/apps/shared_libs/third_party/v/src/a.h",
        "/repo/libs/ra8_fonts/src/a.h",
    }) |path| {
        try testing.expect(try implementation.isExcluded(testing.allocator, path, "/repo"));
    }
}

test "the fragment match is a substring, so depth does not matter" {
    try testing.expect(try implementation.isExcluded(
        testing.allocator,
        "/repo/nested/libs/third_party/v/src/a.h",
        "/repo",
    ));
}

test "a first-party header is in scope" {
    try testing.expect(!try implementation.isExcluded(
        testing.allocator,
        "/repo/libs/ra8_net/src/a.h",
        "/repo",
    ));
}

test "a lookalike vendored name without the trailing slash stays in scope" {
    try testing.expect(!try implementation.isExcluded(
        testing.allocator,
        "/repo/libs/third_partyish/src/a.h",
        "/repo",
    ));
}

// ---------------------------------------------------------------------------
// relative rendering and ordering
// ---------------------------------------------------------------------------

test "a path under the root renders repo-relative" {
    try testing.expectEqualStrings(
        "libs/m/src/a.h",
        implementation.relativeTo("/repo/libs/m/src/a.h", "/repo"),
    );
}

test "a path outside the root renders unchanged" {
    try testing.expectEqualStrings(
        "/elsewhere/src/a.h",
        implementation.relativeTo("/elsewhere/src/a.h", "/repo"),
    );
}

test "a bare prefix of the root is not treated as relative" {
    try testing.expectEqualStrings(
        "/repository/src/a.h",
        implementation.relativeTo("/repository/src/a.h", "/repo"),
    );
}

test "sorting is code-point order, so capitals lead" {
    var names = [_][]const u8{ "b.h", "A.h", "a.h", "_x.h", "B.h" };
    std.mem.sort([]const u8, &names, {}, implementation.pythonLessThan);
    try testing.expectEqualStrings("A.h", names[0]);
    try testing.expectEqualStrings("B.h", names[1]);
    try testing.expectEqualStrings("_x.h", names[2]);
    try testing.expectEqualStrings("a.h", names[3]);
    try testing.expectEqualStrings("b.h", names[4]);
}

test "a shared prefix sorts shorter first" {
    var names = [_][]const u8{ "src/ab.h", "src/a.h" };
    std.mem.sort([]const u8, &names, {}, implementation.pythonLessThan);
    try testing.expectEqualStrings("src/a.h", names[0]);
}

// ---------------------------------------------------------------------------
// the audit
// ---------------------------------------------------------------------------

test "only headers under src/ are counted" {
    const targets = [_][]const u8{
        "/repo/libs/m/src/a_internal.h",
        "/repo/libs/m/inc/b.h",
        "/repo/libs/m/c.h",
    };
    const audit = try implementation.auditTargets(testing.allocator, &targets, "/repo");
    defer testing.allocator.free(audit.offenders);
    try testing.expectEqual(@as(usize, 1), audit.scanned);
    try testing.expectEqual(@as(usize, 0), audit.offenders.len);
}

test "a misplaced header is reported repo-relative" {
    const targets = [_][]const u8{"/repo/libs/m/src/widget.h"};
    const audit = try implementation.auditTargets(testing.allocator, &targets, "/repo");
    defer testing.allocator.free(audit.offenders);
    try testing.expectEqual(@as(usize, 1), audit.scanned);
    try testing.expectEqualStrings("libs/m/src/widget.h", audit.offenders[0]);
}

test "offenders come back sorted" {
    const targets = [_][]const u8{
        "/repo/libs/z/src/z.h",
        "/repo/libs/a/src/a.h",
        "/repo/libs/m/src/m.h",
    };
    const audit = try implementation.auditTargets(testing.allocator, &targets, "/repo");
    defer testing.allocator.free(audit.offenders);
    try testing.expectEqual(@as(usize, 3), audit.offenders.len);
    try testing.expectEqualStrings("libs/a/src/a.h", audit.offenders[0]);
    try testing.expectEqualStrings("libs/m/src/m.h", audit.offenders[1]);
    try testing.expectEqualStrings("libs/z/src/z.h", audit.offenders[2]);
}

test "an empty target list audits clean and vacuous" {
    const targets = [_][]const u8{};
    const audit = try implementation.auditTargets(testing.allocator, &targets, "/repo");
    defer testing.allocator.free(audit.offenders);
    try testing.expectEqual(@as(usize, 0), audit.scanned);
    try testing.expect(!implementation.censusOk(audit.scanned, false));
}

test "a nested inc keeps a header out of the count entirely" {
    const targets = [_][]const u8{"/repo/libs/m/src/sub/inc/public.h"};
    const audit = try implementation.auditTargets(testing.allocator, &targets, "/repo");
    defer testing.allocator.free(audit.offenders);
    try testing.expectEqual(@as(usize, 0), audit.scanned);
}

// ---------------------------------------------------------------------------
// the census floor
// ---------------------------------------------------------------------------

test "the whole-tree floor is 100" {
    try testing.expectEqual(@as(usize, 100), implementation.min_private_headers);
}

test "a whole-tree sweep at the floor passes" {
    try testing.expect(implementation.censusOk(implementation.min_private_headers, false));
}

test "a whole-tree sweep one below the floor fails" {
    try testing.expect(!implementation.censusOk(implementation.min_private_headers - 1, false));
}

test "explicit paths are exempt from the floor" {
    try testing.expect(implementation.censusOk(0, true));
}

// ---------------------------------------------------------------------------
// renderers, byte for byte
// ---------------------------------------------------------------------------

test "the no-headers line" {
    const text = try render(implementation.renderNoHeaders, .{});
    defer testing.allocator.free(text);
    try testing.expectEqualStrings("check_header_file_placement.py: no headers to scan\n", text);
}

test "the collapsed-census line names the count and the floor" {
    const text = try render(implementation.renderCollapsed, .{@as(usize, 7)});
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "check_header_file_placement.py: whole-tree scan reached only 7 private header(s), below floor 100\n",
        text,
    );
}

test "the clean line names the scanned count" {
    const text = try render(implementation.renderClean, .{@as(usize, 142)});
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "check_header_file_placement.py: 142 src/ header(s) scanned, all module-private (*_internal.h).\n",
        text,
    );
}

test "the offender report keeps the blank line the format string carried" {
    const offenders = [_][]const u8{"libs/m/src/widget.h"};
    const text = try render(implementation.renderOffenders, .{@as([]const []const u8, &offenders)});
    defer testing.allocator.free(text);
    try testing.expect(std.mem.startsWith(
        u8,
        text,
        "check_header_file_placement.py: 1 src/ header(s) are not *_internal.h:\n\n  libs/m/src/widget.h\n\n",
    ));
}

test "the offender report ends with the guidance block" {
    const offenders = [_][]const u8{ "a/src/a.h", "b/src/b.h" };
    const text = try render(implementation.renderOffenders, .{@as([]const []const u8, &offenders)});
    defer testing.allocator.free(text);
    try testing.expect(std.mem.endsWith(u8, text, "waiver marker -- placement is the contract.\n"));
    try testing.expect(std.mem.indexOf(u8, text, "  a/src/a.h\n  b/src/b.h\n") != null);
}

test "the guidance offers both fixes and no waiver" {
    try testing.expect(std.mem.indexOf(u8, implementation.guidance, "move it to the") != null);
    try testing.expect(std.mem.indexOf(u8, implementation.guidance, "rename it '*_internal.h'") != null);
    try testing.expect(std.mem.indexOf(u8, implementation.guidance, "There is no") != null);
}

test "the selftest pass line" {
    const text = try render(implementation.renderSelftestPass, .{});
    defer testing.allocator.free(text);
    try testing.expectEqualStrings(
        "check_header_file_placement.py --selftest: PASS (fire, quiet, tests, exclusions)\n",
        text,
    );
}

test "a selftest failure is indented and tagged" {
    const text = try render(implementation.renderSelftestFailure, .{@as([]const u8, "boom")});
    defer testing.allocator.free(text);
    try testing.expectEqualStrings("  [FAIL] boom\n", text);
}

test "the selftest-with-paths refusal" {
    const text = try render(implementation.renderSelftestWithPaths, .{});
    defer testing.allocator.free(text);
    try testing.expectEqualStrings("--selftest does not accept paths\n", text);
}

// ---------------------------------------------------------------------------
// the constant tables themselves
// ---------------------------------------------------------------------------

test "the scan roots are the six the predecessor walked" {
    try testing.expectEqual(@as(usize, 6), implementation.scan_roots.len);
    try testing.expectEqualStrings("libs", implementation.scan_roots[0]);
    try testing.expectEqualStrings("tests", implementation.scan_roots[5]);
}

test "the header suffixes are the four the predecessor accepted" {
    try testing.expectEqual(@as(usize, 4), implementation.header_suffixes.len);
}

test "the exclude fragments are the three the predecessor carried" {
    try testing.expectEqual(@as(usize, 3), implementation.exclude_fragments.len);
}
