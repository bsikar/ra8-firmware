//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the example board-pin gate's decisions
//! (#858).  Every case here pins something the Python predecessor did, most of
//! it established by probing CPython first: the Unicode reach of `\d` and `\s`
//! inside ENCODING_RE, the `str.splitlines()` break set, the `str.strip()`
//! whitespace set (which is NOT the same as `\s`), the build-output predicate
//! and the two path-suffix notions the predecessor used in its two branches.

const std = @import("std");
const testing = std.testing;
const implementation = @import("implementation");

test "the idiom the gate exists to catch is a finding" {
    try testing.expect(implementation.matchEncoding(implementation.selftest_idiom));
}

test "a board-symbol reference is quiet" {
    try testing.expect(!implementation.matchEncoding(implementation.selftest_board_reference));
}

test "no whitespace at all between the tokens still matches" {
    try testing.expect(implementation.matchEncoding("k_ra8_port_6<<8)|(uint16_t)k_ra8_pin_11"));
}

test "tabs in every gap still match" {
    try testing.expect(implementation.matchEncoding(
        "k_ra8_port_6\t<<\t8\t)\t|\t(\tuint16_t\t)\tk_ra8_pin_1",
    ));
}

test "a newline inside a gap matches, because the pattern never anchored to a line" {
    try testing.expect(implementation.matchEncoding("k_ra8_port_6 <<\n8) | (uint16_t)k_ra8_pin_1"));
}

test "a non-breaking space is whitespace to the pattern" {
    try testing.expect(implementation.matchEncoding(
        "k_ra8_port_6\u{00a0}<<\u{00a0}8)\u{00a0}|\u{00a0}(\u{00a0}uint16_t\u{00a0})\u{00a0}k_ra8_pin_1",
    ));
}

test "an ideographic space is whitespace to the pattern" {
    try testing.expect(implementation.matchEncoding(
        "k_ra8_port_6\u{3000}<<\u{3000}8)|(uint16_t)k_ra8_pin_1",
    ));
}

test "a vertical tab is whitespace to the pattern" {
    try testing.expect(implementation.matchEncoding("k_ra8_port_6\x0b<<\x0b8)|(uint16_t)k_ra8_pin_1"));
}

test "U+001F is whitespace to the pattern even though it never breaks a line" {
    try testing.expect(implementation.matchEncoding("k_ra8_port_6\x1f<<8)|(uint16_t)k_ra8_pin_1"));
}

test "Arabic-Indic digits satisfy the port and pin numbers" {
    try testing.expect(implementation.matchEncoding(
        "k_ra8_port_\u{0666} << 8) | (uint16_t)k_ra8_pin_\u{0663}",
    ));
}

test "a superscript two is not a digit, so the port number is absent" {
    try testing.expect(!implementation.matchEncoding(
        "k_ra8_port_\u{00b2} << 8) | (uint16_t)k_ra8_pin_1",
    ));
}

test "the shift width is the literal 8, not any digit" {
    try testing.expect(!implementation.matchEncoding("k_ra8_port_6 << 7) | (uint16_t)k_ra8_pin_1"));
}

test "an Arabic-Indic eight is not the literal 8" {
    try testing.expect(!implementation.matchEncoding(
        "k_ra8_port_6 << \u{0668}) | (uint16_t)k_ra8_pin_1",
    ));
}

test "the cast spelling is case sensitive" {
    try testing.expect(!implementation.matchEncoding("k_ra8_port_6 << 8) | (UINT16_T)k_ra8_pin_1"));
}

test "a missing close paren after the shift is not the idiom" {
    try testing.expect(!implementation.matchEncoding("k_ra8_port_6 << 8 | (uint16_t)k_ra8_pin_1"));
}

test "a second close paren breaks the shape" {
    try testing.expect(!implementation.matchEncoding("k_ra8_port_6 << 8)) | (uint16_t)k_ra8_pin_1"));
}

test "no leading word boundary, so a glued prefix still matches" {
    try testing.expect(implementation.matchEncoding("xk_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_11"));
}

test "no trailing boundary either, so trailing junk after the pin number matches" {
    try testing.expect(implementation.matchEncoding(
        "k_ra8_port_60 << 8) | (uint16_t)k_ra8_pin_123x",
    ));
}

test "a pin number with no digits at all is not a finding" {
    try testing.expect(!implementation.matchEncoding("k_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_"));
}

test "a port number with no digits at all is not a finding" {
    try testing.expect(!implementation.matchEncoding("k_ra8_port_ << 8) | (uint16_t)k_ra8_pin_1"));
}

test "the match may start anywhere in the line" {
    try testing.expect(implementation.matchEncodingAt(implementation.selftest_idiom, 23));
    try testing.expect(!implementation.matchEncodingAt(implementation.selftest_idiom, 0));
}

test "splitlines breaks on the whole CPython break set" {
    var lines = implementation.LineIterator{
        .text = "a\x0bb\x0cc\x1cd\x1ee\u{0085}f\u{2028}g\x1fh\r\ni",
    };
    const expected = [_][]const u8{ "a", "b", "c", "d", "e", "f", "g\x1fh", "i" };
    for (expected) |want| {
        const got = lines.next() orelse return error.TooFewLines;
        try testing.expectEqualStrings(want, got);
    }
    try testing.expect(lines.next() == null);
}

test "CRLF counts as one break" {
    var lines = implementation.LineIterator{ .text = "a\r\nb" };
    try testing.expectEqualStrings("a", lines.next().?);
    try testing.expectEqualStrings("b", lines.next().?);
    try testing.expect(lines.next() == null);
}

test "a bare CR breaks a line" {
    var lines = implementation.LineIterator{ .text = "a\rb" };
    try testing.expectEqualStrings("a", lines.next().?);
    try testing.expectEqualStrings("b", lines.next().?);
}

test "a trailing break yields no empty final line" {
    var lines = implementation.LineIterator{ .text = "a\n" };
    try testing.expectEqualStrings("a", lines.next().?);
    try testing.expect(lines.next() == null);
}

test "an empty body has no lines" {
    var lines = implementation.LineIterator{ .text = "" };
    try testing.expect(lines.next() == null);
}

test "U+2029 breaks a line" {
    var lines = implementation.LineIterator{ .text = "a\u{2029}b" };
    try testing.expectEqualStrings("a", lines.next().?);
    try testing.expectEqualStrings("b", lines.next().?);
}

test "strip removes ASCII whitespace from both ends" {
    try testing.expectEqualStrings("x", implementation.strip("  \tx\t "));
}

test "strip removes a non-breaking space, matching str.strip" {
    try testing.expectEqualStrings("x", implementation.strip("\u{00a0} x \u{00a0}"));
}

test "strip removes U+001F, which splitlines never treated as a break" {
    try testing.expectEqualStrings("x", implementation.strip("\x1f x \x1f"));
}

test "strip leaves an interior space alone" {
    try testing.expectEqualStrings("a b", implementation.strip("  a b  "));
}

test "strip of all whitespace is empty" {
    try testing.expectEqualStrings("", implementation.strip(" \t\n "));
}

test "a bare build directory at the repo root is build output" {
    try testing.expect(implementation.isBuildOutput("build/gen.c"));
}

test "a build directory under examples is build output at any depth" {
    try testing.expect(implementation.isBuildOutput("examples/blinky/build/gen.c"));
    try testing.expect(implementation.isBuildOutput("examples/a/b/c/build-cov/gen.c"));
}

test "a build directory under scripts is source, not build output (#359)" {
    try testing.expect(!implementation.isBuildOutput("scripts/build/helper.c"));
}

test "builders is not a build directory" {
    try testing.expect(!implementation.isBuildDirName("builders"));
    try testing.expect(!implementation.isBuildOutput("scripts/builders/x.c"));
}

test "the build- build_ and cmake-build- prefixes are build directories" {
    try testing.expect(implementation.isBuildDirName("build"));
    try testing.expect(implementation.isBuildDirName("build-cov"));
    try testing.expect(implementation.isBuildDirName("build_fuzz"));
    try testing.expect(implementation.isBuildDirName("cmake-build-debug"));
}

test "tool-owned directory names are build output at any depth" {
    try testing.expect(implementation.isBuildOutput("libs/ra8_core/__pycache__/x.c"));
    try testing.expect(implementation.isBuildOutput("scripts/a/.zig-cache/x.c"));
    try testing.expect(implementation.isBuildOutput("a/b/CMakeFiles/x.c"));
    try testing.expect(implementation.isBuildOutput("a/b/_deps/x.c"));
    try testing.expect(implementation.isBuildOutput("a/b/node_modules/x.c"));
}

test "a FILE named build is not a build tree" {
    try testing.expect(!implementation.isBuildOutput("examples/blinky/build"));
}

test "the path predicate strips the repository root" {
    try testing.expect(implementation.isBuildOutputPath("/repo/examples/x/build/gen.c", "/repo"));
    try testing.expect(!implementation.isBuildOutputPath("/repo/examples/x/src/main.c", "/repo"));
}

test "the path predicate strips a leading ./" {
    try testing.expect(implementation.isBuildOutputPath("./examples/x/build/gen.c", "/repo"));
}

test "the path predicate folds backslashes to slashes" {
    try testing.expect(implementation.isBuildOutputPath("examples\\x\\build\\gen.c", "/repo"));
}

test "a build tree outside the repository root is NOT build output, and CPython settled it" {
    // The root prefix does not strip, so the leading component is `elsewhere`:
    // not index 0 and not one of the build-tree roots, so the rule declines.
    // My first expectation here was the opposite; probing the predecessor
    // decided it, and the asymmetry is pinned rather than tidied away.
    try testing.expect(!implementation.isBuildOutputPath("/elsewhere/build/gen.c", "/repo"));
    try testing.expect(!implementation.isBuildOutputPath("/elsewhere/src/main.c", "/repo"));
    // A build directory that IS the leading component still counts.
    try testing.expect(implementation.isBuildOutputPath("/repo/build/gen.c", "/repo"));
    try testing.expect(implementation.isBuildOutputPath("build/gen.c", "/repo"));
}

test "pathlib suffix semantics: the last dot segment" {
    try testing.expectEqualStrings(".c", implementation.pathlibSuffix("main.c"));
    try testing.expectEqualStrings(".c", implementation.pathlibSuffix("a.tar.c"));
    try testing.expectEqualStrings(".hpp", implementation.pathlibSuffix("view.hpp"));
    try testing.expectEqualStrings("", implementation.pathlibSuffix("README"));
}

test "a dot-prefixed name with no stem has no suffix, so it is not source on the argv path" {
    try testing.expectEqualStrings("", implementation.pathlibSuffix(".c"));
    try testing.expect(!implementation.isSourceName(".c"));
}

test "the sweep's glob DOES list a file called exactly .c" {
    try testing.expect(implementation.globMatchesSuffix(".c", ".c"));
}

test "the four source suffixes are source and nothing else is" {
    try testing.expect(implementation.isSourceName("a.c"));
    try testing.expect(implementation.isSourceName("a.h"));
    try testing.expect(implementation.isSourceName("a.cpp"));
    try testing.expect(implementation.isSourceName("a.hpp"));
    try testing.expect(!implementation.isSourceName("a.C"));
    try testing.expect(!implementation.isSourceName("a.cc"));
    try testing.expect(!implementation.isSourceName("a.py"));
}

test "scanText numbers findings by line and strips the snippet" {
    var findings = try implementation.scanText(
        testing.allocator,
        "examples/x/src/main.c",
        "ok\n" ++ implementation.selftest_idiom ++ "\nok\n",
    );
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 1), findings.items.len);
    try testing.expectEqual(@as(usize, 2), findings.items[0].line_number);
    try testing.expectEqualStrings("examples/x/src/main.c", findings.items[0].path);
    try testing.expectEqualStrings(
        "cfg.pin = ((uint16_t)k_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_11;",
        findings.items[0].snippet,
    );
}

test "scanText reports every offending line, not just the first" {
    const body = implementation.selftest_idiom ++ "\nclean\n" ++ implementation.selftest_idiom ++ "\n";
    var findings = try implementation.scanText(testing.allocator, "p.c", body);
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 2), findings.items.len);
    try testing.expectEqual(@as(usize, 1), findings.items[0].line_number);
    try testing.expectEqual(@as(usize, 3), findings.items[1].line_number);
}

test "scanText finds nothing in a clean body" {
    var findings = try implementation.scanText(testing.allocator, "p.c", "int main(void) { return 0; }\n");
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 0), findings.items.len);
}

test "an idiom split across two lines is not a finding, because the scan is per line" {
    var findings = try implementation.scanText(
        testing.allocator,
        "p.c",
        "  cfg.pin = ((uint16_t)k_ra8_port_6 << 8)\n | (uint16_t)k_ra8_pin_11;\n",
    );
    defer findings.deinit();
    try testing.expectEqual(@as(usize, 0), findings.items.len);
}

test "undecodable bytes become U+FFFD instead of aborting the sweep" {
    const decoded = try implementation.decodeLossy(testing.allocator, "a\xffb");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("a\u{FFFD}b", decoded);
}

test "a valid multi-byte sequence survives decoding unchanged" {
    const decoded = try implementation.decodeLossy(testing.allocator, "caf\u{00e9}\u{3000}");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("caf\u{00e9}\u{3000}", decoded);
}

test "a truncated sequence at end of file becomes ONE replacement character" {
    // Measured against CPython 3.13: b"a\xe3\x80".decode("utf-8", "replace")
    // is "a\uFFFD", not "a\uFFFD\uFFFD". The lead byte and the continuation
    // byte that was still valid for its position are one maximal subpart.
    const decoded = try implementation.decodeLossy(testing.allocator, "a\xe3\x80");
    defer testing.allocator.free(decoded);
    try testing.expectEqualStrings("a\u{FFFD}", decoded);
}

test "each maximal invalid subpart costs exactly one replacement character" {
    // Every expectation here was read off CPython 3.13 first.
    const cases = [_]struct { bytes: []const u8, want: []const u8 }{
        // A truncated three-byte sequence: lead plus one good continuation.
        .{ .bytes = "pre\xef\xbfpost", .want = "pre\u{FFFD}post" },
        // A truncated four-byte sequence: lead plus two good continuations.
        .{ .bytes = "\xf0\x9f\x98", .want = "\u{FFFD}" },
        // Two bytes that can neither start nor continue anything: two subparts.
        .{ .bytes = "\xff\xfe", .want = "\u{FFFD}\u{FFFD}" },
        // E0 admits A0-BF only, so 0x80 does not continue it.
        .{ .bytes = "\xe0\x80", .want = "\u{FFFD}\u{FFFD}" },
        // ED admits 80-9F only: a surrogate is three separate subparts.
        .{ .bytes = "\xed\xa0\x80", .want = "\u{FFFD}\u{FFFD}\u{FFFD}" },
        // F4 admits 80-8F only, so anything past U+10FFFF splits apart.
        .{ .bytes = "\xf4\x90\x80\x80", .want = "\u{FFFD}\u{FFFD}\u{FFFD}\u{FFFD}" },
        // An overlong two-byte encoding: C0 leads nothing.
        .{ .bytes = "\xc0\xaf", .want = "\u{FFFD}\u{FFFD}" },
        // A BOM and a real U+FFFD are valid and survive untouched.
        .{ .bytes = "\u{FEFF}ok\u{FFFD}", .want = "\u{FEFF}ok\u{FFFD}" },
    };
    for (cases) |case| {
        const decoded = try implementation.decodeLossy(testing.allocator, case.bytes);
        defer testing.allocator.free(decoded);
        try testing.expectEqualStrings(case.want, decoded);
    }
}

test "a finding survives an undecodable byte elsewhere on the line" {
    const body = try implementation.decodeLossy(
        testing.allocator,
        "  x = \xff((uint16_t)k_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_11;",
    );
    defer testing.allocator.free(body);
    try testing.expect(implementation.matchEncoding(body));
}

test "the clean line names the scanned count on one line" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.renderClean(buffer.writer(), 408);
    try testing.expectEqualStrings(
        "check_example_board_pins: 408 example file(s) scanned, none hand-encode a board pin.\n",
        buffer.items,
    );
}

test "the floor message names the count and the floor" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.renderFatalFloor(buffer.writer(), 3);
    try testing.expect(std.mem.indexOf(u8, buffer.items, "only 3 example file(s) in scope") != null);
    try testing.expect(std.mem.indexOf(u8, buffer.items, "floor is 320") != null);
}

test "the finding header ends with a blank line, as the Python print did" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.renderFindingHeader(buffer.writer(), 2);
    try testing.expectEqualStrings(
        "check_example_board_pins: 2 hand-encoded board pin(s) in examples:\n\n",
        buffer.items,
    );
}

test "a finding line carries path, line number and the stripped source" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try implementation.renderFinding(buffer.writer(), .{
        .path = "examples/x/src/main.c",
        .line_number = 42,
        .snippet = "cfg.pin = 1;",
    });
    try testing.expectEqualStrings("  examples/x/src/main.c:42  cfg.pin = 1;\n", buffer.items);
}

test "the guidance names the board layer as the fix" {
    try testing.expect(std.mem.indexOf(u8, implementation.guidance, "libs/ra8_board_ek_ra8d2") != null);
    try testing.expect(std.mem.indexOf(u8, implementation.guidance, "ra8_board_sw_pin") != null);
}

test "the floor is the measured one and the scan root is examples" {
    try testing.expectEqual(@as(usize, 320), implementation.file_floor);
    try testing.expectEqualStrings("examples", implementation.scan_root);
}

test "the source suffix order is the order the sweep enumerated in" {
    try testing.expectEqualStrings(".c", implementation.source_suffixes[0]);
    try testing.expectEqualStrings(".h", implementation.source_suffixes[1]);
    try testing.expectEqualStrings(".cpp", implementation.source_suffixes[2]);
    try testing.expectEqualStrings(".hpp", implementation.source_suffixes[3]);
}
