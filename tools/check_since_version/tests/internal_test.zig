// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// Behavioural regression tests for the pure half of `check_since_version`
// (#858). Every expectation here was read off `check-since-version.py`'s
// regexes and helpers before it was deleted, so a later edit that widens or
// narrows the gate has to break one of these first.

const std = @import("std");
const gate = @import("implementation");

const testing = std.testing;

// --- PUBLIC_DECL -----------------------------------------------------------

test "a plain ra8 declaration captures its symbol" {
    try testing.expectEqualStrings("ra8_foo", gate.matchPublicDecl("ra8_err_t ra8_foo(void);").?);
}

test "the capture is the symbol, not the return type" {
    const symbol = gate.matchPublicDecl("ra8_status_t ra8_display_init(int);").?;
    try testing.expectEqualStrings("ra8_display_init", symbol);
}

test "leading whitespace is allowed by the pattern's own backslash-s-star" {
    try testing.expectEqualStrings("ra8_bar", gate.matchPublicDecl("    ra8_err_t ra8_bar(void);").?);
}

test "a nodiscard attribute is accepted" {
    const line = "[[nodiscard]] ra8_err_t ra8_baz(void);";
    try testing.expectEqualStrings("ra8_baz", gate.matchPublicDecl(line).?);
}

test "nodiscard needs whitespace after it" {
    try testing.expect(gate.matchPublicDecl("[[nodiscard]]ra8_err_t ra8_baz(void);") == null);
}

test "static inline is accepted" {
    const line = "static inline ra8_u32_t ra8_ticks(void)";
    try testing.expectEqualStrings("ra8_ticks", gate.matchPublicDecl(line).?);
}

test "static alone is not the prefix the pattern allows" {
    try testing.expect(gate.matchPublicDecl("static ra8_err_t ra8_foo(void);") == null);
}

test "inline alone is not the prefix the pattern allows" {
    try testing.expect(gate.matchPublicDecl("inline ra8_err_t ra8_foo(void);") == null);
}

test "nodiscard and static inline together are accepted" {
    const line = "[[nodiscard]] static inline ra8_err_t ra8_q(void);";
    try testing.expectEqualStrings("ra8_q", gate.matchPublicDecl(line).?);
}

test "a pointer return type with the star on the type matches" {
    const line = "ra8_buf_t* ra8_alloc(void);";
    try testing.expectEqualStrings("ra8_alloc", gate.matchPublicDecl(line).?);
}

test "a pointer return type with a space before the star matches" {
    const line = "ra8_buf_t * ra8_alloc(void);";
    try testing.expectEqualStrings("ra8_alloc", gate.matchPublicDecl(line).?);
}

test "a star bound to the symbol does NOT match, as in the Python" {
    // The `(?:\s*\*)` group can only end on the star, leaving the following
    // `\s+` nothing to consume. Inherited, not a widening.
    try testing.expect(gate.matchPublicDecl("ra8_buf_t *ra8_alloc(void);") == null);
}

test "a non-ra8 return type does not match" {
    try testing.expect(gate.matchPublicDecl("int ra8_foo(void);") == null);
}

test "a non-ra8 symbol does not match" {
    try testing.expect(gate.matchPublicDecl("ra8_err_t helper(void);") == null);
}

test "the type needs at least one word character after ra8_" {
    try testing.expect(gate.matchPublicDecl("ra8_ ra8_foo(void);") == null);
}

test "the symbol needs at least one word character after ra8_" {
    try testing.expect(gate.matchPublicDecl("ra8_err_t ra8_(void);") == null);
}

test "whitespace is allowed between the symbol and the open paren" {
    try testing.expectEqualStrings("ra8_foo", gate.matchPublicDecl("ra8_err_t ra8_foo (void);").?);
}

test "a declaration with no open paren is not a declaration" {
    try testing.expect(gate.matchPublicDecl("ra8_err_t ra8_foo;") == null);
}

test "the match is anchored, so a mid-line declaration is ignored" {
    try testing.expect(gate.matchPublicDecl("x = ra8_err_t ra8_foo(void);") == null);
}

test "a tab counts as the separating whitespace" {
    try testing.expectEqualStrings("ra8_foo", gate.matchPublicDecl("ra8_err_t\tra8_foo(void);").?);
}

test "a non-breaking space counts, because Python's backslash-s is Unicode" {
    try testing.expectEqualStrings("ra8_foo", gate.matchPublicDecl("ra8_err_t\u{00A0}ra8_foo(void);").?);
}

test "a non-ASCII letter is a word character in the symbol" {
    const line = "ra8_err_t ra8_caf\u{00E9}(void);";
    try testing.expectEqualStrings("ra8_caf\u{00E9}", gate.matchPublicDecl(line).?);
}

test "an empty line matches nothing" {
    try testing.expect(gate.matchPublicDecl("") == null);
}

test "a comment line matches nothing" {
    try testing.expect(gate.matchPublicDecl(" * ra8_err_t ra8_foo(void);") == null);
}

// --- SINCE_VALUE -----------------------------------------------------------

test "a bare two-part version is captured" {
    try testing.expectEqualStrings("1.2", gate.findSinceValue(" * @since 1.2").?);
}

test "a three-part version is captured" {
    try testing.expectEqualStrings("0.1.0", gate.findSinceValue(" * @since 0.1.0").?);
}

test "a four-part version captures only the first three groups" {
    try testing.expectEqualStrings("1.2.3", gate.findSinceValue("@since 1.2.3.4").?);
}

test "the legacy Version spelling is accepted" {
    try testing.expectEqualStrings("0.1.0", gate.findSinceValue("@since Version 0.1.0").?);
}

test "Version needs whitespace after it" {
    try testing.expect(gate.findSinceValue("@since Version0.1.0") == null);
}

test "a trailing lowercase letter is part of the capture" {
    try testing.expectEqualStrings("1.2.3a", gate.findSinceValue("@since 1.2.3a").?);
}

test "a trailing uppercase letter is not part of the capture" {
    try testing.expectEqualStrings("1.2.3", gate.findSinceValue("@since 1.2.3A").?);
}

test "the tag needs whitespace before the number" {
    try testing.expect(gate.findSinceValue("@since1.2.3") == null);
}

test "a single number is not a version" {
    try testing.expect(gate.findSinceValue("@since 1") == null);
}

test "a dot with no digits after it is not a version" {
    try testing.expect(gate.findSinceValue("@since 1.") == null);
}

test "the search finds a tag mid-line" {
    try testing.expectEqualStrings("0.1.0", gate.findSinceValue("/** @since 0.1.0 */").?);
}

test "a second tag is reached when the first cannot match" {
    try testing.expectEqualStrings("2.0.0", gate.findSinceValue("@since bad @since 2.0.0").?);
}

test "only the first matching tag on a line is reported" {
    try testing.expectEqualStrings("1.0.0", gate.findSinceValue("@since 1.0.0 @since 2.0.0").?);
}

test "Arabic-Indic digits do not match, because the class is 0-9" {
    try testing.expect(gate.findSinceValue("@since \u{0661}.\u{0662}.\u{0663}") == null);
}

test "several spaces before the version are fine" {
    try testing.expectEqualStrings("0.1.0", gate.findSinceValue("@since    0.1.0").?);
}

test "a newline-class separator counts as whitespace inside a line" {
    try testing.expectEqualStrings("0.1.0", gate.findSinceValue("@since\t0.1.0").?);
}

test "a line with no tag has no value" {
    try testing.expect(gate.findSinceValue(" * nothing here") == null);
}

// --- SINCE_TAG_PRESENT -----------------------------------------------------

test "the tag test is a plain substring" {
    try testing.expect(gate.hasSinceTag("/** @since 0.1.0 */"));
}

test "the tag test does not match a different tag" {
    try testing.expect(!gate.hasSinceTag("/** @brief hello */"));
}

// --- splitlines ------------------------------------------------------------

test "lines split on a newline" {
    var iterator = gate.LineIterator{ .text = "a\nb\n" };
    try testing.expectEqualStrings("a", iterator.next().?);
    try testing.expectEqualStrings("b", iterator.next().?);
    try testing.expect(iterator.next() == null);
}

test "a CRLF pair is one break" {
    var iterator = gate.LineIterator{ .text = "a\r\nb" };
    try testing.expectEqualStrings("a", iterator.next().?);
    try testing.expectEqualStrings("b", iterator.next().?);
    try testing.expect(iterator.next() == null);
}

test "a lone CR is a break, as in str.splitlines" {
    var iterator = gate.LineIterator{ .text = "a\rb" };
    try testing.expectEqualStrings("a", iterator.next().?);
    try testing.expectEqualStrings("b", iterator.next().?);
}

test "a form feed is a break, as in str.splitlines" {
    var iterator = gate.LineIterator{ .text = "a\x0Cb" };
    try testing.expectEqualStrings("a", iterator.next().?);
    try testing.expectEqualStrings("b", iterator.next().?);
}

test "U+2028 is a break, as in str.splitlines" {
    var iterator = gate.LineIterator{ .text = "a\u{2028}b" };
    try testing.expectEqualStrings("a", iterator.next().?);
    try testing.expectEqualStrings("b", iterator.next().?);
}

test "text with no trailing break yields its last line" {
    var iterator = gate.LineIterator{ .text = "only" };
    try testing.expectEqualStrings("only", iterator.next().?);
    try testing.expect(iterator.next() == null);
}

test "empty text yields no lines" {
    var iterator = gate.LineIterator{ .text = "" };
    try testing.expect(iterator.next() == null);
}

// --- VERSION parsing -------------------------------------------------------

test "a semver line is accepted" {
    try testing.expect(gate.isSemver("0.1.0"));
}

test "a two-part version is not semver" {
    try testing.expect(!gate.isSemver("0.1"));
}

test "a four-part version is not semver" {
    try testing.expect(!gate.isSemver("0.1.0.1"));
}

test "a suffixed version is not semver" {
    try testing.expect(!gate.isSemver("0.1.0a"));
}

test "an empty VERSION is not semver" {
    try testing.expect(!gate.isSemver(""));
}

test "strip removes ASCII whitespace both ends" {
    try testing.expectEqualStrings("0.1.0", gate.pythonStrip("  0.1.0\n"));
}

test "strip removes Unicode whitespace, as str.strip does" {
    try testing.expectEqualStrings("0.1.0", gate.pythonStrip("\u{00A0}0.1.0\u{2003}"));
}

test "strip of all-whitespace is empty" {
    try testing.expectEqualStrings("", gate.pythonStrip(" \n\t"));
}

// --- scope -----------------------------------------------------------------

test "a public library header is under lib inc" {
    try testing.expect(gate.isUnderLibInc("/repo/libs/ra8_gpio/inc/ra8_gpio.h"));
}

test "a private library source is not under lib inc" {
    try testing.expect(!gate.isUnderLibInc("/repo/libs/ra8_gpio/src/ra8_gpio.c"));
}

test "a header outside libs/ra8_ is not under lib inc" {
    try testing.expect(!gate.isUnderLibInc("/repo/tools/vela/inc/vela.h"));
}

test "a C file under libs/ra8_ inc is not under lib inc" {
    try testing.expect(!gate.isUnderLibInc("/repo/libs/ra8_gpio/inc/impl.c"));
}

test "the four source suffixes are in scope" {
    try testing.expect(gate.hasSourceSuffix("a.c"));
    try testing.expect(gate.hasSourceSuffix("a.h"));
    try testing.expect(gate.hasSourceSuffix("a.cpp"));
    try testing.expect(gate.hasSourceSuffix("a.hpp"));
}

test "other suffixes are out of scope" {
    try testing.expect(!gate.hasSourceSuffix("a.md"));
    try testing.expect(!gate.hasSourceSuffix("a.cc"));
    try testing.expect(!gate.hasSourceSuffix("Makefile"));
}

test "a dotfile has no suffix, as in pathlib" {
    try testing.expect(!gate.hasSourceSuffix(".h"));
}

test "build is a build directory name" {
    try testing.expect(gate.isBuildDirName("build"));
    try testing.expect(gate.isBuildDirName("build-cov"));
    try testing.expect(gate.isBuildDirName("cmake-build-debug"));
}

test "builders is NOT a build directory name" {
    try testing.expect(!gate.isBuildDirName("builders"));
}

test "a build tree under a known root is build output" {
    try testing.expect(gate.isBuildOutput("tools/ra8_emulator/build/x.c"));
}

test "a build directory at the repo root is build output" {
    try testing.expect(gate.isBuildOutput("build/x.c"));
}

test "a build directory under an unlisted root is source" {
    try testing.expect(!gate.isBuildOutput("scripts/build/x.c"));
}

test "a tool-owned directory is build output at any depth" {
    try testing.expect(gate.isBuildOutput("libs/ra8_gpio/CMakeFiles/x.c"));
}

test "a file named build is not a build tree" {
    try testing.expect(!gate.isBuildOutput("scripts/build"));
}

test "first-party scope keeps an ordinary source file" {
    try testing.expect(gate.inFirstPartyScope("libs/ra8_gpio/src/ra8_gpio.c"));
}

test "first-party scope keeps a tools source file" {
    try testing.expect(gate.inFirstPartyScope("tools/vela/src/main.c"));
}

test "first-party scope drops vendored SOUP" {
    try testing.expect(!gate.inFirstPartyScope("libs/third_party/lvgl/lvgl.c"));
    try testing.expect(!gate.inFirstPartyScope("apps/shared_libs/third_party/x.c"));
}

test "first-party scope drops generated tables" {
    try testing.expect(!gate.inFirstPartyScope("libs/ra8_fonts/font.c"));
    try testing.expect(!gate.inFirstPartyScope("tools/vela/generated/model.h"));
}

test "first-party scope drops the vendored C port tree" {
    try testing.expect(!gate.inFirstPartyScope("port/threadx/tx_api.c"));
}

test "first-party scope drops build output" {
    try testing.expect(!gate.inFirstPartyScope("tests/build/x.c"));
}

test "first-party scope drops a non-source suffix" {
    try testing.expect(!gate.inFirstPartyScope("docs/guide.md"));
}

// --- the two halves --------------------------------------------------------

fn collectPresence(text: []const u8, out: *std.ArrayList([]const u8)) !void {
    try gate.presenceProblems(testing.allocator, "h.h", text, out);
}

test "a declaration with no tag above it is a problem" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try collectPresence("ra8_err_t ra8_foo(void);\n", &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
    try testing.expectEqualStrings("h.h:1: ra8_foo missing @since", problems.items[0]);
}

test "a declaration with a tag just above it is quiet" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try collectPresence("/** @since 0.1.0 */\nra8_err_t ra8_foo(void);\n", &problems);
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

test "a tag exactly 30 lines above still counts" {
    var text = std.ArrayList(u8).init(testing.allocator);
    defer text.deinit();
    try text.appendSlice("/** @since 0.1.0 */\n");
    var filler: usize = 0;
    while (filler < 29) : (filler += 1) try text.appendSlice("\n");
    try text.appendSlice("ra8_err_t ra8_foo(void);\n");
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try collectPresence(text.items, &problems);
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

test "a tag 31 lines above is out of the lookback" {
    var text = std.ArrayList(u8).init(testing.allocator);
    defer text.deinit();
    try text.appendSlice("/** @since 0.1.0 */\n");
    var filler: usize = 0;
    while (filler < 30) : (filler += 1) try text.appendSlice("\n");
    try text.appendSlice("ra8_err_t ra8_foo(void);\n");
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try collectPresence(text.items, &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
}

test "the line number counts from one" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try collectPresence("\n\nra8_err_t ra8_foo(void);\n", &problems);
    try testing.expectEqualStrings("h.h:3: ra8_foo missing @since", problems.items[0]);
}

test "invalid UTF-8 is skipped, as a UnicodeDecodeError was" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try collectPresence("ra8_err_t ra8_foo(void);\n\xFF\xFE", &problems);
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

test "a wrong value is a problem naming both versions" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try gate.valueProblems(testing.allocator, "a.c", "/** @since 9.9.9 */\n", "0.1.0", &problems);
    try testing.expectEqual(@as(usize, 1), problems.items.len);
    try testing.expectEqualStrings("a.c:1: @since 9.9.9 != project 0.1.0", problems.items[0]);
}

test "the right value is quiet" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try gate.valueProblems(testing.allocator, "a.c", "/** @since 0.1.0 */\n", "0.1.0", &problems);
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

test "the legacy Version spelling of the right value is quiet" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try gate.valueProblems(
        testing.allocator,
        "a.c",
        "/** @since Version 0.1.0 */\n",
        "0.1.0",
        &problems,
    );
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

test "one problem per offending line" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try gate.valueProblems(
        testing.allocator,
        "a.c",
        "@since 1.0.0\nok\n@since 2.0.0\n",
        "0.1.0",
        &problems,
    );
    try testing.expectEqual(@as(usize, 2), problems.items.len);
    try testing.expectEqualStrings("a.c:3: @since 2.0.0 != project 0.1.0", problems.items[1]);
}

test "a value problem on a CRLF file numbers lines the same way" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try gate.valueProblems(testing.allocator, "a.c", "x\r\n@since 9.9.9\r\n", "0.1.0", &problems);
    try testing.expectEqualStrings("a.c:2: @since 9.9.9 != project 0.1.0", problems.items[0]);
}

test "invalid UTF-8 skips the value check too" {
    var problems = std.ArrayList([]const u8).init(testing.allocator);
    defer freeAll(&problems);
    try gate.valueProblems(testing.allocator, "a.c", "@since 9.9.9\n\xFF", "0.1.0", &problems);
    try testing.expectEqual(@as(usize, 0), problems.items.len);
}

// --- rendering -------------------------------------------------------------

test "the problem report carries the header, the lines and the count" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    const problems = [_][]const u8{ "a.c:1: one", "a.c:2: two" };
    try gate.writeProblems(buffer.writer(), "0.1.0", &problems);
    try testing.expectEqualStrings(
        "check_since_version: project version is 0.1.0\na.c:1: one\na.c:2: two\n\n2 issue(s) found.\n",
        buffer.items,
    );
}

test "an expectation prints ok or FAIL" {
    var buffer = std.ArrayList(u8).init(testing.allocator);
    defer buffer.deinit();
    try gate.writeExpectation(buffer.writer(), true, "holds");
    try gate.writeExpectation(buffer.writer(), false, "breaks");
    try testing.expectEqualStrings("  [ok] holds\n  [FAIL] breaks\n", buffer.items);
}

test "a clean selftest verdict exits zero" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    defer err.deinit();
    const status = try gate.writeSelftestVerdict(out.writer(), err.writer(), &[_][]const u8{});
    try testing.expectEqual(@as(u8, 0), status);
    try testing.expectEqualStrings("selftest: all assertions held (both directions).\n", out.items);
    try testing.expectEqualStrings("", err.items);
}

test "a failing selftest verdict lists each assertion on stderr" {
    var out = std.ArrayList(u8).init(testing.allocator);
    defer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    defer err.deinit();
    const failures = [_][]const u8{"a wrong @since value fires"};
    const status = try gate.writeSelftestVerdict(out.writer(), err.writer(), &failures);
    try testing.expectEqual(@as(u8, 1), status);
    try testing.expectEqualStrings(
        "\nSELFTEST FAILED: 1 assertion(s)\n  a wrong @since value fires\n",
        err.items,
    );
}

test "scope ordering is byte order, as Python's sorted was" {
    var names = [_][]const u8{ "b.c", "A.c", "a.c" };
    std.mem.sort([]const u8, &names, {}, gate.lessThanByBytes);
    try testing.expectEqualStrings("A.c", names[0]);
    try testing.expectEqualStrings("a.c", names[1]);
    try testing.expectEqualStrings("b.c", names[2]);
}

fn freeAll(list: *std.ArrayList([]const u8)) void {
    for (list.items) |item| testing.allocator.free(item);
    list.deinit();
}
